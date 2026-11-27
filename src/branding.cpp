#include "branding.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QThreadPool>

namespace {

const int kAppSizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};
const int kTraySizes[] = {16, 22, 24, 32};

// Output of a short command, '' quotes removed (gsettings prints 'Adwaita').
QString run(const QString &program, const QStringList &args)
{
    const QString exe = QStandardPaths::findExecutable(program);
    if (exe.isEmpty())
        return {};
    QProcess p;
    p.start(exe, args);
    if (!p.waitForFinished(2000)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        return {};
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed().remove(QLatin1Char('\''));
}

bool darkName(const QString &theme)
{
    return theme.contains(QLatin1String("dark"), Qt::CaseInsensitive);
}

QString variant(bool dark)
{
    return dark ? QStringLiteral("dark") : QStringLiteral("light");
}

// Writes the file only when its content differs (no needless icon cache churn).
void writeIfChanged(const QString &resource, const QString &path)
{
    QFile src(resource);
    if (!src.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = src.readAll();
    QFile old(path);
    if (old.open(QIODevice::ReadOnly) && old.readAll() == data)
        return;
    old.close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
        f.commit();
    }
}

} // namespace

Branding::Branding(QObject *parent)
    : QObject(parent)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { detect(); });
#endif
    // First answer synchronously: the windows get the right icon from the start.
    Detected d;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();
    if (cs != Qt::ColorScheme::Unknown)
        d = {cs == Qt::ColorScheme::Dark ? 1 : 0, QStringLiteral("Qt")};
#endif
    if (d.dark < 0)
        d = detectDesktop();
    setDetected(d);

    // Desktops without change notifications for Qt: look again from time to time.
    m_poll.setInterval(5000);
    connect(&m_poll, &QTimer::timeout, this, &Branding::detect);
    m_poll.start();
}

void Branding::setStyle(int style)
{
    style = qBound(0, style, 2);
    if (style == m_style)
        return;
    m_style = style;
    apply();
}

bool Branding::darkVariant() const
{
    if (m_style == ForDark)
        return true;
    if (m_style == ForLight)
        return false;
    return m_systemDark;
}

QIcon Branding::appIcon() const
{
    QIcon icon;
    const QString v = variant(darkVariant());
    for (int s : kAppSizes)
        icon.addFile(QStringLiteral(":/branding/app-%1-%2.png").arg(v).arg(s), QSize(s, s));
    return icon;
}

QIcon Branding::trayIcon() const
{
    QIcon icon;
    const QString v = variant(darkVariant());
    for (int s : kTraySizes)
        icon.addFile(QStringLiteral(":/branding/tray-%1-%2.png").arg(v).arg(s), QSize(s, s));
    return icon;
}

Branding::Detected Branding::detectDesktop()
{
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper();
    const QString gtkTheme = qEnvironmentVariable("GTK_THEME");
    if (!gtkTheme.isEmpty())
        return {darkName(gtkTheme) ? 1 : 0, QStringLiteral("GTK_THEME=") + gtkTheme};

    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (desktop.contains(QLatin1String("KDE"))) {
        QSettings kde(config + QStringLiteral("/kdeglobals"), QSettings::IniFormat);
        // QSettings maps the INI section [General] to keys without a group.
        const QString scheme = kde.value(QStringLiteral("ColorScheme")).toString();
        if (!scheme.isEmpty())
            return {darkName(scheme) ? 1 : 0, QStringLiteral("KDE: ") + scheme};
        const QStringList rgb = kde.value(QStringLiteral("Colors:Window/BackgroundNormal")).toStringList();
        if (rgb.size() >= 3) {
            const int l = (rgb[0].toInt() * 3 + rgb[1].toInt() * 6 + rgb[2].toInt()) / 10;
            return {l < 128 ? 1 : 0, QStringLiteral("KDE: цвет окон")};
        }
    }
    if (desktop.contains(QLatin1String("XFCE"))) {
        const QString t = run(QStringLiteral("xfconf-query"),
                              {QStringLiteral("-c"), QStringLiteral("xsettings"), QStringLiteral("-p"),
                               QStringLiteral("/Net/ThemeName")});
        if (!t.isEmpty())
            return {darkName(t) ? 1 : 0, QStringLiteral("XFCE: ") + t};
    }
    QString schema = QStringLiteral("org.gnome.desktop.interface");
    if (desktop.contains(QLatin1String("CINNAMON")))
        schema = QStringLiteral("org.cinnamon.desktop.interface");
    else if (desktop.contains(QLatin1String("MATE")))
        schema = QStringLiteral("org.mate.interface");
    if (QStandardPaths::findExecutable(QStringLiteral("gsettings")).size()) {
        if (schema.startsWith(QLatin1String("org.gnome"))) {
            const QString cs = run(QStringLiteral("gsettings"), {QStringLiteral("get"), schema, QStringLiteral("color-scheme")});
            if (cs == QLatin1String("prefer-dark"))
                return {1, QStringLiteral("GNOME: prefer-dark")};
            if (cs == QLatin1String("prefer-light"))
                return {0, QStringLiteral("GNOME: prefer-light")};
        }
        const QString t = run(QStringLiteral("gsettings"), {QStringLiteral("get"), schema, QStringLiteral("gtk-theme")});
        if (!t.isEmpty())
            return {darkName(t) ? 1 : 0, QStringLiteral("GTK: ") + t};
    }
    for (const char *dir : {"/gtk-4.0/settings.ini", "/gtk-3.0/settings.ini"}) {
        const QString path = config + QLatin1String(dir);
        if (!QFile::exists(path))
            continue;
        QSettings gtk(path, QSettings::IniFormat);
        const QString prefer = gtk.value(QStringLiteral("Settings/gtk-application-prefer-dark-theme")).toString();
        if (prefer == QLatin1String("1") || prefer == QLatin1String("true"))
            return {1, QStringLiteral("GTK: prefer-dark")};
        const QString t = gtk.value(QStringLiteral("Settings/gtk-theme-name")).toString();
        if (!t.isEmpty())
            return {darkName(t) ? 1 : 0, QStringLiteral("GTK: ") + t};
    }
    return {};
}

void Branding::detect()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();
    if (cs != Qt::ColorScheme::Unknown) {
        setDetected({cs == Qt::ColorScheme::Dark ? 1 : 0, QStringLiteral("Qt")});
        return;
    }
#endif
    // gsettings / xfconf-query are separate processes: not in the GUI thread.
    if (m_busy)
        return;
    m_busy = true;
    QPointer<Branding> self(this);
    QThreadPool::globalInstance()->start([self] {
        const Detected d = detectDesktop();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, d] {
            if (!self)
                return;
            self->m_busy = false;
            self->setDetected(d);
        }, Qt::QueuedConnection);
    });
}

void Branding::setDetected(const Detected &d)
{
    m_known = d.dark >= 0;
    m_systemDark = d.dark == 1;      // unknown: light theme (the most common default)
    m_source = d.source;
    apply();
}

void Branding::apply()
{
    const int dark = darkVariant() ? 1 : 0;
    if (dark == m_applied)
        return;
    m_applied = dark;
    QGuiApplication::setWindowIcon(appIcon());
    installMenuIcon();
    Q_EMIT changed();
}

// The desktop menu takes the icon by name ("hmi-vms") from the icon theme;
// the user's own hicolor directory comes first, so the variant for the
// current theme is put there. Only when the program is installed (the menu
// entry exists): a copy run from the build directory leaves no traces.
void Branding::installMenuIcon()
{
    if (QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QStringLiteral("hmi-vms.desktop")).isEmpty())
        return;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                         + QStringLiteral("/icons/hicolor/");
    const QString v = variant(darkVariant());
    for (int s : kAppSizes)
        writeIfChanged(QStringLiteral(":/branding/app-%1-%2.png").arg(v).arg(s),
                       base + QStringLiteral("%1x%1/apps/hmi-vms.png").arg(s));
    writeIfChanged(QStringLiteral(":/branding/app-%1.svg").arg(v), base + QStringLiteral("scalable/apps/hmi-vms.svg"));
}
