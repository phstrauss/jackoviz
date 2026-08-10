/*
 * jackoviz-remote.cpp — Qt Quick mockup for the jackoviz gRPC remote controller.
 *
 * UI only for now (no gRPC / fork-exec). Build target: jackoviz-remote.
 */

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QUrl>

int main(int argc, char* argv[])
{
    QQuickStyle::setStyle(QStringLiteral("Imagine"));

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("jackoviz-remote"));
    QCoreApplication::setOrganizationName(QStringLiteral("jackoviz"));

    QQmlApplicationEngine engine;
    const QUrl url(QStringLiteral("qrc:/JackovizRemote/jackoviz-remote.qml"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.load(url);

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
