#include "player.h"
#include "camai.h"
#include "gstutils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonObject>
#include <QQuickItem>
#include <QRegularExpression>
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
#include <memory>

namespace {
bool g_glRendering = false;
int g_waitMs = 3000;
bool g_autoQuality = true;
double g_minFps = 10;
bool g_smooth = true;
int g_smoothMs = 500;
QString g_audioDevice;
QPointer<Player> g_audioPlayer;     // the only player with sound on
} // namespace

void Player::setGlRendering(bool on) { g_glRendering = on; }
bool Player::glRendering() { return g_glRendering; }
static QString g_renderNote;
void Player::setRenderNote(const QString &note) { g_renderNote = note; }
QString Player::renderNote() { return g_renderNote; }
void Player::setWaitTimeout(int ms) { g_waitMs = qBound(500, ms, 60000); }
int Player::waitTimeout() { return g_waitMs; }
void Player::setAutoQuality(bool on) { g_autoQuality = on; }
bool Player::autoQuality() { return g_autoQuality; }
void Player::setMinFps(double fps) { g_minFps = qBound(0.0, fps, 60.0); }
void Player::setSmoothDefault(bool on) { g_smooth = on; }
bool Player::smoothDefault() { return g_smooth; }
void Player::setSmoothBufferMs(int ms) { g_smoothMs = qBound(100, ms, 5000); }
int Player::smoothBufferMs() { return g_smoothMs; }

bool Player::smoothPlayback() const
{
    return m_playMode == PlayModeSmooth || (m_playMode == PlayModeDefault && g_smooth);
}

void Player::applyPlaybackSettings()
{
    if (m_playMode == PlayModeDefault && m_active && (m_ctx || m_pending))
        rebuildSeamless(QStringLiteral("изменены настройки воспроизведения"));
}
double Player::minFps() { return g_minFps; }

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
    bool smooth = false;           // frames shown by their timestamps after a jitter buffer
    // Presentation regularity (debug statistics): time between frames
    // handed to the screen and its mean absolute deviation, in microseconds.
    std::atomic<qint64> lastShowUs{0};
    std::atomic<qint64> intervalUs{0};
    std::atomic<qint64> jitterUs{0};
    void noteShown()
    {
        const qint64 now = g_get_monotonic_time();
        const qint64 last = lastShowUs.exchange(now, std::memory_order_relaxed);
        if (!last)
            return;
        const qint64 d = now - last;
        qint64 mean = intervalUs.load(std::memory_order_relaxed);
        mean = mean ? (mean * 15 + d) / 16 : d;
        intervalUs.store(mean, std::memory_order_relaxed);
        const qint64 dev = d > mean ? d - mean : mean - d;
        const qint64 j = jitterUs.load(std::memory_order_relaxed);
        jitterUs.store(j ? (j * 15 + dev) / 16 : dev, std::memory_order_relaxed);
    }
    QString username;
    QString password;
    GstElement *pipeline = nullptr;
    GstElement *tail = nullptr;
    std::atomic<bool> linked{false};
    std::atomic<int> videoStream{-1};
    // Sound: detected in the stream; played only when wanted (one player).
    bool wantAudio = false;
    QString audioDevice;               // display name of the output, "" = system default
    std::atomic<bool> hasAudio{false};
    std::atomic<int> audioStream{-1};
    std::atomic<bool> audioLinked{false};
    std::atomic<quint64> audioBuffers{0};
    GstElement *audioBin = nullptr;    // owned by the pipeline
    std::atomic<quint64> frames{0};      // decoded (tail input)
    std::atomic<quint64> inFrames{0};    // compressed frames into the decoder
    std::atomic<quint64> inBytes{0};     // compressed bytes into the decoder
    std::atomic<quint64> shown{0};       // frames reaching the video sink
    std::atomic<quint64> uploaded{0};    // frames after glupload (GL path)
    bool debug = false;
    std::atomic<qint64> lastFrameUs{0};  // monotonic time of the last decoded frame
    std::atomic<bool> hasDecoderProbe{false};

    QString url;                   // address played by this pipeline
    QString host;                  // host[:port] for status texts
    bool pending = false;          // prepared stream: output blocked until activation
    GstElement *sink = nullptr;    // video sink (owned by the pipeline)
    int surface = 0;               // index of the tile surface drawn on
    bool liveHttp = false;         // endless HTTP byte stream (MPEG-TS, MJPEG): cannot be paused
    // Hidden player with a live HTTP source: 1 = drop everything before the
    // decoder, 2 = drop until the next key frame (resuming), 0 = pass.
    std::atomic<int> dropMode{0};
    int stallLimit = 10;           // seconds without frames before reconnect
    // Stream information of a pending pipeline, applied on activation (GUI thread).
    QString codec, decoder;
    bool hw = false, hasDecoder = false, hasSize = false;
    int w = 0, h = 0, parN = 1, parD = 1, fpsN = 0, fpsD = 1;

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
            if (ctx->audioBin && gst_object_has_as_ancestor(GST_MESSAGE_SRC(msg), GST_OBJECT(ctx->audioBin))) {
                // The sound failed (no output device, codec...): the video goes on.
                const QString text = QString::fromUtf8(err ? err->message : "ошибка");
                qWarning("[%s] audio error from %s: %s (%s)", qPrintable(ctx->name),
                         GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), qPrintable(text), dbg ? dbg : "");
                g_clear_error(&err);
                g_free(dbg);
                Player *p = ctx->player;
                const quint64 g = ctx->gen;
                ctx->post([p, g, text] { p->onAudioError(g, text); });
                break;
            }
            const QString text = QString::fromUtf8(err ? err->message : "unknown error");
            qWarning("[%s] error from %s: %s (%s)", qPrintable(ctx->name),
                     GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), qPrintable(text), dbg ? dbg : "");
            // The details (e.g. the RTSP status "Service Unavailable (503)")
            // are often only in the debug part: both go to the explanation.
            const QString details = QString::fromUtf8(dbg ? dbg : "");
            g_clear_error(&err);
            g_free(dbg);
            ctx->postError(details.isEmpty() ? text : text + QLatin1Char('\n') + details);
            break;
        }
        case GST_MESSAGE_EOS:
            ctx->postError(QStringLiteral("Поток завершён сервером"));
            break;
        case GST_MESSAGE_PROGRESS: {
            // rtspsrc reports the steps of the RTSP exchange.
            GstProgressType type;
            gchar *code = nullptr;
            gchar *text = nullptr;
            gst_message_parse_progress(msg, &type, &code, &text);
            QString stage;
            const bool running = type == GST_PROGRESS_TYPE_START || type == GST_PROGRESS_TYPE_CONTINUE;
            if (running && !g_strcmp0(code, "connect"))
                stage = QStringLiteral("Подключение к %1…").arg(ctx->host);
            else if (running && !g_strcmp0(code, "open"))
                stage = QStringLiteral("%1: запрос описания потока…").arg(ctx->host);
            else if (type == GST_PROGRESS_TYPE_COMPLETE && !g_strcmp0(code, "open"))
                stage = QStringLiteral("%1: поток открыт, запрос воспроизведения…").arg(ctx->host);
            else if (running && !g_strcmp0(code, "request"))
                stage = QStringLiteral("%1: запрос воспроизведения…").arg(ctx->host);
            else if (type == GST_PROGRESS_TYPE_COMPLETE && !g_strcmp0(code, "request"))
                stage = QStringLiteral("%1: сервер начал передачу, ожидание видеоданных…").arg(ctx->host);
            g_free(code);
            g_free(text);
            if (!stage.isEmpty()) {
                Player *p = ctx->player;
                const quint64 g = ctx->gen;
                ctx->post([p, g, stage] { p->onProgress(g, stage); });
            }
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = nullptr;
            gst_message_parse_warning(msg, &err, nullptr);
            if (ctx->audioBin && gst_object_has_as_ancestor(GST_MESSAGE_SRC(msg), GST_OBJECT(ctx->audioBin))) {
                // e.g. autoaudiosink found no device and plays into a fake sink.
                const QString text = QStringLiteral("устройство вывода недоступно (%1)")
                                         .arg(QString::fromUtf8(err ? err->message : ""));
                Player *p = ctx->player;
                const quint64 g = ctx->gen;
                ctx->post([p, g, text] {
                    if ((p->isActive(g) || p->isPending(g)) && p->m_audioOn) {
                        p->m_audioNote = QStringLiteral("Звук не слышен: ") + text;
                        Q_EMIT p->audioChanged();
                    }
                });
            }
            qWarning("[%s] warning from %s: %s", qPrintable(ctx->name),
                     GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err ? err->message : "");
            g_clear_error(&err);
            break;
        }
        default:
            break;
        }
        // Nobody pops this bus: drop everything to avoid accumulation. A sync
        // handler returning GST_BUS_DROP must release the message itself;
        // without it every message (QoS ones arrive continuously) and the
        // element it refers to - up to whole stopped pipelines with their
        // decoder buffers - stayed in memory forever.
        // HMI_KEEP_BUS_MESSAGES=1: the old behaviour (messages and stopped
        // pipelines are never freed) - only to compare when diagnosing.
        static const bool keep = qEnvironmentVariableIsSet("HMI_KEEP_BUS_MESSAGES");
        if (!keep)
            gst_message_unref(msg);
        return GST_BUS_DROP;
    }

    static GstPadProbeReturn tailProbe(GstPad *, GstPadProbeInfo *info, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
            ctx->lastFrameUs.store(g_get_monotonic_time(), std::memory_order_relaxed);
            if (ctx->frames.fetch_add(1, std::memory_order_relaxed) == 0 && !ctx->pending) {
                Player *p = ctx->player;
                const quint64 g = ctx->gen;
                ctx->post([p, g] { p->onFirstFrame(g); });
            }
        } else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
            GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
            if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
                GstCaps *caps = nullptr;
                gst_event_parse_caps(ev, &caps);
                if (ctx->debug && caps) {
                    gchar *str = gst_caps_to_string(caps);
                    qInfo("[%s] decoder output caps: %s", qPrintable(ctx->name), str);
                    g_free(str);
                }
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

    // Developer aid: emulates a hardware decoder shared by all streams with a
    // total capacity of HMI_TEST_HW_FPS frames/s (used with HMI_TEST_FAKE_HW).
    static GstPadProbeReturn hwLimitProbe(GstPad *, GstPadProbeInfo *, gpointer)
    {
        static QMutex mutex;
        static qint64 nextUs = 0;
        static const qint64 periodUs = 1000000 / qMax(1, qEnvironmentVariableIntValue("HMI_TEST_HW_FPS"));
        qint64 slot;
        {
            QMutexLocker l(&mutex);
            const qint64 now = g_get_monotonic_time();
            slot = qMax(now, nextUs);
            nextUs = slot + periodUs;
        }
        const qint64 wait = slot - g_get_monotonic_time();
        if (wait > 0)
            g_usleep(gulong(wait));
        return GST_PAD_PROBE_OK;
    }

    static GstPadProbeReturn bytesProbe(GstPad *, GstPadProbeInfo *info, gpointer data)
    {
        if (GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info))
            static_cast<std::atomic<quint64> *>(data)->fetch_add(gst_buffer_get_size(b), std::memory_order_relaxed);
        return GST_PAD_PROBE_OK;
    }

    // Decoder input while the player is hidden (live HTTP sources).
    static GstPadProbeReturn dropProbe(GstPad *, GstPadProbeInfo *info, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        const int mode = ctx->dropMode.load(std::memory_order_relaxed);
        if (mode == 0)
            return GST_PAD_PROBE_OK;
        if (mode == 2) {
            GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info);
            if (b && !GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DELTA_UNIT)) {
                ctx->dropMode.store(0, std::memory_order_relaxed);   // key frame: decode again
                return GST_PAD_PROBE_OK;
            }
        }
        return GST_PAD_PROBE_DROP;
    }

    // Frames reaching the video sink. A prepared stream becomes the shown
    // one as soon as its first frame is on its (hidden) surface.
    static GstPadProbeReturn shownProbe(GstPad *, GstPadProbeInfo *, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        if (ctx->shown.fetch_add(1, std::memory_order_relaxed) == 0 && ctx->pending) {
            Player *p = ctx->player;
            const quint64 g = ctx->gen;
            ctx->post([p, g] { p->onFirstFrame(g); });
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
        ctx->noteShown();      // appsink hands the frame over at its display time
        const QVideoFrame frame = sampleToFrame(sample);
        gst_sample_unref(sample);
        if (frame.isValid())
            ctx->withPlayer([&](Player *p) { p->pushFrame(frame, ctx->surface, ctx->gen); });
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
        if (ctx->smooth && append("queue", true) && !chain.isEmpty()) {
            // Smooth playback: frames wait in the sink for their display time
            // (pipeline latency). The jitter buffer is kept here, on the
            // compressed stream (kilobytes per frame): decoded frames are
            // megabytes and the decoder has few output buffers, so waiting
            // there would stall the decoder and make the source drop data.
            GstElement *q = chain.last();
            gsu::setArg(q, "max-size-buffers", QStringLiteral("0"));
            gsu::setArg(q, "max-size-bytes", QStringLiteral("0"));
            gsu::setArg(q, "max-size-time", QString::number(qint64(qMax(2000, g_smoothMs * 3)) * 1000000));
        }
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

        // Developer aid (machines without a VPU): HMI_TEST_FAKE_HW treats the
        // decoder chosen from the hardware list as hardware.
        static const bool fakeHw = qEnvironmentVariableIsSet("HMI_TEST_FAKE_HW");
        const bool isHw = gsu::isHardwareDecoder(decoderFactory)
                          || (fakeHw && ctx->pref != gsu::DecoderPref::Software);
        if (GstPad *dsink = gst_element_get_static_pad(decoder, "sink")) {
            // Frames received from the network (statistics / traffic light).
            gst_pad_add_probe(dsink, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::countProbe, &ctx->inFrames, nullptr);
            gst_pad_add_probe(dsink, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::bytesProbe, &ctx->inBytes, nullptr);
            gst_pad_add_probe(dsink, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::dropProbe, ctx, nullptr);
            if ((isHw || qEnvironmentVariableIsSet("HMI_TEST_LIMIT_ALL")) && qEnvironmentVariableIsSet("HMI_TEST_HW_FPS"))
                gst_pad_add_probe(dsink, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::hwLimitProbe, nullptr, nullptr);
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
        const bool hw = isHw;
        ctx->post([p, g, cname, decoderFactory, hw] { p->onDecoderSelected(g, cname, decoderFactory, hw); });
        return true;
    }

    static gboolean rtspSelectStream(GstElement *, guint num, GstCaps *caps, gpointer data)
    {
        auto *ctx = static_cast<PipelineCtx *>(data);
        const GstStructure *s = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        const gchar *media = s ? gst_structure_get_string(s, "media") : nullptr;
        if (media && g_strcmp0(media, "audio") == 0) {
            noteAudio(ctx);
            if (!ctx->wantAudio)
                return FALSE;  // nobody listens: the sound is not even requested
            int first = -1;
            if (ctx->audioStream.compare_exchange_strong(first, int(num)))
                return TRUE;
            return first == int(num);
        }
        if (!media || g_strcmp0(media, "video") != 0)
            return FALSE;      // skip metadata streams entirely
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
        const bool isAudio = media && g_strcmp0(media, "audio") == 0;
        gst_caps_unref(caps);
        if (isAudio) {
            noteAudio(ctx);
            if (ctx->wantAudio)
                linkAudio(ctx, pad);
            return;
        }
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

    static void noteAudio(PipelineCtx *ctx)
    {
        bool was = false;
        if (ctx->hasAudio.compare_exchange_strong(was, true)) {
            Player *p = ctx->player;
            const quint64 g = ctx->gen;
            ctx->post([p, g] { p->onAudioDetected(g); });
        }
    }

    static GstElement *makeAudioSink(const QString &device)
    {
        // Developer aid / headless tests: HMI_AUDIO_SINK=fakesink
        if (qEnvironmentVariableIsSet("HMI_AUDIO_SINK"))
            return make(qgetenv("HMI_AUDIO_SINK").constData());
        if (!device.isEmpty()) {
            GstDeviceMonitor *mon = gst_device_monitor_new();
            gst_device_monitor_add_filter(mon, "Audio/Sink", nullptr);
            GstElement *sink = nullptr;
            if (gst_device_monitor_start(mon)) {
                GList *devs = gst_device_monitor_get_devices(mon);
                for (GList *l = devs; l && !sink; l = l->next) {
                    gchar *name = gst_device_get_display_name(GST_DEVICE(l->data));
                    if (device == QString::fromUtf8(name))
                        sink = gst_device_create_element(GST_DEVICE(l->data), nullptr);
                    g_free(name);
                }
                g_list_free_full(devs, gst_object_unref);
                gst_device_monitor_stop(mon);
            }
            gst_object_unref(mon);
            if (sink)
                return sink;
            qWarning("Audio output \"%s\" not found: using the system default", qPrintable(device));
        }
        return make("autoaudiosink");
    }

    static void audioDecodedPad(GstElement *, GstPad *pad, gpointer data)
    {
        auto *conv = static_cast<GstElement *>(data);
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps)
            caps = gst_pad_query_caps(pad, nullptr);
        const bool raw = caps && g_str_has_prefix(gst_structure_get_name(gst_caps_get_structure(caps, 0)), "audio/x-raw");
        if (caps)
            gst_caps_unref(caps);
        if (!raw)
            return;
        GstPad *sink = gst_element_get_static_pad(conv, "sink");
        if (!gst_pad_is_linked(sink))
            gst_pad_link(pad, sink);
        gst_object_unref(sink);
    }

    // RTP or parsed audio -> queue ! decodebin ! audioconvert ! audioresample ! output,
    // in its own bin: errors there switch the sound off, not the video.
    static bool linkAudio(PipelineCtx *ctx, GstPad *srcPad)
    {
        bool expected = false;
        if (!ctx->audioLinked.compare_exchange_strong(expected, true))
            return false;
        GstElement *queue = make("queue");
        GstElement *dec = make("decodebin");
        GstElement *conv = make("audioconvert");
        GstElement *res = make("audioresample");
        GstElement *sink = makeAudioSink(ctx->audioDevice);
        if (!queue || !dec || !conv || !res || !sink) {
            for (GstElement *e : {queue, dec, conv, res, sink})
                if (e)
                    gst_object_unref(e);
            Player *p = ctx->player;
            const quint64 g = ctx->gen;
            ctx->post([p, g] { p->onAudioError(g, QStringLiteral("нет элементов вывода звука (gstreamer1.0-pulseaudio/alsa)")); });
            return false;
        }
        // Old sound is dropped instead of delaying the stream.
        gsu::setArg(queue, "max-size-buffers", QStringLiteral("0"));
        gsu::setArg(queue, "max-size-bytes", QStringLiteral("0"));
        gsu::setArg(queue, "max-size-time", QStringLiteral("1000000000"));
        gsu::setArg(queue, "leaky", QStringLiteral("downstream"));
        GstElement *bin = gst_bin_new("audio");
        gst_bin_add_many(GST_BIN(bin), queue, dec, conv, res, sink, nullptr);
        gst_element_link(queue, dec);
        gst_element_link_many(conv, res, sink, nullptr);
        g_signal_connect(dec, "pad-added", G_CALLBACK(&PipelineCallbacks::audioDecodedPad), conv);
        if (GstPad *sp = gst_element_get_static_pad(sink, "sink")) {
            gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::countProbe, &ctx->audioBuffers, nullptr);
            gst_object_unref(sp);
        }
        GstPad *qpad = gst_element_get_static_pad(queue, "sink");
        gst_element_add_pad(bin, gst_ghost_pad_new("sink", qpad));
        gst_object_unref(qpad);
        ctx->audioBin = bin;
        gst_bin_add(GST_BIN(ctx->pipeline), bin);
        gst_element_sync_state_with_parent(bin);
        GstPad *binSink = gst_element_get_static_pad(bin, "sink");
        const bool ok = !GST_PAD_LINK_FAILED(gst_pad_link(srcPad, binSink));
        gst_object_unref(binSink);
        return ok;
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
        if (name.startsWith(QLatin1String("audio/"))) {
            noteAudio(ctx);
            if (!ctx->wantAudio || !linkAudio(ctx, pad))
                linkFakeSink(ctx, pad);
            return;
        }
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
    m_waitTimer.setSingleShot(true);
    connect(&m_waitTimer, &QTimer::timeout, this, [this] {
        m_waitExpired = true;
        Q_EMIT overlayChanged();
    });
    // Stream choice: evaluated 0.5 s after the size settled; a smaller
    // stream only if the player stays small for 5 s (no flapping, and the
    // camera does not restart encoders on every resize).
    m_selectTimer.setSingleShot(true);
    m_selectTimer.setInterval(500);
    connect(&m_selectTimer, &QTimer::timeout, this, [this] { evaluateProfile(false); });
    m_downgradeTimer.setSingleShot(true);
    m_downgradeTimer.setInterval(5000);
    connect(&m_downgradeTimer, &QTimer::timeout, this, [this] { evaluateProfile(true); });
    connect(this, &Player::orientationChanged, this, [this] { m_selectTimer.start(); });
    connect(this, &Player::streamUrlChanged, this, &Player::profilesChanged);   // "active" marks
    m_hideTimer.setSingleShot(true);
    connect(&m_hideTimer, &QTimer::timeout, this, [this] {
        if (!m_wantHidden || m_hidden || keepWhenHidden())
            return;
        m_hidden = true;
        Q_EMIT hiddenChanged();
        qInfo("[%s] not visible (%s): stream paused", qPrintable(m_name), qPrintable(m_hiddenReason));
        ensureRunning();
    });
    connect(this, &Player::switchFailed, this, [this](const QString &url, const QString &) {
        // Do not ask for this stream again for a minute; try the next best.
        m_failedUntil.insert(url, QDateTime::currentMSecsSinceEpoch() + 60000);
        QTimer::singleShot(61000, this, [this] { m_selectTimer.start(); });
        evaluateProfile(true);
    });
}

Player::~Player()
{
    if (g_audioPlayer == this)
        g_audioPlayer = nullptr;
    if (m_camai)
        m_camai->removeUser(this);
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
    // "lowLatency" of older versions was the default for every player: such
    // players follow the application setting now.
    p->m_playMode = qBound(0, o.value(QLatin1String("playMode")).toInt(PlayModeDefault), 2);
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
    p->m_output = qBound(0, o.value(QLatin1String("output")).toInt(OutputAuto), 3);
    // Fallbacks learned by versions before 2 were caused by an HLS/DASH bug
    // (leaky queue), not by the hardware: discard them.
    if (o.value(QLatin1String("autoVersion")).toInt(0) >= 2) {
        p->m_learned = qBound(1, o.value(QLatin1String("autoOutput")).toInt(OutputGl), 3);
        p->m_hwSlow = o.value(QLatin1String("autoSoftware")).toBool(false);
        p->m_balancedSw = o.value(QLatin1String("balancedSoftware")).toBool(false);
    }
    for (const QJsonValue &v : o.value(QLatin1String("profiles")).toArray()) {
        const StreamProfile sp = StreamProfile::fromJson(v.toObject());
        if (!sp.url.isEmpty() && profiles::indexOf(p->m_profiles, sp.url) < 0)
            p->m_profiles << sp;
    }
    p->m_profilePin = o.value(QLatin1String("profilePin")).toString();
    p->m_z = o.value(QLatin1String("z")).toInt(0);
    const QJsonArray span = o.value(QLatin1String("span")).toArray();
    if (span.size() == 2) {
        p->m_colSpan = qBound(1, span.at(0).toInt(1), 16);
        p->m_rowSpan = qBound(1, span.at(1).toInt(1), 16);
    }
    const QJsonObject camai = o.value(QLatin1String("camai")).toObject();
    p->m_camaiService = camai.value(QLatin1String("service")).toString();
    p->m_camaiChannel = camai.value(QLatin1String("channel")).toInt(-1);
    p->m_camaiRtspHost = camai.value(QLatin1String("rtspHost")).toString();
    p->bindCamai();
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
    o.insert(QLatin1String("playMode"), m_playMode);
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
    o.insert(QLatin1String("output"), m_output);
    o.insert(QLatin1String("autoOutput"), m_learned);
    o.insert(QLatin1String("autoSoftware"), m_hwSlow);
    o.insert(QLatin1String("balancedSoftware"), m_balancedSw);
    o.insert(QLatin1String("autoVersion"), 2);
    if (!m_profiles.isEmpty()) {
        QJsonArray arr;
        for (const StreamProfile &sp : m_profiles)
            arr.append(sp.toJson());
        o.insert(QLatin1String("profiles"), arr);
    }
    if (!m_profilePin.isEmpty())
        o.insert(QLatin1String("profilePin"), m_profilePin);
    if (m_z)
        o.insert(QLatin1String("z"), m_z);
    if (m_colSpan != 1 || m_rowSpan != 1)
        o.insert(QLatin1String("span"), QJsonArray{m_colSpan, m_rowSpan});
    if (!m_camaiService.isEmpty())
        o.insert(QLatin1String("camai"), QJsonObject{{QStringLiteral("service"), m_camaiService},
                                                     {QStringLiteral("channel"), m_camaiChannel},
                                                     {QStringLiteral("rtspHost"), m_camaiRtspHost}});
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

void Player::setSpan(int cols, int rows)
{
    cols = qBound(1, cols, 16);
    rows = qBound(1, rows, 16);
    if (cols == m_colSpan && rows == m_rowSpan)
        return;
    m_colSpan = cols;
    m_rowSpan = rows;
    Q_EMIT spanChanged();
    Q_EMIT persistentChanged();
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
        {QStringLiteral("playMode"), m_playMode},
        {QStringLiteral("snapshotInterval"), m_snapshotInterval},
        {QStringLiteral("webUrl"), m_webUrl},
        {QStringLiteral("output"), m_output},
        {QStringLiteral("rotation"), m_rotation},
        {QStringLiteral("flipH"), m_flipH},
        {QStringLiteral("flipV"), m_flipV},
        {QStringLiteral("profiles"), profilesVariant()},
        {QStringLiteral("profilePin"), m_profilePin},
        {QStringLiteral("camaiService"), m_camaiService},
        {QStringLiteral("camaiChannel"), m_camaiChannel},
        {QStringLiteral("camaiRtspHost"), m_camaiRtspHost},
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
    const int playMode = qBound(0, c.value(QStringLiteral("playMode"), m_playMode).toInt(), 2);
    m_snapshotInterval = qBound(100, c.value(QStringLiteral("snapshotInterval"), m_snapshotInterval).toInt(), 3600000);
    // Output path: changing it rebuilds the pipeline (and maybe the surface).
    const int output = qBound(0, c.value(QStringLiteral("output"), m_output).toInt(), 3);
    const bool outputChanged = output != m_output;
    if (outputChanged) {
        m_output = output;
        if (output == OutputAuto) {
            m_learned = OutputGl;      // choosing "Auto" again re-measures
            m_hwSlow = false;
        }
        m_fallbackNote.clear();
        Q_EMIT renderPathChanged();
    }
    // Display-only settings: no stream restart needed.
    setWebUrl(c.value(QStringLiteral("webUrl"), m_webUrl).toString());
    setRotation(c.value(QStringLiteral("rotation"), m_rotation).toInt());
    setFlipH(c.value(QStringLiteral("flipH"), m_flipH).toBool());
    setFlipV(c.value(QStringLiteral("flipV"), m_flipV).toBool());

    const bool urlChanged = url != m_url;
    bool profilesEdited = false;
    if (c.contains(QStringLiteral("profiles"))) {
        QList<StreamProfile> list;
        for (const QVariant &v : c.value(QStringLiteral("profiles")).toList()) {
            const StreamProfile sp = StreamProfile::fromVariant(v.toMap());
            if (!sp.url.isEmpty() && profiles::indexOf(list, sp.url) < 0)
                list << sp;
        }
        QStringList before, after;
        for (const StreamProfile &sp : m_profiles)
            before << QJsonDocument(sp.toJson()).toJson(QJsonDocument::Compact);
        for (const StreamProfile &sp : list)
            after << QJsonDocument(sp.toJson()).toJson(QJsonDocument::Compact);
        if (before != after) {
            m_profiles = list;
            m_failedUntil.clear();
            profilesEdited = true;
        }
    }
    if (c.contains(QStringLiteral("profilePin"))) {
        const QString pin = c.value(QStringLiteral("profilePin")).toString();
        if (pin != m_profilePin) {
            m_profilePin = pin;
            profilesEdited = true;
        }
    }
    if (profilesEdited)
        Q_EMIT profilesChanged();
    const QString camaiService = c.value(QStringLiteral("camaiService"), m_camaiService).toString().trimmed();
    const int camaiChannel = c.value(QStringLiteral("camaiChannel"), m_camaiChannel).toInt();
    const QString camaiHost = c.value(QStringLiteral("camaiRtspHost"), m_camaiRtspHost).toString().trimmed();
    const bool camaiChanged = camaiService != m_camaiService || camaiChannel != m_camaiChannel
                              || camaiHost != m_camaiRtspHost || user != m_username || pass != m_password;
    const bool otherStreamChanged = user != m_username || pass != m_password
                                    || transport != m_transport || latency != m_latency
                                    || decoder != m_decoder || playMode != m_playMode;
    m_name = c.value(QStringLiteral("name"), m_name).toString().trimmed();
    if (m_name.isEmpty())
        m_name = QUrl(url).host().isEmpty() ? url : QUrl(url).host();
    m_url = url;
    m_username = user;
    m_password = pass;
    m_transport = transport;
    m_latency = latency;
    if (decoder != m_decoder)
        m_hwSlow = false;          // a new decoder choice is measured again
    if (decoder != m_decoder)
        m_balancedSw = false;
    m_decoder = decoder;
    m_playMode = playMode;
    if (camaiChanged) {
        m_camaiService = camaiService;
        m_camaiChannel = camaiChannel;
        m_camaiRtspHost = camaiHost;
        bindCamai();
    }
    Q_EMIT configChanged();
    Q_EMIT persistentChanged();
    if (otherStreamChanged || outputChanged) {
        m_streamUrl = m_url;
        restart();
    } else if (urlChanged || profilesEdited) {
        // Only the address / the stream list changed: open the new stream
        // while the old one is still shown.
        if (m_profiles.isEmpty() && !m_active)
            m_streamUrl = m_url;
        else if (m_profiles.isEmpty())
            switchStream(m_url, QStringLiteral("изменён адрес потока"));
        else
            evaluateProfile(true);
    }
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
    endWait();
    m_lastError.clear();
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
        endWait();
        if (m_active)
            setStatus(Suspended);
    } else {
        ensureRunning();
    }
}

bool Player::canRun() const
{
    return m_active && !m_suspended && !m_hidden && m_surf[m_activeSurf].ready && !m_url.isEmpty();
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
    if (m_hidden) {
        m_retryTimer.stop();
        endWait();
        if (m_pending) {
            releasePipeline(m_pending, false);
            m_pending = nullptr;
            Q_EMIT overlayChanged();
        }
        // The pipeline is paused, not destroyed: no data from the network,
        // the decoder and the GPU idle, the last frame stays on the tile.
        // Destroying and rebuilding the GL output on every hide/show (tiles
        // covered while dragging, windows not shown) hung the Mali GPU driver.
        // A live HTTP byte stream cannot be paused (resuming it fails with
        // "server does not support seeking"): it keeps flowing, but nothing
        // reaches the decoder.
        Source kind = Source::Other;
        resolveUrl(m_streamUrl, &kind);
        const bool dropping = m_ctx && m_ctx->liveHttp && m_ctx->hasDecoderProbe;
        const bool pausable = m_ctx && (dropping || (kind != Source::Snapshot && kind != Source::MseLd));
        if (pausable && !m_paused) {
            m_watchdog.stop();
            if (dropping)
                m_ctx->dropMode.store(1);
            else
                gst_element_set_state(m_ctx->pipeline, GST_STATE_PAUSED);
            m_paused = true;
        } else if (!pausable) {
            teardown();
        }
        setStatus(Suspended, QStringLiteral("Не виден — поток приостановлен\n") + m_hiddenReason);
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
    Q_EMIT overlayChanged();
    updateHealth();
}

void Player::setStage(const QString &stage)
{
    m_stage = stage;
    if (m_status == Connecting)
        setStatus(Connecting, stage);
    else if (m_status == Reconnecting)
        setStatus(Reconnecting, m_lastError.isEmpty() ? stage : m_lastError + QLatin1Char('\n') + stage);
}

void Player::onProgress(quint64 gen, const QString &stage)
{
    if (isPending(gen)) {
        m_pendingStage = stage;
        Q_EMIT overlayChanged();
    } else if (isActive(gen) && !m_gotFrame) {
        setStage(stage);
    }
}

bool Player::isActive(quint64 gen) const { return m_ctx && m_ctx->gen == gen; }
bool Player::isPending(quint64 gen) const { return m_pending && m_pending->gen == gen; }

void Player::beginWait()
{
    if (m_waitExpired || m_waitTimer.isActive())
        return;
    m_waitTimer.start(g_waitMs);
}

void Player::endWait()
{
    m_waitTimer.stop();
    if (m_waitExpired) {
        m_waitExpired = false;
        Q_EMIT overlayChanged();
    }
}

bool Player::overlayVisible() const
{
    switch (m_status) {
    case Stopped:
    case Suspended:
        return true;
    case Connecting:
    case Reconnecting:
        return m_waitExpired;
    case Playing:
        return m_pending && m_waitExpired;
    }
    return false;
}

QString Player::overlayText() const
{
    if (m_status == Playing)
        return m_pending ? QStringLiteral("Переключение потока…\n") + m_pendingStage : QString();
    return m_statusText;
}

// ---------------------------------------------------------------------------
// Pipeline construction
// ---------------------------------------------------------------------------
GstElement *Player::buildTail(PipelineCtx *ctx, QString *error)
{
    GstElement *bin = gst_bin_new("tail");
    GstElement *queue = make("queue");
    gsu::setArg(queue, "max-size-bytes", QStringLiteral("0"));
    gsu::setArg(queue, "max-size-time", QStringLiteral("0"));
    if (ctx->forceSync) {
        // HLS/DASH/MSS: a segment is decoded much faster than real time and
        // must be shown paced by the clock. A leaky queue would throw away
        // almost the whole segment (seen as ~0.5 fps): block the decoder
        // instead (back-pressure), the demuxer buffers the rest.
        gsu::setArg(queue, "max-size-buffers", QStringLiteral("4"));
    } else if (ctx->smooth) {
        // Smooth: frames wait in the sink for their display time; the queue
        // must not throw them away meanwhile (that is what made motion
        // irregular), the jitter is absorbed by the pipeline latency.
        gsu::setArg(queue, "max-size-buffers", QStringLiteral("3"));
    } else {
        // Minimum latency: keep at most 2 decoded frames, drop old ones
        // instead of accumulating latency.
        gsu::setArg(queue, "max-size-buffers", QStringLiteral("2"));
        gsu::setArg(queue, "leaky", QStringLiteral("downstream"));
    }
    const QString sync = (ctx->smooth || ctx->forceSync) ? QStringLiteral("true") : QStringLiteral("false");

    QList<GstElement *> elems{queue};
    if (qEnvironmentVariableIsSet("HMI_TEST_SLOW_UPLOAD")) {
        // Developer aid: emulate a slow GPU upload to test the fallbacks.
        if (GstElement *slow = make("identity")) {
            gsu::setArg(slow, "sleep-time", qEnvironmentVariable("HMI_TEST_SLOW_UPLOAD"));
            elems << slow;
        }
    }
    if (ctx->glSink && ctx->convert) {
        // Compatibility mode for unusual decoder output (e.g. 4:4:4, 12 bit).
        if (GstElement *vc = make("videoconvert")) {
            GstElement *f = make("capsfilter");
            // HMI_GL_COPY=2: always a CPU conversion into a new buffer (NV12
            // from the decoder would pass through and the GPU would import
            // the decoder's DMA buffer) - diagnostics of the panthor fault.
            static const bool forceCopy = qEnvironmentVariableIntValue("HMI_GL_COPY") >= 2;
            GstCaps *c = gst_caps_from_string(forceCopy ? "video/x-raw,format=I420"
                                                        : "video/x-raw,format={NV12,I420,RGBA}");
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
        g_object_set(sink, "widget", static_cast<gpointer>(m_surf[ctx->surface].glItem.data()), nullptr);
        ctx->sink = sink;
        gsu::setArg(sink, "sync", sync);
        gsu::setArg(sink, "enable-last-sample", QStringLiteral("false"));
        if (GstPad *sp = gst_element_get_static_pad(sink, "sink")) {
            gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::shownProbe, ctx, nullptr);
            gst_object_unref(sp);
        }
        if (GstPad *cp = gst_element_get_static_pad(conv, "sink")) {
            gst_pad_add_probe(cp, GST_PAD_PROBE_TYPE_BUFFER, &PipelineCallbacks::countProbe, &ctx->uploaded, nullptr);
            gst_object_unref(cp);
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
        ctx->sink = sink;
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
    m_codec.clear();
    m_decoderName.clear();
    m_hwDecoding = false;
    Q_EMIT videoInfoChanged();
    {
        QString why;
        const QString chosen = m_profiles.isEmpty() ? (m_streamUrl.isEmpty() ? m_url : m_streamUrl) : chooseUrl(&why);
        if (chosen != m_streamUrl) {
            m_streamUrl = chosen;
            Q_EMIT streamUrlChanged();
        }
        if (!m_profiles.isEmpty()) {
            qInfo("[%s] stream %s (%s)", qPrintable(m_name), qPrintable(chosen), qPrintable(why));
            probeUnknownProfiles();
        }
    }

    QString err;
    PipelineCtx *ctx = buildPipeline(m_streamUrl, false, &err);
    if (!ctx) {
        scheduleRetry(err);
        return;
    }
    Source kind = Source::Other;
    m_sourceUrl = resolveUrl(m_streamUrl, &kind);
    m_pipeline = ctx->pipeline;
    m_ctx = ctx;
    m_gotFrame = false;
    m_lastFrames = 0;
    m_stallTicks = 0;
    m_connectTicks = 0;
    m_stallLimit = ctx->stallLimit;
    m_stage = QStringLiteral("Подключение к %1…").arg(ctx->host);
    if (m_status == Reconnecting)
        setStage(m_stage);
    else
        setStatus(Connecting, m_stage);
    beginWait();
    m_watchdog.start();

    if (gst_element_set_state(ctx->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        scheduleRetry(QStringLiteral("Не удалось запустить воспроизведение"));
        return;
    }
    if (kind == Source::Snapshot)
        requestSnapshot();
    else if (kind == Source::MseLd)
        startMseLd(m_sourceUrl);
}

PipelineCtx *Player::buildPipeline(const QString &urlText, bool pending, QString *error)
{
    auto *ctx = new PipelineCtx;
    ctx->player = this;
    ctx->gen = ++m_gen;
    ctx->name = m_name;
    ctx->url = urlText;
    ctx->pending = pending;
    // A prepared stream draws on the hidden surface of the tile.
    ctx->surface = pending ? 1 - m_activeSurf : m_activeSurf;
    ctx->glSink = g_glRendering && m_surf[ctx->surface].glItem;
    ctx->convert = m_needConvert || copyMode()
                   || qEnvironmentVariableIntValue("HMI_GL_COPY") > 0;
    ctx->debug = qEnvironmentVariableIntValue("HMI_DEBUG_STATS") > 0;
    ctx->pref = (m_decoder == DecoderAuto && (m_hwFailed || m_hwSlow || m_balancedSw)) ? gsu::DecoderPref::Software
                                                         : gsu::DecoderPref(m_decoder);
    ctx->username = m_username;
    ctx->password = m_password;
    ctx->wantAudio = m_audioOn;
    ctx->audioDevice = g_audioDevice;

    GstElement *pipe = gst_pipeline_new(nullptr);
    ctx->pipeline = pipe;
    auto abort = [&](const QString &msg) -> PipelineCtx * {
        gst_object_unref(pipe);
        delete ctx;
        *error = msg;
        return nullptr;
    };

    Source kind = Source::Other;
    const QUrl url = resolveUrl(urlText, &kind);
    ctx->host = url.host().isEmpty() ? url.toString(QUrl::RemoveUserInfo)
                                     : url.host() + (url.port() > 0 ? QStringLiteral(":%1").arg(url.port()) : QString());
    if (pending && (kind == Source::Snapshot || kind == Source::MseLd))
        return abort(QStringLiteral("Источник не поддерживает подготовку потока"));
    if (kind == Source::HttpStream) {
        const QString path = url.path().toLower();
        ctx->adaptive = path.endsWith(QLatin1String(".m3u8")) || path.endsWith(QLatin1String(".mpd"))
                        || path.contains(QLatin1String(".isml")) || path.contains(QLatin1String(".ism/"))
                        || path.endsWith(QLatin1String("/manifest"));
        // HLS/DASH/MSS deliver whole segments at once: without clock sync a
        // segment would be shown in a burst followed by a freeze.
        ctx->forceSync = ctx->adaptive;
        ctx->liveHttp = !ctx->adaptive;
    }
    // Snapshots (one JPEG per interval) are shown when they arrive.
    ctx->smooth = smoothPlayback() && kind != Source::Snapshot && !ctx->forceSync;
    if (ctx->smooth) {
        // Frames are displayed this long after capture: network and decoder
        // irregularities within it do not reach the screen.
        const int ms = qMax(g_smoothMs, kind == Source::Rtsp ? m_latency + 100 : 0);
        gst_pipeline_set_latency(GST_PIPELINE(pipe), GstClockTime(ms) * GST_MSECOND);
    }

    QString err;
    GstElement *tail = buildTail(ctx, &err);
    if (!tail)
        return abort(err);
    gst_bin_add(GST_BIN(pipe), tail);
    ctx->tail = tail;

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
            ctx->stallLimit = 20;     // segments arrive in bursts
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
        ctx->stallLimit = qMax(10, 2 * m_snapshotInterval / 1000 + 5);
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
        ctx->codec = QStringLiteral("RAW");
        ctx->decoder = QStringLiteral("videotestsrc");
        ctx->hasDecoder = true;
        if (!pending)
            applyDecoderInfo(ctx->codec, ctx->decoder, false);
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
    return ctx;
}

// ---------------------------------------------------------------------------
// Seamless stream switching
// ---------------------------------------------------------------------------
void Player::switchStream(const QString &url, const QString &reason, bool force)
{
    const QString target = url.trimmed().isEmpty() ? m_url : url.trimmed();
    if (m_pending) {
        if (m_pending->url == target && !force)
            return;                               // already being prepared
        releasePipeline(m_pending, false);        // only the last request counts
        m_pending = nullptr;
    }
    if (target == m_streamUrl && m_ctx && !force) {
        Q_EMIT overlayChanged();
        return;
    }
    Source kind = Source::Other;
    resolveUrl(target, &kind);
    Source curKind = Source::Other;
    resolveUrl(m_streamUrl, &curKind);
    const bool shown = m_ctx && m_status == Playing && m_gotFrame;
    const Surface &spare = m_surf[1 - m_activeSurf];
    const bool sameKind = bool(spare.glItem) == bool(m_surf[m_activeSurf].glItem);
    const bool seamless = shown && canRun() && spare.ready && sameKind && kind != Source::Snapshot && kind != Source::MseLd
                          && curKind != Source::Snapshot && curKind != Source::MseLd;
    if (!seamless) {
        // Nothing live on the screen: switch directly (the last frame stays).
        m_streamUrl = target;
        Q_EMIT streamUrlChanged();
        m_retry = 0;
        m_retryTimer.stop();
        teardown();
        ensureRunning();
        return;
    }
    QString err;
    PipelineCtx *ctx = buildPipeline(target, true, &err);
    if (!ctx) {
        Q_EMIT switchFailed(target, err);
        return;
    }
    m_pending = ctx;
    m_pendingTicks = 0;
    m_pendingReason = reason;
    m_pendingStage = QStringLiteral("Подключение к %1…").arg(ctx->host);
    if (!reason.isEmpty())
        qInfo("[%s] switching to %s: %s", qPrintable(m_name), qPrintable(target), qPrintable(reason));
    beginWait();
    Q_EMIT overlayChanged();
    if (gst_element_set_state(ctx->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        failPending(QStringLiteral("Не удалось запустить воспроизведение"));
}

// The prepared stream delivered its first frame (or the shown one failed):
// it becomes the active one; the old pipeline stops drawing first, then the
// new output is released, so the surface always shows a picture.
void Player::activatePending()
{
    PipelineCtx *ctx = m_pending;
    if (!ctx)
        return;
    m_pending = nullptr;
    const bool ready = ctx->frames.load(std::memory_order_relaxed) > 0;
    PipelineCtx *old = m_ctx;
    if (old) {
        stopNetworkSources();
        m_ctx = nullptr;
        m_pipeline = nullptr;
    }
    m_ctx = ctx;
    m_pipeline = ctx->pipeline;
    if (m_streamUrl != ctx->url)
        m_streamHasAudio = false;
    m_streamUrl = ctx->url;
    if (ctx->hasAudio && !m_streamHasAudio) {
        m_streamHasAudio = true;
        Q_EMIT audioChanged();
    }
    if (m_activeSurf != ctx->surface) {
        m_activeSurf = ctx->surface;     // the tile shows the other surface now
        Q_EMIT surfaceIndexChanged();
    }
    m_sourceUrl = resolveUrl(ctx->url, nullptr);
    m_stallLimit = ctx->stallLimit;
    resetStats();
    m_lastFrames = ctx->frames.load(std::memory_order_relaxed);
    m_lastIn = ctx->hasDecoderProbe ? ctx->inFrames.load(std::memory_order_relaxed) : m_lastFrames;
    m_lastBytes = ctx->inBytes.load(std::memory_order_relaxed);
    m_stallTicks = 0;
    m_connectTicks = 0;
    m_gotFrame = ready;
    if (ctx->hasDecoder)
        applyDecoderInfo(ctx->codec, ctx->decoder, ctx->hw);
    else {
        m_codec.clear();
        m_decoderName.clear();
        m_hwDecoding = false;
        Q_EMIT videoInfoChanged();
    }
    if (ctx->hasSize)
        applyVideoSize(ctx->w, ctx->h, ctx->parN, ctx->parD, ctx->fpsN, ctx->fpsD);
    Q_EMIT streamUrlChanged();
    if (ready) {
        m_retry = 0;
        m_lastError.clear();
        endWait();
        setStatus(Playing, QString());
        qInfo("[%s] now playing %s", qPrintable(m_name), qPrintable(ctx->url));
        Q_EMIT switchDone(ctx->url);
    } else {
        m_stage = m_pendingStage;
        setStatus(m_status == Reconnecting ? Reconnecting : Connecting, m_stage);
        beginWait();
    }
    m_watchdog.start();
    Q_EMIT overlayChanged();

    if (old)
        releasePipeline(old, false);
}

void Player::failPending(const QString &reason)
{
    if (!m_pending)
        return;
    const QString url = m_pending->url;
    releasePipeline(m_pending, false);
    m_pending = nullptr;
    qInfo("[%s] switch to %s failed: %s", qPrintable(m_name), qPrintable(url), qPrintable(reason));
    if (m_status == Playing)
        endWait();
    Q_EMIT overlayChanged();
    Q_EMIT switchFailed(url, reason);
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
    const quint64 gen = m_ctx ? m_ctx->gen : 0;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen] {
        reply->deleteLater();
        if (!isActive(gen))
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
    const quint64 gen = m_ctx ? m_ctx->gen : 0;
    connect(m_ws, &QWebSocket::textMessageReceived, this, [this, gen](const QString &m) { onMseText(gen, m); });
    connect(m_ws, &QWebSocket::binaryMessageReceived, this, [this, gen](const QByteArray &d) { onMseBinary(gen, d); });
    connect(m_ws, &QWebSocket::disconnected, this, [this, gen] {
        if (isActive(gen))
            scheduleRetry(QStringLiteral("WebSocket закрыт") + (m_ws && !m_ws->closeReason().isEmpty()
                                                                   ? QStringLiteral(": ") + m_ws->closeReason()
                                                                   : QString()));
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_ws, &QWebSocket::errorOccurred, this, [this, gen](QAbstractSocket::SocketError) {
#else
    connect(m_ws, qOverload<QAbstractSocket::SocketError>(&QWebSocket::error), this, [this, gen](QAbstractSocket::SocketError) {
#endif
        if (isActive(gen))
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
    if (!isActive(gen))
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
    if (!isActive(gen) || !m_mseInit)
        return;
    // Media fragment (moof+mdat) of one track; byte 47 = tfhd track_id.
    if (data.size() > 47 && int(uchar(data.at(47))) == m_mseTrack)
        pushToAppSrc(data);
}

void Player::resetStats()
{
    m_fps = m_inFps = m_shownFps = m_frameAge = 0;
    m_lastIn = m_lastShown = m_lastUploaded = m_lastBytes = 0;
    m_bitrate = 0;
    m_uploadFps = 0;
    m_slowOutTicks = 0;
    m_statsUs = 0;
    m_qtShown = 0;
    m_inFpsSlow = 0;
}

void Player::teardown(bool synchronous)
{
    m_paused = false;
    m_watchdog.stop();
    stopNetworkSources();
    resetStats();
    Q_EMIT statsChanged();
    {
        QMutexLocker l(&m_frameMutex);
        m_latestFrame[0] = m_latestFrame[1] = QVideoFrame();
    }
    if (m_pending) {
        releasePipeline(m_pending, synchronous);
        m_pending = nullptr;
        Q_EMIT overlayChanged();
    }
    if (!m_ctx)
        return;
    PipelineCtx *ctx = m_ctx;
    m_ctx = nullptr;
    m_pipeline = nullptr;
    releasePipeline(ctx, synchronous);
}

namespace {
// Detaches qml6glsink from its QML item before the pipeline stops: the sink
// clears the item when it goes to READY (black tile), a detached sink leaves
// the last frame on the screen until the next stream draws.
void detachSurface(PipelineCtx *ctx)
{
    if (!ctx->glSink || !ctx->sink)
        return;
    GstPad *pad = gst_element_get_static_pad(ctx->sink, "sink");
    if (!pad)
        return;
    // The streaming thread must not be inside the sink while the item
    // pointer is replaced: take the stream lock of its input (it is released
    // between frames; a frame waiting for the clock takes < 1 frame period).
    GRecMutex *lock = GST_PAD_GET_STREAM_LOCK(pad);
    bool locked = false;
    for (int i = 0; i < 100 && !(locked = g_rec_mutex_trylock(lock)); ++i)
        g_usleep(5000);
    if (locked) {
        g_object_set(ctx->sink, "widget", nullptr, nullptr);
        g_rec_mutex_unlock(lock);
    }
    gst_object_unref(pad);
}
} // namespace

void Player::releasePipeline(PipelineCtx *ctx, bool synchronous, std::function<void()> after)
{
    if (!ctx)
        return;
    if (qEnvironmentVariableIsSet("HMI_DEBUG_RELEASE"))
        qInfo("[%s] stopping %s (%s)", qPrintable(m_name), GST_OBJECT_NAME(ctx->pipeline),
              qPrintable(QUrl(ctx->url).toString(QUrl::RemoveUserInfo)));
    ctx->kill();
    GstElement *pipe = ctx->pipeline;
    ++m_teardowns;
    QPointer<Player> self(this);
    // Stopping rtspsrc may block on network I/O, so it is done off the GUI thread.
    auto job = [pipe, ctx, self, after]() {
        detachSurface(ctx);
        if (after)
            after();
        gst_element_set_state(pipe, GST_STATE_NULL);
        GstBus *bus = gst_element_get_bus(pipe);
        gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
        // Messages left in the bus queue hold a reference to their source,
        // including the pipeline itself (state changes): bus -> message ->
        // pipeline -> bus is a cycle, and the whole pipeline (network
        // sessions, decoder buffers, threads) would never be freed.
        gst_bus_set_flushing(bus, TRUE);
        gst_object_unref(bus);
        static const bool debugRelease = qEnvironmentVariableIsSet("HMI_DEBUG_RELEASE");
        const QByteArray pname = GST_OBJECT_NAME(pipe);
        if (debugRelease)
            qInfo("release %s: stopped, refcount %d, freeing...", pname.constData(), GST_OBJECT_REFCOUNT_VALUE(pipe));
        gst_object_unref(pipe);
        if (debugRelease)
            qInfo("release %s: freed", pname.constData());
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
    m_lastError = text;
    // Only errors of this particular stream (503, 404) make another profile
    // worth trying; an unreachable server affects all of them alike.
    const bool streamSpecific = text.contains(QLatin1String("(503)")) || text.contains(QLatin1String("(404)"));
    if (!m_gotFrame && m_retry >= 2 && streamSpecific && m_profiles.size() > 1 && m_profilePin.isEmpty()) {
        // This stream of the camera does not work: use another one for a while.
        m_failedUntil.insert(m_streamUrl, QDateTime::currentMSecsSinceEpoch() + 60000);
        QTimer::singleShot(61000, this, [this] { m_selectTimer.start(); });
    }
    if (m_pending && m_active && !m_suspended) {
        // The shown stream failed while another one is being prepared: the
        // prepared one takes over at once.
        qInfo("[%s] %s: using the prepared stream", qPrintable(m_name), qPrintable(text));
        if (m_status == Playing)
            m_status = Reconnecting;
        activatePending();
        return;
    }
    teardown();
    if (!m_active || m_suspended) {
        ensureRunning();
        return;
    }
    setStatus(Reconnecting, QStringLiteral("%1\nПовтор через %2 с").arg(text).arg(qMax(1, delay / 1000)));
    beginWait();
    m_retryTimer.start(delay);
}

void Player::onWatchdog()
{
    if (m_pending) {
        ++m_pendingTicks;
        const quint64 bytes = m_pending->inBytes.load(std::memory_order_relaxed);
        if (m_pending->frames.load(std::memory_order_relaxed) == 0 && bytes > 0) {
            m_pendingStage = QStringLiteral("%1: получено %2 КБ, ожидание ключевого кадра…")
                                 .arg(m_pending->host).arg(bytes / 1024);
            Q_EMIT overlayChanged();
        }
        if (m_pendingTicks >= qMax(25, m_pending->stallLimit))
            failPending(QStringLiteral("Таймаут подключения"));
    }
    if (!m_ctx)
        return;
    if (!m_gotFrame) {
        const quint64 bytes = m_ctx->inBytes.load(std::memory_order_relaxed);
        if (bytes > 0)
            setStage(QStringLiteral("%1: получено %2 КБ, ожидание ключевого кадра…").arg(m_ctx->host).arg(bytes / 1024));
    }
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
    m_inFpsSlow = m_inFpsSlow <= 0 ? m_inFps : m_inFpsSlow * 0.9 + m_inFps * 0.1;
    m_lastIn = in;
    const quint64 shown = m_ctx->glSink ? m_ctx->shown.load(std::memory_order_relaxed)
                                        : m_qtShown.load(std::memory_order_relaxed);
    m_shownFps = ema(m_shownFps, shown - m_lastShown);
    m_lastShown = shown;
    const quint64 bytes = m_ctx->inBytes.load(std::memory_order_relaxed);
    m_bitrate = ema(m_bitrate * 1e6 / 8.0, bytes - m_lastBytes) * 8.0 / 1e6;   // EMA in bytes/s
    m_bitrate = qMax<qreal>(0, m_bitrate);
    m_lastBytes = bytes;
    const quint64 uploaded = m_ctx->uploaded.load(std::memory_order_relaxed);
    m_uploadFps = ema(m_uploadFps, uploaded - m_lastUploaded);
    m_lastUploaded = uploaded;
    const qint64 last = m_ctx->lastFrameUs.load(std::memory_order_relaxed);
    m_frameAge = last > 0 ? qreal(g_get_monotonic_time() - last) / 1e6 : 0;
    updateHealth();
    static const bool debugStats = qEnvironmentVariableIntValue("HMI_DEBUG_STATS") > 0;
    if (debugStats)
        qInfo("[%s] in %.1f dec %.1f upload %.1f shown %.1f render %.1f nominal %.1f age %.2f interval %.1f jitter %.1f ms %s path %s decoder %s audio %s/%llu | %s",
              qPrintable(m_name), m_inFps, m_fps, m_uploadFps, m_shownFps, m_renderFps, m_nominalFps, m_frameAge,
              m_ctx->intervalUs.load() / 1000.0, m_ctx->jitterUs.load() / 1000.0, m_ctx->smooth ? "smooth" : "low-latency",
              qPrintable(renderPath()), qPrintable(m_decoderName), m_audioOn ? "on" : "off",
              (unsigned long long)m_ctx->audioBuffers.load(std::memory_order_relaxed),
              qPrintable(QString(m_healthText).replace(QLatin1Char('\n'), QLatin1String(" / "))));
    checkOutputSpeed();
    if (!m_ctx)
        return;     // the pipeline was rebuilt

    if (m_status == Playing) {
        m_stallTicks = delta == 0 ? m_stallTicks + 1 : 0;
        if (m_stallTicks >= m_stallLimit)
            scheduleRetry(QStringLiteral("Нет видеоданных более %1 с").arg(m_stallLimit));
    } else if (!m_gotFrame) {
        if (++m_connectTicks >= qMax(25, m_stallLimit))
            scheduleRetry(QStringLiteral("Таймаут подключения"));
    }
}

bool Player::useQtSink() const
{
    const int o = m_output == OutputAuto ? m_learned : m_output;
    return !g_glRendering || o == OutputQt;
}

bool Player::copyMode() const
{
    const int o = m_output == OutputAuto ? m_learned : m_output;
    return o == OutputGlCopy;
}

void Player::resetLearned()
{
    m_balancedSw = false;
    m_hwSlow = false;
    m_hwFailed = false;
    m_needConvert = false;
    m_learned = OutputGl;
    m_fallbackNote.clear();
    Q_EMIT renderPathChanged();
    Q_EMIT persistentChanged();
}

void Player::setBalancedSoftware(bool sw)
{
    if (sw == m_balancedSw)
        return;
    m_balancedSw = sw;
    Q_EMIT persistentChanged();
    rebuild(sw ? QStringLiteral("Аппаратный декодер перегружен: поток переведён на программное декодирование")
               : QStringLiteral("В аппаратном декодере есть резерв: поток возвращён на аппаратное декодирование"));
}

qreal Player::pixelRate() const
{
    const qreal rate = nominalFps() > 0 ? nominalFps() : 0;
    return qreal(m_videoWidth) * m_videoHeight * rate;
}

bool Player::decoderLagging() const
{
    return m_status == Playing && nominalFps() > 0 && m_fps < 0.85 * nominalFps();
}

QString Player::renderPath() const
{
    if (useQtSink())
        return QStringLiteral("Qt Multimedia");
    if (copyMode() || m_needConvert)
        return QStringLiteral("GL (копирование)");
    return QStringLiteral("GL (zero-copy)");
}

void Player::rebuild(const QString &reason)
{
    m_lastReconnectMs = QDateTime::currentMSecsSinceEpoch();
    qInfo("[%s] %s", qPrintable(m_name), qPrintable(reason));
    m_fallbackNote = reason;
    teardown();
    ensureRunning();
}

// Decoded frames that never reach the sink mean the GPU upload path is the
// bottleneck (e.g. DMA-BUF import is slow on this kernel/Mesa combination).
// Try the copy mode first, then the Qt Multimedia output.
void Player::checkOutputSpeed()
{
    const bool canSwitchDecoder = m_decoder == DecoderAuto && m_hwDecoding && !m_hwSlow;
    if ((m_output != OutputAuto && !canSwitchDecoder) || m_status != Playing || !m_ctx || !m_gotFrame) {
        m_slowOutTicks = 0;
        return;
    }
    if (m_fps >= 3 && m_shownFps < 0.3 * m_fps)
        ++m_slowOutTicks;
    else
        m_slowOutTicks = 0;
    if (m_slowOutTicks < 6)
        return;
    m_slowOutTicks = 0;
    // The result is remembered (persisted) so the next start uses it at once.
    // First suspect: frames of the hardware decoder live in memory that is
    // very slow to read/import (seen with V4L2 stateless decoders on RK3588
    // mainline kernels). Software-decoded frames are in normal cached memory
    // and can use the fastest output again.
    if (canSwitchDecoder) {
        m_hwSlow = true;
        if (m_output == OutputAuto)
            m_learned = OutputGl;
        Q_EMIT renderPathChanged();
        Q_EMIT persistentChanged();
        rebuild(QStringLiteral("Кадры аппаратного декодера не успевают на экран: включено программное декодирование"));
    } else if (m_output != OutputAuto) {
        return;
    } else if (m_learned == OutputGl && !m_needConvert) {
        m_learned = OutputGlCopy;
        Q_EMIT renderPathChanged();
        Q_EMIT persistentChanged();
        rebuild(QStringLiteral("Медленная загрузка кадров в GPU: включён режим копирования"));
    } else if (m_learned != OutputQt) {
        m_learned = OutputQt;
        Q_EMIT renderPathChanged();   // the tile replaces its video surface
        Q_EMIT persistentChanged();
        rebuild(QStringLiteral("Вывод через GPU не успевает: переключено на Qt Multimedia"));
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
        const qreal ref = nominalFps() > 0 ? nominalFps() : qMax(m_inFps, m_fps);
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
        } else if (ref >= 2 && shown < 0.8 * ref) {
            // Which stage loses the frames: source/network -> decoder -> display.
            const qreal lossSource = nominalFps() > 0 ? qMax<qreal>(0, 1 - m_inFps / nominalFps()) : 0;
            const qreal lossDecoder = m_inFps > 0.5 ? qMax<qreal>(0, 1 - m_fps / m_inFps) : 0;
            const qreal lossDisplay = m_fps > 0.5 ? qMax<qreal>(0, 1 - shown / m_fps) : 0;
            h = shown < 0.3 * ref ? HealthBad : HealthWarning;
            if (lossSource >= lossDecoder && lossSource >= lossDisplay)
                t = QStringLiteral("Источник/сеть присылают мало кадров: %1 из %2 к/с").arg(f1(m_inFps), f1(ref));
            else if (lossDecoder >= lossDisplay)
                t = QStringLiteral("Декодер не успевает: %1 из %2 к/с").arg(f1(m_fps), f1(m_inFps));
            else
                t = (h == HealthBad ? QStringLiteral("Отображение не успевает: %1 из %2 к/с (перегружен GPU/вывод)")
                                    : QStringLiteral("Отображение пропускает кадры: %1 из %2 к/с"))
                        .arg(f1(shown), f1(m_fps));
        } else if (nominalFps() > 0 && m_inFps < 0.6 * nominalFps()) {
            h = HealthWarning;
            t = QStringLiteral("Источник присылает мало кадров: %1 из %2 к/с").arg(f1(m_inFps), f1(nominalFps()));
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
        if (!m_sourceNote.isEmpty() && h < HealthWarning) {
            h = HealthWarning;
            t = m_sourceNote;
        }
        if (!m_fallbackNote.isEmpty())
            t += QStringLiteral("\nВывод: ") + renderPath() + QStringLiteral(" (") + m_fallbackNote + QLatin1Char(')');
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
// msg = error message, then (after '\n') the debug details of GStreamer. The
// message is matched as before; from the details only explicit HTTP/RTSP
// status codes are taken (the details are long free text: a generic word
// such as "not found" there must not decide the explanation).
static QString explainError(const QString &msg)
{
    const QString message = msg.section(QLatin1Char('\n'), 0, 0);
    const QString details = msg.section(QLatin1Char('\n'), 1);
    static const struct { const char *needle; const char *text; } map[] = {
        {"Unauthorized", "Ошибка авторизации: проверьте логин и пароль"},
        {"401", "Ошибка авторизации: проверьте логин и пароль"},
        {"403", "Доступ запрещён (403)"},
        {"Not Found", "Поток не найден (404): проверьте адрес"},
        {"404", "Поток не найден (404): проверьте адрес"},
        {"Service Unavailable", "Сервер временно не отдаёт поток (503)"},
        {"Could not open resource", "Не удалось подключиться к источнику"},
        {"Could not resolve", "Не удалось найти сервер по имени"},
        {"Could not read from resource", "Ошибка чтения из источника"},
        {"Internal data stream error", "Поток прерван"},
        {"Could not connect", "Не удалось подключиться к источнику"},
        {"timeout", "Таймаут сети"},
    };
    QString text;
    for (const auto &m : map) {
        if (message.contains(QLatin1String(m.needle), Qt::CaseInsensitive)) {
            text = QString::fromUtf8(m.text);
            break;
        }
    }
    // Status codes written by souphttpsrc / rtspsrc: "Not Found (404)", "Service Unavailable (503)".
    static const QRegularExpression code(QStringLiteral("\\b(Unauthorized|Forbidden|Not Found|Service Unavailable) \\((\\d{3})\\)"));
    const QRegularExpressionMatch m = code.match(details);
    if (m.hasMatch()) {
        const int c = m.captured(2).toInt();
        text = c == 401 ? QStringLiteral("Ошибка авторизации (401): проверьте логин и пароль")
             : c == 403 ? QStringLiteral("Доступ запрещён (403)")
             : c == 404 ? QStringLiteral("Поток не найден (404): проверьте адрес")
             : QStringLiteral("Сервер временно не отдаёт поток (503)");
        static const QRegularExpression urlRe(QStringLiteral("URL: (\\S+?),? "));
        const QRegularExpressionMatch u = urlRe.match(details + QLatin1Char(' '));
        if (u.hasMatch())
            text += QStringLiteral("\nЗапрошен: ") + QUrl(u.captured(1)).toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery);
    }
    if (text.isEmpty())
        return message;
    // The original message helps when the explanation is not enough.
    if (!text.contains(message))
        text += QStringLiteral("\n(") + message + QLatin1Char(')');
    return text;
}

void Player::onPipelineError(quint64 gen, const QString &msg)
{
    // A CAMAI profile that disappeared (device reconfigured): new catalogue.
    if (m_camai && (isActive(gen) || isPending(gen))
        && (msg.contains(QLatin1String("Not Found")) || msg.contains(QLatin1String("(404)"))))
        m_camai->refreshCatalog();
    PipelineCtx *ctx = isPending(gen) ? m_pending : isActive(gen) ? m_ctx : nullptr;
    if (!ctx)
        return;
    QString text = explainError(msg);
    qWarning("[%s] %s: %s", qPrintable(m_name),
             qPrintable(QUrl(ctx->url).toString(QUrl::RemoveUserInfo)), qPrintable(QString(text).replace(QLatin1Char('\n'), QLatin1String(" | "))));
    if (ctx == m_pending)
        failPending(text);
    else
        scheduleRetry(text);
}

void Player::onDecoderSelected(quint64 gen, const QString &codec, const QString &decoder, bool hw)
{
    PipelineCtx *ctx = isPending(gen) ? m_pending : isActive(gen) ? m_ctx : nullptr;
    if (!ctx)
        return;
    ctx->codec = codec;
    ctx->decoder = decoder;
    ctx->hw = hw;
    ctx->hasDecoder = true;
    const QString stage = QStringLiteral("%1: поток %2 получен, ожидание ключевого кадра…").arg(ctx->host, codec);
    if (ctx == m_pending) {
        m_pendingStage = stage;
        Q_EMIT overlayChanged();
        return;
    }
    applyDecoderInfo(codec, decoder, hw);
    if (!m_gotFrame)
        setStage(stage);
}

void Player::applyDecoderInfo(const QString &codec, const QString &decoder, bool hw)
{
    m_codec = codec;
    m_decoderName = decoder;
    m_hwDecoding = hw;
    Q_EMIT videoInfoChanged();
    learnProfile();
}

void Player::onVideoSize(quint64 gen, int w, int h, int parN, int parD, int fpsN, int fpsD)
{
    if (w <= 0 || h <= 0)
        return;
    if (isPending(gen)) {
        m_pending->w = w;
        m_pending->h = h;
        m_pending->parN = parN;
        m_pending->parD = parD;
        m_pending->fpsN = fpsN;
        m_pending->fpsD = fpsD;
        m_pending->hasSize = true;
    } else if (isActive(gen)) {
        applyVideoSize(w, h, parN, parD, fpsN, fpsD);
    }
}

void Player::applyVideoSize(int w, int h, int parN, int parD, int fpsN, int fpsD)
{
    m_nominalFps = (fpsN > 0 && fpsD > 0) ? qreal(fpsN) / fpsD : 0;
    if (parN > 0 && parD > 0 && parN != parD)
        w = int(qint64(w) * parN / parD);
    if (w == m_videoWidth && h == m_videoHeight) {
        learnProfile();
        return;
    }
    const qreal oldAspect = aspect();
    m_videoWidth = w;
    m_videoHeight = h;
    m_savedAspect = videoAspect();
    Q_EMIT videoInfoChanged();
    if (!qFuzzyCompare(oldAspect, aspect())) {
        Q_EMIT geometryChanged();
        Q_EMIT persistentChanged();
    }
    learnProfile();
}

void Player::onFirstFrame(quint64 gen)
{
    if (isPending(gen)) {
        activatePending();
        return;
    }
    if (!isActive(gen))
        return;
    m_gotFrame = true;
    m_retry = 0;
    m_lastError.clear();
    endWait();
    setStatus(Playing, QString());
}

// ---------------------------------------------------------------------------
// Stream profiles: automatic choice by the player size
// ---------------------------------------------------------------------------
QVariantList Player::profilesVariant() const
{
    QVariantList out;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const StreamProfile &sp : m_profiles) {
        QVariantMap m = sp.toVariant();
        m.insert(QStringLiteral("active"), sp.url == m_streamUrl);
        m.insert(QStringLiteral("failed"), m_failedUntil.value(sp.url) > now);
        out << m;
    }
    return out;
}

void Player::setViewSize(qreal w, qreal h)
{
    if (qAbs(w - m_viewW) < 2 && qAbs(h - m_viewH) < 2)
        return;
    m_viewW = w;
    m_viewH = h;
    if (!m_profiles.isEmpty())
        m_selectTimer.start();
}

void Player::reevaluateProfile()
{
    if (!m_profiles.isEmpty())
        evaluateProfile(true);
}

QString Player::chooseUrl(QString *why) const
{
    if (m_profiles.isEmpty())
        return m_url;
    if (!m_profilePin.isEmpty() && profiles::indexOf(m_profiles, m_profilePin) >= 0) {
        if (why)
            *why = QStringLiteral("закреплён вручную");
        return m_profilePin;
    }
    if (!g_autoQuality) {
        if (why)
            *why = QStringLiteral("автовыбор выключен в настройках программы");
        return m_url.isEmpty() ? m_profiles.first().url : m_url;
    }
    ProfileRequest r;
    // A picture turned by 90° fills the player with its width along the height.
    const bool swap = m_rotation % 180 == 90;
    r.boxWidth = swap ? m_viewH : m_viewW;
    r.boxHeight = swap ? m_viewW : m_viewH;
    r.minFps = g_minFps;
    r.needAudio = m_audioOn;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_failedUntil.cbegin(); it != m_failedUntil.cend(); ++it)
        if (it.value() > now)
            r.excluded.insert(it.key());
    const int i = profiles::select(m_profiles, r, why);
    return i >= 0 ? m_profiles[i].url : m_url;
}

void Player::setZOrder(int z)
{
    if (z == m_z)
        return;
    m_z = z;
    Q_EMIT zOrderChanged();
    Q_EMIT persistentChanged();
}

void Player::setVisibility(bool visible, int hideDelayMs, const QString &reason)
{
    // HMI_NO_HIDE=1: never stop invisible players (diagnostics / comparison).
    static const bool noHide = qEnvironmentVariableIsSet("HMI_NO_HIDE");
    if (noHide)
        visible = true;
    if (visible) {
        m_hideTimer.stop();
        m_wantHidden = false;
        if (m_hidden) {
            m_hidden = false;
            Q_EMIT hiddenChanged();
            qInfo("[%s] visible again: stream resumed", qPrintable(m_name));
            if (m_paused && m_ctx && !m_suspended && m_active) {
                // Continue the paused pipeline; if the source does not come
                // back, the stall watchdog reconnects as usual.
                m_paused = false;
                resetStats();
                m_lastFrames = m_ctx->frames.load(std::memory_order_relaxed);
                m_lastIn = m_ctx->hasDecoderProbe ? m_ctx->inFrames.load(std::memory_order_relaxed) : m_lastFrames;
                m_lastBytes = m_ctx->inBytes.load(std::memory_order_relaxed);
                m_stallTicks = 0;
                m_connectTicks = 0;
                if (m_ctx->dropMode.load() != 0)
                    m_ctx->dropMode.store(2);    // decode again from the next key frame
                else
                    gst_element_set_state(m_ctx->pipeline, GST_STATE_PLAYING);
                setStatus(m_gotFrame ? Playing : Connecting, m_gotFrame ? QString() : m_stage);
                m_watchdog.start();
                m_selectTimer.start();       // the size may have changed meanwhile
                return;
            }
            if (m_status == Suspended && !m_suspended)
                setStatus(Connecting);
            ensureRunning();
        }
        return;
    }
    if (m_hiddenReason != reason) {
        m_hiddenReason = reason;
        if (m_hidden) {
            Q_EMIT hiddenChanged();
            ensureRunning();          // updates the status text
        }
    }
    if (m_hidden)
        return;
    // Nothing is playing yet (e.g. at startup): do not start a stream that
    // would be stopped again in a moment.
    if (!m_ctx && !m_pending)
        hideDelayMs = 0;
    if (!m_wantHidden || (!m_hideTimer.isActive() || m_hideTimer.remainingTime() > hideDelayMs)) {
        m_wantHidden = true;
        m_hideTimer.start(qMax(0, hideDelayMs));
    }
}

// ---------------------------------------------------------------------------
// CAMAI devices: profiles from the device catalogue, input state from /status
// ---------------------------------------------------------------------------
void Player::bindCamai()
{
    if (m_camai) {
        disconnect(m_camai, nullptr, this, nullptr);
        m_camai->removeUser(this);
        m_camai = nullptr;
    }
    if (m_camaiService.isEmpty() || m_camaiChannel < 0) {
        setSourceNote(QString());
        return;
    }
    m_camai = CamaiService::get(m_camaiService, m_username, m_password);
    if (!m_camai)
        return;
    m_camai->setRtspHost(m_camaiRtspHost);
    connect(m_camai, &CamaiService::catalogChanged, this, &Player::onCamaiCatalog);
    connect(m_camai, &CamaiService::statusChanged, this, &Player::onCamaiStatus);
    m_camai->addUser(this);
    if (m_camai->catalogKnown())
        onCamaiCatalog();
    onCamaiStatus();
}

void Player::onCamaiCatalog()
{
    if (!m_camai)
        return;
    const QList<StreamProfile> list = m_camai->profiles(m_camaiChannel);
    if (list.isEmpty()) {
        setSourceNote(QStringLiteral("Канала %1 нет в каталоге устройства CAMAI").arg(m_camaiChannel));
        return;
    }
    setDiscoveredProfiles(QStringLiteral("camai"), list);
    // Main address (used when the automatic choice is off): stream 0.
    if (profiles::indexOf(list, m_url) < 0) {
        QString main = list.first().url;
        for (const StreamProfile &sp : list) {
            if (sp.kind == QLatin1String("stream")) {
                main = sp.url;
                break;
            }
        }
        m_url = main;
        Q_EMIT configChanged();
        Q_EMIT persistentChanged();
    }
    onCamaiStatus();
}

void Player::onCamaiStatus()
{
    if (!m_camai)
        return;
    const CamaiService::ChannelState st = m_camai->state(m_camaiChannel);
    QString note;
    if (st.known && !st.signal)
        note = st.splash ? QStringLiteral("Нет сигнала на входе камеры — сервер показывает заставку")
                         : QStringLiteral("Нет сигнала на входе камеры");
    if (m_camai->catalogKnown() && m_camai->profiles(m_camaiChannel).isEmpty())
        note = QStringLiteral("Канала %1 нет в каталоге устройства CAMAI").arg(m_camaiChannel);
    setSourceNote(note);
}

void Player::setSourceNote(const QString &n)
{
    if (n == m_sourceNote)
        return;
    m_sourceNote = n;
    Q_EMIT sourceNoteChanged();
    updateHealth();
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------
void Player::setAudioDevice(const QString &name) { g_audioDevice = name; }
QString Player::audioDevice() { return g_audioDevice; }

void Player::reapplyAudio()
{
    if (g_audioPlayer && g_audioPlayer->m_audioOn)
        g_audioPlayer->rebuildSeamless(QStringLiteral("устройство вывода звука изменено"));
}

bool Player::audioAvailable() const
{
    if (m_streamHasAudio)
        return true;
    for (const StreamProfile &sp : m_profiles)
        if (sp.audio == 1)
            return true;
    return false;
}

void Player::setAudioEnabled(bool on)
{
    if (on == m_audioOn)
        return;
    if (on) {
        // Only one player plays sound: the previous one is switched off.
        if (g_audioPlayer && g_audioPlayer != this)
            g_audioPlayer->setAudioEnabled(false);
        g_audioPlayer = this;
    } else if (g_audioPlayer == this) {
        g_audioPlayer = nullptr;
    }
    m_audioOn = on;
    m_audioNote.clear();
    Q_EMIT audioChanged();
    qInfo("[%s] sound %s", qPrintable(m_name), on ? "on" : "off");
    if (!m_active)
        return;
    // A stream with sound may be another profile (CAMAI previews have none).
    const QString target = m_profiles.isEmpty() ? m_streamUrl : chooseUrl();
    if (!target.isEmpty() && target != m_streamUrl)
        switchStream(target, on ? QStringLiteral("включён звук") : QStringLiteral("выключен звук"));
    else
        rebuildSeamless(on ? QStringLiteral("включён звук") : QStringLiteral("выключен звук"));
}

// Same stream, new pipeline (e.g. with / without the sound branch), without
// a black frame.
void Player::rebuildSeamless(const QString &reason)
{
    if (m_ctx && m_status == Playing && m_gotFrame) {
        switchStream(m_streamUrl, reason, true);
    } else {
        m_retryTimer.stop();
        teardown();
        ensureRunning();
    }
}

void Player::onAudioDetected(quint64 gen)
{
    if (isPending(gen))
        return;                 // applied on activation (ctx->hasAudio)
    if (!isActive(gen))
        return;
    const int i = profiles::indexOf(m_profiles, m_streamUrl);
    if (i >= 0 && m_profiles[i].audio != 1) {
        m_profiles[i].audio = 1;
        Q_EMIT profilesChanged();
        Q_EMIT persistentChanged();
    }
    if (!m_streamHasAudio) {
        m_streamHasAudio = true;
        Q_EMIT audioChanged();
    }
}

void Player::onAudioError(quint64 gen, const QString &msg)
{
    if (!isActive(gen) && !isPending(gen))
        return;
    if (!m_audioOn)
        return;
    m_audioOn = false;
    if (g_audioPlayer == this)
        g_audioPlayer = nullptr;
    m_audioNote = QStringLiteral("Звук недоступен: ") + msg;
    Q_EMIT audioChanged();
    rebuildSeamless(m_audioNote);
}

// Profiles entered without parameters are examined in the background (once
// per session), so that the automatic choice can use them.
void Player::probeUnknownProfiles()
{
    for (const StreamProfile &sp : std::as_const(m_profiles)) {
        // Unknown parameters, or unknown sound (CAMAI main streams).
        if ((sp.known() && sp.audio >= 0) || m_probed.contains(sp.url))
            continue;
        m_probed.insert(sp.url);
        const QString url = sp.url;
        profiles::probe(resolveUrl(url, nullptr).toString(QUrl::FullyEncoded), m_username, m_password, this,
                        [this, url](const StreamProfile &r, const QString &error) {
            const int i = profiles::indexOf(m_profiles, url);
            if (i < 0)
                return;
            if (error.isEmpty() && r.audio >= 0 && m_profiles[i].audio != r.audio && m_profiles[i].known()) {
                m_profiles[i].audio = r.audio;
                Q_EMIT profilesChanged();
                Q_EMIT audioChanged();
                Q_EMIT persistentChanged();
            }
            if (!error.isEmpty() || !r.known()) {
                qInfo("[%s] stream %s: parameters unknown (%s)", qPrintable(m_name), qPrintable(url), qPrintable(error));
                return;
            }
            StreamProfile &sp = m_profiles[i];
            sp.width = r.width;
            sp.height = r.height;
            if (r.fps > 0) sp.fps = r.fps;
            if (!r.codec.isEmpty()) sp.codec = r.codec;
            if (r.bitrateKbps > 0 && sp.bitrateKbps <= 0) sp.bitrateKbps = r.bitrateKbps;
            if (r.audio >= 0) sp.audio = r.audio;
            qInfo("[%s] stream %s: %s", qPrintable(m_name), qPrintable(url), qPrintable(sp.describe()));
            Q_EMIT profilesChanged();
            Q_EMIT persistentChanged();
            m_selectTimer.start();
        });
    }
}

void Player::setProfileText(const QString &t)
{
    if (t == m_profileText)
        return;
    m_profileText = t;
    Q_EMIT profileTextChanged();
}

void Player::evaluateProfile(bool allowDowngrade)
{
    if (m_profiles.isEmpty()) {
        setProfileText(QString());
        return;
    }
    if (m_active)
        probeUnknownProfiles();
    QString why;
    const QString target = chooseUrl(&why);
    const int ti = profiles::indexOf(m_profiles, target);
    if (ti >= 0) {
        const StreamProfile &tp = m_profiles[ti];
        setProfileText((m_profilePin.isEmpty() ? QStringLiteral("Авто: ") : QStringLiteral("Вручную: "))
                       + (tp.label.isEmpty() ? tp.describe() : tp.label + QStringLiteral(" (") + tp.describe() + QLatin1Char(')')));
    }
    if (!m_active || m_suspended || m_hidden || target.isEmpty())
        return;
    if (target == m_streamUrl) {
        m_downgradeTimer.stop();
        if (m_pending)
            switchStream(target);          // cancels a no longer wanted switch
        return;
    }
    if (m_pending && m_pending->url == target) {
        m_downgradeTimer.stop();
        return;
    }
    if (!m_ctx && !m_pending) {
        // Not running yet (or between reconnects): used at the next start.
        m_streamUrl = target;
        Q_EMIT streamUrlChanged();
        return;
    }
    const int ci = profiles::indexOf(m_profiles, m_streamUrl);
    bool upgrade = ci < 0 || !m_profiles[ci].known() || ti < 0 || !m_profilePin.isEmpty();
    if (!upgrade) {
        const StreamProfile &cp = m_profiles[ci];
        const StreamProfile &tp = m_profiles[ti];
        const double ca = double(cp.width) * cp.height, ta = double(tp.width) * tp.height;
        upgrade = ta > ca * 1.01 || (ta > ca * 0.99 && tp.fps > cp.fps + 0.5);
    }
    if (upgrade || allowDowngrade || m_status != Playing) {
        m_downgradeTimer.stop();
        switchStream(target, why);
    } else if (!m_downgradeTimer.isActive() || m_downTarget != target) {
        m_downTarget = target;
        m_downgradeTimer.start();
    }
}

// The parameters of the profile being played are corrected from the real
// stream (catalogues and manual entries may be inexact or empty).
void Player::learnProfile()
{
    const int i = profiles::indexOf(m_profiles, m_streamUrl);
    if (i < 0)
        return;
    StreamProfile &sp = m_profiles[i];
    bool changed = false;
    if (m_videoWidth > 0 && m_videoHeight > 0 && (sp.width != m_videoWidth || sp.height != m_videoHeight)) {
        sp.width = m_videoWidth;
        sp.height = m_videoHeight;
        changed = true;
    }
    if (nominalFps() > 0 && qAbs(sp.fps - nominalFps()) > 0.5) {
        sp.fps = qRound(nominalFps() * 100) / 100.0;
        changed = true;
    }
    QString codec = m_codec == QLatin1String("H.264") ? QStringLiteral("h264")
                  : m_codec == QLatin1String("H.265") ? QStringLiteral("h265")
                  : m_codec == QLatin1String("MJPEG") ? QStringLiteral("jpeg") : QString();
    if (!codec.isEmpty() && sp.codec != codec) {
        sp.codec = codec;
        changed = true;
    }
    if (!changed)
        return;
    Q_EMIT profilesChanged();
    Q_EMIT persistentChanged();
    m_selectTimer.start();
}

void Player::setDiscoveredProfiles(const QString &source, const QList<StreamProfile> &list)
{
    QList<StreamProfile> merged;
    for (const StreamProfile &sp : list) {
        StreamProfile n = sp;
        n.source = source;
        // Keep what was learned from playback when the catalogue has no value.
        const int old = profiles::indexOf(m_profiles, sp.url);
        if (old >= 0) {
            const StreamProfile &o = m_profiles[old];
            if (!n.known()) { n.width = o.width; n.height = o.height; }
            if (n.fps <= 0) n.fps = o.fps;
            if (n.codec.isEmpty()) n.codec = o.codec;
            if (n.audio < 0) n.audio = o.audio;
        }
        if (profiles::indexOf(merged, n.url) < 0)
            merged << n;
    }
    for (const StreamProfile &sp : m_profiles)
        if (sp.source != source && profiles::indexOf(merged, sp.url) < 0)
            merged << sp;
    QStringList before, after;
    for (const StreamProfile &sp : m_profiles)
        before << QJsonDocument(sp.toJson()).toJson(QJsonDocument::Compact);
    for (const StreamProfile &sp : merged)
        after << QJsonDocument(sp.toJson()).toJson(QJsonDocument::Compact);
    if (before == after)
        return;
    m_profiles = merged;
    Q_EMIT profilesChanged();
    Q_EMIT persistentChanged();
    m_selectTimer.start();
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------
void Player::clearSurface(int index)
{
    Surface &sf = m_surf[index];
    if (sf.conn)
        disconnect(sf.conn);
    // qml6glsink must be stopped before its QML item is destroyed.
    const bool sync = bool(m_surf[0].glItem) || bool(m_surf[1].glItem);
    if ((m_ctx && m_ctx->surface == index) || (m_pending && m_pending->surface == index))
        teardown(sync);
    sf.obj = nullptr;
    sf.glItem = nullptr;
    sf.videoSink = nullptr;
    sf.ready = false;
    {
        QMutexLocker l(&m_frameMutex);
        m_latestFrame[index] = QVideoFrame();
    }
}

void Player::attachGlItem(QQuickItem *item, int index)
{
    index = index ? 1 : 0;
    if (!item || m_surf[index].obj == item)
        return;
    clearSurface(index);
    Surface &sf = m_surf[index];
    sf.obj = item;
    sf.glItem = item;
    sf.conn = connect(item, &QObject::destroyed, this, [this, index] { clearSurface(index); ensureRunning(); });
    scheduleGlReady(index);
    ensureRunning();
}

void Player::scheduleGlReady(int index)
{
    QQuickItem *item = m_surf[index].glItem;
    if (!item)
        return;
    QQuickWindow *w = item->window();
    if (!w) {
        connect(item, &QQuickItem::windowChanged, this, [this, index] { scheduleGlReady(index); },
                Qt::SingleShotConnection);
        return;
    }
    // qml6glsink may only start once the item has its GL context, i.e. after
    // the window's scene graph was initialised. A render job runs exactly then.
    QPointer<Player> self(this);
    QPointer<QQuickItem> it(item);
    w->scheduleRenderJob(new FnJob([self, it, index] {
                             QMetaObject::invokeMethod(qApp, [self, it, index] {
                                 if (self && it && self->m_surf[index].glItem == it) {
                                     self->m_surf[index].ready = true;
                                     self->ensureRunning();
                                 }
                             }, Qt::QueuedConnection);
                         }),
                         QQuickWindow::BeforeSynchronizingStage);
    w->update();
}

void Player::attachVideoSink(QVideoSink *sink, int index)
{
    index = index ? 1 : 0;
    if (!sink || m_surf[index].obj == sink)
        return;
    clearSurface(index);
    Surface &sf = m_surf[index];
    sf.obj = sink;
    sf.videoSink = sink;
    sf.conn = connect(sink, &QObject::destroyed, this, [this, index] { clearSurface(index); ensureRunning(); });
    sf.ready = true;
    ensureRunning();
}

void Player::detachSink(QObject *surface)
{
    for (int i = 0; i < 2; ++i) {
        if (surface && m_surf[i].obj == surface) {
            clearSurface(i);
            ensureRunning();
        }
    }
}

void Player::pushFrame(const QVideoFrame &frame, int surface, quint64 gen)
{
    QMutexLocker l(&m_frameMutex);
    m_latestFrame[surface] = frame;
    m_frameGen[surface] = gen;
    if (m_framePosted[surface])
        return;        // GUI thread has not consumed the previous frame: replace it
    m_framePosted[surface] = true;
    l.unlock();
    QMetaObject::invokeMethod(this, [this, surface] { flushFrame(surface); }, Qt::QueuedConnection);
}

void Player::flushFrame(int surface)
{
    QVideoFrame f;
    quint64 gen = 0;
    {
        QMutexLocker l(&m_frameMutex);
        f = m_latestFrame[surface];
        gen = m_frameGen[surface];
        m_latestFrame[surface] = QVideoFrame();
        m_framePosted[surface] = false;
    }
    QVideoSink *sink = m_surf[surface].videoSink;
    if (!sink || !f.isValid())
        return;
    const bool active = isActive(gen);
    if (!active && !isPending(gen))
        return;        // stale frame of a stopped pipeline
    sink->setVideoFrame(f);
    if (active) {
        m_qtShown.fetch_add(1, std::memory_order_relaxed);
        if (!m_gotFrame)
            onFirstFrame(gen);
    } else {
        onFirstFrame(gen);   // prepared stream is on its surface: show it
    }
}
