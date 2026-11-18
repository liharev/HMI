#include "player.h"
#include "gstutils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <atomic>
#include <cstring>
#include <functional>

namespace {
bool g_glRendering = false;
} // namespace

void Player::setGlRendering(bool on) { g_glRendering = on; }
bool Player::glRendering() { return g_glRendering; }

// ---------------------------------------------------------------------------
// Shared state between the GUI-thread Player and GStreamer streaming threads.
// Owned by the pipeline: deleted only after the pipeline reached NULL state.
// ---------------------------------------------------------------------------
struct PipelineCtx
{
    Player *player = nullptr;      // valid while alive == true
    quint64 gen = 0;               // generation, stale events are ignored
    QString name;
    gsu::DecoderPref pref = gsu::DecoderPref::Auto;
    bool glSink = false;
    bool convert = false;          // compatibility: CPU colour conversion
    GstElement *pipeline = nullptr;
    GstElement *tail = nullptr;
    std::atomic<bool> linked{false};
    std::atomic<int> videoStream{-1};
    std::atomic<quint64> frames{0};

    QMutex mutex;
    bool alive = true;

    void kill()
    {
        QMutexLocker l(&mutex);
        alive = false;
    }
    // Queue a functor to the GUI thread (dropped if the player is gone).
    template <typename F> void post(F f)
    {
        QMutexLocker l(&mutex);
        if (alive)
            QMetaObject::invokeMethod(player, std::move(f), Qt::QueuedConnection);
    }
    template <typename F> void withPlayer(F f)
    {
        QMutexLocker l(&mutex);
        if (alive)
            f(player);
    }
    void postError(const QString &msg)
    {
        Player *p = player;
        const quint64 g = gen;
        post([p, g, msg] { p->onPipelineError(g, msg); });
    }
};

namespace {

class FnJob : public QRunnable
{
public:
    explicit FnJob(std::function<void()> f) : m_f(std::move(f)) {}
    void run() override { m_f(); }
private:
    std::function<void()> m_f;
};

GstElement *make(const char *factory)
{
    return gst_element_factory_make(factory, nullptr);
}

QVideoFrameFormat::PixelFormat toQtFormat(GstVideoFormat f)
{
    switch (f) {
    case GST_VIDEO_FORMAT_NV12: return QVideoFrameFormat::Format_NV12;
    case GST_VIDEO_FORMAT_I420: return QVideoFrameFormat::Format_YUV420P;
    case GST_VIDEO_FORMAT_YV12: return QVideoFrameFormat::Format_YV12;
    case GST_VIDEO_FORMAT_Y42B: return QVideoFrameFormat::Format_YUV422P;
    case GST_VIDEO_FORMAT_YUY2: return QVideoFrameFormat::Format_YUYV;
    case GST_VIDEO_FORMAT_UYVY: return QVideoFrameFormat::Format_UYVY;
    case GST_VIDEO_FORMAT_BGRA: return QVideoFrameFormat::Format_BGRA8888;
    case GST_VIDEO_FORMAT_RGBA: return QVideoFrameFormat::Format_RGBA8888;
    case GST_VIDEO_FORMAT_BGRx: return QVideoFrameFormat::Format_BGRX8888;
    case GST_VIDEO_FORMAT_RGBx: return QVideoFrameFormat::Format_RGBX8888;
    default: return QVideoFrameFormat::Format_Invalid;
    }
}

constexpr const char *kAppSinkCaps =
    "video/x-raw, format=(string){ NV12, I420, YV12, Y42B, YUY2, UYVY, BGRA, RGBA, BGRx, RGBx }";

// Copies a decoded GStreamer sample into a QVideoFrame (fallback render path).
QVideoFrame sampleToFrame(GstSample *sample)
{
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buf = gst_sample_get_buffer(sample);
    if (!caps || !buf)
        return {};
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps))
        return {};
    const auto pf = toQtFormat(GST_VIDEO_INFO_FORMAT(&info));
    if (pf == QVideoFrameFormat::Format_Invalid)
        return {};

    QVideoFrameFormat fmt(QSize(GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info)), pf);
    switch (info.colorimetry.matrix) {
    case GST_VIDEO_COLOR_MATRIX_BT601: fmt.setColorSpace(QVideoFrameFormat::ColorSpace_BT601); break;
    case GST_VIDEO_COLOR_MATRIX_BT2020: fmt.setColorSpace(QVideoFrameFormat::ColorSpace_BT2020); break;
    default: fmt.setColorSpace(QVideoFrameFormat::ColorSpace_BT709); break;
    }
    fmt.setColorRange(info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255
                          ? QVideoFrameFormat::ColorRange_Full
                          : QVideoFrameFormat::ColorRange_Video);

    GstVideoFrame vf;
    if (!gst_video_frame_map(&vf, &info, buf, GST_MAP_READ))
        return {};

    QVideoFrame frame(fmt);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const bool mapped = frame.map(QtVideo::MapMode::WriteOnly);
#else
    const bool mapped = frame.map(QVideoFrame::WriteOnly);
#endif
    if (!mapped) {
        gst_video_frame_unmap(&vf);
        return {};
    }
    const int planes = qMin<int>(GST_VIDEO_FRAME_N_PLANES(&vf), frame.planeCount());
    for (int p = 0; p < planes; ++p) {
        gint comp[GST_VIDEO_MAX_COMPONENTS];
        gst_video_format_info_component(vf.info.finfo, p, comp);
        const int rows = GST_VIDEO_FRAME_COMP_HEIGHT(&vf, comp[0] >= 0 ? comp[0] : 0);
        const auto *src = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&vf, p));
        const int sstride = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, p);
        uchar *dst = frame.bits(p);
        const int dstride = frame.bytesPerLine(p);
        if (sstride == dstride) {
            std::memcpy(dst, src, size_t(sstride) * rows);
        } else {
            const int n = qMin(sstride, dstride);
            for (int r = 0; r < rows; ++r)
                std::memcpy(dst + size_t(r) * dstride, src + size_t(r) * sstride, n);
        }
    }
    frame.unmap();
    gst_video_frame_unmap(&vf);
    return frame;
}

} // namespace

// ---------------------------------------------------------------------------
// Static GStreamer callbacks (run in streaming threads).
// ---------------------------------------------------------------------------
struct PipelineCallbacks
{
    static GstBusSyncReply busSync(GstBus *, GstMessage *msg, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            const QString text = QString::fromUtf8(err ? err->message : "unknown error");
            qWarning("[%s] error from %s: %s (%s)", qPrintable(ctx->name),
                     GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), qPrintable(text), dbg ? dbg : "");
            g_clear_error(&err);
            g_free(dbg);
            ctx->postError(text);
            break;
        }
        case GST_MESSAGE_EOS:
            ctx->postError(QStringLiteral("Поток завершён сервером"));
            break;
        case GST_MESSAGE_WARNING: {
            GError *err = nullptr;
            gst_message_parse_warning(msg, &err, nullptr);
            qWarning("[%s] warning from %s: %s", qPrintable(ctx->name),
                     GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err ? err->message : "");
            g_clear_error(&err);
            break;
        }
        default:
            break;
        }
        // Nobody pops this bus: drop everything to avoid accumulation.
        return GST_BUS_DROP;
    }

    static GstPadProbeReturn tailProbe(GstPad *, GstPadProbeInfo *info, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
            if (ctx->frames.fetch_add(1, std::memory_order_relaxed) == 0) {
                Player *p = ctx->player;
                const quint64 g = ctx->gen;
                ctx->post([p, g] { p->onFirstFrame(g); });
            }
        } else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
            GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
            if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
                GstCaps *caps = nullptr;
                gst_event_parse_caps(ev, &caps);
                const GstStructure *s = caps ? gst_caps_get_structure(caps, 0) : nullptr;
                int w = 0, h = 0, pn = 1, pd = 1;
                if (s && gst_structure_get_int(s, "width", &w) && gst_structure_get_int(s, "height", &h)) {
                    gst_structure_get_fraction(s, "pixel-aspect-ratio", &pn, &pd);
                    Player *p = ctx->player;
                    const quint64 g = ctx->gen;
                    ctx->post([p, g, w, h, pn, pd] { p->onVideoSize(g, w, h, pn, pd); });
                }
            }
        }
        return GST_PAD_PROBE_OK;
    }

    static GstFlowReturn newSample(GstAppSink *sink, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (!sample)
            return GST_FLOW_OK;
        const QVideoFrame frame = sampleToFrame(sample);
        gst_sample_unref(sample);
        if (frame.isValid())
            ctx->withPlayer([&](Player *p) { p->pushFrame(frame); });
        return GST_FLOW_OK;
    }

    // depay ! parse ! decoder ! tail, created when the source exposes its pad.
    static bool linkDecodeChain(PipelineCtx *ctx, GstPad *srcPad, gsu::Codec codec, bool rtp)
    {
        GstBin *bin = GST_BIN(ctx->pipeline);
        QList<GstElement *> chain;   // upstream -> downstream (without decoder)
        auto fail = [&](const QString &msg) {
            for (GstElement *e : chain) {
                gst_element_set_state(e, GST_STATE_NULL);
                gst_bin_remove(bin, e);
            }
            ctx->postError(msg);
            return false;
        };
        auto append = [&](const char *factory, bool optional) -> bool {
            GstElement *e = make(factory);
            if (!e)
                return optional;
            gst_bin_add(bin, e);
            if (!chain.isEmpty() && !gst_element_link(chain.last(), e)) {
                gst_bin_remove(bin, e);
                return optional;
            }
            chain << e;
            return true;
        };

        const char *depay = nullptr;
        const char *parse = nullptr;
        switch (codec) {
        case gsu::Codec::H264: depay = "rtph264depay"; parse = "h264parse"; break;
        case gsu::Codec::H265: depay = "rtph265depay"; parse = "h265parse"; break;
        case gsu::Codec::Jpeg: depay = "rtpjpegdepay"; parse = "jpegparse"; break;
        default: return fail(QStringLiteral("Неподдерживаемый кодек"));
        }
        if (rtp && !append(depay, false))
            return fail(QStringLiteral("Нет элемента %1 (gst-plugins-good)").arg(depay));
        if (rtp) {
            gsu::setArg(chain.last(), "request-keyframe", QStringLiteral("true"));
            gsu::setArg(chain.last(), "wait-for-keyframe", QStringLiteral("true"));
        }
        // JPEG parser is optional, H.264/H.265 parsers are required.
        if (!append(parse, codec == gsu::Codec::Jpeg))
            return fail(QStringLiteral("Нет элемента %1 (gst-plugins-bad)").arg(parse));

        GstElement *decoder = nullptr;
        QString decoderFactory;
        for (const QString &f : gsu::decoderCandidates(codec, ctx->pref)) {
            GstElement *d = make(f.toUtf8().constData());
            if (!d)
                continue;
            if (f == QLatin1String("mppvideodec")) {
                // 10-bit HEVC -> NV12 via RGA so that GL upload can import it.
                gsu::setArg(d, "format", QStringLiteral("NV12"));
                gsu::setArg(d, "arm-afbc", QStringLiteral("false"));
            }
            gst_bin_add(bin, d);
            const bool ok = (chain.isEmpty() || gst_element_link(chain.last(), d))
                            && gst_element_link(d, ctx->tail);
            if (ok) {
                decoder = d;
                decoderFactory = f;
                break;
            }
            gst_bin_remove(bin, d);
        }
        if (!decoder)
            return fail(QStringLiteral("Не найден декодер %1").arg(gsu::codecName(codec)));

        gst_element_sync_state_with_parent(decoder);
        for (int i = chain.size() - 1; i >= 0; --i)
            gst_element_sync_state_with_parent(chain[i]);

        GstElement *first = chain.isEmpty() ? decoder : chain.first();
        GstPad *sinkPad = gst_element_get_static_pad(first, "sink");
        const GstPadLinkReturn lr = gst_pad_link(srcPad, sinkPad);
        gst_object_unref(sinkPad);
        if (GST_PAD_LINK_FAILED(lr)) {
            chain << decoder;
            return fail(QStringLiteral("Не удалось подключить поток %1").arg(gsu::codecName(codec)));
        }

        Player *p = ctx->player;
        const quint64 g = ctx->gen;
        const QString cname = gsu::codecName(codec);
        const bool hw = gsu::isHardwareDecoder(decoderFactory);
        ctx->post([p, g, cname, decoderFactory, hw] { p->onDecoderSelected(g, cname, decoderFactory, hw); });
        return true;
    }

    static gboolean rtspSelectStream(GstElement *, guint num, GstCaps *caps, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        const GstStructure *s = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        const gchar *media = s ? gst_structure_get_string(s, "media") : nullptr;
        if (!media || g_strcmp0(media, "video") != 0)
            return FALSE;      // skip audio / metadata streams entirely
        int expected = -1;
        if (ctx->videoStream.compare_exchange_strong(expected, int(num)))
            return TRUE;
        return expected == int(num);
    }

    static void rtspPadAdded(GstElement *, GstPad *pad, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps)
            caps = gst_pad_query_caps(pad, nullptr);
        const GstStructure *s = gst_caps_get_structure(caps, 0);
        const gchar *media = gst_structure_get_string(s, "media");
        const QString enc = QString::fromUtf8(gst_structure_get_string(s, "encoding-name"));
        const bool isVideo = media && g_strcmp0(media, "video") == 0;
        gst_caps_unref(caps);
        if (!isVideo)
            return;
        bool expected = false;
        if (!ctx->linked.compare_exchange_strong(expected, true))
            return;
        const gsu::Codec codec = gsu::codecFromRtpEncoding(enc);
        if (codec == gsu::Codec::Unknown) {
            ctx->postError(QStringLiteral("Неподдерживаемый кодек: %1").arg(enc));
            return;
        }
        linkDecodeChain(ctx, pad, codec, true);
    }

    static void multipartPadAdded(GstElement *, GstPad *pad, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps)
            caps = gst_pad_query_caps(pad, nullptr);
        const bool jpeg = gst_structure_has_name(gst_caps_get_structure(caps, 0), "image/jpeg");
        gst_caps_unref(caps);
        if (!jpeg)
            return;
        bool expected = false;
        if (!ctx->linked.compare_exchange_strong(expected, true))
            return;
        linkDecodeChain(ctx, pad, gsu::Codec::Jpeg, false);
    }

    static void uriPadAdded(GstElement *, GstPad *pad, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps)
            caps = gst_pad_query_caps(pad, nullptr);
        const bool video = g_str_has_prefix(gst_structure_get_name(gst_caps_get_structure(caps, 0)), "video/");
        gst_caps_unref(caps);
        if (!video)
            return;
        bool expected = false;
        if (!ctx->linked.compare_exchange_strong(expected, true))
            return;
        GstPad *sinkPad = gst_element_get_static_pad(ctx->tail, "sink");
        const GstPadLinkReturn lr = gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
        if (GST_PAD_LINK_FAILED(lr)) {
            ctx->postError(QStringLiteral("Не удалось подключить видеопоток"));
            return;
        }
        Player *p = ctx->player;
        const quint64 g = ctx->gen;
        ctx->post([p, g] { p->onDecoderSelected(g, QStringLiteral("auto"), QStringLiteral("decodebin"), false); });
    }
};

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------
Player::Player(QObject *parent)
    : QObject(parent)
    , m_uid(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_created(QDateTime::currentMSecsSinceEpoch())
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &Player::ensureRunning);
    m_watchdog.setInterval(1000);
    connect(&m_watchdog, &QTimer::timeout, this, &Player::onWatchdog);
}

Player::~Player()
{
    m_retryTimer.stop();
    teardown(false);
}

Player *Player::fromJson(const QJsonObject &o, QObject *parent)
{
    auto *p = new Player(parent);
    if (o.contains(QLatin1String("uid")))
        p->m_uid = o.value(QLatin1String("uid")).toString(p->m_uid);
    p->m_name = o.value(QLatin1String("name")).toString();
    p->m_url = o.value(QLatin1String("url")).toString();
    p->m_username = o.value(QLatin1String("user")).toString();
    p->m_password = o.value(QLatin1String("pass")).toString();
    p->m_transport = o.value(QLatin1String("transport")).toInt(TransportAuto);
    p->m_latency = o.value(QLatin1String("latency")).toInt(200);
    p->m_decoder = o.value(QLatin1String("decoder")).toInt(DecoderAuto);
    p->m_lowLatency = o.value(QLatin1String("lowLatency")).toBool(true);
    p->m_active = o.value(QLatin1String("active")).toBool(true);
    p->m_created = qint64(o.value(QLatin1String("created")).toDouble(double(p->m_created)));
    p->m_displayWidth = qBound(kMinWidth, o.value(QLatin1String("width")).toDouble(480), kMaxWidth);
    p->m_posX = o.value(QLatin1String("x")).toDouble();
    p->m_posY = o.value(QLatin1String("y")).toDouble();
    p->m_order = o.value(QLatin1String("order")).toInt();
    const double a = o.value(QLatin1String("aspect")).toDouble(0);
    if (a > 0.1 && a < 10)
        p->m_savedAspect = a;   // keeps tile sizes stable before the stream starts
    return p;
}

QJsonObject Player::toJson() const
{
    QJsonObject o;
    o.insert(QLatin1String("uid"), m_uid);
    o.insert(QLatin1String("name"), m_name);
    o.insert(QLatin1String("url"), m_url);
    o.insert(QLatin1String("user"), m_username);
    o.insert(QLatin1String("pass"), m_password);
    o.insert(QLatin1String("transport"), m_transport);
    o.insert(QLatin1String("latency"), m_latency);
    o.insert(QLatin1String("decoder"), m_decoder);
    o.insert(QLatin1String("lowLatency"), m_lowLatency);
    o.insert(QLatin1String("active"), m_active);
    o.insert(QLatin1String("created"), double(m_created));
    o.insert(QLatin1String("width"), m_displayWidth);
    o.insert(QLatin1String("x"), m_posX);
    o.insert(QLatin1String("y"), m_posY);
    o.insert(QLatin1String("order"), m_order);
    o.insert(QLatin1String("aspect"), aspect());
    return o;
}

void Player::setName(const QString &n)
{
    if (n == m_name)
        return;
    m_name = n;
    Q_EMIT configChanged();
    Q_EMIT persistentChanged();
}

qreal Player::aspect() const
{
    if (m_videoWidth > 0 && m_videoHeight > 0)
        return qreal(m_videoWidth) / m_videoHeight;
    return m_savedAspect;
}

void Player::setDisplayWidth(qreal w)
{
    w = qRound(qBound(kMinWidth, w, kMaxWidth));
    if (qFuzzyCompare(w, m_displayWidth))
        return;
    m_displayWidth = w;
    Q_EMIT geometryChanged();
    Q_EMIT persistentChanged();
}

void Player::setPosX(qreal x)
{
    x = qRound(qMax<qreal>(0, x));
    if (qFuzzyCompare(x + 1, m_posX + 1))
        return;
    m_posX = x;
    Q_EMIT posChanged();
    Q_EMIT persistentChanged();
}

void Player::setPosY(qreal y)
{
    y = qRound(qMax<qreal>(0, y));
    if (qFuzzyCompare(y + 1, m_posY + 1))
        return;
    m_posY = y;
    Q_EMIT posChanged();
    Q_EMIT persistentChanged();
}

void Player::setLayoutPos(qreal x, qreal y)
{
    if (qFuzzyCompare(x + 1, m_layoutX + 1) && qFuzzyCompare(y + 1, m_layoutY + 1))
        return;
    m_layoutX = x;
    m_layoutY = y;
    Q_EMIT layoutChanged();
}

void Player::setOrder(int o)
{
    if (o == m_order)
        return;
    m_order = o;
    Q_EMIT orderChanged();
    Q_EMIT persistentChanged();
}

QVariantMap Player::config() const
{
    return {
        {QStringLiteral("name"), m_name},
        {QStringLiteral("url"), m_url},
        {QStringLiteral("username"), m_username},
        {QStringLiteral("password"), m_password},
        {QStringLiteral("transport"), m_transport},
        {QStringLiteral("latency"), m_latency},
        {QStringLiteral("decoder"), m_decoder},
        {QStringLiteral("lowLatency"), m_lowLatency},
    };
}

void Player::applyConfig(const QVariantMap &c)
{
    const QString url = c.value(QStringLiteral("url"), m_url).toString().trimmed();
    const QString user = c.value(QStringLiteral("username"), m_username).toString();
    const QString pass = c.value(QStringLiteral("password"), m_password).toString();
    const int transport = c.value(QStringLiteral("transport"), m_transport).toInt();
    const int latency = qBound(0, c.value(QStringLiteral("latency"), m_latency).toInt(), 10000);
    const int decoder = c.value(QStringLiteral("decoder"), m_decoder).toInt();
    const bool lowLatency = c.value(QStringLiteral("lowLatency"), m_lowLatency).toBool();

    const bool streamChanged = url != m_url || user != m_username || pass != m_password
                               || transport != m_transport || latency != m_latency
                               || decoder != m_decoder || lowLatency != m_lowLatency;
    m_name = c.value(QStringLiteral("name"), m_name).toString().trimmed();
    if (m_name.isEmpty())
        m_name = QUrl(url).host().isEmpty() ? url : QUrl(url).host();
    m_url = url;
    m_username = user;
    m_password = pass;
    m_transport = transport;
    m_latency = latency;
    m_decoder = decoder;
    m_lowLatency = lowLatency;
    Q_EMIT configChanged();
    Q_EMIT persistentChanged();
    if (streamChanged)
        restart();
}

void Player::setActive(bool a)
{
    if (a == m_active)
        return;
    m_active = a;
    Q_EMIT activeChanged();
    Q_EMIT persistentChanged();
}

void Player::start()
{
    setActive(true);
    restart();
}

void Player::stop()
{
    setActive(false);
    m_retryTimer.stop();
    teardown();
    setStatus(Stopped);
}

void Player::restart()
{
    m_retry = 0;
    m_hwFailed = false;
    m_needConvert = false;
    m_retryTimer.stop();
    teardown();
    ensureRunning();
}

void Player::resetSize()
{
    setDisplayWidth(m_videoWidth > 0 ? m_videoWidth : 640);
}

void Player::setSuspended(bool s, bool synchronous)
{
    if (s == m_suspended)
        return;
    m_suspended = s;
    if (s) {
        m_retryTimer.stop();
        teardown(synchronous);
        if (m_active)
            setStatus(Suspended);
    } else {
        ensureRunning();
    }
}

bool Player::canRun() const
{
    return m_active && !m_suspended && m_sinkReady && !m_url.isEmpty();
}

void Player::ensureRunning()
{
    if (!m_active) {
        teardown();
        setStatus(Stopped);
        return;
    }
    if (m_suspended) {
        teardown();
        setStatus(Suspended);
        return;
    }
    if (m_url.isEmpty()) {
        setStatus(Stopped, QStringLiteral("Не задан адрес потока"));
        return;
    }
    if (!canRun()) {
        if (m_status != Reconnecting)
            setStatus(Connecting);
        return;
    }
    if (m_pipeline)
        return;
    if (m_teardowns > 0) {       // wait until the old pipeline released the sink
        m_startPending = true;
        return;
    }
    startPipeline();
}

void Player::setStatus(Status s, const QString &text)
{
    QString t = text;
    if (t.isEmpty()) {
        switch (s) {
        case Stopped: t = QStringLiteral("Остановлено"); break;
        case Connecting: t = QStringLiteral("Подключение…"); break;
        case Suspended: t = QStringLiteral("Приостановлено"); break;
        default: break;
        }
    }
    if (s == m_status && t == m_statusText)
        return;
    m_status = s;
    m_statusText = t;
    Q_EMIT statusChanged();
}

// ---------------------------------------------------------------------------
// Pipeline construction
// ---------------------------------------------------------------------------
GstElement *Player::buildTail(PipelineCtx *ctx, QString *error)
{
    GstElement *bin = gst_bin_new("tail");
    GstElement *queue = make("queue");
    // Keep at most 2 decoded frames: drop old ones instead of adding latency.
    gsu::setArg(queue, "max-size-buffers", QStringLiteral("2"));
    gsu::setArg(queue, "max-size-bytes", QStringLiteral("0"));
    gsu::setArg(queue, "max-size-time", QStringLiteral("0"));
    gsu::setArg(queue, "leaky", QStringLiteral("downstream"));
    const QString sync = m_lowLatency ? QStringLiteral("false") : QStringLiteral("true");

    QList<GstElement *> elems{queue};
    if (ctx->glSink && ctx->convert) {
        // Compatibility mode for unusual decoder output (e.g. 4:4:4, 12 bit).
        if (GstElement *vc = make("videoconvert")) {
            GstElement *f = make("capsfilter");
            GstCaps *c = gst_caps_from_string("video/x-raw,format={NV12,I420,RGBA}");
            g_object_set(f, "caps", c, nullptr);
            gst_caps_unref(c);
            gsu::setArg(vc, "n-threads", QString::number(qMin(4, QThread::idealThreadCount())));
            elems << vc << f;
        }
    }
    if (ctx->glSink) {
        GstElement *up = make("glupload");
        GstElement *conv = make("glcolorconvert");
        GstElement *rgba = make("capsfilter");
        GstElement *sink = make("qml6glsink");
        if (!up || !conv || !rgba || !sink) {
            *error = QStringLiteral("Нет GL-элементов GStreamer (gstreamer1.0-gl, gstreamer1.0-qt6)");
            for (GstElement *e : {queue, up, conv, rgba, sink})
                if (e)
                    gst_object_unref(e);
            gst_object_unref(bin);
            return nullptr;
        }
        // qml6glsink advertises RGB/YV12 too, but only RGBA is safe across
        // versions (1.22-1.24 abort on other formats). The YUV->RGBA
        // conversion is a single shader pass in glcolorconvert.
        GstCaps *c = gst_caps_from_string("video/x-raw(memory:GLMemory),format=RGBA,texture-target=2D");
        g_object_set(rgba, "caps", c, nullptr);
        gst_caps_unref(c);
        g_object_set(sink, "widget", static_cast<gpointer>(m_glItem.data()), nullptr);
        gsu::setArg(sink, "sync", sync);
        gsu::setArg(sink, "enable-last-sample", QStringLiteral("false"));
        elems << up << conv << rgba << sink;
    } else {
        GstElement *conv = make("videoconvert");     // passthrough for NV12/I420
        GstElement *sink = make("appsink");
        if (!conv || !sink) {
            *error = QStringLiteral("Нет элементов videoconvert/appsink (gstreamer1.0-plugins-base)");
            for (GstElement *e : {queue, conv, sink})
                if (e)
                    gst_object_unref(e);
            gst_object_unref(bin);
            return nullptr;
        }
        GstCaps *caps = gst_caps_from_string(kAppSinkCaps);
        gst_app_sink_set_caps(GST_APP_SINK(sink), caps);
        gst_caps_unref(caps);
        gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 1);
        gst_app_sink_set_drop(GST_APP_SINK(sink), TRUE);
        gsu::setArg(sink, "sync", sync);
        gsu::setArg(sink, "enable-last-sample", QStringLiteral("false"));
        GstAppSinkCallbacks cb;
        std::memset(&cb, 0, sizeof(cb));
        cb.new_sample = &PipelineCallbacks::newSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, ctx, nullptr);
        elems << conv << sink;
    }
    for (GstElement *e : elems)
        gst_bin_add(GST_BIN(bin), e);
    for (int i = 0; i + 1 < elems.size(); ++i) {
        if (!gst_element_link(elems[i], elems[i + 1])) {
            *error = QStringLiteral("Не удалось собрать цепочку отображения");
            gst_object_unref(bin);
            return nullptr;
        }
    }
    GstPad *qpad = gst_element_get_static_pad(queue, "sink");
    GstPad *ghost = gst_ghost_pad_new("sink", qpad);
    gst_object_unref(qpad);
    gst_pad_add_probe(ghost,
                      GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                      &PipelineCallbacks::tailProbe, ctx, nullptr);
    gst_element_add_pad(bin, ghost);
    return bin;
}

void Player::startPipeline()
{
    m_startPending = false;
    ++m_gen;
    auto *ctx = new PipelineCtx;
    ctx->player = this;
    ctx->gen = m_gen;
    ctx->name = m_name;
    ctx->glSink = g_glRendering && m_glItem;
    ctx->convert = m_needConvert;
    ctx->pref = (m_decoder == DecoderAuto && m_hwFailed) ? gsu::DecoderPref::Software
                                                         : gsu::DecoderPref(m_decoder);
    m_codec.clear();
    m_decoderName.clear();
    m_hwDecoding = false;
    Q_EMIT videoInfoChanged();

    GstElement *pipe = gst_pipeline_new(nullptr);
    ctx->pipeline = pipe;
    auto abort = [&](const QString &msg) {
        gst_object_unref(pipe);
        delete ctx;
        scheduleRetry(msg);
    };

    QString err;
    GstElement *tail = buildTail(ctx, &err);
    if (!tail)
        return abort(err);
    gst_bin_add(GST_BIN(pipe), tail);
    ctx->tail = tail;

    QUrl url = QUrl::fromUserInput(m_url);
    const QString scheme = url.scheme().toLower();
    if (scheme.startsWith(QLatin1String("rtsp"))) {
        GstElement *src = make("rtspsrc");
        if (!src)
            return abort(QStringLiteral("Нет элемента rtspsrc (gstreamer1.0-plugins-good)"));
        gsu::setArg(src, "location", m_url);
        gsu::setArg(src, "latency", QString::number(m_latency));
        gsu::setArg(src, "drop-on-latency", QStringLiteral("true"));
        gsu::setArg(src, "do-retransmission", QStringLiteral("false"));
        if (m_transport == TransportTcp)
            gsu::setArg(src, "protocols", QStringLiteral("tcp"));
        else if (m_transport == TransportUdp)
            gsu::setArg(src, "protocols", QStringLiteral("udp+udp-mcast"));
        if (!m_username.isEmpty()) {
            gsu::setArg(src, "user-id", m_username);
            gsu::setArg(src, "user-pw", m_password);
        }
        if (g_signal_lookup("select-stream", G_OBJECT_TYPE(src)))
            g_signal_connect(src, "select-stream", G_CALLBACK(&PipelineCallbacks::rtspSelectStream), ctx);
        g_signal_connect(src, "pad-added", G_CALLBACK(&PipelineCallbacks::rtspPadAdded), ctx);
        gst_bin_add(GST_BIN(pipe), src);
    } else if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
        // MJPEG over HTTP (multipart/x-mixed-replace), common on IP cameras.
        GstElement *src = make("souphttpsrc");
        GstElement *demux = make("multipartdemux");
        if (!src || !demux) {
            if (src) gst_object_unref(src);
            if (demux) gst_object_unref(demux);
            return abort(QStringLiteral("Нет souphttpsrc/multipartdemux (gstreamer1.0-plugins-good)"));
        }
        gsu::setArg(src, "location", m_url);
        gsu::setArg(src, "is-live", QStringLiteral("true"));
        gsu::setArg(src, "do-timestamp", QStringLiteral("true"));
        if (!m_username.isEmpty()) {
            gsu::setArg(src, "user-id", m_username);
            gsu::setArg(src, "user-pw", m_password);
        }
        gst_bin_add_many(GST_BIN(pipe), src, demux, nullptr);
        gst_element_link(src, demux);
        g_signal_connect(demux, "pad-added", G_CALLBACK(&PipelineCallbacks::multipartPadAdded), ctx);
    } else if (scheme == QLatin1String("test")) {
        // test://smpte?w=1280&h=720&fps=25  - synthetic source for diagnostics
        const QUrlQuery q(url);
        auto qi = [&](const char *k, int def) {
            bool ok = false;
            const int v = q.queryItemValue(QLatin1String(k)).toInt(&ok);
            return ok && v > 0 ? v : def;
        };
        GstElement *src = make("videotestsrc");
        GstElement *caps = make("capsfilter");
        if (!src || !caps)
            return abort(QStringLiteral("Нет элемента videotestsrc"));
        gsu::setArg(src, "is-live", QStringLiteral("true"));
        if (!url.host().isEmpty())
            gsu::setArg(src, "pattern", url.host());
        const QString capsStr = QStringLiteral("video/x-raw,format=NV12,width=%1,height=%2,framerate=%3/1")
                                    .arg(qi("w", 1280)).arg(qi("h", 720)).arg(qi("fps", 25));
        GstCaps *c = gst_caps_from_string(capsStr.toUtf8().constData());
        g_object_set(caps, "caps", c, nullptr);
        gst_caps_unref(c);
        gst_bin_add_many(GST_BIN(pipe), src, caps, nullptr);
        gst_element_link_many(src, caps, tail, nullptr);
        ctx->linked = true;
        m_codec = QStringLiteral("RAW");
        m_decoderName = QStringLiteral("videotestsrc");
        Q_EMIT videoInfoChanged();
    } else {
        // Files and anything else GStreamer can open (uses auto-plugged decoders).
        GstElement *src = make("uridecodebin");
        if (!src)
            return abort(QStringLiteral("Нет элемента uridecodebin"));
        gsu::setArg(src, "uri", url.toString(QUrl::FullyEncoded));
        g_signal_connect(src, "pad-added", G_CALLBACK(&PipelineCallbacks::uriPadAdded), ctx);
        gst_bin_add(GST_BIN(pipe), src);
    }

    GstBus *bus = gst_element_get_bus(pipe);
    gst_bus_set_sync_handler(bus, &PipelineCallbacks::busSync, ctx, nullptr);
    gst_object_unref(bus);

    m_pipeline = pipe;
    m_ctx = ctx;
    m_gotFrame = false;
    m_lastFrames = 0;
    m_stallTicks = 0;
    m_connectTicks = 0;
    setStatus(m_status == Reconnecting ? Reconnecting : Connecting,
              m_status == Reconnecting ? m_statusText : QString());
    m_watchdog.start();

    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        scheduleRetry(QStringLiteral("Не удалось запустить воспроизведение"));
}

void Player::teardown(bool synchronous)
{
    m_watchdog.stop();
    if (m_fps != 0) {
        m_fps = 0;
        Q_EMIT statsChanged();
    }
    if (!m_pipeline)
        return;
    ++m_gen;   // events already queued by the old pipeline become stale
    PipelineCtx *ctx = m_ctx;
    GstElement *pipe = m_pipeline;
    m_ctx = nullptr;
    m_pipeline = nullptr;
    ctx->kill();
    {
        QMutexLocker l(&m_frameMutex);
        m_latestFrame = QVideoFrame();
    }
    ++m_teardowns;
    QPointer<Player> self(this);
    // Stopping rtspsrc may block on network I/O, so it is done off the GUI thread.
    auto job = [pipe, ctx, self]() {
        gst_element_set_state(pipe, GST_STATE_NULL);
        GstBus *bus = gst_element_get_bus(pipe);
        gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
        gst_object_unref(bus);
        gst_object_unref(pipe);
        delete ctx;
        QMetaObject::invokeMethod(qApp, [self] { if (self) self->onTeardownDone(); }, Qt::QueuedConnection);
    };
    if (synchronous)
        job();
    else
        QThreadPool::globalInstance()->start(job);
}

void Player::onTeardownDone()
{
    if (--m_teardowns == 0 && m_startPending) {
        m_startPending = false;
        ensureRunning();
    }
}

void Player::scheduleRetry(const QString &reason)
{
    QString text = reason;
    int delay = qMin(30000, 1000 << qMin(m_retry, 5));
    const bool decoderStage = m_ctx && m_ctx->linked;
    if (m_decoder == DecoderAuto && m_hwDecoding && decoderStage && !m_gotFrame && !m_hwFailed) {
        m_hwFailed = true;     // next attempt uses a software decoder
        delay = 200;
        text += QStringLiteral(" — переход на программный декодер");
    } else if (decoderStage && !m_gotFrame && !m_needConvert && m_ctx->glSink) {
        m_needConvert = true;  // next attempt converts colours on the CPU
        delay = 200;
        text += QStringLiteral(" — режим совместимости");
    } else {
        ++m_retry;
    }
    teardown();
    if (!m_active || m_suspended) {
        ensureRunning();
        return;
    }
    setStatus(Reconnecting, QStringLiteral("%1\nПовтор через %2 с").arg(text).arg(qMax(1, delay / 1000)));
    m_retryTimer.start(delay);
}

void Player::onWatchdog()
{
    if (!m_ctx)
        return;
    const quint64 frames = m_ctx->frames.load(std::memory_order_relaxed);
    const quint64 delta = frames - m_lastFrames;
    m_lastFrames = frames;
    const qreal fps = m_fps <= 0 ? qreal(delta) : m_fps * 0.5 + qreal(delta) * 0.5;
    if (qAbs(fps - m_fps) > 0.05) {
        m_fps = fps;
        Q_EMIT statsChanged();
    }
    if (m_status == Playing) {
        m_stallTicks = delta == 0 ? m_stallTicks + 1 : 0;
        if (m_stallTicks >= 10)
            scheduleRetry(QStringLiteral("Нет видеоданных более 10 с"));
    } else if (!m_gotFrame) {
        if (++m_connectTicks >= 25)
            scheduleRetry(QStringLiteral("Таймаут подключения"));
    }
}

void Player::onPipelineError(quint64 gen, const QString &msg)
{
    if (gen != m_gen || !m_pipeline)
        return;
    scheduleRetry(msg);
}

void Player::onDecoderSelected(quint64 gen, const QString &codec, const QString &decoder, bool hw)
{
    if (gen != m_gen)
        return;
    m_codec = codec;
    m_decoderName = decoder;
    m_hwDecoding = hw;
    Q_EMIT videoInfoChanged();
}

void Player::onVideoSize(quint64 gen, int w, int h, int parN, int parD)
{
    if (gen != m_gen || w <= 0 || h <= 0)
        return;
    if (parN > 0 && parD > 0 && parN != parD)
        w = int(qint64(w) * parN / parD);
    if (w == m_videoWidth && h == m_videoHeight)
        return;
    const qreal oldAspect = aspect();
    m_videoWidth = w;
    m_videoHeight = h;
    m_savedAspect = aspect();
    Q_EMIT videoInfoChanged();
    if (!qFuzzyCompare(oldAspect, aspect())) {
        Q_EMIT geometryChanged();
        Q_EMIT persistentChanged();
    }
}

void Player::onFirstFrame(quint64 gen)
{
    if (gen != m_gen)
        return;
    m_gotFrame = true;
    m_retry = 0;
    setStatus(Playing, QString());
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------
void Player::clearSurface()
{
    if (m_surfaceConn)
        disconnect(m_surfaceConn);
    teardown();
    m_surface = nullptr;
    m_glItem = nullptr;
    m_videoSink = nullptr;
    m_sinkReady = false;
}

void Player::attachGlItem(QQuickItem *item)
{
    if (!item || m_surface == item)
        return;
    clearSurface();
    m_surface = item;
    m_glItem = item;
    m_surfaceConn = connect(item, &QObject::destroyed, this, [this] { clearSurface(); ensureRunning(); });
    scheduleGlReady();
    ensureRunning();
}

void Player::scheduleGlReady()
{
    QQuickItem *item = m_glItem;
    if (!item)
        return;
    QQuickWindow *w = item->window();
    if (!w) {
        connect(item, &QQuickItem::windowChanged, this, [this] { scheduleGlReady(); },
                Qt::SingleShotConnection);
        return;
    }
    // qml6glsink may only start once the item has its GL context, i.e. after
    // the window's scene graph was initialised. A render job runs exactly then.
    QPointer<Player> self(this);
    QPointer<QQuickItem> it(item);
    w->scheduleRenderJob(new FnJob([self, it] {
                             QMetaObject::invokeMethod(qApp, [self, it] {
                                 if (self && it && self->m_glItem == it) {
                                     self->m_sinkReady = true;
                                     self->ensureRunning();
                                 }
                             }, Qt::QueuedConnection);
                         }),
                         QQuickWindow::BeforeSynchronizingStage);
    w->update();
}

void Player::attachVideoSink(QVideoSink *sink)
{
    if (!sink || m_surface == sink)
        return;
    clearSurface();
    m_surface = sink;
    m_videoSink = sink;
    m_surfaceConn = connect(sink, &QObject::destroyed, this, [this] { clearSurface(); ensureRunning(); });
    m_sinkReady = true;
    ensureRunning();
}

void Player::detachSink(QObject *surface)
{
    if (surface && m_surface == surface) {
        clearSurface();
        ensureRunning();
    }
}

void Player::pushFrame(const QVideoFrame &frame)
{
    QMutexLocker l(&m_frameMutex);
    m_latestFrame = frame;
    if (m_framePosted)
        return;        // GUI thread has not consumed the previous frame: replace it
    m_framePosted = true;
    l.unlock();
    QMetaObject::invokeMethod(this, [this] { flushFrame(); }, Qt::QueuedConnection);
}

void Player::flushFrame()
{
    QVideoFrame f;
    {
        QMutexLocker l(&m_frameMutex);
        f = m_latestFrame;
        m_latestFrame = QVideoFrame();
        m_framePosted = false;
    }
    if (m_videoSink && f.isValid())
        m_videoSink->setVideoFrame(f);
}
