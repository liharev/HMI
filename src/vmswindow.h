// A monitor window: title, accent colour, layout mode and its players.
#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QRect>
#include <QRectF>
#include <QVariantMap>

class Player;

// Stable list of players. Rows are only inserted/removed, never moved, so the
// QML delegates (and their video surfaces) survive any re-ordering; the
// visual order is the Player::order property.
class PlayerListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles { PlayerRole = Qt::UserRole + 1 };
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_items.size(); }
    const QList<Player *> &items() const { return m_items; }
    void append(Player *p);
    void remove(Player *p);

Q_SIGNALS:
    void countChanged();

private:
    QList<Player *> m_items;
};

class VmsWindow : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString uid READ uid CONSTANT)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(int layoutMode READ layoutMode WRITE setLayoutMode NOTIFY layoutModeChanged)
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    Q_PROPERTY(PlayerListModel *players READ players CONSTANT)
    Q_PROPERTY(int playerCount READ playerCount NOTIFY playersChanged)
    Q_PROPERTY(qreal viewportWidth READ viewportWidth WRITE setViewportWidth NOTIFY viewportChanged)
    Q_PROPERTY(qreal viewportHeight READ viewportHeight WRITE setViewportHeight NOTIFY viewportChanged)
    Q_PROPERTY(qreal contentWidth READ contentWidth NOTIFY contentSizeChanged)
    Q_PROPERTY(qreal contentHeight READ contentHeight NOTIFY contentSizeChanged)
    Q_PROPERTY(QString sortKey READ sortKey NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged)
    Q_PROPERTY(bool autoSort READ autoSort WRITE setAutoSort NOTIFY sortChanged)
    Q_PROPERTY(bool fullScreen READ fullScreen NOTIFY placementChanged)
    Q_PROPERTY(QString screenName READ screenName NOTIFY placementChanged)
    Q_PROPERTY(int gridRows READ gridRows NOTIFY gridChanged)
    Q_PROPERTY(int gridCols READ gridCols NOTIFY gridChanged)
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY contentSizeChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY contentSizeChanged)
    Q_PROPERTY(QString wallpaper READ wallpaper WRITE setWallpaper NOTIFY wallpaperChanged)
    Q_PROPERTY(qreal wallpaperDim READ wallpaperDim WRITE setWallpaperDim NOTIFY wallpaperChanged)
    Q_PROPERTY(qreal renderFps READ renderFps NOTIFY renderFpsChanged)

public:
    enum LayoutMode { Flow = 0, Free = 1, Grid = 2 };
    Q_ENUM(LayoutMode)

    static constexpr qreal kMargin = 6;
    static constexpr qreal kSpacing = 6;

    explicit VmsWindow(QObject *parent = nullptr);
    ~VmsWindow() override;

    static VmsWindow *fromJson(const QJsonObject &o, QObject *parent);
    QJsonObject toJson() const;

    QString uid() const { return m_uid; }
    QString title() const { return m_title; }
    void setTitle(const QString &t);
    QColor color() const { return m_color; }
    void setColor(const QColor &c);
    int layoutMode() const { return m_layoutMode; }
    void setLayoutMode(int m);
    bool isOpen() const { return m_open; }
    void setOpen(bool o);
    PlayerListModel *players() { return &m_players; }
    int playerCount() const { return m_players.count(); }

    qreal viewportWidth() const { return m_viewportW; }
    void setViewportWidth(qreal w);
    qreal viewportHeight() const { return m_viewportH; }
    void setViewportHeight(qreal h);
    qreal contentWidth() const { return m_contentW; }
    qreal contentHeight() const { return m_contentH; }

    QString sortKey() const { return m_sortKey; }
    bool sortAscending() const { return m_sortAsc; }
    bool autoSort() const { return m_autoSort; }
    void setAutoSort(bool on);

    QRect geometry() const { return m_geometry; }
    void setGeometry(const QRect &r);
    bool fullScreen() const { return m_fullScreen; }
    void setFullScreen(bool f);
    QString screenName() const { return m_screenName; }
    void setScreenName(const QString &s);

    QList<Player *> ordered() const;

    int gridRows() const { return m_gridRows; }
    int gridCols() const { return m_gridCols; }
    qreal cellWidth() const { return m_cellW; }
    qreal cellHeight() const { return m_cellH; }
    // N rows x M columns of equal cells filling the window.
    Q_INVOKABLE void setGrid(int rows, int cols);
    // Top-left of grid cell `index` (row-major), for empty-cell placeholders.
    Q_INVOKABLE QPointF cellPos(int index) const;

    QString wallpaper() const { return m_wallpaper; }
    void setWallpaper(const QString &w);
    qreal wallpaperDim() const { return m_wallpaperDim; }
    void setWallpaperDim(qreal d);

    qreal renderFps() const { return m_renderFps; }
    void setRenderFps(qreal fps);

    Q_INVOKABLE Player *addPlayer(const QVariantMap &cfg);
    Q_INVOKABLE void removePlayer(Player *p);
    // Manual sorting: move to position `index` (in display order).
    Q_INVOKABLE void movePlayer(Player *p, int index);
    // Insertion index for a drag of `dragged` at canvas point (x, y).
    Q_INVOKABLE int dropIndex(Player *dragged, qreal x, qreal y) const;
    Q_INVOKABLE QRectF dropIndicator(Player *dragged, qreal x, qreal y) const;
    // Automatic sorting: key = name | url | size | resolution | status | created
    Q_INVOKABLE void sortBy(const QString &key, bool ascending);
    Q_INVOKABLE void fitAll();            // equal tiles filling the viewport
    Q_INVOKABLE void originalSizeAll();
    Q_INVOKABLE void arrange();           // free mode: place in flow order
    Q_INVOKABLE void startAll();
    Q_INVOKABLE void stopAll();

    void setSuspended(bool s, bool synchronous = false);

Q_SIGNALS:
    void titleChanged();
    void colorChanged();
    void layoutModeChanged();
    void openChanged();
    void playersChanged();
    void viewportChanged();
    void contentSizeChanged();
    void sortChanged();
    void placementChanged();
    void gridChanged();
    void wallpaperChanged();
    void renderFpsChanged();
    void changed();          // anything persistent changed -> save

private:
    void attach(Player *p);
    void relayout();
    void scheduleRelayout();
    void applySort();
    void renumber(const QList<Player *> &list);

    QString m_uid;
    QString m_title;
    QColor m_color;
    int m_layoutMode = Flow;
    bool m_open = true;
    PlayerListModel m_players;
    qreal m_viewportW = 1280;
    qreal m_viewportH = 720;
    qreal m_contentW = 0;
    qreal m_contentH = 0;
    QString m_sortKey;
    bool m_sortAsc = true;
    bool m_autoSort = false;
    QRect m_geometry;
    bool m_fullScreen = false;
    QString m_screenName;
    bool m_relayoutQueued = false;
    int m_gridRows = 2;
    int m_gridCols = 2;
    qreal m_cellW = 0;
    qreal m_cellH = 0;
    QString m_wallpaper;
    qreal m_wallpaperDim = 0.0;
    qreal m_renderFps = -1;
    bool m_inSort = false;
};
