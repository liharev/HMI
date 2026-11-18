#include "appcontroller.h"
#include "gstutils.h"
#include "player.h"
#include "vmswindow.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScreen>
#include <QThreadPool>

namespace {
const char *const kPalette[] = {"#2563eb", "#16a34a", "#dc2626", "#d97706", "#7c3aed",
                                "#0891b2", "#db2777", "#4b5563"};
}

AppController::AppController(QQmlEngine *engine, bool glSink, const QString &configPath, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_glSink(glSink)
    , m_configPath(configPath)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1500);
    connect(&m_saveTimer, &QTimer::timeout, this, &AppController::save);

    auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    connect(app, &QGuiApplication::screenAdded, this, &AppController::screensChanged);
    connect(app, &QGuiApplication::screenRemoved, this, &AppController::screensChanged);
    connect(app, &QCoreApplication::aboutToQuit, this, &AppController::shutdown);
}

AppController::~AppController()
{
    shutdown();
}

QList<QObject *> AppController::windowsList() const
{
    QList<QObject *> l;
    for (VmsWindow *w : m_windows)
        l << w;
    return l;
}

QVariantList AppController::screens() const
{
    QVariantList l;
    for (QScreen *s : QGuiApplication::screens()) {
        const QRect g = s->geometry();
        QString label = s->name();
        if (!s->model().isEmpty())
            label += QStringLiteral(" — ") + s->model();
        label += QStringLiteral(" (%1×%2)").arg(g.width()).arg(g.height());
        l << QVariantMap{{QStringLiteral("name"), s->name()}, {QStringLiteral("label"), label}};
    }
    return l;
}

QString AppController::renderBackend() const
{
    return m_glSink ? QStringLiteral("OpenGL ES (qml6glsink, zero-copy)")
                    : QStringLiteral("Qt Multimedia (QVideoSink)");
}

QString AppController::hardwareDecoders() const
{
    return gsu::hardwareSummary();
}

QString AppController::version() const
{
    return QStringLiteral(HMI_VERSION);
}

QColor AppController::nextColor() const
{
    return QColor(QLatin1String(kPalette[m_windows.size() % (sizeof(kPalette) / sizeof(*kPalette))]));
}

void AppController::load()
{
    QFile f(m_configPath);
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        for (const QJsonValue &v : root.value(QLatin1String("windows")).toArray())
            addWindow(VmsWindow::fromJson(v.toObject(), this));
    }
    if (m_windows.isEmpty()) {
        newWindow();
        return;
    }
    Q_EMIT windowsChanged();
    bool any = false;
    for (VmsWindow *w : m_windows) {
        if (w->isOpen()) {
            createView(w);
            any = true;
        }
    }
    if (!any)
        openWindow(m_windows.first());
}

bool AppController::listen(const QString &serverName)
{
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(serverName)) {
        QLocalServer::removeServer(serverName);
        if (!m_server->listen(serverName))
            return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_server->nextPendingConnection()) {
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                const QByteArray cmd = s->readAll().trimmed();
                if (cmd == "new-window")
                    newWindow();
                s->disconnectFromServer();
            });
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        }
    });
    return true;
}

void AppController::addWindow(VmsWindow *w)
{
    w->setParent(this);
    connect(w, &VmsWindow::changed, this, &AppController::scheduleSave);
    connect(w, &VmsWindow::titleChanged, this, &AppController::windowsChanged);
    connect(w, &VmsWindow::openChanged, this, &AppController::windowsChanged);
    m_windows << w;
}

VmsWindow *AppController::newWindow()
{
    auto *w = new VmsWindow(this);
    int n = m_windows.size() + 1;
    QStringList titles;
    for (VmsWindow *o : m_windows)
        titles << o->title();
    while (titles.contains(QStringLiteral("Мониторинг %1").arg(n)))
        ++n;
    w->setTitle(QStringLiteral("Мониторинг %1").arg(n));
    w->setColor(nextColor());
    addWindow(w);
    Q_EMIT windowsChanged();
    createView(w);
    scheduleSave();
    return w;
}

void AppController::openWindow(VmsWindow *w)
{
    if (!w)
        return;
    if (QQuickWindow *v = m_views.value(w)) {
        if (v->visibility() == QWindow::Minimized)
            v->showNormal();
        v->raise();
        v->requestActivate();
        return;
    }
    w->setOpen(true);
    createView(w);
}

void AppController::createView(VmsWindow *w)
{
    if (!m_windowComponent) {
        m_windowComponent = new QQmlComponent(m_engine, QUrl(QStringLiteral("qrc:/HmiUi/qml/MonitorWindow.qml")),
                                              QQmlComponent::PreferSynchronous, this);
        if (m_windowComponent->isError())
            qCritical() << m_windowComponent->errors();
    }
    QObject *obj = m_windowComponent->createWithInitialProperties({{QStringLiteral("win"), QVariant::fromValue(w)}});
    auto *view = qobject_cast<QQuickWindow *>(obj);
    if (!view) {
        qCritical() << "Cannot create window:" << m_windowComponent->errors();
        delete obj;
        return;
    }
    QQmlEngine::setObjectOwnership(view, QQmlEngine::CppOwnership);
    m_views.insert(w, view);
    w->setOpen(true);
    w->setSuspended(false);

    // Placement: the same screen and geometry as last time when possible.
    QScreen *screen = nullptr;
    for (QScreen *s : QGuiApplication::screens())
        if (s->name() == w->screenName())
            screen = s;
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    view->setScreen(screen);
    QRect g = w->geometry();
    const QRect avail = screen->availableGeometry();
    if (!g.isValid() || !avail.intersects(g)) {
        const QSize size(qMin(1280, avail.width() * 4 / 5), qMin(800, avail.height() * 4 / 5));
        const int shift = 32 * (openWindowCount() % 6);
        g = QRect(avail.topLeft() + QPoint(avail.width() - size.width(), avail.height() - size.height()) / 2
                      + QPoint(shift, shift),
                  size);
    }
    view->setGeometry(g);
    if (w->fullScreen())
        view->showFullScreen();
    else
        view->show();
    trackView(w, view);
}

void AppController::trackView(VmsWindow *w, QQuickWindow *view)
{
    auto update = [w, view] {
        const auto vis = view->visibility();
        if (vis == QWindow::Windowed)
            w->setGeometry(view->geometry());
        if (vis != QWindow::Minimized && vis != QWindow::Hidden)
            w->setFullScreen(vis == QWindow::FullScreen);
        if (view->screen())
            w->setScreenName(view->screen()->name());
        // Minimised windows release their decoders.
        if (vis != QWindow::Hidden)
            w->setSuspended(vis == QWindow::Minimized);
    };
    connect(view, &QWindow::xChanged, w, update);
    connect(view, &QWindow::yChanged, w, update);
    connect(view, &QWindow::widthChanged, w, update);
    connect(view, &QWindow::heightChanged, w, update);
    connect(view, &QWindow::visibilityChanged, w, update);
    connect(view, &QWindow::screenChanged, w, update);
}

void AppController::destroyView(VmsWindow *w)
{
    QPointer<QQuickWindow> view = m_views.take(w);
    if (!view)
        return;
    QObject::disconnect(view, nullptr, w, nullptr);
    // Release GL video sinks while the window's GL context still exists.
    // Players stay "active"; they resume when the window is opened again.
    w->setSuspended(true, true);
    view->hide();
    view->deleteLater();
}

int AppController::openWindowCount() const
{
    int n = 0;
    for (auto it = m_views.cbegin(); it != m_views.cend(); ++it)
        if (it.value())
            ++n;
    return n;
}

void AppController::closeWindow(VmsWindow *w)
{
    if (openWindowCount() <= 1) {
        quit();     // last window: leave it marked as open for the next start
        return;
    }
    w->setOpen(false);
    destroyView(w);
    save();
}

void AppController::deleteWindow(VmsWindow *w)
{
    if (!w || !m_windows.contains(w))
        return;
    const bool last = openWindowCount() <= 1 && m_views.value(w);
    destroyView(w);
    m_windows.removeAll(w);
    w->deleteLater();
    Q_EMIT windowsChanged();
    if (m_windows.isEmpty())
        newWindow();
    else if (last)
        openWindow(m_windows.first());
    save();
}

void AppController::moveToScreen(VmsWindow *w, const QString &screenName)
{
    QQuickWindow *view = m_views.value(w);
    if (!view)
        return;
    QScreen *target = nullptr;
    for (QScreen *s : QGuiApplication::screens())
        if (s->name() == screenName)
            target = s;
    if (!target)
        return;
    const bool fs = view->visibility() == QWindow::FullScreen;
    if (fs)
        view->showNormal();
    const QRect avail = target->availableGeometry();
    QSize size = view->size().boundedTo(avail.size());
    view->setScreen(target);
    view->setGeometry(QRect(avail.topLeft() + QPoint(avail.width() - size.width(), avail.height() - size.height()) / 2,
                            size));
    if (fs)
        view->showFullScreen();
    w->setScreenName(screenName);
}

void AppController::setFullScreen(VmsWindow *w, bool on)
{
    QQuickWindow *view = m_views.value(w);
    if (!view)
        return;
    if (on)
        view->showFullScreen();
    else
        view->showNormal();
}

void AppController::toggleFullScreen(VmsWindow *w)
{
    QQuickWindow *view = m_views.value(w);
    if (view)
        setFullScreen(w, view->visibility() != QWindow::FullScreen);
}

void AppController::scheduleSave()
{
    if (!m_quitting)
        m_saveTimer.start();
}

void AppController::save()
{
    m_saveTimer.stop();
    QJsonArray arr;
    for (VmsWindow *w : m_windows)
        arr.append(w->toJson());
    QJsonObject root{{QStringLiteral("version"), 1}, {QStringLiteral("windows"), arr}};
    QDir().mkpath(QFileInfo(m_configPath).absolutePath());
    QSaveFile f(m_configPath);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot save" << m_configPath << f.errorString();
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    // Camera passwords are stored here: only the owner may read the file.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!f.commit())
        qWarning() << "Cannot save" << m_configPath << f.errorString();
}

void AppController::quit()
{
    save();
    // Deferred: quit() is usually called from a QML handler of a window that
    // shutdown() is going to delete.
    QTimer::singleShot(0, this, [this] {
        shutdown();
        QCoreApplication::quit();
    });
}

void AppController::shutdown()
{
    if (m_quitting)
        return;
    m_quitting = true;
    save();
    // Stop all pipelines in parallel, then wait for them before the GL
    // contexts and the QML engine go away.
    for (VmsWindow *w : m_windows)
        w->setSuspended(true);
    QThreadPool::globalInstance()->waitForDone(5000);
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (QQuickWindow *v = it.value()) {
            QObject::disconnect(v, nullptr, it.key(), nullptr);
            delete v;
        }
    }
    m_views.clear();
}
