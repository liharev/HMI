#include "streamprofile.h"
#include "gstutils.h"

#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QUrl>

#include <gst/pbutils/pbutils.h>

#include <cmath>

QString StreamProfile::describe() const
{
    QStringList parts;
    if (known())
        parts << QStringLiteral("%1×%2").arg(width).arg(height);
    if (fps > 0)
        parts << QStringLiteral("%1 к/с").arg(fps, 0, 'f', fps < 10 && std::fmod(fps, 1.0) > 0.01 ? 1 : 0);
    if (!codec.isEmpty()) {
        const QString c = codec == QLatin1String("h264") ? QStringLiteral("H.264")
                        : codec == QLatin1String("h265") ? QStringLiteral("H.265")
                        : codec == QLatin1String("jpeg") ? QStringLiteral("MJPEG")
                        : codec.toUpper();
        parts << c;
    }
    if (bitrateKbps > 0)
        parts << (bitrateKbps >= 1000 ? QStringLiteral("%1 Мбит/с").arg(bitrateKbps / 1000.0, 0, 'f', 1)
                                      : QStringLiteral("%1 кбит/с").arg(bitrateKbps));
    if (audio == 1)
        parts << QStringLiteral("звук");
    if (parts.isEmpty())
        return QStringLiteral("параметры неизвестны");
    return parts.join(QStringLiteral(" · "));
}

QJsonObject StreamProfile::toJson() const
{
    QJsonObject o{{QStringLiteral("url"), url}};
    auto put = [&](const char *k, const QJsonValue &v, bool cond) {
        if (cond)
            o.insert(QLatin1String(k), v);
    };
    put("label", label, !label.isEmpty());
    put("kind", kind, !kind.isEmpty());
    put("source", source, !source.isEmpty());
    put("codec", codec, !codec.isEmpty());
    put("w", width, width > 0);
    put("h", height, height > 0);
    put("fps", fps, fps > 0);
    put("kbps", bitrateKbps, bitrateKbps > 0);
    put("audio", audio, audio >= 0);
    return o;
}

StreamProfile StreamProfile::fromJson(const QJsonObject &o)
{
    StreamProfile p;
    p.url = o.value(QLatin1String("url")).toString();
    p.label = o.value(QLatin1String("label")).toString();
    p.kind = o.value(QLatin1String("kind")).toString();
    p.source = o.value(QLatin1String("source")).toString();
    p.codec = o.value(QLatin1String("codec")).toString();
    p.width = o.value(QLatin1String("w")).toInt();
    p.height = o.value(QLatin1String("h")).toInt();
    p.fps = o.value(QLatin1String("fps")).toDouble();
    p.bitrateKbps = o.value(QLatin1String("kbps")).toInt();
    p.audio = o.value(QLatin1String("audio")).toInt(-1);
    return p;
}

QVariantMap StreamProfile::toVariant() const
{
    return {
        {QStringLiteral("url"), url},
        {QStringLiteral("label"), label},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("source"), source},
        {QStringLiteral("codec"), codec},
        {QStringLiteral("width"), width},
        {QStringLiteral("height"), height},
        {QStringLiteral("fps"), fps},
        {QStringLiteral("bitrateKbps"), bitrateKbps},
        {QStringLiteral("audio"), audio},
        {QStringLiteral("description"), describe()},
    };
}

StreamProfile StreamProfile::fromVariant(const QVariantMap &m)
{
    StreamProfile p;
    p.url = m.value(QStringLiteral("url")).toString().trimmed();
    p.label = m.value(QStringLiteral("label")).toString();
    p.kind = m.value(QStringLiteral("kind")).toString();
    p.source = m.value(QStringLiteral("source")).toString();
    p.codec = m.value(QStringLiteral("codec")).toString();
    p.width = m.value(QStringLiteral("width")).toInt();
    p.height = m.value(QStringLiteral("height")).toInt();
    p.fps = m.value(QStringLiteral("fps")).toDouble();
    p.bitrateKbps = m.value(QStringLiteral("bitrateKbps")).toInt();
    p.audio = m.value(QStringLiteral("audio"), -1).toInt();
    return p;
}

namespace profiles {

namespace {

bool hardwareFor(const QString &codec)
{
    static QHash<QString, bool> cache;
    auto it = cache.constFind(codec);
    if (it != cache.constEnd())
        return *it;
    gsu::Codec c = gsu::Codec::Unknown;
    if (codec == QLatin1String("h264"))
        c = gsu::Codec::H264;
    else if (codec == QLatin1String("h265"))
        c = gsu::Codec::H265;
    else if (codec == QLatin1String("jpeg"))
        c = gsu::Codec::Jpeg;
    bool hw = false;
    for (const QString &f : gsu::hardwareDecoders(c))
        if (gsu::hasElement(f.toUtf8().constData()))
            hw = true;
    // Unknown codec: assume the common case (decoded in hardware) so that
    // unprobed streams are not pushed aside.
    if (c == gsu::Codec::Unknown)
        hw = true;
    cache.insert(codec, hw);
    return hw;
}

// How many times the player area is larger than the picture (<= 1: the
// picture has at least the resolution of the screen area).
double magnification(const StreamProfile &p, const ProfileRequest &r)
{
    return qMin(r.boxWidth / p.width, r.boxHeight / p.height);
}

} // namespace

double cost(const StreamProfile &p)
{
    const double fps = p.fps > 0 ? p.fps : 25.0;
    return double(p.width) * p.height * fps * (hardwareFor(p.codec) ? 1.0 : 3.0);
}

int indexOf(const QList<StreamProfile> &list, const QString &url)
{
    for (int i = 0; i < list.size(); ++i)
        if (list[i].url == url)
            return i;
    return -1;
}

int select(const QList<StreamProfile> &list, const ProfileRequest &req, QString *why)
{
    QList<int> cand;
    for (int i = 0; i < list.size(); ++i) {
        const StreamProfile &p = list[i];
        if (p.url.isEmpty() || !p.known() || req.excluded.contains(p.url))
            continue;
        if (req.needAudio && p.audio == 0)
            continue;
        cand << i;
    }
    if (cand.isEmpty()) {
        // Nothing with known parameters: keep the first usable address.
        for (int i = 0; i < list.size(); ++i)
            if (!list[i].url.isEmpty() && !req.excluded.contains(list[i].url))
                return i;
        return -1;
    }
    // Frame rate floor (low-rate previews look jerky), unless nothing is left.
    QList<int> fast;
    for (int i : cand)
        if (list[i].fps <= 0 || list[i].fps + 0.01 >= req.minFps)
            fast << i;
    if (!fast.isEmpty())
        cand = fast;

    auto better = [&](int a, int b) {    // a lighter than b (ties: running stream, lower bitrate)
        const double ca = cost(list[a]), cb = cost(list[b]);
        if (qAbs(ca - cb) > 0.15 * qMax(ca, cb))
            return ca < cb;
        const bool sa = list[a].kind == QLatin1String("stream"), sb = list[b].kind == QLatin1String("stream");
        if (sa != sb)
            return sa;
        if (list[a].bitrateKbps != list[b].bitrateKbps && list[a].bitrateKbps > 0 && list[b].bitrateKbps > 0)
            return list[a].bitrateKbps < list[b].bitrateKbps;
        return ca < cb;
    };

    const bool sized = req.boxWidth > 1 && req.boxHeight > 1;
    int best = -1;
    if (sized) {
        for (int i : cand) {
            if (magnification(list[i], req) > 1.1)
                continue;          // smaller than the player: would be upscaled
            if (best < 0 || better(i, best))
                best = i;
        }
        if (best >= 0) {
            if (why)
                *why = QStringLiteral("плеер %1×%2: достаточно %3")
                           .arg(qRound(req.boxWidth)).arg(qRound(req.boxHeight)).arg(list[best].describe());
            return best;
        }
    }
    // No profile is as large as the player (or its size is unknown): the
    // largest picture, the lightest of equally large ones.
    for (int i : cand) {
        if (best < 0) {
            best = i;
            continue;
        }
        const double ai = double(list[i].width) * list[i].height;
        const double ab = double(list[best].width) * list[best].height;
        if (ai > ab * 1.05 || (ai > ab / 1.05 && better(i, best)))
            best = i;
    }
    if (why && best >= 0)
        *why = sized ? QStringLiteral("плеер %1×%2 больше всех потоков: самый крупный %3")
                           .arg(qRound(req.boxWidth)).arg(qRound(req.boxHeight)).arg(list[best].describe())
                     : QStringLiteral("размер плеера неизвестен");
    return best;
}

namespace {

class ProbeJob : public QRunnable
{
public:
    std::function<void()> fn;
    void run() override { fn(); }
};

QString codecOf(GstCaps *caps)
{
    if (!caps || gst_caps_is_empty(caps))
        return QString();
    const char *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    if (g_str_equal(name, "video/x-h264"))
        return QStringLiteral("h264");
    if (g_str_equal(name, "video/x-h265"))
        return QStringLiteral("h265");
    if (g_str_equal(name, "image/jpeg"))
        return QStringLiteral("jpeg");
    return QString::fromUtf8(name).section(QLatin1Char('/'), 1).remove(QStringLiteral("x-"));
}

} // namespace

void probe(const QString &url, const QString &user, const QString &password, QObject *context,
           std::function<void(const StreamProfile &, const QString &)> done, int timeoutSec)
{
    QPointer<QObject> ctx(context);
    auto *job = new ProbeJob;
    job->fn = [url, user, password, ctx, done, timeoutSec] {
        StreamProfile p;
        p.url = url;
        p.source = QStringLiteral("probe");
        QString error;
        QUrl u(url);
        if (!user.isEmpty() && u.userName().isEmpty()) {
            u.setUserName(user);
            u.setPassword(password);
        }
        GError *err = nullptr;
        GstDiscoverer *disc = gst_discoverer_new(GstClockTime(timeoutSec) * GST_SECOND, &err);
        if (!disc) {
            error = QStringLiteral("Нет GstDiscoverer: %1").arg(QString::fromUtf8(err ? err->message : ""));
            g_clear_error(&err);
        } else {
            GstDiscovererInfo *info =
                gst_discoverer_discover_uri(disc, u.toString(QUrl::FullyEncoded).toUtf8().constData(), &err);
            const GstDiscovererResult res = info ? gst_discoverer_info_get_result(info) : GST_DISCOVERER_ERROR;
            if (res == GST_DISCOVERER_TIMEOUT)
                error = QStringLiteral("Нет ответа за %1 с").arg(timeoutSec);
            else if (res == GST_DISCOVERER_URI_INVALID)
                error = QStringLiteral("Неверный адрес");
            else if (res == GST_DISCOVERER_ERROR) {
                error = QString::fromUtf8(err ? err->message : "ошибка");
                if (error == QLatin1String("Unhandled error"))
                    error = QStringLiteral("сервер отклонил запрос потока");
                else if (error.contains(QLatin1String("Could not open resource")))
                    error = QStringLiteral("не удалось подключиться");
            }
            if (info && (res == GST_DISCOVERER_OK || res == GST_DISCOVERER_MISSING_PLUGINS)) {
                GList *videos = gst_discoverer_info_get_video_streams(info);
                for (GList *l = videos; l; l = l->next) {
                    auto *vi = GST_DISCOVERER_VIDEO_INFO(l->data);
                    if (gst_discoverer_video_info_is_image(vi) && !p.known())
                        continue;
                    p.width = int(gst_discoverer_video_info_get_width(vi));
                    p.height = int(gst_discoverer_video_info_get_height(vi));
                    const guint fn = gst_discoverer_video_info_get_framerate_num(vi);
                    const guint fd = gst_discoverer_video_info_get_framerate_denom(vi);
                    p.fps = (fn > 0 && fd > 0) ? double(fn) / fd : 0;
                    const guint br = gst_discoverer_video_info_get_bitrate(vi);
                    const guint mbr = gst_discoverer_video_info_get_max_bitrate(vi);
                    p.bitrateKbps = int((br ? br : mbr) / 1000);
                    GstCaps *caps = gst_discoverer_stream_info_get_caps(GST_DISCOVERER_STREAM_INFO(vi));
                    p.codec = codecOf(caps);
                    if (caps)
                        gst_caps_unref(caps);
                    break;
                }
                gst_discoverer_stream_info_list_free(videos);
                GList *audios = gst_discoverer_info_get_audio_streams(info);
                p.audio = audios ? 1 : 0;
                gst_discoverer_stream_info_list_free(audios);
                if (!p.known() && error.isEmpty())
                    error = QStringLiteral("В потоке нет видео");
            }
            g_clear_error(&err);
            if (info)
                gst_discoverer_info_unref(info);
            g_object_unref(disc);
        }
        if (!ctx)
            return;
        QMetaObject::invokeMethod(ctx.data(), [ctx, done, p, error] {
            if (ctx)
                done(p, error);
        }, Qt::QueuedConnection);
    };
    // Own small pool: a probe may wait up to its timeout and must not hold
    // up the pipeline teardowns of the global pool.
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(2);
        return p;
    }();
    pool->start(job);
}

} // namespace profiles
