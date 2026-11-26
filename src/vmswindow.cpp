#include "vmswindow.h"
#include <QRegion>
#include "player.h"

#include <QCollator>
#include <QJsonArray>
#include <QUuid>
#include <QtMath>

#include <algorithm>

// ---------------------------------------------------------------------------
// PlayerListModel
// ---------------------------------------------------------------------------
int PlayerListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant PlayerListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size() || role != PlayerRole)
        return {};
    return QVariant::fromValue(static_cast<QObject *>(m_items.at(index.row())));
}

QHash<int, QByteArray> PlayerListModel::roleNames() const
{
    return {{PlayerRole, "player"}};
}

void PlayerListModel::append(Player *p)
{
    beginInsertRows(QModelIndex(), m_items.size(), m_items.size());
    m_items.append(p);
    endInsertRows();
    Q_EMIT countChanged();
}

void PlayerListModel::remove(Player *p)
{
    const int row = m_items.indexOf(p);
    if (row < 0)
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_items.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
}

// ---------------------------------------------------------------------------
// VmsWindow
// ---------------------------------------------------------------------------
VmsWindow::VmsWindow(QObject *parent)
    : QObject(parent)
    , m_uid(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_title(QStringLiteral("Мониторинг"))
    , m_color(QStringLiteral("#2563eb"))
{
}

VmsWindow::~VmsWindow() = default;

VmsWindow *VmsWindow::fromJson(const QJsonObject &o, QObject *parent)
{
    auto *w = new VmsWindow(parent);
    w->m_uid = o.value(QLatin1String("uid")).toString(w->m_uid);
    w->m_title = o.value(QLatin1String("title")).toString(w->m_title);
    const QColor c(o.value(QLatin1String("color")).toString());
    if (c.isValid())
        w->m_color = c;
    const int mode = o.value(QLatin1String("layoutMode")).toInt(Flow);
    w->m_layoutMode = (mode == Free || mode == Grid) ? mode : Flow;
    w->m_gridRows = qBound(1, o.value(QLatin1String("gridRows")).toInt(2), 16);
    w->m_gridCols = qBound(1, o.value(QLatin1String("gridCols")).toInt(2), 16);
    w->m_wallpaper = o.value(QLatin1String("wallpaper")).toString();
    w->m_wallpaperDim = qBound(0.0, o.value(QLatin1String("wallpaperDim")).toDouble(0.0), 0.95);
    w->m_open = o.value(QLatin1String("open")).toBool(true);
    w->m_sortKey = o.value(QLatin1String("sortKey")).toString();
    w->m_sortAsc = o.value(QLatin1String("sortAscending")).toBool(true);
    w->m_autoSort = o.value(QLatin1String("autoSort")).toBool(false);
    const QJsonArray g = o.value(QLatin1String("geometry")).toArray();
    if (g.size() == 4)
        w->m_geometry = QRect(g[0].toInt(), g[1].toInt(), g[2].toInt(), g[3].toInt());
    w->m_fullScreen = o.value(QLatin1String("fullScreen")).toBool(false);
    w->m_screenName = o.value(QLatin1String("screen")).toString();
    const QJsonArray players = o.value(QLatin1String("players")).toArray();
    for (const QJsonValue &v : players)
        w->attach(Player::fromJson(v.toObject(), w));
    w->renumber(w->ordered());
    w->scheduleRelayout();
    return w;
}

QJsonObject VmsWindow::toJson() const
{
    QJsonObject o;
    o.insert(QLatin1String("uid"), m_uid);
    o.insert(QLatin1String("title"), m_title);
    o.insert(QLatin1String("color"), m_color.name(QColor::HexRgb));
    o.insert(QLatin1String("layoutMode"), m_layoutMode);
    o.insert(QLatin1String("gridRows"), m_gridRows);
    o.insert(QLatin1String("gridCols"), m_gridCols);
    o.insert(QLatin1String("wallpaper"), m_wallpaper);
    o.insert(QLatin1String("wallpaperDim"), m_wallpaperDim);
    o.insert(QLatin1String("open"), m_open);
    o.insert(QLatin1String("sortKey"), m_sortKey);
    o.insert(QLatin1String("sortAscending"), m_sortAsc);
    o.insert(QLatin1String("autoSort"), m_autoSort);
    if (m_geometry.isValid())
        o.insert(QLatin1String("geometry"),
                 QJsonArray{m_geometry.x(), m_geometry.y(), m_geometry.width(), m_geometry.height()});
    o.insert(QLatin1String("fullScreen"), m_fullScreen);
    o.insert(QLatin1String("screen"), m_screenName);
    QJsonArray players;
    for (Player *p : ordered())
        players.append(p->toJson());
    o.insert(QLatin1String("players"), players);
    return o;
}

void VmsWindow::setTitle(const QString &t)
{
    const QString v = t.trimmed().isEmpty() ? m_title : t.trimmed();
    if (v == m_title)
        return;
    m_title = v;
    Q_EMIT titleChanged();
    Q_EMIT changed();
}

void VmsWindow::setColor(const QColor &c)
{
    if (!c.isValid() || c == m_color)
        return;
    m_color = c;
    Q_EMIT colorChanged();
    Q_EMIT changed();
}

void VmsWindow::setLayoutMode(int m)
{
    m = (m == Free || m == Grid) ? m : Flow;
    if (m == m_layoutMode)
        return;
    if (m_layoutMode == Grid) {
        // Leaving the grid: keep the size the video had in its cell.
        for (Player *p : m_players.items()) {
            if (p->layoutWidth() > 0 && p->layoutHeight() > 0)
                p->setDisplayWidth(qMin(p->layoutWidth(), p->layoutHeight() * p->aspect()));
        }
        relayout();
    }
    if (m == Free) {
        // Start free mode exactly where the flow layout put the players.
        for (Player *p : m_players.items()) {
            p->setPosX(p->layoutX());
            p->setPosY(p->layoutY());
        }
    }
    m_layoutMode = m;
    Q_EMIT layoutModeChanged();
    Q_EMIT changed();
    relayout();
}

void VmsWindow::setGrid(int rows, int cols)
{
    rows = qBound(1, rows, 16);
    cols = qBound(1, cols, 16);
    if (rows != m_gridRows || cols != m_gridCols) {
        m_gridRows = rows;
        m_gridCols = cols;
        Q_EMIT gridChanged();
        Q_EMIT changed();
    }
    if (m_layoutMode != Grid)
        setLayoutMode(Grid);
    relayout();
}

QPointF VmsWindow::cellPos(int index) const
{
    const int r = index / m_gridCols;
    const int col = index % m_gridCols;
    return QPointF(kMargin + col * (m_cellW + kSpacing), kMargin + r * (m_cellH + kSpacing));
}

void VmsWindow::setWallpaper(const QString &w)
{
    if (w == m_wallpaper)
        return;
    m_wallpaper = w;
    Q_EMIT wallpaperChanged();
    Q_EMIT changed();
}

void VmsWindow::setWallpaperDim(qreal d)
{
    d = qBound(0.0, d, 0.95);
    if (qFuzzyCompare(d + 1, m_wallpaperDim + 1))
        return;
    m_wallpaperDim = d;
    Q_EMIT wallpaperChanged();
    Q_EMIT changed();
}

void VmsWindow::setRenderFps(qreal fps)
{
    m_renderFps = fps;
    for (Player *p : m_players.items())
        p->setRenderFps(fps);
    Q_EMIT renderFpsChanged();
}

void VmsWindow::setOpen(bool o)
{
    if (o == m_open)
        return;
    m_open = o;
    Q_EMIT openChanged();
    Q_EMIT changed();
}

void VmsWindow::setViewportWidth(qreal w)
{
    if (qFuzzyCompare(w + 1, m_viewportW + 1))
        return;
    m_viewportW = w;
    Q_EMIT viewportChanged();
    scheduleRelayout();
}

void VmsWindow::setViewportHeight(qreal h)
{
    if (qFuzzyCompare(h + 1, m_viewportH + 1))
        return;
    m_viewportH = h;
    Q_EMIT viewportChanged();
    scheduleRelayout();
}

void VmsWindow::setAutoSort(bool on)
{
    if (on == m_autoSort)
        return;
    m_autoSort = on;
    if (m_autoSort && m_sortKey.isEmpty())
        m_sortKey = QStringLiteral("name");
    Q_EMIT sortChanged();
    Q_EMIT changed();
    if (m_autoSort)
        applySort();
}

void VmsWindow::setGeometry(const QRect &r)
{
    if (r == m_geometry || !r.isValid())
        return;
    m_geometry = r;
    Q_EMIT changed();
}

void VmsWindow::setFullScreen(bool f)
{
    if (f == m_fullScreen)
        return;
    m_fullScreen = f;
    Q_EMIT placementChanged();
    Q_EMIT changed();
}

void VmsWindow::setScreenName(const QString &s)
{
    if (s == m_screenName)
        return;
    m_screenName = s;
    Q_EMIT placementChanged();
    Q_EMIT changed();
}

QList<Player *> VmsWindow::ordered() const
{
    QList<Player *> list = m_players.items();
    std::stable_sort(list.begin(), list.end(),
                     [](Player *a, Player *b) { return a->order() < b->order(); });
    return list;
}

void VmsWindow::renumber(const QList<Player *> &list)
{
    for (int i = 0; i < list.size(); ++i)
        list[i]->setOrder(i);
}

void VmsWindow::attach(Player *p)
{
    p->setParent(this);
    connect(p, &Player::geometryChanged, this, &VmsWindow::scheduleRelayout);
    connect(p, &Player::orderChanged, this, &VmsWindow::scheduleRelayout);
    connect(p, &Player::posChanged, this, &VmsWindow::scheduleRelayout);
    connect(p, &Player::layoutChanged, this, &VmsWindow::scheduleVisibility);
    connect(p, &Player::geometryChanged, this, &VmsWindow::scheduleVisibility);
    connect(p, &Player::posChanged, this, &VmsWindow::scheduleVisibility);
    connect(p, &Player::zOrderChanged, this, &VmsWindow::scheduleVisibility);
    connect(p, &Player::persistentChanged, this, &VmsWindow::changed);
    // Keep the automatic sort up to date when a sort criterion changes.
    connect(p, &Player::configChanged, this, [this] { if (m_autoSort) applySort(); });
    connect(p, &Player::videoInfoChanged, this, [this] {
        if (m_autoSort && m_sortKey == QLatin1String("resolution"))
            applySort();
    });
    connect(p, &Player::statusChanged, this, [this] {
        if (m_autoSort && m_sortKey == QLatin1String("status"))
            applySort();
    });
    m_players.append(p);
}

Player *VmsWindow::addPlayer(const QVariantMap &cfg)
{
    auto *p = new Player(this);
    p->setOrder(m_players.count());
    if (m_layoutMode == Free) {
        // Below everything that is already there.
        qreal bottom = kMargin;
        for (Player *o : m_players.items())
            bottom = qMax(bottom, o->posY() + o->displayHeight() + kSpacing);
        p->setPosX(kMargin);
        p->setPosY(bottom);
    }
    attach(p);
    p->applyConfig(cfg);      // also starts it
    Q_EMIT playersChanged();
    Q_EMIT changed();
    if (m_autoSort)
        applySort();
    relayout();
    return p;
}

void VmsWindow::removePlayer(Player *p)
{
    if (!p || !m_players.items().contains(p))
        return;
    p->disconnect(this);
    p->setSuspended(true, true);   // stop the pipeline before the tile (GL item) goes away
    m_players.remove(p);
    p->deleteLater();          // stops its pipeline asynchronously
    renumber(ordered());
    Q_EMIT playersChanged();
    Q_EMIT changed();
    relayout();
}

void VmsWindow::movePlayer(Player *p, int index)
{
    QList<Player *> list = ordered();
    const int from = list.indexOf(p);
    if (from < 0)
        return;
    list.removeAt(from);
    index = qBound(0, index, int(list.size()));
    list.insert(index, p);
    if (index != from && m_autoSort) {   // a manual move ends automatic sorting
        m_autoSort = false;
        Q_EMIT sortChanged();
    }
    renumber(list);
    relayout();
}

int VmsWindow::dropIndex(Player *dragged, qreal x, qreal y) const
{
    QList<Player *> list = ordered();
    list.removeAll(dragged);
    if (list.isEmpty())
        return 0;
    int best = -1;
    qreal bestDist = 0;
    for (int i = 0; i < list.size(); ++i) {
        const Player *p = list[i];
        const QRectF r(p->layoutX(), p->layoutY(), p->layoutWidth(), p->layoutHeight());
        if (r.contains(x, y))
            return x < r.center().x() ? i : i + 1;
        // Distance to the rectangle, rows weighted stronger than columns.
        const qreal dx = qMax<qreal>(0, qMax(r.left() - x, x - r.right()));
        const qreal dy = qMax<qreal>(0, qMax(r.top() - y, y - r.bottom()));
        const qreal d = dx + dy * 4;
        if (best < 0 || d < bestDist) {
            best = i;
            bestDist = d;
        }
    }
    const Player *p = list[best];
    return x < p->layoutX() + p->layoutWidth() / 2 ? best : best + 1;
}

QRectF VmsWindow::dropIndicator(Player *dragged, qreal x, qreal y) const
{
    QList<Player *> list = ordered();
    list.removeAll(dragged);
    if (list.isEmpty())
        return QRectF(kMargin, kMargin, 4, 120);
    const int idx = dropIndex(dragged, x, y);
    if (idx < list.size()) {
        const Player *p = list[idx];
        return QRectF(p->layoutX() - kSpacing / 2 - 2, p->layoutY(), 4, p->layoutHeight());
    }
    const Player *p = list.last();
    return QRectF(p->layoutX() + p->layoutWidth() + kSpacing / 2 - 2, p->layoutY(), 4, p->layoutHeight());
}

void VmsWindow::sortBy(const QString &key, bool ascending)
{
    m_sortKey = key;
    m_sortAsc = ascending;
    Q_EMIT sortChanged();
    Q_EMIT changed();
    applySort();
    if (m_layoutMode == Free)
        arrange();
}

void VmsWindow::applySort()
{
    if (m_inSort || m_sortKey.isEmpty())
        return;
    m_inSort = true;
    QCollator coll;
    coll.setNumericMode(true);       // "Камера 2" < "Камера 10"
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    const QString key = m_sortKey;
    auto less = [&](Player *a, Player *b) -> bool {
        if (key == QLatin1String("url"))
            return coll.compare(a->url(), b->url()) < 0;
        if (key == QLatin1String("size"))
            return a->displayWidth() < b->displayWidth();
        if (key == QLatin1String("resolution"))
            return qint64(a->videoWidth()) * a->videoHeight() < qint64(b->videoWidth()) * b->videoHeight();
        if (key == QLatin1String("status"))
            return a->status() < b->status();
        if (key == QLatin1String("created"))
            return a->created() < b->created();
        return coll.compare(a->name(), b->name()) < 0;
    };
    QList<Player *> list = ordered();
    std::stable_sort(list.begin(), list.end(), [&](Player *a, Player *b) {
        return m_sortAsc ? less(a, b) : less(b, a);
    });
    renumber(list);
    m_inSort = false;
    relayout();
}

void VmsWindow::fitAll()
{
    if (m_layoutMode == Grid)
        return;          // the grid always fills the window
    const QList<Player *> list = ordered();
    const int n = list.size();
    if (n == 0)
        return;
    const qreal W = m_viewportW - 2 * kMargin;
    const qreal H = m_viewportH - 2 * kMargin;
    qreal minAspect = 100;
    for (Player *p : list)
        minAspect = qMin(minAspect, p->aspect());
    qreal bestW = 0;
    for (int cols = 1; cols <= n; ++cols) {
        const int rows = (n + cols - 1) / cols;
        qreal w = (W - (cols - 1) * kSpacing) / cols;
        const qreal hMax = (H - (rows - 1) * kSpacing) / rows;
        w = qMin(w, hMax * minAspect);
        if (w > bestW)
            bestW = w;
    }
    bestW = qMax(Player::kMinWidth, qreal(qFloor(bestW)));
    for (Player *p : list)
        p->setDisplayWidth(bestW);
    relayout();
    if (m_layoutMode == Free)
        arrange();
}

void VmsWindow::originalSizeAll()
{
    if (m_layoutMode == Grid)
        setLayoutMode(Flow);   // native sizes are meaningless in fixed cells
    for (Player *p : m_players.items())
        p->resetSize();
    relayout();
    if (m_layoutMode == Free)
        arrange();
}

void VmsWindow::arrange()
{
    relayout();
    for (Player *p : m_players.items()) {
        p->setPosX(p->layoutX());
        p->setPosY(p->layoutY());
    }
    relayout();
}

void VmsWindow::startAll()
{
    for (Player *p : m_players.items())
        p->start();
}

void VmsWindow::stopAll()
{
    for (Player *p : m_players.items())
        p->stop();
}

void VmsWindow::setViewportX(qreal x)
{
    if (qFuzzyCompare(x + 1, m_viewX + 1))
        return;
    m_viewX = x;
    Q_EMIT viewportChanged();
    scheduleVisibility();
}

void VmsWindow::setViewportY(qreal y)
{
    if (qFuzzyCompare(y + 1, m_viewY + 1))
        return;
    m_viewY = y;
    Q_EMIT viewportChanged();
    scheduleVisibility();
}

void VmsWindow::setExpandedPlayer(Player *p)
{
    if (p == m_expanded)
        return;
    m_expanded = p;
    Q_EMIT expandedPlayerChanged();
    scheduleVisibility();
}

void VmsWindow::raise(Player *p)
{
    if (!p)
        return;
    // Compact the stacking order to 1..n (the newest on top).
    QList<Player *> list = m_players.items();
    std::stable_sort(list.begin(), list.end(), [](Player *a, Player *b) { return a->zOrder() < b->zOrder(); });
    list.removeAll(p);
    list.append(p);
    for (int i = 0; i < list.size(); ++i)
        list[i]->setZOrder(i + 1);
}

void VmsWindow::setWindowHidden(bool hidden, int delayMs, const QString &reason)
{
    if (hidden == m_windowHidden && reason == m_windowHiddenReason)
        return;
    m_windowHidden = hidden;
    m_windowHiddenDelay = delayMs;
    m_windowHiddenReason = hidden ? reason : QString();
    qInfo("Window \"%s\": %s", qPrintable(m_title),
          hidden ? qPrintable(QStringLiteral("not visible (%1)").arg(reason)) : "visible");
    Q_EMIT windowHiddenChanged();
    updateVisibility();
}

void VmsWindow::scheduleVisibility()
{
    if (m_visibilityQueued)
        return;
    m_visibilityQueued = true;
    QMetaObject::invokeMethod(this, [this] { updateVisibility(); }, Qt::QueuedConnection);
}

// Which players can be seen: inside the visible part of the canvas, not
// covered completely by players above them (free layout), not behind an
// expanded player, in a window that is on a working display.
void VmsWindow::updateVisibility()
{
    m_visibilityQueued = false;
    const QList<Player *> list = m_players.items();
    if (m_windowHidden) {
        for (Player *p : list)
            p->setVisibility(false, m_windowHiddenDelay, m_windowHiddenReason);
        return;
    }
    const QRect view = QRectF(m_viewX, m_viewY, m_viewportW, m_viewportH).toAlignedRect();
    auto rectOf = [this](Player *p) {
        if (m_layoutMode == Free)
            return QRectF(p->posX(), p->posY(), p->displayWidth(), p->displayHeight());
        return QRectF(p->layoutX(), p->layoutY(), p->layoutWidth(), p->layoutHeight());
    };
    constexpr int kDelay = 2000;
    for (int i = 0; i < list.size(); ++i) {
        Player *p = list[i];
        if (m_expanded) {
            if (p == m_expanded)
                p->setVisibility(true, 0, QString());
            else
                p->setVisibility(false, kDelay, QStringLiteral("на всё окно развёрнут другой плеер"));
            continue;
        }
        // Inner part (1 px border ignored): touching edges do not count.
        const QRect r = rectOf(p).toAlignedRect().adjusted(1, 1, -1, -1);
        QRegion visible = QRegion(r.intersected(view));
        if (visible.isEmpty()) {
            p->setVisibility(false, kDelay, QStringLiteral("за пределами видимой части окна"));
            continue;
        }
        if (m_layoutMode == Free) {
            for (int j = 0; j < list.size() && !visible.isEmpty(); ++j) {
                Player *q = list[j];
                if (q == p)
                    continue;
                // Above: higher z, or the same z and later in the list (drawn later).
                if (q->zOrder() > p->zOrder() || (q->zOrder() == p->zOrder() && j > i))
                    visible -= rectOf(q).toAlignedRect();
            }
            if (visible.isEmpty()) {
                p->setVisibility(false, kDelay, QStringLiteral("полностью закрыт другими плеерами"));
                continue;
            }
        }
        p->setVisibility(true, 0, QString());
    }
}

void VmsWindow::setSuspended(bool s, bool synchronous)
{
    for (Player *p : m_players.items())
        p->setSuspended(s, synchronous);
}

void VmsWindow::scheduleRelayout()
{
    if (m_relayoutQueued)
        return;
    m_relayoutQueued = true;
    QMetaObject::invokeMethod(this, [this] { relayout(); }, Qt::QueuedConnection);
}

void VmsWindow::relayout()
{
    m_relayoutQueued = false;
    if (m_layoutMode == Grid) {
        // Equal cells; N rows fill the viewport, further players continue
        // on additional rows (scrolling).
        const qreal cw = qMax<qreal>(40, (m_viewportW - 2 * kMargin - (m_gridCols - 1) * kSpacing) / m_gridCols);
        const qreal ch = qMax<qreal>(30, (m_viewportH - 2 * kMargin - (m_gridRows - 1) * kSpacing) / m_gridRows);
        m_cellW = cw;
        m_cellH = ch;
        const QList<Player *> list = ordered();
        for (int i = 0; i < list.size(); ++i) {
            const QPointF pos = cellPos(i);
            list[i]->setLayoutRect(pos.x(), pos.y(), cw, ch);
        }
        const int rowsUsed = qMax(m_gridRows, int((list.size() + m_gridCols - 1) / m_gridCols));
        m_contentW = m_viewportW;
        m_contentH = 2 * kMargin + rowsUsed * ch + (rowsUsed - 1) * kSpacing;
        Q_EMIT contentSizeChanged();
        scheduleVisibility();
        return;
    }
    // Flow layout: left to right, wrap at the viewport edge. Always computed:
    // it is also the target of drag & drop and of "arrange" in free mode.
    qreal x = kMargin, y = kMargin, rowH = 0, right = 0;
    const qreal limit = m_viewportW - kMargin;
    for (Player *p : ordered()) {
        const qreal w = p->displayWidth();
        const qreal h = p->displayHeight();
        if (x > kMargin && x + w > limit) {
            x = kMargin;
            y += rowH + kSpacing;
            rowH = 0;
        }
        p->setLayoutRect(x, y, w, h);
        x += w + kSpacing;
        rowH = qMax(rowH, h);
        right = qMax(right, x - kSpacing);
    }
    qreal cw = right + kMargin;
    qreal ch = y + rowH + kMargin;
    if (m_layoutMode == Free) {
        cw = ch = 0;
        for (Player *p : m_players.items()) {
            cw = qMax(cw, p->posX() + p->displayWidth() + kMargin);
            ch = qMax(ch, p->posY() + p->displayHeight() + kMargin);
        }
    }
    if (!qFuzzyCompare(cw + 1, m_contentW + 1) || !qFuzzyCompare(ch + 1, m_contentH + 1)) {
        m_contentW = cw;
        m_contentH = ch;
        Q_EMIT contentSizeChanged();
    }
    scheduleVisibility();
}
