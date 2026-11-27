#pragma once

#include <QIcon>
#include <QObject>
#include <QTimer>

// Application icons (window, taskbar, tray, desktop menu) in the variant that
// matches the system theme: light glyphs on dark themes, dark glyphs on light
// ones. The theme is taken from Qt (6.5+) or from the desktop settings
// (GNOME / Cinnamon / MATE gsettings, XFCE xfconf, KDE kdeglobals, GTK
// settings.ini) and re-checked while the program runs.
class Branding : public QObject
{
    Q_OBJECT
public:
    enum Style { Auto = 0, ForLight = 1, ForDark = 2 };

    explicit Branding(QObject *parent = nullptr);

    int style() const { return m_style; }
    void setStyle(int style);
    bool systemDark() const { return m_systemDark; }
    QString themeSource() const { return m_source; }
    bool themeKnown() const { return m_known; }
    bool darkVariant() const;                      // icons for a dark background in use

    QIcon appIcon() const;
    QIcon trayIcon() const;

Q_SIGNALS:
    void changed();

private:
    struct Detected { int dark = -1; QString source; };   // dark: 1 / 0 / -1 unknown
    static Detected detectDesktop();
    void detect();
    void setDetected(const Detected &d);
    void apply();
    void installMenuIcon();

    int m_style = Auto;
    bool m_systemDark = false;
    bool m_known = false;
    bool m_busy = false;
    int m_applied = -1;
    QString m_source;
    QTimer m_poll;
};
