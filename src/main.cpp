// HMI VMS - video surveillance operator/administrator workstation.
// Qt 6 Quick UI + GStreamer pipelines with hardware decoding (RK3588 MPP/V4L2).
#include "appcontroller.h"
#include "diagnostics.h"
#include "gstutils.h"
#include "player.h"
#include "vmswindow.h"

#include <QCommandLineParser>
#include <QFont>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtQml>

#include <gst/gst.h>

namespace {

QString serverName()
{
    const QByteArray user = qgetenv("USER");
    return QStringLiteral("hmi-vms-") + QString::fromLocal8Bit(user.isEmpty() ? QByteArray("user") : user);
}

// A second launch asks the running instance to open one more window.
bool forwardToRunningInstance()
{
    QLocalSocket s;
    s.connectToServer(serverName());
    if (!s.waitForConnected(300))
        return false;
    s.write("new-window\n");
    s.flush();
    s.waitForBytesWritten(500);
    s.waitForDisconnected(500);
    return true;
}

void applyDarkPalette(QGuiApplication &app)
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(0x1d, 0x20, 0x24));
    p.setColor(QPalette::WindowText, QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::Base, QColor(0x15, 0x17, 0x1a));
    p.setColor(QPalette::AlternateBase, QColor(0x24, 0x27, 0x2c));
    p.setColor(QPalette::Text, QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::Button, QColor(0x2a, 0x2e, 0x34));
    p.setColor(QPalette::ButtonText, QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::Highlight, QColor(0x25, 0x63, 0xeb));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(0x2a, 0x2e, 0x34));
    p.setColor(QPalette::ToolTipText, QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x86, 0x8e));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x70, 0x70, 0x70));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x70, 0x70, 0x70));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x70, 0x70, 0x70));
    app.setPalette(p);
}

} // namespace

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    // Several top-level windows share GL resources; the GStreamer GL sink
    // needs an OpenGL (ES) scene graph.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("hmi-vms"));
    QGuiApplication::setOrganizationName(QStringLiteral("hmi"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("CAMAI VMS"));
    QGuiApplication::setApplicationVersion(QStringLiteral(HMI_VERSION));
    // Only announce the desktop file when it is installed: otherwise the
    // xdg-desktop-portal registration fails with a (harmless) warning.
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QStringLiteral("hmi-vms.desktop")).isEmpty())
        QGuiApplication::setDesktopFileName(QStringLiteral("hmi-vms"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Рабочее место администратора видеонаблюдения"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption renderOpt(QStringList{QStringLiteral("r"), QStringLiteral("render")},
                                 QStringLiteral("Вывод видео: auto | gl | qt (по умолчанию auto)."),
                                 QStringLiteral("mode"), QStringLiteral("auto"));
    QCommandLineOption configOpt(QStringList{QStringLiteral("c"), QStringLiteral("config")},
                                 QStringLiteral("Файл конфигурации раскладки."), QStringLiteral("file"));
    QCommandLineOption fontOpt(QStringLiteral("font-scale"),
                               QStringLiteral("Масштаб шрифта интерфейса, например 1.25 (также HMI_FONT_SCALE)."),
                               QStringLiteral("factor"));
    QCommandLineOption separateOpt(QStringLiteral("separate"),
                                   QStringLiteral("Не передавать запуск уже работающему экземпляру."));
    parser.addOption(renderOpt);
    parser.addOption(configOpt);
    parser.addOption(separateOpt);
    parser.addOption(fontOpt);
    parser.process(app);

    const bool separate = parser.isSet(separateOpt) || parser.isSet(configOpt);
    if (!separate && forwardToRunningInstance())
        return 0;

    QString configPath = parser.value(configOpt);
    if (configPath.isEmpty())
        configPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                     + QStringLiteral("/layout.json");

    QString render = parser.value(renderOpt).toLower();
    if (render == QLatin1String("auto") && qEnvironmentVariableIsSet("HMI_RENDER"))
        render = qEnvironmentVariable("HMI_RENDER").toLower();
    bool glSink = false;
    if (render != QLatin1String("qt")) {
        // Creating the element loads the plugin, which registers the
        // GstGLQt6VideoItem QML type; must happen before QML is loaded.
        if (GstElement *probe = gst_element_factory_make("qml6glsink", nullptr)) {
            glSink = true;
            gst_object_unref(probe);
        } else if (render == QLatin1String("gl")) {
            qWarning("qml6glsink is not available (install gstreamer1.0-qt6); using Qt Multimedia output");
        }
    }
    Player::setGlRendering(glSink);
    qInfo("CAMAI VMS %s: render=%s, HW decoders: %s", HMI_VERSION, glSink ? "qml6glsink" : "QVideoSink",
          qPrintable(gsu::hardwareSummary()));

    // Teardown of RTSP pipelines runs in this pool (may block on network).
    QThreadPool::globalInstance()->setMaxThreadCount(qMax(4, QThread::idealThreadCount()));

    QQuickStyle::setStyle(QStringLiteral("Fusion"));
    applyDarkPalette(app);

    // Optional extra font scaling on top of the system font size.
    QString fontScale = parser.value(fontOpt);
    if (fontScale.isEmpty())
        fontScale = qEnvironmentVariable("HMI_FONT_SCALE");
    bool okScale = false;
    const double scale = fontScale.toDouble(&okScale);
    if (okScale && scale >= 0.5 && scale <= 4.0) {
        QFont f = QGuiApplication::font();
        if (f.pointSizeF() > 0)
            f.setPointSizeF(f.pointSizeF() * scale);
        else if (f.pixelSize() > 0)
            f.setPixelSize(qRound(f.pixelSize() * scale));
        QGuiApplication::setFont(f);
    }

    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    qmlRegisterUncreatableType<Player>("HmiCore", 1, 0, "Player", QStringLiteral("created by VmsWindow"));
    qmlRegisterUncreatableType<VmsWindow>("HmiCore", 1, 0, "VmsWindow", QStringLiteral("created by App"));
    qmlRegisterUncreatableType<PlayerListModel>("HmiCore", 1, 0, "PlayerListModel", QStringLiteral("internal"));
    qmlRegisterUncreatableType<Diagnostics>("HmiCore", 1, 0, "Diagnostics", QStringLiteral("internal"));

    AppController controller(&engine, glSink, configPath);
    qmlRegisterSingletonInstance("HmiCore", 1, 0, "App", &controller);
    if (!separate)
        controller.listen(serverName());
    controller.load();

    return app.exec();
}
