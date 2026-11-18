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

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#ifdef HMI_HAVE_WEBSOCKETS
#include <QWebSocket>
#endif

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
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
    bool adaptive = false;         // HLS / DASH / MSS manifest (not a live byte stream)
    bool forceSync = false;        // segment based: frames must be paced by the clock
    QString username;
    QString password;
    GstElement *pipeline = nullptr;
    GstElement *tail = nullptr;
    std::atomic<bool> linked{false};
    std::atomic<int> videoStream{-1};
    std::atomic<quint64> frames{0};      // decoded (tail input)
    std::atomic<quint64> inFrames{0};    // compressed frames into the decoder
    std::atomic<quint64> shown{0};       // frames reaching the video sink
    std::atomic<qint64> lastFrameUs{0};  // monotonic time of the last decoded frame
    std::atomic<bool> hasDecoderProbe{false};

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
    // QVideoFrame::WriteOnly exists in every Qt 6 release (deprecated in
    // newer ones in favour of QtVideo::MapMode, whose header varies).
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
    const bool mapped = frame.map(QVideoFrame::WriteOnly);
QT_WARNING_POP
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
            ctx->lastFrameUs.store(g_get_monotonic_time(), std::memory_order_relaxed);
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
                int w = 0, h = 0, pn = 1, pd = 1, fn = 0, fd = 1;
                if (s && gst_structure_get_int(s, "width", &w) && gst_structure_get_int(s, "height", &h)) {
                    gst_structure_get_fraction(s, "pixel-aspect-ratio", &pn, &pd);
                    gst_structure_get_fraction(s, "framerate", &fn, &fd);
                    Player *p = ctx->player;
                    const quint64 g = ctx->gen;
                    ctx->post([p, g, w, h, pn, pd, fn, fd] { p->onVideoSize(g, w, h, pn, pd, fn, fd); });
                }
            }
        }
        return GST_PAD_PROBE_OK;
    }

    static GstPadProbeReturn countProbe(GstPad *, GstPadProbeInfo *, gpointer data)
    {
        static_cast<std::atomic<quint64> *>(data)->fetch_add(1, std::memory_order_relaxed);
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
        if (codec == gsu::Codec::Jpeg && append("queue", true) && !chain.isEmpty()) {
            // Every JPEG is a key frame: if decoding falls behind, drop old
            // compressed frames instead of accumulating delay.
            GstElement *q = chain.last();
            gsu::setArg(q, "max-size-buffers", QStringLiteral("2"));
            gsu::setArg(q, "max-size-bytes", QStringLiteral("0"));
            gsu::setArg(q, "max-size-time", QStringLiteral("0"));
            gsu::setArg(q, "leaky", QStringLiteral("downstream"));
        }

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

        if (GstPad *dsink = gst_element_get_static_pad(decoder, "sink")) {
            // Frames received from the network (statistics / traffic light).
            gst_pad_add_probe(dsink, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::countProbe, &ctx->inFrames, nullptr);
            gst_object_unref(dsink);
            ctx->hasDecoderProbe = true;
        }
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

    // Streams we do not show (audio, metadata, extra video) go to a fakesink,
    // otherwise demuxers stop with "not-linked".
    static void linkFakeSink(PipelineCtx *ctx, GstPad *pad)
    {
        GstElement *sink = make("fakesink");
        if (!sink)
            return;
        gsu::setArg(sink, "sync", QStringLiteral("false"));
        gsu::setArg(sink, "async", QStringLiteral("false"));
        gst_bin_add(GST_BIN(ctx->pipeline), sink);
        gst_element_sync_state_with_parent(sink);
        GstPad *sp = gst_element_get_static_pad(sink, "sink");
        gst_pad_link(pad, sp);
        gst_object_unref(sp);
    }

    // Pads with parsed elementary streams: urisourcebin(parse-streams),
    // parsebin, qtdemux (MSE-LD). Decoder is chosen by us (HW first).
    static void parsedPadAdded(GstElement *, GstPad *pad, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps)
            caps = gst_pad_query_caps(pad, nullptr);
        const bool fixed = caps && !gst_caps_is_any(caps) && !gst_caps_is_empty(caps) && gst_caps_is_fixed(caps);
        if (!fixed) {
            // Adaptive streams (HLS/DASH/MSS) expose pads before the stream
            // type is known: decide when the caps arrive.
            if (caps)
                gst_caps_unref(caps);
            g_signal_connect(pad, "notify::caps", G_CALLBACK(&PipelineCallbacks::padCapsKnown), data);
            return;
        }
        const QString name = QString::fromUtf8(gst_structure_get_name(gst_caps_get_structure(caps, 0)));
        gst_caps_unref(caps);
        gsu::Codec codec = gsu::Codec::Unknown;
        if (name == QLatin1String("video/x-h264"))
            codec = gsu::Codec::H264;
        else if (name == QLatin1String("video/x-h265"))
            codec = gsu::Codec::H265;
        else if (name == QLatin1String("image/jpeg"))
            codec = gsu::Codec::Jpeg;
        const bool video = codec != gsu::Codec::Unknown || name.startsWith(QLatin1String("video/"));
        bool expected = false;
        if (!video || !ctx->linked.compare_exchange_strong(expected, true)) {
            linkFakeSink(ctx, pad);
            return;
        }
        if (codec == gsu::Codec::Unknown) {
            linkFakeSink(ctx, pad);
            ctx->postError(QStringLiteral("Неподдерживаемый кодек: %1").arg(name));
            return;
        }
        linkDecodeChain(ctx, pad, codec, false);
    }

    static void padCapsKnown(GstPad *pad, GParamSpec *, gpointer data)
    {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps)
            return;
        gst_caps_unref(caps);
        g_signal_handlers_disconnect_by_func(pad, gpointer(&PipelineCallbacks::padCapsKnown), data);
        parsedPadAdded(nullptr, pad, data);
    }

    // urisourcebin: credentials and live flags for the HTTP source.
    static void sourceSetup(GstElement *, GstElement *source, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        if (!ctx->username.isEmpty()) {
            gsu::setArg(source, "user-id", ctx->username);
            gsu::setArg(source, "user-pw", ctx->password);
        }
        if (!ctx->adaptive) {
            // Endless live byte stream (MPEG-TS, multipart MJPEG).
            gsu::setArg(source, "is-live", QStringLiteral("true"));
            gsu::setArg(source, "do-timestamp", QStringLiteral("true"));
        }
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
    m_snapshotTimer.setSingleShot(true);
    connect(&m_snapshotTimer, &QTimer::timeout, this, &Player::requestSnapshot);
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
    p->m_snapshotInterval = qBound(100, o.value(QLatin1String("snapshotInterval")).toInt(1000), 3600000);
    p->m_active = o.value(QLatin1String("active")).toBool(true);
    p->m_created = qint64(o.value(QLatin1String("created")).toDouble(double(p->m_created)));
    p->m_displayWidth = qBound(kMinWidth, o.value(QLatin1String("width")).toDouble(480), kMaxWidth);
    p->m_posX = o.value(QLatin1String("x")).toDouble();
    p->m_posY = o.value(QLatin1String("y")).toDouble();
    p->m_order = o.value(QLatin1String("order")).toInt();
    p->m_rotation = (((o.value(QLatin1String("rotation")).toInt(0) % 360) + 360) % 360 / 90) * 90;
    p->m_flipH = o.value(QLatin1String("flipH")).toBool(false);
    p->m_flipV = o.value(QLatin1String("flipV")).toBool(false);
    p->m_webUrl = o.value(QLatin1String("webUrl")).toString();
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
    o.insert(QLatin1String("snapshotInterval"), m_snapshotInterval);
    o.insert(QLatin1String("active"), m_active);
    o.insert(QLatin1String("created"), double(m_created));
    o.insert(QLatin1String("width"), m_displayWidth);
    o.insert(QLatin1String("x"), m_posX);
    o.insert(QLatin1String("y"), m_posY);
    o.insert(QLatin1String("order"), m_order);
    o.insert(QLatin1String("aspect"), videoAspect());
    o.insert(QLatin1String("rotation"), m_rotation);
    o.insert(QLatin1String("flipH"), m_flipH);
    o.insert(QLatin1String("flipV"), m_flipV);
    o.insert(QLatin1String("webUrl"), m_webUrl);
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

qreal Player::videoAspect() const
{
    if (m_videoWidth > 0 && m_videoHeight > 0)
        return qreal(m_videoWidth) / m_videoHeight;
    return m_savedAspect;
}

qreal Player::aspect() const
{
    const qreal a = videoAspect();
    return (m_rotation % 180 == 90) ? 1.0 / a : a;
}

void Player::setRotation(int r)
{
    r = ((r % 360) + 360) % 360;
    r = (r / 90) * 90;
    if (r == m_rotation)
        return;
    m_rotation = r;
    Q_EMIT orientationChanged();
    Q_EMIT geometryChanged();     // 90/270 swap the aspect ratio
    Q_EMIT persistentChanged();
}

void Player::setFlipH(bool f)
{
    if (f == m_flipH)
        return;
    m_flipH = f;
    Q_EMIT orientationChanged();
    Q_EMIT persistentChanged();
}

void Player::setFlipV(bool f)
{
    if (f == m_flipV)
        return;
    m_flipV = f;
    Q_EMIT orientationChanged();
    Q_EMIT persistentChanged();
}

void Player::rotateBy(int degrees)
{
    setRotation(m_rotation + degrees);
}

void Player::resetOrientation()
{
    setRotation(0);
    setFlipH(false);
    setFlipV(false);
}

void Player::setWebUrl(const QString &u)
{
    const QString v = u.trimmed();
    if (v == m_webUrl)
        return;
    m_webUrl = v;
    Q_EMIT configChanged();
    Q_EMIT persistentChanged();
}

QString Player::webPageUrl() const
{
    if (m_webUrl.isEmpty())
        return QString();
    if (m_webUrl.contains(QLatin1String("://")))
        return m_webUrl;
    return QStringLiteral("http://") + m_webUrl;
}

QString Player::streamHost() const
{
    const QUrl u = resolveUrl(m_url, nullptr);
    return u.host();
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

void Player::setLayoutRect(qreal x, qreal y, qreal w, qreal h)
{
    if (qFuzzyCompare(x + 1, m_layoutX + 1) && qFuzzyCompare(y + 1, m_layoutY + 1)
        && qFuzzyCompare(w + 1, m_layoutW + 1) && qFuzzyCompare(h + 1, m_layoutH + 1))
        return;
    m_layoutX = x;
    m_layoutY = y;
    m_layoutW = w;
    m_layoutH = h;
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
        {QStringLiteral("snapshotInterval"), m_snapshotInterval},
        {QStringLiteral("webUrl"), m_webUrl},
        {QStringLiteral("rotation"), m_rotation},
        {QStringLiteral("flipH"), m_flipH},
        {QStringLiteral("flipV"), m_flipV},
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
    m_snapshotInterval = qBound(100, c.value(QStringLiteral("snapshotInterval"), m_snapshotInterval).toInt(), 3600000);
    // Display-only settings: no stream restart needed.
    setWebUrl(c.value(QStringLiteral("webUrl"), m_webUrl).toString());
    setRotation(c.value(QStringLiteral("rotation"), m_rotation).toInt());
    setFlipH(c.value(QStringLiteral("flipH"), m_flipH).toBool());
    setFlipV(c.value(QStringLiteral("flipV"), m_flipV).toBool());

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
    updateHealth();
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
    const QString sync = (m_lowLatency && !ctx->forceSync) ? QStringLiteral("false") : QStringLiteral("true");

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
        if (GstPad *sp = gst_element_get_static_pad(sink, "sink")) {
            gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::countProbe, &ctx->shown, nullptr);
            gst_object_unref(sp);
        }
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

    Source kind = Source::Other;
    const QUrl url = resolveUrl(m_url, &kind);
    if (kind == Source::HttpStream) {
        const QString path = url.path().toLower();
        ctx->adaptive = path.endsWith(QLatin1String(".m3u8")) || path.endsWith(QLatin1String(".mpd"))
                        || path.contains(QLatin1String(".isml")) || path.contains(QLatin1String(".ism/"))
                        || path.endsWith(QLatin1String("/manifest"));
        // HLS/DASH/MSS deliver whole segments at once: without clock sync a
        // segment would be shown in a burst followed by a freeze.
        ctx->forceSync = ctx->adaptive;
    }

    QString err;
    GstElement *tail = buildTail(ctx, &err);
    if (!tail)
        return abort(err);
    gst_bin_add(GST_BIN(pipe), tail);
    ctx->tail = tail;

    m_sourceUrl = url;
    m_stallLimit = 10;
    ctx->username = m_username;
    ctx->password = m_password;

    switch (kind) {
    case Source::Rtsp: {
        GstElement *src = make("rtspsrc");
        if (!src)
            return abort(QStringLiteral("Нет элемента rtspsrc (gstreamer1.0-plugins-good)"));
        gsu::setArg(src, "location", url.toString(QUrl::FullyEncoded));
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
        break;
    }
    case Source::HttpStream: {
        // HLS, DASH, MSS, HTTP MPEG-TS, multipart MJPEG, fMP4 over HTTP.
        // The container is detected from the content; urisourcebin with
        // parse-streams (GStreamer >= 1.22) also drives the adaptive demuxers.
        const QString uri = url.toString(QUrl::FullyEncoded);
        GstElement *src = make("urisourcebin");
        if (src && gsu::hasProperty(src, "parse-streams")) {
            gsu::setArg(src, "uri", uri);
            gsu::setArg(src, "parse-streams", QStringLiteral("true"));
            g_signal_connect(src, "source-setup", G_CALLBACK(&PipelineCallbacks::sourceSetup), ctx);
            g_signal_connect(src, "pad-added", G_CALLBACK(&PipelineCallbacks::parsedPadAdded), ctx);
            gst_bin_add(GST_BIN(pipe), src);
        } else {
            if (src)
                gst_object_unref(src);
            GstElement *http = make("souphttpsrc");
            GstElement *parse = make("parsebin");
            if (!http || !parse) {
                if (http) gst_object_unref(http);
                if (parse) gst_object_unref(parse);
                return abort(QStringLiteral("Нет souphttpsrc/parsebin (gstreamer1.0-plugins-good/base)"));
            }
            gsu::setArg(http, "location", uri);
            PipelineCallbacks::sourceSetup(nullptr, http, ctx);
            gst_bin_add_many(GST_BIN(pipe), http, parse, nullptr);
            gst_element_link(http, parse);
            g_signal_connect(parse, "pad-added", G_CALLBACK(&PipelineCallbacks::parsedPadAdded), ctx);
        }
        if (ctx->adaptive)
            m_stallLimit = 20;     // segments arrive in bursts
        break;
    }
    case Source::Snapshot: {
        // JPEG snapshots (e.g. Flussonic preview.jpg) polled over HTTP and fed
        // into a persistent pipeline: no flicker between images.
        m_appsrc = makeAppSrc("image/jpeg", true);
        if (!m_appsrc)
            return abort(QStringLiteral("Нет элемента appsrc"));
        gst_bin_add(GST_BIN(pipe), m_appsrc);
        GstPad *pad = gst_element_get_static_pad(m_appsrc, "src");
        ctx->linked = true;
        const bool ok = PipelineCallbacks::linkDecodeChain(ctx, pad, gsu::Codec::Jpeg, false);
        gst_object_unref(pad);
        if (!ok) {
            m_appsrc = nullptr;
            return abort(QStringLiteral("Не найден декодер JPEG"));
        }
        m_stallLimit = qMax(10, 2 * m_snapshotInterval / 1000 + 5);
        break;
    }
    case Source::MseLd: {
#ifdef HMI_HAVE_WEBSOCKETS
        // Flussonic MSE-LD: fragmented MP4 over WebSocket -> qtdemux.
        m_appsrc = makeAppSrc("video/quicktime", false);
        GstElement *demux = make("qtdemux");
        if (!m_appsrc || !demux) {
            if (m_appsrc) gst_object_unref(m_appsrc);
            if (demux) gst_object_unref(demux);
            m_appsrc = nullptr;
            return abort(QStringLiteral("Нет appsrc/qtdemux (gstreamer1.0-plugins-good)"));
        }
        gst_bin_add_many(GST_BIN(pipe), m_appsrc, demux, nullptr);
        gst_element_link(m_appsrc, demux);
        g_signal_connect(demux, "pad-added", G_CALLBACK(&PipelineCallbacks::parsedPadAdded), ctx);
        break;
#else
        return abort(QStringLiteral("Программа собрана без Qt WebSockets (установите qt6-websockets-dev и пересоберите)"));
#endif
    }
    case Source::Test: {
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
        break;
    }
    case Source::Other: {
        // Files and anything else GStreamer can open (uses auto-plugged decoders).
        GstElement *src = make("uridecodebin");
        if (!src)
            return abort(QStringLiteral("Нет элемента uridecodebin"));
        gsu::setArg(src, "uri", url.toString(QUrl::FullyEncoded));
        g_signal_connect(src, "pad-added", G_CALLBACK(&PipelineCallbacks::uriPadAdded), ctx);
        gst_bin_add(GST_BIN(pipe), src);
        break;
    }
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

    if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        scheduleRetry(QStringLiteral("Не удалось запустить воспроизведение"));
        return;
    }
    if (kind == Source::Snapshot)
        requestSnapshot();
    else if (kind == Source::MseLd)
        startMseLd(url);
}

// ---------------------------------------------------------------------------
// URL handling and network-fed sources (snapshots, MSE-LD)
// ---------------------------------------------------------------------------
QUrl Player::resolveUrl(const QString &text, Source *kind)
{
    QUrl url = QUrl::fromUserInput(text.trimmed());
    QString scheme = url.scheme().toLower();
    // Flussonic server-to-server protocols are not available to players;
    // the same stream is taken from the equivalent public output:
    //   m4s://host/stream -> ws://host/stream/mse_ld  (fMP4, real time)
    //   m4f://host/stream -> http://host/stream/mpegts (MPEG-TS over HTTP)
    if (scheme == QLatin1String("m4s") || scheme == QLatin1String("m4ss")) {
        url.setScheme(scheme == QLatin1String("m4ss") ? QStringLiteral("wss") : QStringLiteral("ws"));
        QString path = url.path();
        while (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        url.setPath(path + QStringLiteral("/mse_ld"));
        scheme = url.scheme();
    } else if (scheme == QLatin1String("m4f") || scheme == QLatin1String("m4fs")) {
        url.setScheme(scheme == QLatin1String("m4fs") ? QStringLiteral("https") : QStringLiteral("http"));
        QString path = url.path();
        while (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        url.setPath(path + QStringLiteral("/mpegts"));
        scheme = url.scheme();
    }

    Source k = Source::Other;
    const QString path = url.path().toLower();
    if (scheme.startsWith(QLatin1String("rtsp")))
        k = Source::Rtsp;
    else if (scheme == QLatin1String("ws") || scheme == QLatin1String("wss"))
        k = Source::MseLd;
    else if (scheme == QLatin1String("test"))
        k = Source::Test;
    else if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        k = (path.endsWith(QLatin1String(".jpg")) || path.endsWith(QLatin1String(".jpeg")))
                ? Source::Snapshot : Source::HttpStream;
    if (kind)
        *kind = k;
    return url;
}

bool Player::webSocketsAvailable()
{
#ifdef HMI_HAVE_WEBSOCKETS
    return true;
#else
    return false;
#endif
}

QString Player::sourceType() const
{
    Source k = Source::Other;
    const QUrl url = resolveUrl(m_url, &k);
    const QString path = url.path().toLower();
    switch (k) {
    case Source::Rtsp: return QStringLiteral("RTSP");
    case Source::Snapshot: return QStringLiteral("JPEG");
    case Source::MseLd: return QStringLiteral("MSE-LD");
    case Source::Test: return QStringLiteral("TEST");
    case Source::HttpStream:
        if (path.endsWith(QLatin1String(".m3u8"))) return QStringLiteral("HLS");
        if (path.endsWith(QLatin1String(".mpd"))) return QStringLiteral("DASH");
        if (path.contains(QLatin1String(".ism"))) return QStringLiteral("MSS");
        if (path.endsWith(QLatin1String("mpegts")) || path.endsWith(QLatin1String(".ts")))
            return QStringLiteral("MPEG-TS");
        return QStringLiteral("HTTP");
    default: return QString();
    }
}

GstElement *Player::makeAppSrc(const char *caps, bool doTimestamp)
{
    GstElement *src = make("appsrc");
    if (!src)
        return nullptr;
    GstCaps *c = gst_caps_from_string(caps);
    gst_app_src_set_caps(GST_APP_SRC(src), c);
    gst_caps_unref(c);
    gst_app_src_set_stream_type(GST_APP_SRC(src), GST_APP_STREAM_TYPE_STREAM);
    gst_app_src_set_max_bytes(GST_APP_SRC(src), 16 * 1024 * 1024);
    gsu::setArg(src, "is-live", QStringLiteral("true"));
    gsu::setArg(src, "format", QStringLiteral("time"));
    gsu::setArg(src, "block", QStringLiteral("false"));
    gsu::setArg(src, "do-timestamp", doTimestamp ? QStringLiteral("true") : QStringLiteral("false"));
    return src;
}

void Player::pushToAppSrc(const QByteArray &data)
{
    if (!m_appsrc || data.isEmpty())
        return;
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, gsize(data.size()), nullptr);
    gst_buffer_fill(buf, 0, data.constData(), gsize(data.size()));
    gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), buf);   // takes ownership
}

void Player::stopNetworkSources()
{
    m_snapshotTimer.stop();
    if (m_snapReply) {
        m_snapReply->disconnect(this);
        m_snapReply->abort();
        m_snapReply->deleteLater();
        m_snapReply = nullptr;
    }
#ifdef HMI_HAVE_WEBSOCKETS
    if (m_ws) {
        m_ws->disconnect(this);
        m_ws->abort();
        m_ws->deleteLater();
        m_ws = nullptr;
    }
#endif
    m_mseTrack = -1;
    m_mseInit = false;
    m_appsrc = nullptr;
}

static void addBasicAuth(QNetworkRequest &req, const QString &user, const QString &pass)
{
    if (user.isEmpty())
        return;
    req.setRawHeader("Authorization", "Basic " + (user + QLatin1Char(':') + pass).toUtf8().toBase64());
}

void Player::requestSnapshot()
{
    if (!m_pipeline || !m_appsrc)
        return;
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    QNetworkRequest req(m_sourceUrl);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setTransferTimeout(15000);
    addBasicAuth(req, m_username, m_password);
    QNetworkReply *reply = m_nam->get(req);
    m_snapReply = reply;
    const quint64 gen = m_gen;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen] {
        reply->deleteLater();
        if (gen != m_gen || !m_pipeline)
            return;
        m_snapReply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            scheduleRetry(reply->errorString());
            return;
        }
        pushToAppSrc(reply->readAll());
        m_snapshotTimer.start(qMax(100, m_snapshotInterval));
    });
}

void Player::startMseLd(const QUrl &url)
{
#ifdef HMI_HAVE_WEBSOCKETS
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    const quint64 gen = m_gen;
    connect(m_ws, &QWebSocket::textMessageReceived, this, [this, gen](const QString &m) { onMseText(gen, m); });
    connect(m_ws, &QWebSocket::binaryMessageReceived, this, [this, gen](const QByteArray &d) { onMseBinary(gen, d); });
    connect(m_ws, &QWebSocket::disconnected, this, [this, gen] {
        if (gen == m_gen && m_pipeline)
            scheduleRetry(QStringLiteral("WebSocket закрыт") + (m_ws && !m_ws->closeReason().isEmpty()
                                                                   ? QStringLiteral(": ") + m_ws->closeReason()
                                                                   : QString()));
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_ws, &QWebSocket::errorOccurred, this, [this, gen](QAbstractSocket::SocketError) {
#else
    connect(m_ws, qOverload<QAbstractSocket::SocketError>(&QWebSocket::error), this, [this, gen](QAbstractSocket::SocketError) {
#endif
        if (gen == m_gen && m_pipeline)
            scheduleRetry(m_ws ? m_ws->errorString() : QStringLiteral("Ошибка WebSocket"));
    });
    QNetworkRequest req(url);
    addBasicAuth(req, m_username, m_password);
    m_ws->open(req);
#else
    Q_UNUSED(url)
#endif
}

void Player::onMseText(quint64 gen, const QString &msg)
{
#ifdef HMI_HAVE_WEBSOCKETS
    if (gen != m_gen || !m_pipeline)
        return;
    const QJsonObject o = QJsonDocument::fromJson(msg.toUtf8()).object();
    if (o.value(QLatin1String("type")).toString() != QLatin1String("mse_init_segment"))
        return;   // events, statistics
    if (m_mseInit) {
        // New initialisation (track change / server restart): start over.
        scheduleRetry(QStringLiteral("Параметры потока изменились"));
        return;
    }
    const QJsonArray tracks = o.value(QLatin1String("tracks")).toArray();
    QJsonObject video;
    for (const QJsonValue &v : tracks) {
        const QJsonObject t = v.toObject();
        const QString content = t.value(QLatin1String("content")).toString();
        if (content == QLatin1String("video") || (content.isEmpty() && video.isEmpty()))
            video = t;
        if (content == QLatin1String("video"))
            break;
    }
    if (video.isEmpty()) {
        scheduleRetry(QStringLiteral("В потоке MSE-LD нет видеодорожки"));
        return;
    }
    m_mseTrack = video.value(QLatin1String("id")).toInt(1);
    m_mseInit = true;
    pushToAppSrc(QByteArray::fromBase64(video.value(QLatin1String("payload")).toString().toLatin1()));
    m_ws->sendTextMessage(QStringLiteral("resume"));
#else
    Q_UNUSED(gen) Q_UNUSED(msg)
#endif
}

void Player::onMseBinary(quint64 gen, const QByteArray &data)
{
    if (gen != m_gen || !m_pipeline || !m_mseInit)
        return;
    // Media fragment (moof+mdat) of one track; byte 47 = tfhd track_id.
    if (data.size() > 47 && int(uchar(data.at(47))) == m_mseTrack)
        pushToAppSrc(data);
}

void Player::teardown(bool synchronous)
{
    m_watchdog.stop();
    stopNetworkSources();
    m_fps = m_inFps = m_shownFps = m_frameAge = 0;
    m_lastIn = m_lastShown = 0;
    m_statsUs = 0;
    m_qtShown = 0;
    Q_EMIT statsChanged();
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
    m_lastReconnectMs = QDateTime::currentMSecsSinceEpoch();
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
    // Rates use the real elapsed time: the GUI thread may be late when the
    // system is overloaded, a fixed 1 s period would overstate the rates.
    const qint64 nowUs = g_get_monotonic_time();
    const qreal dt = m_statsUs > 0 ? qMax(0.2, qreal(nowUs - m_statsUs) / 1e6) : 1.0;
    m_statsUs = nowUs;
    auto ema = [dt](qreal old, quint64 delta) {
        const qreal rate = qreal(delta) / dt;
        return old <= 0 ? rate : old * 0.5 + rate * 0.5;
    };
    const quint64 frames = m_ctx->frames.load(std::memory_order_relaxed);
    const quint64 delta = frames - m_lastFrames;
    m_lastFrames = frames;
    m_fps = ema(m_fps, delta);
    const quint64 in = m_ctx->hasDecoderProbe ? m_ctx->inFrames.load(std::memory_order_relaxed) : frames;
    m_inFps = ema(m_inFps, in - m_lastIn);
    m_lastIn = in;
    const quint64 shown = m_ctx->glSink ? m_ctx->shown.load(std::memory_order_relaxed)
                                        : m_qtShown.load(std::memory_order_relaxed);
    m_shownFps = ema(m_shownFps, shown - m_lastShown);
    m_lastShown = shown;
    const qint64 last = m_ctx->lastFrameUs.load(std::memory_order_relaxed);
    m_frameAge = last > 0 ? qreal(g_get_monotonic_time() - last) / 1e6 : 0;
    updateHealth();
    static const bool debugStats = qEnvironmentVariableIntValue("HMI_DEBUG_STATS") > 0;
    if (debugStats)
        qInfo("[%s] in %.1f dec %.1f shown %.1f render %.1f nominal %.1f age %.2f | %s", qPrintable(m_name),
              m_inFps, m_fps, m_shownFps, m_renderFps, m_nominalFps, m_frameAge, qPrintable(m_healthText));

    if (m_status == Playing) {
        m_stallTicks = delta == 0 ? m_stallTicks + 1 : 0;
        if (m_stallTicks >= m_stallLimit)
            scheduleRetry(QStringLiteral("Нет видеоданных более %1 с").arg(m_stallLimit));
    } else if (!m_gotFrame) {
        if (++m_connectTicks >= qMax(25, m_stallLimit))
            scheduleRetry(QStringLiteral("Таймаут подключения"));
    }
}

// Traffic light. Compares what the source delivers, what the decoder
// produces and what reaches the screen, so the operator sees where the
// problem is (network/camera, decoder, or display/GPU).
void Player::updateHealth()
{
    int h = HealthNone;
    QString t;
    auto f1 = [](qreal v) { return QString::number(v, 'f', v < 10 ? 1 : 0); };
    switch (m_status) {
    case Stopped:
    case Suspended:
        break;
    case Connecting:
        h = HealthWarning;
        t = QStringLiteral("Подключение к источнику…");
        break;
    case Reconnecting:
        h = HealthBad;
        t = QStringLiteral("Нет видеопотока");
        break;
    case Playing: {
        // Reference rate: what the stream declares, otherwise what arrives.
        const qreal ref = m_nominalFps > 0 ? m_nominalFps : qMax(m_inFps, m_fps);
        qreal shown = m_shownFps;
        if (m_renderFps >= 0 && m_renderFps < shown)
            shown = m_renderFps;      // the window repaints less often than frames arrive
        const qreal period = ref > 0.1 ? 1.0 / ref : 1.0;
        const qreal yellowAge = qMax(1.0, 2.5 * period);
        const qreal redAge = qMax(3.0, 6.0 * period);
        const bool recent = m_lastReconnectMs > 0
                            && QDateTime::currentMSecsSinceEpoch() - m_lastReconnectMs < 30000;
        h = HealthGood;
        t = QStringLiteral("Видеопоток в норме: %1 к/с").arg(f1(shown));
        if (m_frameAge >= redAge) {
            h = HealthBad;
            t = QStringLiteral("Нет новых кадров %1 с").arg(f1(m_frameAge));
        } else if (ref >= 2 && shown < 0.3 * ref) {
            h = HealthBad;
            t = m_fps >= 0.8 * m_inFps
                    ? QStringLiteral("Отображение не успевает: %1 из %2 к/с (перегружен GPU/вывод)").arg(f1(shown), f1(ref))
                    : QStringLiteral("Декодер не успевает: %1 из %2 к/с").arg(f1(m_fps), f1(m_inFps));
        } else if (ref >= 2 && shown < 0.8 * ref) {
            h = HealthWarning;
            t = m_fps < 0.8 * m_inFps
                    ? QStringLiteral("Декодер не успевает: %1 из %2 к/с").arg(f1(m_fps), f1(m_inFps))
                    : QStringLiteral("Отображение пропускает кадры: %1 из %2 к/с").arg(f1(shown), f1(ref));
        } else if (m_nominalFps > 0 && m_inFps < 0.6 * m_nominalFps) {
            h = HealthWarning;
            t = QStringLiteral("Источник присылает мало кадров: %1 из %2 к/с").arg(f1(m_inFps), f1(m_nominalFps));
        } else if (m_frameAge >= yellowAge) {
            h = HealthWarning;
            t = QStringLiteral("Перебои: кадра не было %1 с").arg(f1(m_frameAge));
        } else if (recent) {
            h = HealthWarning;
            t = QStringLiteral("Недавно было переподключение");
        } else if (m_hwFailed && m_decoder == DecoderAuto) {
            h = HealthWarning;
            t = QStringLiteral("Аппаратный декодер не справился, работает программный");
        }
        break;
    }
    }
    if (h != m_health || t != m_healthText) {
        m_health = h;
        m_healthText = t;
    }
    Q_EMIT statsChanged();
}

// Short Russian explanation for the most common GStreamer errors.
static QString explainError(const QString &msg)
{
    static const struct { const char *needle; const char *text; } map[] = {
        {"Unauthorized", "Ошибка авторизации: проверьте логин и пароль"},
        {"401", "Ошибка авторизации: проверьте логин и пароль"},
        {"403", "Доступ запрещён (403)"},
        {"Not Found", "Поток не найден (404): проверьте адрес"},
        {"404", "Поток не найден (404): проверьте адрес"},
        {"Could not open resource", "Не удалось подключиться к источнику"},
        {"Could not resolve", "Не удалось найти сервер по имени"},
        {"Could not read from resource", "Ошибка чтения из источника"},
        {"Internal data stream error", "Поток прерван"},
        {"Could not connect", "Не удалось подключиться к источнику"},
        {"timeout", "Таймаут сети"},
    };
    for (const auto &m : map)
        if (msg.contains(QLatin1String(m.needle), Qt::CaseInsensitive))
            return QString::fromUtf8(m.text);
    return msg;
}

void Player::onPipelineError(quint64 gen, const QString &msg)
{
    if (gen != m_gen || !m_pipeline)
        return;
    scheduleRetry(explainError(msg));
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

void Player::onVideoSize(quint64 gen, int w, int h, int parN, int parD, int fpsN, int fpsD)
{
    if (gen != m_gen || w <= 0 || h <= 0)
        return;
    m_nominalFps = (fpsN > 0 && fpsD > 0) ? qreal(fpsN) / fpsD : 0;
    if (parN > 0 && parD > 0 && parN != parD)
        w = int(qint64(w) * parN / parD);
    if (w == m_videoWidth && h == m_videoHeight)
        return;
    const qreal oldAspect = aspect();
    m_videoWidth = w;
    m_videoHeight = h;
    m_savedAspect = videoAspect();
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
    if (m_videoSink && f.isValid()) {
        m_videoSink->setVideoFrame(f);
        m_qtShown.fetch_add(1, std::memory_order_relaxed);
    }
}
