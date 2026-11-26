// A monitor window: title, accent colour, layout mode and its players.
#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QVariantList>
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
    // Grid actually used: the configured one, enlarged automatically when
    // the players (with their spans) do not fit into it.
    Q_PROPERTY(int effectiveRows READ effectiveRows NOTIFY contentSizeChanged)
    Q_PROPERTY(int effectiveCols READ effectiveCols NOTIFY contentSizeChanged)
    // Empty cells of the grid (QRectF list) for the "add camera" placeholders.
    Q_PROPERTY(QVariantList freeCells READ freeCells NOTIFY contentSizeChanged)
    Q_PROPERTY(QString wallpaper READ wallpaper WRITE setWallpaper NOTIFY wallpaperChanged)
    Q_PROPERTY(qreal wallpaperDim READ wallpaperDim WRITE setWallpaperDim NOTIFY wallpaperChanged)
    Q_PROPERTY(qreal renderFps READ renderFps NOTIFY renderFpsChanged)
    // Scroll position of the canvas and the player expanded over the window
    // (set by the window): used to find players nobody can see.
    Q_PROPERTY(qreal viewportX READ viewportX WRITE setViewportX NOTIFY viewportChanged)
    Q_PROPERTY(qreal viewportY READ viewportY WRITE setViewportY NOTIFY viewportChanged)
    Q_PROPERTY(Player *expandedPlayer READ expandedPlayer WRITE setExpandedPlayer NOTIFY expandedPlayerChanged)
    Q_PROPERTY(bool windowHidden READ windowHidden NOTIFY windowHiddenChanged)
    Q_PROPERTY(QString windowHiddenReason READ windowHiddenReason NOTIFY windowHiddenChanged)

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
    // Top-left of grid cell `index` (row-major).
    Q_INVOKABLE QPointF cellPos(int index) const;
    int effectiveRows() const { return m_effRows; }
    int effectiveCols() const { return m_effCols; }
    QVariantList freeCells() const { return m_freeCells; }
    // Size of a player in grid cells; the others are packed around it.
    Q_INVOKABLE void setSpan(Player *p, int cols, int rows);
    // Span for a tile of w x h pixels (snapped to whole cells).
    Q_INVOKABLE QSize snapSpan(qreal w, qreal h) const;

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

    qreal viewportX() const { return m_viewX; }
    void setViewportX(qreal x);
    qreal viewportY() const { return m_viewY; }
    void setViewportY(qreal y);
    Player *expandedPlayer() const { return m_expanded; }
    void setExpandedPlayer(Player *p);
    // Bring to the front in the free layout.
    Q_INVOKABLE void raise(Player *p);
    // The whole window cannot be seen (display off, not shown by the
    // compositor...): every player stops after delayMs.
    void setWindowHidden(bool hidden, int delayMs, const QString &reason);
    bool windowHidden() const { return m_windowHidden; }
    QString windowHiddenReason() const { return m_windowHiddenReason; }

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
    void expandedPlayerChanged();
    void windowHiddenChanged();
    void changed();          // anything persistent changed -> save

private:
    void attach(Player *p);
    void relayout();
    void scheduleRelayout();
    void applySort();
    void renumber(const QList<Player *> &list);
    void scheduleVisibility();
    void updateVisibility();

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
    qreal m_viewX = 0;
    qreal m_viewY = 0;
    QPointer<Player> m_expanded;
    bool m_windowHidden = false;
    int m_windowHiddenDelay = 0;
    QString m_windowHiddenReason;
    bool m_visibilityQueued = false;
    int m_effRows = 2;
    int m_effCols = 2;
    QVariantList m_freeCells;
    void layoutGrid();
};
