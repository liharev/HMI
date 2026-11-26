#include "camai.h"

#include <QAuthenticator>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

QHash<QString, CamaiService *> &registry()
{
    static QHash<QString, CamaiService *> r;
    return r;
}

QString httpError(QNetworkReply *reply)
{
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 401)
        return QStringLiteral("сервис требует авторизацию: проверьте логин и пароль");
    if (code == 404)
        return QStringLiteral("на устройстве нет API превью (404): проверьте адрес и порт сервиса");
    if (code > 0)
        return QStringLiteral("ошибка HTTP %1").arg(code);
    return reply->errorString();
}

void request(QNetworkAccessManager *nam, const QUrl &url, const QString &user, const QString &password,
             QObject *context, std::function<void(const QByteArray &, const QString &)> done)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(8000);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setRawHeader("Accept", "application/json");
    QNetworkReply *reply = nam->get(req);
    // Basic and Digest are answered by Qt itself when credentials are given.
    QObject::connect(nam, &QNetworkAccessManager::authenticationRequired, reply,
                     [reply, user, password](QNetworkReply *r, QAuthenticator *auth) {
        if (r != reply || user.isEmpty() || r->property("hmiAuthTried").toBool())
            return;
        r->setProperty("hmiAuthTried", true);
        auth->setUser(user);
        auth->setPassword(password);
    });
    QPointer<QObject> ctx(context);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, ctx, done] {
        reply->deleteLater();
        if (!ctx)
            return;
        if (reply->error() != QNetworkReply::NoError)
            done(QByteArray(), httpError(reply));
        else
            done(reply->readAll(), QString());
    });
}

} // namespace

QString CamaiService::normalize(const QString &service)
{
    QString s = service.trimmed();
    if (s.isEmpty())
        return s;
    if (!s.contains(QLatin1String("://")))
        s.prepend(QStringLiteral("http://"));
    QUrl u(s);
    if (u.port() < 0 && u.scheme() == QLatin1String("http"))
        u.setPort(8080);
    u.setPath(QString());
    u.setQuery(QString());
    u.setFragment(QString());
    QString out = u.toString(QUrl::RemoveUserInfo);
    while (out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}

CamaiService *CamaiService::get(const QString &service, const QString &user, const QString &password)
{
    const QString base = normalize(service);
    if (base.isEmpty())
        return nullptr;
    const QString key = base + QLatin1Char('\n') + user;
    auto &r = registry();
    if (CamaiService *s = r.value(key)) {
        s->m_password = password;
        return s;
    }
    auto *s = new CamaiService(base, user, password);
    r.insert(key, s);
    return s;
}

CamaiService::CamaiService(const QString &base, const QString &user, const QString &password)
    : m_base(base), m_user(user), m_password(password)
{
    m_statusTimer.setInterval(5000);
    connect(&m_statusTimer, &QTimer::timeout, this, &CamaiService::pollStatus);
    // The catalogue changes rarely (configuration of the device): re-read
    // every 5 minutes, and at once when a profile disappeared.
    m_catalogTimer.setInterval(5 * 60 * 1000);
    connect(&m_catalogTimer, &QTimer::timeout, this, &CamaiService::refreshCatalog);
}

void CamaiService::setRtspHost(const QString &hostPort)
{
    if (hostPort.trimmed() == m_rtspHost)
        return;
    m_rtspHost = hostPort.trimmed();
    refreshCatalog();
}

void CamaiService::addUser(QObject *user)
{
    for (const auto &u : std::as_const(m_users))
        if (u == user)
            return;
    m_users << QPointer<QObject>(user);
    if (!m_statusTimer.isActive()) {
        m_statusTimer.start();
        m_catalogTimer.start();
        pollStatus();
    }
    if (!m_catalogKnown)
        refreshCatalog();
}

void CamaiService::removeUser(QObject *user)
{
    m_users.removeIf([user](const QPointer<QObject> &u) { return !u || u == user; });
    if (m_users.isEmpty()) {
        m_statusTimer.stop();
        m_catalogTimer.stop();
    }
}

QList<int> CamaiService::channels() const
{
    QList<int> ids = m_catalog.keys();
    std::sort(ids.begin(), ids.end());
    return ids;
}

void CamaiService::get(const QString &path, std::function<void(const QByteArray &, const QString &)> done)
{
    request(&m_nam, QUrl(m_base + path), m_user, m_password, this, std::move(done));
}

QList<StreamProfile> CamaiService::parseProfiles(const QJsonArray &arr, const QString &rtspHost)
{
    QList<StreamProfile> list;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        StreamProfile p;
        QUrl u(o.value(QLatin1String("rtsp_uri")).toString());
        if (!u.isValid() || u.isEmpty())
            continue;
        if (!rtspHost.isEmpty()) {
            // Proxy / NAT: the device writes its own address into rtsp_uri.
            const QString host = rtspHost.section(QLatin1Char(':'), 0, 0);
            const int port = rtspHost.section(QLatin1Char(':'), 1).toInt();
            u.setHost(host);
            if (port > 0)
                u.setPort(port);
        }
        p.url = u.toString();
        p.label = o.value(QLatin1String("id")).toString();
        p.kind = o.value(QLatin1String("kind")).toString();
        p.codec = o.value(QLatin1String("codec")).toString().toLower();
        p.width = o.value(QLatin1String("width")).toInt();
        p.height = o.value(QLatin1String("height")).toInt();
        p.fps = o.value(QLatin1String("fps")).toDouble();
        p.bitrateKbps = o.value(QLatin1String("bitrate_kbps")).toInt();
        // Previews are video only; main streams usually carry the sound.
        p.audio = p.kind == QLatin1String("preview") ? 0 : -1;
        p.source = QStringLiteral("camai");
        list << p;
    }
    return list;
}

void CamaiService::refreshCatalog()
{
    if (m_catalogBusy)
        return;
    // A burst of 404s from several players: one request is enough.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_catalogKnown && now - m_lastCatalogMs < 3000)
        return;
    m_catalogBusy = true;
    get(QStringLiteral("/api/v1/preview"), [this](const QByteArray &data, const QString &err) {
        m_catalogBusy = false;
        m_lastCatalogMs = QDateTime::currentMSecsSinceEpoch();
        if (!err.isEmpty()) {
            m_error = err;
            qWarning("CAMAI %s: catalogue: %s", qPrintable(m_base), qPrintable(err));
            return;
        }
        const QJsonArray chans = QJsonDocument::fromJson(data).object().value(QLatin1String("channels")).toArray();
        QHash<int, QList<StreamProfile>> cat;
        for (const QJsonValue &v : chans) {
            const QJsonObject c = v.toObject();
            cat.insert(c.value(QLatin1String("id")).toInt(), parseProfiles(c.value(QLatin1String("profiles")).toArray(), m_rtspHost));
        }
        m_error.clear();
        int n = 0;
        for (const auto &l : std::as_const(cat))
            n += l.size();
        qInfo("CAMAI %s: catalogue: %d channels, %d profiles", qPrintable(m_base), int(cat.size()), n);
        m_catalog = cat;
        m_catalogKnown = true;
        Q_EMIT catalogChanged();
    });
}

void CamaiService::pollStatus()
{
    if (m_statusBusy)
        return;
    m_statusBusy = true;
    get(QStringLiteral("/api/v1/status"), [this](const QByteArray &data, const QString &err) {
        m_statusBusy = false;
        if (!err.isEmpty()) {
            m_error = err;
            return;
        }
        const QJsonArray chans = QJsonDocument::fromJson(data).object().value(QLatin1String("channels")).toArray();
        QHash<int, ChannelState> st;
        for (const QJsonValue &v : chans) {
            const QJsonObject c = v.toObject();
            const QJsonObject src = c.value(QLatin1String("source")).toObject();
            ChannelState s;
            s.known = true;
            s.signal = src.value(QLatin1String("signal")).toBool(true);
            s.splash = src.value(QLatin1String("splash")).toBool(false);
            s.description = src.value(QLatin1String("description")).toString();
            st.insert(c.value(QLatin1String("id")).toInt(), s);
        }
        bool changed = st.size() != m_state.size();
        for (auto it = st.cbegin(); !changed && it != st.cend(); ++it) {
            const ChannelState o = m_state.value(it.key());
            changed = o.signal != it->signal || o.splash != it->splash || o.description != it->description;
        }
        m_state = st;
        if (changed)
            Q_EMIT statusChanged();
    });
}

void CamaiService::fetchChannels(const QString &service, const QString &user, const QString &password,
                                 QObject *context, std::function<void(const QVariantList &, const QString &)> done)
{
    const QString base = normalize(service);
    if (base.isEmpty()) {
        done({}, QStringLiteral("не задан адрес сервиса"));
        return;
    }
    static QNetworkAccessManager *nam = new QNetworkAccessManager;
    QPointer<QObject> ctx(context);
    request(nam, QUrl(base + QStringLiteral("/api/v1/preview")), user, password, context,
            [nam, base, user, password, ctx, done](const QByteArray &data, const QString &err) {
        if (!err.isEmpty()) {
            done({}, err);
            return;
        }
        const QJsonArray chans = QJsonDocument::fromJson(data).object().value(QLatin1String("channels")).toArray();
        if (chans.isEmpty()) {
            done({}, QStringLiteral("ответ сервиса не похож на каталог CAMAI (нет каналов)"));
            return;
        }
        // The input state is a nice-to-have: ask, but do not fail without it.
        request(nam, QUrl(base + QStringLiteral("/api/v1/status")), user, password, ctx.data(),
                [chans, done](const QByteArray &sdata, const QString &) {
            QHash<int, QJsonObject> sources;
            for (const QJsonValue &v : QJsonDocument::fromJson(sdata).object().value(QLatin1String("channels")).toArray())
                sources.insert(v.toObject().value(QLatin1String("id")).toInt(),
                               v.toObject().value(QLatin1String("source")).toObject());
            QVariantList out;
            for (const QJsonValue &v : chans) {
                const QJsonObject c = v.toObject();
                const int id = c.value(QLatin1String("id")).toInt();
                QVariantList profs;
                for (const StreamProfile &p : parseProfiles(c.value(QLatin1String("profiles")).toArray(), QString()))
                    profs << p.toVariant();
                const QJsonObject src = sources.value(id);
                out << QVariantMap{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("profiles"), profs},
                    {QStringLiteral("signal"), src.value(QLatin1String("signal")).toBool(true)},
                    {QStringLiteral("splash"), src.value(QLatin1String("splash")).toBool(false)},
                    {QStringLiteral("description"), src.value(QLatin1String("description")).toString()},
                };
            }
            done(out, QString());
        });
    });
}
