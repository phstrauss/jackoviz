/*
 * jackoviz-remote.cpp — Qt Quick remote controller for jackoviz.
 *
 * Launch: fork + execve of sibling `jackoviz-cli` (-n / --fast / --rpc-only).
 * JACK: list audio output ports for the capture dropdown.
 * gRPC: all runtime settings + Quit via JvzController on 127.0.0.1:50051.
 */

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <jack/jack.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "jvz_version.h"

#if defined(JVZ_HAS_GRPC)
#include "jvzcontroller.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#endif

class RemoteController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool launched READ launched NOTIFY launchedChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(int viewModeIndex READ viewModeIndex NOTIFY viewModeIndexChanged)
    /* False in oscilloscope (fmax unused there). */
    Q_PROPERTY(bool plotFreqEnabled READ plotFreqEnabled NOTIFY viewModeIndexChanged)
    /* dB floor/ceil unused on oscilloscope (time × amplitude). */
    Q_PROPERTY(bool dbRangeEnabled READ dbRangeEnabled NOTIFY viewModeIndexChanged)
    /* Line width only affects scope + 1D paths (not 2D/3D spectrograms). */
    Q_PROPERTY(bool lineWidthEnabled READ lineWidthEnabled NOTIFY viewModeIndexChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool jackRunning READ jackRunning NOTIFY jackRunningChanged)
    Q_PROPERTY(QStringList jackPorts READ jackPorts NOTIFY jackPortsChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    /* Persisted UI prefs (QSettings); restored at startup, saved on exit. */
    Q_PROPERTY(int fftSize READ fftSize WRITE setFftSize NOTIFY fftSizeChanged)
    Q_PROPERTY(bool fastMode READ fastMode WRITE setFastMode NOTIFY fastModeChanged)
    Q_PROPERTY(QString jackPort READ jackPort NOTIFY jackPortChanged)
    Q_PROPERTY(double plotFreq READ plotFreq NOTIFY plotFreqChanged)
    Q_PROPERTY(double dbCeil READ dbCeil NOTIFY dbCeilChanged)
    Q_PROPERTY(double dbFloor READ dbFloor NOTIFY dbFloorChanged)
    Q_PROPERTY(double kaiserBeta READ kaiserBeta NOTIFY kaiserBetaChanged)
    Q_PROPERTY(double lineWidth READ lineWidth NOTIFY lineWidthChanged)

public:
    explicit RemoteController(QObject* parent = nullptr)
        : QObject(parent)
    {
        loadSettings();

        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &RemoteController::reapChild);
        timer->start(400);

        openJackClient();
        refreshJackPorts();

        auto* port_timer = new QTimer(this);
        connect(port_timer, &QTimer::timeout, this, &RemoteController::refreshJackPorts);
        port_timer->start(2000);
    }

    ~RemoteController() override { closeJackClient(); }

    bool launched() const { return m_pid > 0; }
    bool paused() const { return m_paused; }
    int viewModeIndex() const { return m_view_index; }
    /* qmlIndex: 0=scope — fmax unused on oscilloscope. */
    bool plotFreqEnabled() const { return m_view_index != 0; }
    bool dbRangeEnabled() const { return m_view_index != 0; }
    /* qmlIndex: 0=scope, 1=1D — only those use stroke width. */
    bool lineWidthEnabled() const { return m_view_index == 0 || m_view_index == 1; }
    QString statusText() const { return m_status; }
    bool jackRunning() const { return m_jack_running; }
    QStringList jackPorts() const { return m_jack_ports; }
    QString appVersion() const { return QStringLiteral(JACKOVIZ_VERSION); }
    int fftSize() const { return m_fft_size; }
    bool fastMode() const { return m_fast_mode; }
    QString jackPort() const { return m_jack_port; }
    double plotFreq() const { return m_plot_freq; }
    double dbCeil() const { return m_db_ceil; }
    double dbFloor() const { return m_db_floor; }
    double kaiserBeta() const { return m_kaiser_beta; }
    double lineWidth() const { return static_cast<double>(m_line_width); }

    void setFftSize(int fftSize)
    {
        if (fftSize != 1024 && fftSize != 2048 && fftSize != 4096 && fftSize != 8192)
            return;
        if (m_fft_size == fftSize)
            return;
        m_fft_size = fftSize;
        emit fftSizeChanged();
    }

    void setFastMode(bool fast)
    {
        if (m_fast_mode == fast)
            return;
        m_fast_mode = fast;
        emit fastModeChanged();
    }

    Q_INVOKABLE void saveSettings()
    {
        QSettings s;
        s.beginGroup(QStringLiteral("ui"));
        s.setValue(QStringLiteral("fftSize"), m_fft_size);
        s.setValue(QStringLiteral("fastMode"), m_fast_mode);
        s.setValue(QStringLiteral("jackPort"), m_jack_port);
        s.setValue(QStringLiteral("viewMode"), m_view_index);
        s.setValue(QStringLiteral("plotFreq"), m_plot_freq);
        s.setValue(QStringLiteral("dbCeil"), m_db_ceil);
        s.setValue(QStringLiteral("dbFloor"), m_db_floor);
        s.setValue(QStringLiteral("kaiserBeta"), m_kaiser_beta);
        s.setValue(QStringLiteral("lineWidth"), static_cast<double>(m_line_width));
        s.endGroup();
        s.sync();
    }

    Q_INVOKABLE void launch(int fftSize, bool fast)
    {
        if (m_pid > 0)
        {
            setStatus(QStringLiteral("jackoviz already running"));
            return;
        }

        if (fftSize != 1024 && fftSize != 2048 && fftSize != 4096 && fftSize != 8192)
        {
            setStatus(QStringLiteral("invalid FFT size: %1").arg(fftSize));
            return;
        }

        const QString binPath =
            QCoreApplication::applicationDirPath() + QStringLiteral("/jackoviz-cli");
        if (!QFileInfo::exists(binPath))
        {
            setStatus(QStringLiteral("jackoviz-cli not found: %1").arg(binPath));
            return;
        }

        const pid_t pid = fork();
        if (pid < 0)
        {
            setStatus(QStringLiteral("fork failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }

        if (pid == 0)
        {
            const QByteArray path = binPath.toLocal8Bit();
            const QByteArray nFlag = QByteArrayLiteral("-n");
            const QByteArray nVal = QByteArray::number(fftSize);
            const QByteArray fastFlag = QByteArrayLiteral("--fast");
            const QByteArray rpcFlag = QByteArrayLiteral("--rpc-only");

            char* argv[8];
            int argc = 0;
            argv[argc++] = const_cast<char*>(path.constData());
            argv[argc++] = const_cast<char*>(nFlag.constData());
            argv[argc++] = const_cast<char*>(nVal.constData());
            if (fast)
                argv[argc++] = const_cast<char*>(fastFlag.constData());
            argv[argc++] = const_cast<char*>(rpcFlag.constData());
            argv[argc] = nullptr;

            execv(path.constData(), argv);
            _exit(127);
        }

        m_pid = pid;
        m_fast = fast;
        m_paused = false;
        emit pausedChanged();
        /* --fast has no 2D/3D panels; default the remote view to 1D spectrum. */
        if (fast)
            setViewModeIndex(1);
#if defined(JVZ_HAS_GRPC)
        m_stub.reset();
        m_channel.reset();
#endif
        QString cmd = QStringLiteral("jackoviz-cli -n %1").arg(fftSize);
        if (fast)
            cmd += QStringLiteral(" --fast");
        cmd += QStringLiteral(" --rpc-only");
        setStatus(QStringLiteral("launched pid %1: %2").arg(pid).arg(cmd));
        emit launchedChanged();
    }

    Q_INVOKABLE void quitJackoviz()
    {
        if (m_pid <= 0)
        {
            setStatus(QStringLiteral("jackoviz is not running"));
            return;
        }

#if defined(JVZ_HAS_GRPC)
        ensureGrpcStub();
        if (m_stub)
        {
            jvz::QuitRequest request;
            jvz::SetResponse response;
            grpc::ClientContext context;
            context.set_deadline(
                std::chrono::system_clock::now() + std::chrono::seconds(2));
            const grpc::Status status = m_stub->Quit(&context, request, &response);
            if (status.ok() && response.ok())
            {
                setStatus(QStringLiteral("quit requested via gRPC"));
                QTimer::singleShot(800, this, [this]() {
                    if (m_pid > 0)
                        forceKillChild();
                    else
                        reapChild();
                });
                return;
            }
            m_stub.reset();
            m_channel.reset();
        }
#endif
        forceKillChild();
    }

    Q_INVOKABLE void refreshJackPorts()
    {
        QStringList ports;
        ports.append(QStringLiteral("(none)"));

        if (m_jack == nullptr)
            openJackClient();

        if (m_jack != nullptr)
        {
            const char** names = jack_get_ports(
                m_jack, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
            if (names != nullptr)
            {
                for (int i = 0; names[i] != nullptr; ++i)
                {
                    const QString name = QString::fromUtf8(names[i]);
                    if (name.startsWith(QLatin1String("jackoviz-remote:")))
                        continue;
                    ports.append(name);
                }
                jack_free(names);
            }
        }

        updateJackRunning(m_jack != nullptr);

        if (ports != m_jack_ports)
        {
            m_jack_ports = ports;
            emit jackPortsChanged();
        }
    }

    Q_INVOKABLE void connectJackPort(const QString& portName)
    {
        if (m_jack_port != portName)
        {
            m_jack_port = portName;
            emit jackPortChanged();
        }
        if (!m_jack_running)
        {
            setStatus(QStringLiteral("JACK is not running"));
            return;
        }
        if (portName.isEmpty() || portName == QLatin1String("(none)"))
        {
            setStatus(QStringLiteral("JACK port: (none)"));
            return;
        }
        if (!requireLaunched(QStringLiteral("will connect %1 after launch").arg(portName)))
            return;
#if defined(JVZ_HAS_GRPC)
        jvz::ConnectJackPortRequest request;
        request.set_port_name(portName.toStdString());
        jvz::SetResponse response;
        if (!rpc("ConnectJackPort", [&](grpc::ClientContext* ctx) {
                return m_stub->ConnectJackPort(ctx, request, &response);
            }, response))
            return;
        setStatus(QStringLiteral("connected %1").arg(portName));
#endif
    }

    /* qmlIndex: 0=scope, 1=1D, 2=2D, 3=3D */
    Q_INVOKABLE void setViewMode(int qmlIndex)
    {
        if (m_fast && qmlIndex >= 2)
            qmlIndex = 1; /* 1D spectrum default / only spectrogram-less choice */

        setViewModeIndex(qmlIndex);
        if (!requireLaunched(QStringLiteral("view mode queued")))
            return;
#if defined(JVZ_HAS_GRPC)
        jvz::ViewMode mode = jvz::VIEW_MODE_3D;
        const char* label = "3D";
        switch (qmlIndex)
        {
        case 0:
            mode = jvz::VIEW_MODE_SCOPE;
            label = "scope";
            break;
        case 1:
            mode = jvz::VIEW_MODE_1D;
            label = "1D";
            break;
        case 2:
            mode = jvz::VIEW_MODE_2D;
            label = "2D";
            break;
        case 3:
            mode = jvz::VIEW_MODE_3D;
            label = "3D";
            break;
        default:
            setStatus(QStringLiteral("invalid view index"));
            return;
        }

        jvz::SetViewModeRequest request;
        request.set_mode(mode);
        jvz::SetResponse response;
        if (!rpc("SetViewMode", [&](grpc::ClientContext* ctx) {
                return m_stub->SetViewMode(ctx, request, &response);
            }, response))
            return;
        setStatus(QStringLiteral("view: %1").arg(QLatin1String(label)));
#endif
    }

    Q_INVOKABLE void setPlotFreq(double freqHz)
    {
        if (m_plot_freq != freqHz)
        {
            m_plot_freq = freqHz;
            emit plotFreqChanged();
        }
        if (!plotFreqEnabled())
        {
            setStatus(QStringLiteral("max freq: ignored in oscilloscope view"));
            return;
        }
        if (!requireLaunched(QStringLiteral("max freq %1 Hz queued").arg(freqHz, 0, 'f', 0)))
            return;
#if defined(JVZ_HAS_GRPC)
        jvz::SetPlotFreqRequest request;
        request.set_freq_hz(freqHz);
        jvz::SetResponse response;
        if (!rpc("SetPlotFreq", [&](grpc::ClientContext* ctx) {
                return m_stub->SetPlotFreq(ctx, request, &response);
            }, response))
            return;
        setStatus(QStringLiteral("max freq: %1 Hz").arg(response.value(), 0, 'f', 0));
#endif
    }

    Q_INVOKABLE void setDbCeil(double ceilDb)
    {
        if (m_db_ceil != ceilDb)
        {
            m_db_ceil = ceilDb;
            emit dbCeilChanged();
        }
        if (!dbRangeEnabled())
        {
            setStatus(QStringLiteral("dB ceil: ignored in oscilloscope view"));
            return;
        }
        if (!requireLaunched(QStringLiteral("dB ceil %1 queued").arg(ceilDb, 0, 'f', 0)))
            return;
#if defined(JVZ_HAS_GRPC)
        jvz::SetDbCeilRequest request;
        request.set_ceil_db(ceilDb);
        jvz::SetResponse response;
        if (!rpc("SetDbCeil", [&](grpc::ClientContext* ctx) {
                return m_stub->SetDbCeil(ctx, request, &response);
            }, response))
            return;
        setStatus(QStringLiteral("dB ceil: %1").arg(response.value(), 0, 'f', 0));
#endif
    }

    Q_INVOKABLE void setDbFloor(double floorDb)
    {
        if (m_db_floor != floorDb)
        {
            m_db_floor = floorDb;
            emit dbFloorChanged();
        }
        if (!dbRangeEnabled())
        {
            setStatus(QStringLiteral("dB floor: ignored in oscilloscope view"));
            return;
        }
        if (!requireLaunched(QStringLiteral("dB floor %1 queued").arg(floorDb, 0, 'f', 0)))
            return;
#if defined(JVZ_HAS_GRPC)
        jvz::SetDbFloorRequest request;
        request.set_floor_db(floorDb);
        jvz::SetResponse response;
        if (!rpc("SetDbFloor", [&](grpc::ClientContext* ctx) {
                return m_stub->SetDbFloor(ctx, request, &response);
            }, response))
            return;
        setStatus(QStringLiteral("dB floor: %1").arg(response.value(), 0, 'f', 0));
#endif
    }

    Q_INVOKABLE void setKaiserBeta(double beta)
    {
        if (m_kaiser_beta != beta)
        {
            m_kaiser_beta = beta;
            emit kaiserBetaChanged();
        }
        if (!requireLaunched(QStringLiteral("Kaiser β %1 queued").arg(beta, 0, 'f', 1)))
            return;
#if defined(JVZ_HAS_GRPC)
        jvz::SetKaiserBetaRequest request;
        request.set_beta(beta);
        jvz::SetResponse response;
        if (!rpc("SetKaiserBeta", [&](grpc::ClientContext* ctx) {
                return m_stub->SetKaiserBeta(ctx, request, &response);
            }, response))
            return;
        setStatus(QStringLiteral("Kaiser β: %1").arg(response.value(), 0, 'f', 1));
#endif
    }

    Q_INVOKABLE void setLineWidth(float widthPx)
    {
        if (m_line_width != widthPx)
        {
            m_line_width = widthPx;
            emit lineWidthChanged();
        }
        if (!lineWidthEnabled())
        {
            setStatus(QStringLiteral("line width: ignored in 2D/3D spectrogram view"));
            return;
        }
        if (!requireLaunched(QStringLiteral("line width %1 px queued").arg(widthPx, 0, 'f', 0)))
            return;
#if defined(JVZ_HAS_GRPC)
        jvz::SetLineWidthRequest request;
        request.set_width_px(widthPx);
        jvz::SetResponse response;
        if (!rpc("SetLineWidth", [&](grpc::ClientContext* ctx) {
                return m_stub->SetLineWidth(ctx, request, &response);
            }, response))
            return;
        setStatus(QStringLiteral("line width: %1 px").arg(response.value(), 0, 'f', 0));
#endif
    }

    Q_INVOKABLE void setPaused(bool paused)
    {
        m_paused = paused;
        emit pausedChanged();
        if (!requireLaunched(paused ? QStringLiteral("pause queued")
                                    : QStringLiteral("resume queued")))
            return;
#if defined(JVZ_HAS_GRPC)
        jvz::SetPauseRequest request;
        request.set_paused(paused);
        jvz::SetResponse response;
        if (!rpc("SetPause", [&](grpc::ClientContext* ctx) {
                return m_stub->SetPause(ctx, request, &response);
            }, response))
            return;
        setStatus(paused ? QStringLiteral("paused") : QStringLiteral("running"));
#endif
    }

signals:
    void launchedChanged();
    void pausedChanged();
    void viewModeIndexChanged();
    void statusTextChanged();
    void jackRunningChanged();
    void jackPortsChanged();
    void fftSizeChanged();
    void fastModeChanged();
    void jackPortChanged();
    void plotFreqChanged();
    void dbCeilChanged();
    void dbFloorChanged();
    void kaiserBetaChanged();
    void lineWidthChanged();

public slots:
    void handleJackShutdown()
    {
        /* Client pointer is invalid after shutdown; do not jack_client_close. */
        m_jack = nullptr;
        updateJackRunning(false);
    }

private:
    void loadSettings()
    {
        QSettings s;
        s.beginGroup(QStringLiteral("ui"));
        const int fft = s.value(QStringLiteral("fftSize"), 4096).toInt();
        if (fft == 1024 || fft == 2048 || fft == 4096 || fft == 8192)
            m_fft_size = fft;
        m_fast_mode = s.value(QStringLiteral("fastMode"), false).toBool();
        m_jack_port = s.value(QStringLiteral("jackPort"), QStringLiteral("(none)")).toString();
        if (m_jack_port.isEmpty())
            m_jack_port = QStringLiteral("(none)");
        const int view = s.value(QStringLiteral("viewMode"), 1).toInt();
        if (view >= 0 && view <= 3)
            m_view_index = view;
        if (m_fast_mode && m_view_index >= 2)
            m_view_index = 1;
        m_plot_freq = s.value(QStringLiteral("plotFreq"), 6000.0).toDouble();
        m_db_ceil = s.value(QStringLiteral("dbCeil"), -20.0).toDouble();
        m_db_floor = s.value(QStringLiteral("dbFloor"), -120.0).toDouble();
        m_kaiser_beta = s.value(QStringLiteral("kaiserBeta"), 4.5).toDouble();
        m_line_width = static_cast<float>(s.value(QStringLiteral("lineWidth"), 1.0).toDouble());
        s.endGroup();
    }

    void setViewModeIndex(int index)
    {
        if (m_view_index == index)
            return;
        m_view_index = index;
        emit viewModeIndexChanged();
    }

    void setStatus(const QString& text)
    {
        if (m_status == text)
            return;
        m_status = text;
        emit statusTextChanged();
    }

    bool requireLaunched(const QString& queued_msg)
    {
        if (m_pid > 0)
            return true;
        setStatus(queued_msg);
        return false;
    }

    void updateJackRunning(bool running)
    {
        if (m_jack_running != running)
        {
            m_jack_running = running;
            emit jackRunningChanged();
        }
        if (!running)
        {
            setStatus(QStringLiteral("JACK is not running"));
            return;
        }
        if (m_status == QLatin1String("JACK is not running")
            || m_status.startsWith(QLatin1String("JACK open failed")))
            setStatus(QStringLiteral("OK"));
    }

    void openJackClient()
    {
        if (m_jack != nullptr)
            return;

        jack_status_t status = static_cast<jack_status_t>(0);
        m_jack = jack_client_open("jackoviz-remote", JackNoStartServer, &status);
        if (m_jack == nullptr)
        {
            updateJackRunning(false);
            return;
        }
        jack_on_shutdown(
            m_jack,
            [](void* arg) {
                QMetaObject::invokeMethod(
                    static_cast<RemoteController*>(arg),
                    "handleJackShutdown",
                    Qt::QueuedConnection);
            },
            this);
        updateJackRunning(true);
    }

    void closeJackClient()
    {
        if (m_jack == nullptr)
            return;
        jack_client_close(m_jack);
        m_jack = nullptr;
        m_jack_running = false;
    }

    void forceKillChild()
    {
        if (m_pid <= 0)
            return;
        if (kill(m_pid, SIGTERM) != 0)
        {
            setStatus(QStringLiteral("kill(%1) failed: %2")
                          .arg(m_pid)
                          .arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }
        setStatus(QStringLiteral("sent SIGTERM to pid %1").arg(m_pid));
        reapChild();
    }

#if defined(JVZ_HAS_GRPC)
    void ensureGrpcStub()
    {
        if (m_stub)
            return;
        m_channel = grpc::CreateChannel(
            "127.0.0.1:50051", grpc::InsecureChannelCredentials());
        m_stub = jvz::JvzController::NewStub(m_channel);
    }

    template <typename Call>
    bool rpc(const char* op, Call&& call, const jvz::SetResponse& response)
    {
        ensureGrpcStub();
        if (!m_stub)
        {
            setStatus(QStringLiteral("%1: stub unavailable").arg(QLatin1String(op)));
            return false;
        }

        grpc::ClientContext context;
        context.set_deadline(
            std::chrono::system_clock::now() + std::chrono::seconds(3));
        const grpc::Status status = call(&context);
        if (!status.ok())
        {
            setStatus(QStringLiteral("%1 RPC failed: %2")
                          .arg(QLatin1String(op), QString::fromStdString(status.error_message())));
            m_stub.reset();
            m_channel.reset();
            return false;
        }
        if (!response.ok())
        {
            const QString msg = response.message().empty()
                                    ? QStringLiteral("rejected")
                                    : QString::fromStdString(response.message());
            setStatus(QStringLiteral("%1: %2").arg(QLatin1String(op), msg));
            return false;
        }
        return true;
    }

    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<jvz::JvzController::Stub> m_stub;
#endif

    void reapChild()
    {
        if (m_pid <= 0)
            return;

        int status = 0;
        const pid_t r = waitpid(m_pid, &status, WNOHANG);
        if (r == 0)
            return;
        if (r < 0)
        {
            if (errno == ECHILD)
            {
                m_pid = -1;
                setStatus(QStringLiteral("jackoviz exited"));
                emit launchedChanged();
#if defined(JVZ_HAS_GRPC)
                m_stub.reset();
                m_channel.reset();
#endif
            }
            return;
        }

        m_pid = -1;
#if defined(JVZ_HAS_GRPC)
        m_stub.reset();
        m_channel.reset();
#endif
        if (WIFEXITED(status))
            setStatus(QStringLiteral("jackoviz exited (%1)").arg(WEXITSTATUS(status)));
        else if (WIFSIGNALED(status))
            setStatus(QStringLiteral("jackoviz killed (signal %1)").arg(WTERMSIG(status)));
        else
            setStatus(QStringLiteral("jackoviz exited"));
        emit launchedChanged();
    }

    pid_t m_pid = -1;
    bool m_fast = false;
    bool m_fast_mode = false;
    int m_fft_size = 4096;
    bool m_paused = false;
    QString m_status = QStringLiteral("OK");
    bool m_jack_running = false;
    QStringList m_jack_ports;
    QString m_jack_port = QStringLiteral("(none)");
    int m_view_index = 1; /* 1D spectrum */
    double m_plot_freq = 6000.0;
    double m_db_ceil = -20.0;
    double m_db_floor = -120.0;
    double m_kaiser_beta = 4.5;
    float m_line_width = 1.0f;
    jack_client_t* m_jack = nullptr;
};

int main(int argc, char* argv[])
{
    QQuickStyle::setStyle(QStringLiteral("Imagine"));

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("jackoviz-remote"));
    QCoreApplication::setOrganizationName(QStringLiteral("jackoviz"));
    QCoreApplication::setApplicationVersion(QStringLiteral(JACKOVIZ_VERSION));

    /* Host-wide singleton (separate from jackoviz's /tmp/jackoviz.lock). */
    QLockFile instance_lock(QStringLiteral("/tmp/jackoviz-remote.lock"));
    instance_lock.setStaleLockTime(5000);
    if (!instance_lock.tryLock(100))
    {
        qWarning("jackoviz-remote: another instance is already running on this host");
        return 1;
    }

    RemoteController controller;
    QObject::connect(
        &app, &QGuiApplication::aboutToQuit, &controller, &RemoteController::saveSettings);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("controller"), &controller);

    const QUrl url(QStringLiteral("qrc:/JackovizRemote/jackoviz-remote.qml"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.load(url);

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}

#include "jackoviz-remote.moc"
