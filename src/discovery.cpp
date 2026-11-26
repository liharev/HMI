#include "discovery.h"

#include <QAuthenticator>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QUrl>
#include <QXmlStreamReader>

namespace discovery {

namespace {

QNetworkAccessManager *nam()
{
    static QNetworkAccessManager *n = new QNetworkAccessManager;
    return n;
}

// Basic / Digest HTTP authentication is answered by Qt for this reply.
void answerAuth(QNetworkReply *reply, const QString &user, const QString &password)
{
    QObject::connect(nam(), &QNetworkAccessManager::authenticationRequired, reply,
                     [reply, user, password](QNetworkReply *r, QAuthenticator *auth) {
        if (r != reply || user.isEmpty() || r->property("hmiAuthTried").toBool())
            return;
        r->setProperty("hmiAuthTried", true);
        auth->setUser(user);
        auth->setPassword(password);
    });
}

QString replyError(QNetworkReply *reply, const QByteArray &body)
{
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // SOAP faults carry a readable reason.
    QXmlStreamReader x(body);
    QString reason;
    while (!x.atEnd()) {
        if (x.readNext() == QXmlStreamReader::StartElement && x.name() == QLatin1String("Text"))
            reason = x.readElementText().trimmed();
    }
    if (code == 401 || reason.contains(QLatin1String("authoriz"), Qt::CaseInsensitive)
        || reason.contains(QLatin1String("Sender not Authorized"), Qt::CaseInsensitive))
        return QStringLiteral("камера отклонила логин/пароль%1").arg(reason.isEmpty() ? QString() : QStringLiteral(" (") + reason + QLatin1Char(')'));
    if (!reason.isEmpty())
        return reason;
    if (code > 0)
        return QStringLiteral("ошибка HTTP %1").arg(code);
    return reply->errorString();
}

// ---------------------------------------------------------------------------
// ONVIF
// ---------------------------------------------------------------------------
class OnvifJob : public QObject
{
public:
    OnvifJob(const QString &address, const QString &user, const QString &password, QObject *ctx, Done done)
        : m_user(user), m_password(password), m_ctx(ctx), m_done(std::move(done))
    {
        QString a = address.trimmed();
        if (!a.contains(QLatin1String("://")))
            a.prepend(QStringLiteral("http://"));
        QUrl u(a);
        if (u.path().isEmpty() || u.path() == QLatin1String("/"))
            u.setPath(QStringLiteral("/onvif/device_service"));
        m_device = u;
    }

    void start()
    {
        // The camera clock: WS-Security tokens are rejected when "Created"
        // differs too much from it.
        call(m_device, QStringLiteral("<tds:GetSystemDateAndTime/>"), false, [this](const QByteArray &body, const QString &err) {
            if (err.isEmpty())
                readClock(body);
            getCapabilities();
        });
    }

private:
    void finish(const QList<StreamProfile> &list, const QString &err)
    {
        if (m_ctx)
            m_done(list, err);
        deleteLater();
    }

    QString security() const
    {
        if (m_user.isEmpty())
            return QString();
        QByteArray nonce(16, Qt::Uninitialized);
        for (char &c : nonce)
            c = char(QRandomGenerator::global()->bounded(256));
        const QString created = QDateTime::currentDateTimeUtc().addSecs(m_clockOffset)
                                    .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
        const QByteArray digest = QCryptographicHash::hash(nonce + created.toUtf8() + m_password.toUtf8(),
                                                           QCryptographicHash::Sha1).toBase64();
        auto esc = [](const QString &s) { return s.toHtmlEscaped(); };
        return QStringLiteral(
                   "<wsse:Security s:mustUnderstand=\"1\" "
                   "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
                   "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
                   "<wsse:UsernameToken><wsse:Username>%1</wsse:Username>"
                   "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%2</wsse:Password>"
                   "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</wsse:Nonce>"
                   "<wsu:Created>%4</wsu:Created></wsse:UsernameToken></wsse:Security>")
            .arg(esc(m_user), QString::fromLatin1(digest), QString::fromLatin1(nonce.toBase64()), created);
    }

    void call(const QUrl &url, const QString &body, bool secure, std::function<void(const QByteArray &, const QString &)> cb)
    {
        const QString env = QStringLiteral(
                                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                                "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
                                "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
                                "xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
                                "<s:Header>%1</s:Header><s:Body>%2</s:Body></s:Envelope>")
                                .arg(secure ? security() : QString(), body);
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/soap+xml; charset=utf-8"));
        req.setTransferTimeout(8000);
        QNetworkReply *reply = nam()->post(req, env.toUtf8());
        answerAuth(reply, m_user, m_password);
        connect(reply, &QNetworkReply::finished, this, [reply, cb] {
            reply->deleteLater();
            const QByteArray data = reply->readAll();
            if (reply->error() != QNetworkReply::NoError)
                cb(data, replyError(reply, data));
            else
                cb(data, QString());
        });
    }

    void readClock(const QByteArray &body)
    {
        QXmlStreamReader x(body);
        bool utc = false;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        while (!x.atEnd()) {
            if (x.readNext() != QXmlStreamReader::StartElement)
                continue;
            const auto n = x.name();
            if (n == QLatin1String("UTCDateTime"))
                utc = true;
            else if (!utc)
                continue;
            else if (n == QLatin1String("Year")) y = x.readElementText().toInt();
            else if (n == QLatin1String("Month")) mo = x.readElementText().toInt();
            else if (n == QLatin1String("Day")) d = x.readElementText().toInt();
            else if (n == QLatin1String("Hour")) h = x.readElementText().toInt();
            else if (n == QLatin1String("Minute")) mi = x.readElementText().toInt();
            else if (n == QLatin1String("Second")) s = x.readElementText().toInt();
        }
        const QDateTime cam = QDateTime::fromString(QStringLiteral("%1-%2-%3T%4:%5:%6Z")
                                                        .arg(y, 4, 10, QLatin1Char('0')).arg(mo, 2, 10, QLatin1Char('0'))
                                                        .arg(d, 2, 10, QLatin1Char('0')).arg(h, 2, 10, QLatin1Char('0'))
                                                        .arg(mi, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0')),
                                                    Qt::ISODate);
        if (cam.isValid())
            m_clockOffset = QDateTime::currentDateTimeUtc().secsTo(cam);
    }

    void getCapabilities()
    {
        call(m_device, QStringLiteral("<tds:GetCapabilities><tds:Category>Media</tds:Category></tds:GetCapabilities>"), true,
             [this](const QByteArray &body, const QString &err) {
            if (!err.isEmpty()) {
                finish({}, QStringLiteral("ONVIF недоступен по %1: %2").arg(m_device.toString(), err));
                return;
            }
            QXmlStreamReader x(body);
            bool inMedia = false;
            while (!x.atEnd()) {
                const auto t = x.readNext();
                if (t == QXmlStreamReader::StartElement && x.name() == QLatin1String("Media"))
                    inMedia = true;
                else if (t == QXmlStreamReader::StartElement && inMedia && x.name() == QLatin1String("XAddr")) {
                    m_media = QUrl(x.readElementText().trimmed());
                    break;
                }
            }
            if (!m_media.isValid() || m_media.isEmpty())
                m_media = m_device;      // many cameras serve all services at one address
            // The camera may report an internal address (NAT): keep the host we reach.
            m_media.setHost(m_device.host());
            if (m_device.port() > 0)
                m_media.setPort(m_device.port());
            getProfiles();
        });
    }

    void getProfiles()
    {
        call(m_media, QStringLiteral("<trt:GetProfiles/>"), true, [this](const QByteArray &body, const QString &err) {
            if (!err.isEmpty()) {
                finish({}, QStringLiteral("GetProfiles: ") + err);
                return;
            }
            QXmlStreamReader x(body);
            QString section;     // VideoEncoderConfiguration / AudioEncoderConfiguration
            Pending cur;
            while (!x.atEnd()) {
                const auto t = x.readNext();
                if (t == QXmlStreamReader::StartElement) {
                    const auto n = x.name();
                    if (n == QLatin1String("Profiles")) {
                        cur = Pending();
                        cur.token = x.attributes().value(QLatin1String("token")).toString();
                        cur.p.audio = 0;
                        section.clear();
                    } else if (n == QLatin1String("VideoEncoderConfiguration")) {
                        section = n.toString();
                    } else if (n == QLatin1String("AudioEncoderConfiguration")) {
                        section = n.toString();
                        cur.p.audio = 1;
                    } else if (n == QLatin1String("Name") && section.isEmpty() && cur.p.label.isEmpty()) {
                        cur.p.label = x.readElementText().trimmed();
                    } else if (section == QLatin1String("VideoEncoderConfiguration")) {
                        if (n == QLatin1String("Encoding")) {
                            const QString e = x.readElementText().trimmed().toLower();
                            cur.p.codec = e == QLatin1String("jpeg") ? QStringLiteral("jpeg")
                                        : e.contains(QLatin1String("265")) || e == QLatin1String("hevc") ? QStringLiteral("h265")
                                        : e.contains(QLatin1String("264")) ? QStringLiteral("h264") : e;
                        } else if (n == QLatin1String("Width")) {
                            cur.p.width = x.readElementText().toInt();
                        } else if (n == QLatin1String("Height")) {
                            cur.p.height = x.readElementText().toInt();
                        } else if (n == QLatin1String("FrameRateLimit")) {
                            cur.p.fps = x.readElementText().toDouble();
                        } else if (n == QLatin1String("BitrateLimit")) {
                            cur.p.bitrateKbps = x.readElementText().toInt();
                        }
                    }
                } else if (t == QXmlStreamReader::EndElement) {
                    const auto n = x.name();
                    if (n == QLatin1String("VideoEncoderConfiguration") || n == QLatin1String("AudioEncoderConfiguration"))
                        section.clear();
                    else if (n == QLatin1String("Profiles") && !cur.token.isEmpty())
                        m_pending << cur;
                }
            }
            if (m_pending.isEmpty()) {
                finish({}, QStringLiteral("камера не сообщила ни одного профиля"));
                return;
            }
            nextUri();
        });
    }

    void nextUri()
    {
        if (m_index >= m_pending.size()) {
            QList<StreamProfile> out;
            for (const Pending &pd : std::as_const(m_pending))
                if (!pd.p.url.isEmpty() && pd.p.codec != QLatin1String("mpeg4"))
                    out << pd.p;
            finish(out, out.isEmpty() ? QStringLiteral("камера не выдала адреса потоков") : QString());
            return;
        }
        const QString body = QStringLiteral(
                                 "<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
                                 "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>"
                                 "<trt:ProfileToken>%1</trt:ProfileToken></trt:GetStreamUri>")
                                 .arg(m_pending[m_index].token.toHtmlEscaped());
        call(m_media, body, true, [this](const QByteArray &data, const QString &err) {
            Pending &pd = m_pending[m_index];
            if (err.isEmpty()) {
                QXmlStreamReader x(data);
                while (!x.atEnd()) {
                    if (x.readNext() == QXmlStreamReader::StartElement && x.name() == QLatin1String("Uri")) {
                        QUrl u(x.readElementText().trimmed());
                        if (u.isValid() && !u.isEmpty()) {
                            u.setHost(m_device.host());   // NAT: the address we reach the camera at
                            pd.p.url = u.toString();
                        }
                        break;
                    }
                }
            }
            pd.p.source = QStringLiteral("onvif");
            if (pd.p.label.isEmpty())
                pd.p.label = pd.token;
            ++m_index;
            nextUri();
        });
    }

    struct Pending
    {
        QString token;
        StreamProfile p;
    };

    QString m_user;
    QString m_password;
    QPointer<QObject> m_ctx;
    Done m_done;
    QUrl m_device;
    QUrl m_media;
    qint64 m_clockOffset = 0;
    QList<Pending> m_pending;
    int m_index = 0;
};

// Base "http://host:port/stream/name" of a Flussonic stream URL.
QUrl flussonicBase(const QString &streamUrl)
{
    QUrl u(streamUrl.trimmed());
    const QString scheme = u.scheme().toLower();
    if (scheme == QLatin1String("rtsp") || scheme == QLatin1String("rtmp")) {
        u.setScheme(QStringLiteral("http"));
        u.setPort(-1);
    } else if (scheme == QLatin1String("ws") || scheme == QLatin1String("m4s") || scheme == QLatin1String("m4f")) {
        u.setScheme(QStringLiteral("http"));
    } else if (scheme == QLatin1String("wss") || scheme == QLatin1String("m4ss") || scheme == QLatin1String("m4fs")) {
        u.setScheme(QStringLiteral("https"));
    }
    QStringList parts = u.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    // Drop the protocol part of the path: index.m3u8, tracks-v1/mono.m3u8, mpegts ...
    static const QStringList tails = {
        QStringLiteral("index.m3u8"), QStringLiteral("video.m3u8"), QStringLiteral("mono.m3u8"),
        QStringLiteral("playlist.m3u8"), QStringLiteral("mpegts"), QStringLiteral("index.mpd"),
        QStringLiteral("manifest.mpd"), QStringLiteral("mse_ld"), QStringLiteral("embed.html"),
        QStringLiteral("preview.jpg"), QStringLiteral("preview.mp4"), QStringLiteral("media_info.json"),
        QStringLiteral("index.ts"), QStringLiteral("Manifest"), QStringLiteral("manifest"),
    };
    while (!parts.isEmpty() && (tails.contains(parts.last()) || parts.last().startsWith(QLatin1String("tracks-"))
                                || parts.last().endsWith(QLatin1String(".isml"))))
        parts.removeLast();
    u.setPath(QLatin1Char('/') + parts.join(QLatin1Char('/')));
    u.setQuery(QString());
    u.setFragment(QString());
    return u;
}

} // namespace

void onvif(const QString &address, const QString &user, const QString &password, QObject *context, Done done)
{
    auto *job = new OnvifJob(address, user, password, context, std::move(done));
    job->start();
}

QString flussonicTrackUrl(const QString &streamUrl, const QString &trackId)
{
    QUrl u = flussonicBase(streamUrl);
    u.setPath(u.path() + QStringLiteral("/tracks-") + trackId + QStringLiteral("/mono.m3u8"));
    return u.toString();
}

void flussonic(const QString &streamUrl, const QString &user, const QString &password, QObject *context, Done done)
{
    QUrl info = flussonicBase(streamUrl);
    info.setPath(info.path() + QStringLiteral("/media_info.json"));
    QNetworkRequest req(info);
    req.setTransferTimeout(8000);
    if (!user.isEmpty())
        req.setRawHeader("Authorization", "Basic " + (user + QLatin1Char(':') + password).toUtf8().toBase64());
    QNetworkReply *reply = nam()->get(req);
    QPointer<QObject> ctx(context);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, ctx, done, streamUrl, info] {
        reply->deleteLater();
        if (!ctx)
            return;
        const QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, QStringLiteral("%1: %2").arg(info.toString(), replyError(reply, data)));
            return;
        }
        const QJsonArray tracks = QJsonDocument::fromJson(data).object().value(QLatin1String("tracks")).toArray();
        QString audio;
        for (const QJsonValue &v : tracks)
            if (v.toObject().value(QLatin1String("content")).toString() == QLatin1String("audio") && audio.isEmpty())
                audio = v.toObject().value(QLatin1String("track_id")).toString();
        QList<StreamProfile> out;
        for (const QJsonValue &v : tracks) {
            const QJsonObject t = v.toObject();
            if (t.value(QLatin1String("content")).toString() != QLatin1String("video"))
                continue;
            StreamProfile p;
            const QString id = t.value(QLatin1String("track_id")).toString();
            p.url = flussonicTrackUrl(streamUrl, id + audio);
            p.label = id;
            p.source = QStringLiteral("flussonic");
            p.kind = QStringLiteral("stream");
            p.codec = t.value(QLatin1String("codec")).toString().toLower();
            p.width = t.value(QLatin1String("width")).toInt();
            p.height = t.value(QLatin1String("height")).toInt();
            p.fps = t.value(QLatin1String("fps")).toDouble();
            p.bitrateKbps = t.value(QLatin1String("bitrate")).toInt();
            p.audio = audio.isEmpty() ? 0 : 1;
            out << p;
        }
        if (out.isEmpty())
            done({}, QStringLiteral("в media_info.json нет видеодорожек"));
        else
            done(out, QString());
    });
}

} // namespace discovery
