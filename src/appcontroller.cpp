#include "appcontroller.h"
#include "diagnostics.h"
#include "gstutils.h"
#include "player.h"
#include <gst/gst.h>
#include "camai.h"
#include "discovery.h"
#include "streamprofile.h"
#include <QUrl>
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
#include <QDateTime>
#include <QThreadPool>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

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

    // Frames per second each window really renders (for the traffic lights).
    m_fpsTimer.setInterval(1000);
    m_fpsClock.start();
    connect(&m_fpsTimer, &QTimer::timeout, this, [this] {
        const qreal dt = qMax<qint64>(200, m_fpsClock.restart()) / 1000.0;   // real elapsed time
        for (auto it = m_frameCounters.begin(); it != m_frameCounters.end(); ++it)
            it.key()->setRenderFps(it.value()->exchange(0) / dt);
        // Displays can be switched off without any event: poll them.
        for (auto it = m_views.cbegin(); it != m_views.cend(); ++it)
            updateWindowVisibility(it.key());
    });
    m_fpsTimer.start();

    m_diag = new Diagnostics(this);

    m_balanceTimer.setInterval(3000);
    connect(&m_balanceTimer, &QTimer::timeout, this, &AppController::balanceDecoders);
    m_balanceTimer.start();

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
    QString s = m_glSink ? QStringLiteral("OpenGL ES (qml6glsink, zero-copy)")
                         : QStringLiteral("Qt Multimedia (QVideoSink)");
    if (!Player::renderNote().isEmpty())
        s += QStringLiteral(". ") + Player::renderNote();
    return s;
}

QString AppController::hardwareDecoders() const
{
    return gsu::hardwareSummary();
}

qreal AppController::readCpuLoad()
{
    QFile f(QStringLiteral("/proc/stat"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QList<QByteArray> v = f.readLine().simplified().split(' ');   // cpu user nice system idle iowait ...
    if (v.size() < 6)
        return 0;
    quint64 total = 0;
    for (int i = 1; i < v.size(); ++i)
        total += v[i].toULongLong();
    const quint64 idle = v[4].toULongLong() + v[5].toULongLong();
    const quint64 dt = total - m_cpuTotal;
    const quint64 di = idle - m_cpuIdle;
    const bool first = m_cpuTotal == 0;
    m_cpuTotal = total;
    m_cpuIdle = idle;
    return (first || dt == 0) ? 0 : 1.0 - qreal(di) / qreal(dt);
}

// The hardware decoder of the mainline kernel serves all streams together and
// has a limited throughput (e.g. ~60 frames/s of 1080p measured on RK3588 with
// v4l2slh264dec): when it saturates, every stream slows down. Players with the
// decoder set to "Auto" are then moved to software decoding (CPU) until the
// hardware streams keep up again, and moved back when there is headroom.
void AppController::balanceDecoders()
{
    const qreal cpu = readCpuLoad();
    QList<Player *> all;
    for (VmsWindow *w : m_windows)
        all += w->players()->items();
    QList<Player *> hw, sw, lagging;
    qreal hwPx = 0, hwActualPx = 0;
    for (Player *p : all) {
        if (p->status() != Player::Playing)
            continue;
        if (p->hardwareDecoding()) {
            hw << p;
            hwPx += p->pixelRate();
            hwActualPx += p->fps() * p->videoWidth() * p->videoHeight();
            if (p->decoderLagging())
                lagging << p;
        } else {
            sw << p;
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool settled = now - m_lastBalanceMs > 9000;
    const bool saturated = lagging.size() >= 2 && lagging.size() * 2 >= hw.size();

    // Did the last move help? If the streams moved to the CPU lag just as
    // much, the decoder is not the bottleneck (network / server / camera):
    // move them back and stop moving for this session.
    if (settled && m_verifyMove) {
        m_verifyMove = false;
        int moved = 0, movedLagging = 0;
        for (Player *p : sw)
            if (p->balancedSoftware()) {
                ++moved;
                if (p->decoderLagging())
                    ++movedLagging;
            }
        if (moved > 0 && movedLagging * 2 >= moved) {
            m_balanceOff = true;
            m_hwCapacity = 0;          // measured while starved by the source: not the VPU limit
            for (Player *p : all)
                if (p->balancedSoftware())
                    p->setBalancedSoftware(false);
            m_lastBalanceMs = now;
            qInfo("[balance] moving streams to the CPU did not help: the source/network limits the frame rate");
        }
    } else if (settled && saturated && !m_balanceOff) {
        // What the decoder really delivers is its capacity.
        m_hwCapacity = hwActualPx;
        if (cpu < 0.75) {
            QList<Player *> movable;
            for (Player *p : hw)
                if (p->movableDecoder() && p->pixelRate() > 0)
                    movable << p;
            std::sort(movable.begin(), movable.end(),
                      [](Player *a, Player *b) { return a->pixelRate() > b->pixelRate(); });
            // Move the heaviest streams until the rest fits into ~90% of the capacity.
            qreal remaining = hwPx;
            for (Player *p : movable) {
                if (remaining <= 0.9 * m_hwCapacity)
                    break;
                remaining -= p->pixelRate();
                p->setBalancedSoftware(true);
            }
            m_lastBalanceMs = now;
            m_verifyMove = true;
            scheduleSave();
        }
    } else if (settled && lagging.isEmpty()) {
        // Unknown capacity: bring streams back one by one and let a new
        // saturation (if any) measure it.
        // Headroom: bring one stream back (the lightest one).
        Player *best = nullptr;
        for (Player *p : sw)
            if (p->balancedSoftware() && p->pixelRate() > 0 && (!best || p->pixelRate() < best->pixelRate()))
                best = p;
        if (best && (m_hwCapacity <= 0 || hwPx + best->pixelRate() < 0.8 * m_hwCapacity)) {
            best->setBalancedSoftware(false);
            m_lastBalanceMs = now;
            scheduleSave();
        }
    }

    int balanced = 0;
    for (Player *p : sw)
        if (p->balancedSoftware())
            ++balanced;
    QString text = QStringLiteral("аппаратно: %1 (%2 Мпикс/с%3), программно: %4 (из них перенесено с аппаратного: %5); загрузка CPU %6%")
                             .arg(hw.size())
                             .arg(hwPx / 1e6, 0, 'f', 0)
                             .arg(m_hwCapacity > 0 ? QStringLiteral(", предел ≈ %1").arg(m_hwCapacity / 1e6, 0, 'f', 0) : QString())
                             .arg(sw.size())
                             .arg(balanced)
                             .arg(qRound(cpu * 100));
    if (m_balanceOff)
        text += QStringLiteral(". Перенос на CPU отключён: кадров не хватает из-за источника/сети, а не декодера");
    if (text != m_balanceText) {
        m_balanceText = text;
        Q_EMIT decoderBalanceChanged();
    }
    static const bool debugStats = qEnvironmentVariableIntValue("HMI_DEBUG_STATS") > 0;
    if (debugStats)
        qInfo("[balance] %s", qPrintable(text));
}

QString AppController::decoderEnvironment() const
{
    QFile f(QStringLiteral("/proc/sys/kernel/osrelease"));
    const QString kernel = f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll().trimmed()) : QString();
    return kernel + QLatin1Char('|') + gsu::hardwareSummary();
}

QList<Player *> AppController::allPlayers() const
{
    QList<Player *> all;
    for (VmsWindow *w : m_windows)
        all += w->players()->items();
    return all;
}

void AppController::openDiagnostics()
{
    if (m_diagView) {
        if (m_diagView->visibility() == QWindow::Minimized)
            m_diagView->showNormal();
        m_diagView->show();
        m_diagView->raise();
        m_diagView->requestActivate();
        return;
    }
    QQmlComponent comp(m_engine, QUrl(QStringLiteral("qrc:/HmiUi/DiagnosticsWindow.qml")));
    QObject *obj = comp.create();
    auto *view = qobject_cast<QQuickWindow *>(obj);
    if (!view) {
        qCritical() << "Cannot create the diagnostics window:" << comp.errors();
        delete obj;
        return;
    }
    QQmlEngine::setObjectOwnership(view, QQmlEngine::CppOwnership);
    m_diagView = view;
    view->show();
    view->raise();
}

bool AppController::softwareRendering() const
{
    const QString r = m_glRenderer.toLower();
    return r.contains(QLatin1String("llvmpipe")) || r.contains(QLatin1String("softpipe"))
           || r.contains(QLatin1String("swrast")) || r.contains(QLatin1String("software"));
}

QVariantList AppController::builtinWallpapers() const
{
    static const char *const names[][2] = {
        {"camai-grid", "Сетка"},     {"camai-lens", "Объектив"}, {"camai-network", "Нейросеть"},
        {"camai-hex", "Соты"},       {"camai-waves", "Волны"},   {"camai-graphite", "Графит"},
    };
    QVariantList l;
    for (const auto &n : names)
        l << QVariantMap{{QStringLiteral("url"), QStringLiteral("qrc:/wallpapers/%1.jpg").arg(QLatin1String(n[0]))},
                         {QStringLiteral("name"), QString::fromUtf8(n[1])}};
    return l;
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
        m_hwCapacity = root.value(QLatin1String("hwCapacity")).toDouble(0);
        const QJsonObject settings = root.value(QLatin1String("settings")).toObject();
        Player::setWaitTimeout(settings.value(QLatin1String("streamWaitMs")).toInt(3000));
        Player::setAutoQuality(settings.value(QLatin1String("autoQuality")).toBool(true));
        Player::setMinFps(settings.value(QLatin1String("minTileFps")).toDouble(10));
        Player::setAudioDevice(settings.value(QLatin1String("audioDevice")).toString());
        for (const QJsonValue &v : root.value(QLatin1String("windows")).toArray())
            addWindow(VmsWindow::fromJson(v.toObject(), this));
        // Automatic decisions are only valid for the environment they were
        // made in (e.g. software decoding chosen on a weak mainline VPU must
        // not stick after switching to the vendor kernel with MPP).
        if (root.value(QLatin1String("decoderEnv")).toString() != decoderEnvironment()) {
            m_hwCapacity = 0;
            for (Player *p : allPlayers())
                p->resetLearned();
            qInfo("Kernel/decoders changed (%s): automatic decoder/output decisions reset",
                  qPrintable(decoderEnvironment()));
        }
    }
    // Developer aid: HMI_TEST_SWITCH="url1;url2;..." switches every player
    // to the next address every HMI_TEST_SWITCH_SEC seconds (default 6).
    if (qEnvironmentVariableIsSet("HMI_TEST_SWITCH")) {
        const QStringList urls = qEnvironmentVariable("HMI_TEST_SWITCH").split(QLatin1Char(';'), Qt::SkipEmptyParts);
        const int sec = qMax(1, qEnvironmentVariableIntValue("HMI_TEST_SWITCH_SEC") > 0
                                    ? qEnvironmentVariableIntValue("HMI_TEST_SWITCH_SEC") : 6);
        auto *t = new QTimer(this);
        auto step = std::make_shared<int>(0);
        connect(t, &QTimer::timeout, this, [this, urls, step] {
            ++*step;
            for (Player *p : allPlayers())
                p->switchStream(urls.at(*step % urls.size()), QStringLiteral("тест"));
        });
        t->start(sec * 1000);
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
        m_windowComponent = new QQmlComponent(m_engine, QUrl(QStringLiteral("qrc:/HmiUi/MonitorWindow.qml")),
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
    auto counter = std::make_shared<std::atomic<int>>(0);
    m_frameCounters.insert(w, counter);
    connect(view, &QQuickWindow::frameSwapped, view, [counter] { counter->fetch_add(1, std::memory_order_relaxed); },
            Qt::DirectConnection);
    if (m_glRenderer.isEmpty()) {
        // Which GPU renders the UI: llvmpipe means no hardware acceleration.
        connect(view, &QQuickWindow::sceneGraphInitialized, view, [this] {
            QString r;
            if (QOpenGLContext *ctx = QOpenGLContext::currentContext()) {
                QOpenGLFunctions *f = ctx->functions();
                const auto *ren = reinterpret_cast<const char *>(f->glGetString(GL_RENDERER));
                const auto *ver = reinterpret_cast<const char *>(f->glGetString(GL_VERSION));
                r = QString::fromLatin1(ren ? ren : "?") + QStringLiteral(" — ") + QString::fromLatin1(ver ? ver : "?");
            }
            QMetaObject::invokeMethod(this, [this, r] {
                if (m_glRenderer.isEmpty() && !r.isEmpty()) {
                    m_glRenderer = r;
                    qInfo("GL renderer: %s", qPrintable(r));
                    Q_EMIT glInfoChanged();
                }
            }, Qt::QueuedConnection);
        }, Qt::DirectConnection);
    }
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
    // Visibility for the players (expose events: the compositor stopped
    // showing the window; display state is polled every second).
    view->installEventFilter(this);
    connect(view, &QWindow::visibilityChanged, w, [this, w] { updateWindowVisibility(w); });
    connect(view, &QWindow::screenChanged, w, [this, w] { updateWindowVisibility(w); });
}

namespace {

// State of the display a screen is shown on, from the DRM connector in
// sysfs ("HDMI-A-1": status, dpms, enabled). X11 names outputs without the
// "-A" ("HDMI-1"), both forms are matched.
struct DisplayState
{
    bool found = false;
    bool on = true;
    QString detail;
};

DisplayState displayState(const QString &screenName)
{
    DisplayState st;
    if (screenName.isEmpty())
        return st;
    static const QString root = qEnvironmentVariableIsSet("HMI_TEST_DRM_ROOT")
                                    ? qEnvironmentVariable("HMI_TEST_DRM_ROOT")
                                    : QStringLiteral("/sys/class/drm");
    auto norm = [](QString n) { return n.toUpper().remove(QStringLiteral("-A-")).remove(QLatin1Char('-')); };
    const QString want = norm(screenName);
    const QDir dir(root);
    for (const QString &entry : dir.entryList({QStringLiteral("card*-*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString conn = entry.section(QLatin1Char('-'), 1);    // card1-HDMI-A-1 -> HDMI-A-1
        if (conn != screenName && norm(conn) != want)
            continue;
        auto read = [&](const char *file) {
            QFile f(dir.filePath(entry + QLatin1Char('/') + QLatin1String(file)));
            return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
        };
        st.found = true;
        const QString status = read("status");
        const QString dpms = read("dpms");
        const QString enabled = read("enabled");
        if (status == QLatin1String("disconnected")) {
            st.on = false;
            st.detail = QStringLiteral("отключён от разъёма %1").arg(conn);
        } else if (!dpms.isEmpty() && dpms != QLatin1String("On")) {
            st.on = false;
            st.detail = QStringLiteral("питание экрана выключено (%1, DPMS %2)").arg(conn, dpms);
        } else if (enabled == QLatin1String("disabled")) {
            st.on = false;
            st.detail = QStringLiteral("вывод %1 отключён").arg(conn);
        }
        return st;
    }
    return st;
}

} // namespace

bool AppController::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::Expose) {
        for (auto it = m_views.cbegin(); it != m_views.cend(); ++it) {
            if (it.value() == obj) {
                VmsWindow *w = it.key();
                QMetaObject::invokeMethod(this, [this, w] { if (m_views.contains(w)) updateWindowVisibility(w); },
                                          Qt::QueuedConnection);
                break;
            }
        }
    }
    return QObject::eventFilter(obj, ev);
}

void AppController::updateWindowVisibility(VmsWindow *w)
{
    QQuickWindow *v = m_views.value(w);
    if (!v)
        return;
    QString reason;
    int delay = 0;
    const auto vis = v->visibility();
    if (vis == QWindow::Minimized || vis == QWindow::Hidden) {
        reason = QStringLiteral("окно свёрнуто");
        delay = 2000;
    } else if (!v->isExposed()) {
        reason = QStringLiteral("окно не выводится на экран (закрыто другими окнами, на другом рабочем столе "
                                "или экран погашен)");
        delay = 5000;
    } else if (QScreen *scr = qEnvironmentVariableIsSet("HMI_NO_DISPLAY_POLL") ? nullptr : v->screen()) {
        // A display that is switched off keeps its place in the desktop
        // (the window does not move): only the connector tells.
        const DisplayState d = displayState(scr->name());
        if (d.found && !d.on) {
            reason = QStringLiteral("дисплей %1 выключен: %2").arg(scr->name(), d.detail);
            delay = 5000;
        }
    }
    w->setWindowHidden(!reason.isEmpty(), delay, reason);
}

void AppController::destroyView(VmsWindow *w)
{
    QPointer<QQuickWindow> view = m_views.take(w);
    if (!view)
        return;
    QObject::disconnect(view, nullptr, w, nullptr);
    m_frameCounters.remove(w);
    w->setRenderFps(-1);
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

int AppController::streamWaitMs() const
{
    return Player::waitTimeout();
}

void AppController::setStreamWaitMs(int ms)
{
    if (ms == Player::waitTimeout())
        return;
    Player::setWaitTimeout(ms);
    Q_EMIT settingsChanged();
    scheduleSave();
}

bool AppController::autoQuality() const
{
    return Player::autoQuality();
}

void AppController::setAutoQuality(bool on)
{
    if (on == Player::autoQuality())
        return;
    Player::setAutoQuality(on);
    settingsUpdated();
}

double AppController::minTileFps() const
{
    return Player::minFps();
}

void AppController::setMinTileFps(double fps)
{
    if (qFuzzyCompare(fps + 1, Player::minFps() + 1))
        return;
    Player::setMinFps(fps);
    settingsUpdated();
}

QString AppController::audioDevice() const
{
    return Player::audioDevice();
}

void AppController::setAudioDevice(const QString &name)
{
    if (name == Player::audioDevice())
        return;
    Player::setAudioDevice(name);
    Q_EMIT settingsChanged();
    scheduleSave();
    Player::reapplyAudio();
}

void AppController::refreshAudioDevices()
{
    QStringList names;
    GstDeviceMonitor *mon = gst_device_monitor_new();
    gst_device_monitor_add_filter(mon, "Audio/Sink", nullptr);
    if (gst_device_monitor_start(mon)) {
        GList *devs = gst_device_monitor_get_devices(mon);
        for (GList *l = devs; l; l = l->next) {
            gchar *n = gst_device_get_display_name(GST_DEVICE(l->data));
            const QString name = QString::fromUtf8(n);
            g_free(n);
            if (!name.isEmpty() && !names.contains(name))
                names << name;
        }
        g_list_free_full(devs, gst_object_unref);
        gst_device_monitor_stop(mon);
    }
    gst_object_unref(mon);
    names.sort();
    if (names != m_audioDevices) {
        m_audioDevices = names;
        Q_EMIT audioDevicesChanged();
    }
}

void AppController::settingsUpdated()
{
    Q_EMIT settingsChanged();
    scheduleSave();
    for (Player *p : allPlayers())
        p->reevaluateProfile();
}

int AppController::camaiChannels(const QString &service, const QString &user, const QString &password)
{
    const int id = ++m_probeId;
    CamaiService::fetchChannels(service, user, password, this, [this, id](const QVariantList &list, const QString &err) {
        Q_EMIT camaiChannelsReady(id, list, err);
    });
    return id;
}

int AppController::discoverStreams(const QString &kind, const QString &address,
                                   const QString &user, const QString &password)
{
    const int id = ++m_probeId;
    auto done = [this, id](const QList<StreamProfile> &list, const QString &err) {
        QVariantList out;
        for (const StreamProfile &p : list)
            out << p.toVariant();
        Q_EMIT discoverFinished(id, out, err);
    };
    if (kind == QLatin1String("onvif"))
        discovery::onvif(address, user, password, this, done);
    else
        discovery::flussonic(address, user, password, this, done);
    return id;
}

QString AppController::camaiNormalize(const QString &service) const
{
    return CamaiService::normalize(service);
}

int AppController::probeStream(const QString &url, const QString &user, const QString &password)
{
    const int id = ++m_probeId;
    const QString resolved = Player::resolveUrl(url, nullptr).toString(QUrl::FullyEncoded);
    profiles::probe(resolved, user, password, this, [this, id, url](const StreamProfile &p, const QString &err) {
        StreamProfile r = p;
        r.url = url;                   // as typed (aliases such as m4f:// stay)
        Q_EMIT probeFinished(id, r.toVariant(), err);
    });
    return id;
}

void AppController::save()
{
    m_saveTimer.stop();
    QJsonArray arr;
    for (VmsWindow *w : m_windows)
        arr.append(w->toJson());
    QJsonObject root{{QStringLiteral("version"), 1}, {QStringLiteral("windows"), arr},
                     {QStringLiteral("hwCapacity"), m_hwCapacity},
                     {QStringLiteral("settings"), QJsonObject{{QStringLiteral("streamWaitMs"), Player::waitTimeout()},
                                                              {QStringLiteral("autoQuality"), Player::autoQuality()},
                                                              {QStringLiteral("minTileFps"), Player::minFps()},
                                                              {QStringLiteral("audioDevice"), Player::audioDevice()}}},
                     {QStringLiteral("decoderEnv"), decoderEnvironment()}};
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
    delete m_diagView.data();
    for (auto it = m_views.begin(); it != m_views.end(); ++it) {
        if (QQuickWindow *v = it.value()) {
            QObject::disconnect(v, nullptr, it.key(), nullptr);
            delete v;
        }
    }
    m_views.clear();
}
