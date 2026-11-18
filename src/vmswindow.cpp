#include "vmswindow.h"
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
    w->m_layoutMode = o.value(QLatin1String("layoutMode")).toInt(Flow) == Free ? Free : Flow;
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
    m = (m == Free) ? Free : Flow;
    if (m == m_layoutMode)
        return;
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
        const QRectF r(p->layoutX(), p->layoutY(), p->displayWidth(), p->displayHeight());
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
    return x < p->layoutX() + p->displayWidth() / 2 ? best : best + 1;
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
        return QRectF(p->layoutX() - kSpacing / 2 - 2, p->layoutY(), 4, p->displayHeight());
    }
    const Player *p = list.last();
    return QRectF(p->layoutX() + p->displayWidth() + kSpacing / 2 - 2, p->layoutY(), 4, p->displayHeight());
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
        p->setLayoutPos(x, y);
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
}
