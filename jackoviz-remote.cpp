/*
 * jackoviz-remote.cpp — Qt Quick remote controller for jackoviz.
 *
 * Launch: fork + execve of sibling `jackoviz` (-n / --fast / --rpc-only).
 * JACK: list audio output ports for the capture dropdown.
 * gRPC: all runtime settings + Quit via JvzController on 127.0.0.1:50051.
 */

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
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
    /* False in scope (unused) and 3D (mesh fixed at 6 kHz). */
    Q_PROPERTY(bool plotFreqEnabled READ plotFreqEnabled NOTIFY viewModeIndexChanged)
    /* dB floor/ceil unused on oscilloscope (time × amplitude). */
    Q_PROPERTY(bool dbRangeEnabled READ dbRangeEnabled NOTIFY viewModeIndexChanged)
    /* Line width only affects scope + 1D paths (not 2D/3D spectrograms). */
    Q_PROPERTY(bool lineWidthEnabled READ lineWidthEnabled NOTIFY viewModeIndexChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QStringList jackPorts READ jackPorts NOTIFY jackPortsChanged)

public:
    explicit RemoteController(QObject* parent = nullptr)
        : QObject(parent)
    {
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
    /* qmlIndex: 0=scope, 3=3D — fmax unused / mesh fixed. */
    bool plotFreqEnabled() const { return m_view_index != 0 && m_view_index != 3; }
    bool dbRangeEnabled() const { return m_view_index != 0; }
    /* qmlIndex: 0=scope, 1=1D — only those use stroke width. */
    bool lineWidthEnabled() const { return m_view_index == 0 || m_view_index == 1; }
    QString statusText() const { return m_status; }
    QStringList jackPorts() const { return m_jack_ports; }

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
            QCoreApplication::applicationDirPath() + QStringLiteral("/jackoviz");
        if (!QFileInfo::exists(binPath))
        {
            setStatus(QStringLiteral("jackoviz not found: %1").arg(binPath));
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
        QString cmd = QStringLiteral("jackoviz -n %1").arg(fftSize);
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

        if (ports == m_jack_ports)
            return;
        m_jack_ports = ports;
        emit jackPortsChanged();
    }

    Q_INVOKABLE void connectJackPort(const QString& portName)
    {
        m_jack_port = portName;
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
        m_plot_freq = freqHz;
        if (!plotFreqEnabled())
        {
            setStatus(
                m_view_index == 3
                    ? QStringLiteral("max freq: ignored in 3D view (mesh fixed at 6000 Hz)")
                    : QStringLiteral("max freq: ignored in oscilloscope view"));
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
        m_db_ceil = ceilDb;
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
        m_db_floor = floorDb;
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
        m_kaiser_beta = beta;
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
        m_line_width = widthPx;
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
    void jackPortsChanged();

private:
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

    void openJackClient()
    {
        if (m_jack != nullptr)
            return;

        jack_status_t status = static_cast<jack_status_t>(0);
        m_jack = jack_client_open("jackoviz-remote", JackNoStartServer, &status);
        if (m_jack == nullptr)
        {
            setStatus(QStringLiteral("JACK open failed (status 0x%1) — is jackd running?")
                          .arg(static_cast<unsigned>(status), 0, 16));
        }
    }

    void closeJackClient()
    {
        if (m_jack == nullptr)
            return;
        jack_client_close(m_jack);
        m_jack = nullptr;
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
    bool m_paused = false;
    QString m_status = QStringLiteral("OK");
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

    RemoteController controller;

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
