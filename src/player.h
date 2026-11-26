// One video player: configuration, GStreamer pipeline and runtime state.
//
// Pipeline (RTSP):
//   rtspsrc ! rtpXdepay ! Xparse ! <HW decoder> ! queue(leaky) ! <render tail>
// Render tail, chosen at startup:
//   GL : glupload ! glcolorconvert ! qml6glsink   (zero-copy DMA-BUF import on RK3588)
//   Qt : videoconvert(passthrough) ! appsink -> QVideoSink/VideoOutput (fallback)
#pragma once

#include <QHash>
#include <QSet>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <QVideoFrame>

#include <gst/gst.h>

#include "streamprofile.h"

#include <atomic>
#include <functional>

class QQuickItem;
class QVideoSink;
class QNetworkAccessManager;
class QNetworkReply;
class QWebSocket;
struct PipelineCtx;

class Player : public QObject
{
    Q_OBJECT
    // --- persistent configuration ---
    Q_PROPERTY(QString uid READ uid CONSTANT)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY configChanged)
    Q_PROPERTY(QString url READ url NOTIFY configChanged)
    Q_PROPERTY(QString username READ username NOTIFY configChanged)
    Q_PROPERTY(int transport READ transport NOTIFY configChanged)
    Q_PROPERTY(int latency READ latency NOTIFY configChanged)
    Q_PROPERTY(int decoder READ decoder NOTIFY configChanged)
    Q_PROPERTY(bool lowLatency READ lowLatency NOTIFY configChanged)
    Q_PROPERTY(int snapshotInterval READ snapshotInterval NOTIFY configChanged)
    Q_PROPERTY(QString sourceType READ sourceType NOTIFY configChanged)
    Q_PROPERTY(QString webUrl READ webUrl WRITE setWebUrl NOTIFY configChanged)
    // --- orientation (persistent) ---
    Q_PROPERTY(int rotation READ rotation WRITE setRotation NOTIFY orientationChanged)
    Q_PROPERTY(bool flipH READ flipH WRITE setFlipH NOTIFY orientationChanged)
    Q_PROPERTY(bool flipV READ flipV WRITE setFlipV NOTIFY orientationChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(qint64 created READ created CONSTANT)
    // --- geometry ---
    Q_PROPERTY(qreal displayWidth READ displayWidth WRITE setDisplayWidth NOTIFY geometryChanged)
    Q_PROPERTY(qreal displayHeight READ displayHeight NOTIFY geometryChanged)
    Q_PROPERTY(qreal aspect READ aspect NOTIFY geometryChanged)
    Q_PROPERTY(qreal posX READ posX WRITE setPosX NOTIFY posChanged)
    Q_PROPERTY(qreal posY READ posY WRITE setPosY NOTIFY posChanged)
    Q_PROPERTY(qreal layoutX READ layoutX NOTIFY layoutChanged)
    Q_PROPERTY(qreal layoutY READ layoutY NOTIFY layoutChanged)
    Q_PROPERTY(qreal layoutWidth READ layoutWidth NOTIFY layoutChanged)
    Q_PROPERTY(qreal layoutHeight READ layoutHeight NOTIFY layoutChanged)
    Q_PROPERTY(int order READ order NOTIFY orderChanged)
    // --- runtime state ---
    Q_PROPERTY(int status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoInfoChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoInfoChanged)
    Q_PROPERTY(QString codec READ codec NOTIFY videoInfoChanged)
    Q_PROPERTY(QString decoderName READ decoderName NOTIFY videoInfoChanged)
    Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding NOTIFY videoInfoChanged)
    Q_PROPERTY(qreal fps READ fps NOTIFY statsChanged)              // decoded frames/s
    Q_PROPERTY(qreal inFps READ inFps NOTIFY statsChanged)          // received (to decoder)
    Q_PROPERTY(qreal shownFps READ shownFps NOTIFY statsChanged)    // delivered to the screen
    Q_PROPERTY(qreal nominalFps READ nominalFps NOTIFY statsChanged) // from the stream, 0 = unknown
    Q_PROPERTY(qreal frameAge READ frameAge NOTIFY statsChanged)    // s since the last frame
    Q_PROPERTY(qreal bitrate READ bitrate NOTIFY statsChanged)      // Mbit/s of compressed video received
    // Traffic light: -1 none (stopped), 0 green, 1 yellow, 2 red.
    Q_PROPERTY(int health READ health NOTIFY statsChanged)
    Q_PROPERTY(QString healthText READ healthText NOTIFY statsChanged)
    // Output path: false - GStreamer GL (zero-copy), true - Qt Multimedia.
    // Switched automatically when the GL upload cannot keep up.
    Q_PROPERTY(bool useQtSink READ useQtSink NOTIFY renderPathChanged)
    // 0 auto (learned), 1 GL zero-copy, 2 GL with copy, 3 Qt Multimedia
    Q_PROPERTY(int output READ output NOTIFY renderPathChanged)
    Q_PROPERTY(QString renderPath READ renderPath NOTIFY renderPathChanged)
    // Status panel over the video: shown when the video is stopped, or when
    // no new frame came within the "waiting for the stream" time (until then
    // the last frame stays on the screen).
    Q_PROPERTY(bool overlayVisible READ overlayVisible NOTIFY overlayChanged)
    Q_PROPERTY(QString overlayText READ overlayText NOTIFY overlayChanged)
    // A new stream is being prepared while the current one is still shown.
    Q_PROPERTY(bool switching READ switching NOTIFY overlayChanged)
    // Address of the stream actually played (may differ from url: profiles).
    Q_PROPERTY(QString streamUrl READ streamUrl NOTIFY streamUrlChanged)
    // Each tile has two video surfaces: the shown one and the one a new
    // stream is prepared on. Index of the shown surface.
    Q_PROPERTY(int surfaceIndex READ surfaceIndex NOTIFY surfaceIndexChanged)
    // Streams of the camera in different resolutions (persistent). The
    // player picks the one that suits its size unless a profile is pinned.
    Q_PROPERTY(QVariantList profiles READ profilesVariant NOTIFY profilesChanged)
    Q_PROPERTY(QString profilePin READ profilePin NOTIFY profilesChanged)
    // Current choice for the operator ("720p, авто: плеер 800×450 …").
    Q_PROPERTY(QString profileText READ profileText NOTIFY profileTextChanged)

public:
    enum Status { Stopped, Connecting, Playing, Reconnecting, Suspended };
    Q_ENUM(Status)
    enum Transport { TransportAuto, TransportTcp, TransportUdp };
    Q_ENUM(Transport)
    enum Decoder { DecoderAuto, DecoderHardware, DecoderSoftware };
    Q_ENUM(Decoder)
    enum Health { HealthNone = -1, HealthGood = 0, HealthWarning = 1, HealthBad = 2 };
    Q_ENUM(Health)

    static constexpr qreal kMinWidth = 160;
    static constexpr qreal kMaxWidth = 7680;

    explicit Player(QObject *parent = nullptr);

    // Render path chosen once at startup (true: qml6glsink, false: QVideoSink).
    static void setGlRendering(bool on);
    static bool glRendering();
    // "Waiting for the video stream" (application setting): how long the last
    // frame is kept on the screen before the exchange status is shown.
    static void setWaitTimeout(int ms);
    static int waitTimeout();
    // Automatic stream choice by player size and the frame rate floor.
    static void setAutoQuality(bool on);
    static bool autoQuality();
    static void setMinFps(double fps);
    static double minFps();
    ~Player() override;

    static Player *fromJson(const QJsonObject &o, QObject *parent);
    QJsonObject toJson() const;

    QString uid() const { return m_uid; }
    QString name() const { return m_name; }
    void setName(const QString &n);
    QString url() const { return m_url; }
    QString username() const { return m_username; }
    int transport() const { return m_transport; }
    int latency() const { return m_latency; }
    int decoder() const { return m_decoder; }
    bool lowLatency() const { return m_lowLatency; }
    int snapshotInterval() const { return m_snapshotInterval; }
    QString sourceType() const;     // human readable protocol name
    QString webUrl() const { return m_webUrl; }
    void setWebUrl(const QString &u);
    // Camera web interface address with a scheme ("" if not configured).
    Q_INVOKABLE QString webPageUrl() const;
    // Host of the stream address (suggestion for the web interface).
    Q_INVOKABLE QString streamHost() const;

    int rotation() const { return m_rotation; }
    void setRotation(int r);
    bool flipH() const { return m_flipH; }
    void setFlipH(bool f);
    bool flipV() const { return m_flipV; }
    void setFlipV(bool f);
    Q_INVOKABLE void rotateBy(int degrees);
    Q_INVOKABLE void resetOrientation();

    // Kind of source, derived from the URL.
    enum class Source { Rtsp, HttpStream, Snapshot, MseLd, Test, Other };
    // Resolves aliases (m4s://, m4f://) and classifies the URL.
    static QUrl resolveUrl(const QString &url, Source *kind);
    static bool webSocketsAvailable();
    bool active() const { return m_active; }
    qint64 created() const { return m_created; }

    qreal displayWidth() const { return m_displayWidth; }
    void setDisplayWidth(qreal w);
    qreal displayHeight() const { return m_displayWidth / aspect(); }
    qreal aspect() const;           // displayed aspect (rotation applied)
    qreal videoAspect() const;      // aspect of the stream itself
    qreal posX() const { return m_posX; }
    void setPosX(qreal x);
    qreal posY() const { return m_posY; }
    void setPosY(qreal y);
    qreal layoutX() const { return m_layoutX; }
    qreal layoutY() const { return m_layoutY; }
    qreal layoutWidth() const { return m_layoutW; }
    qreal layoutHeight() const { return m_layoutH; }
    void setLayoutRect(qreal x, qreal y, qreal w, qreal h);
    int order() const { return m_order; }
    void setOrder(int o);

    int status() const { return m_status; }
    QString statusText() const { return m_statusText; }
    int videoWidth() const { return m_videoWidth; }
    int videoHeight() const { return m_videoHeight; }
    QString codec() const { return m_codec; }
    QString decoderName() const { return m_decoderName; }
    bool hardwareDecoding() const { return m_hwDecoding; }
    qreal fps() const { return m_fps; }
    qreal inFps() const { return m_inFps; }
    qreal shownFps() const { return m_shownFps; }
    qreal nominalFps() const { return m_nominalFps; }
    qreal frameAge() const { return m_frameAge; }
    qreal bitrate() const { return m_bitrate; }
    int health() const { return m_health; }
    QString healthText() const { return m_healthText; }
    // Frames/s the window actually renders (measured by the window).
    void setRenderFps(qreal fps) { m_renderFps = fps; }
    bool useQtSink() const;

    // Load balancing between the hardware decoder and the CPU (AppController).
    // true: this "Auto" decoder player was moved to software decoding.
    bool balancedSoftware() const { return m_balancedSw; }
    void setBalancedSoftware(bool sw);
    // Forget automatic decisions (decoder/output fallbacks, balancing): used
    // when the kernel or the set of hardware decoders changed.
    void resetLearned();
    bool movableDecoder() const { return m_decoder == DecoderAuto && !m_hwFailed && !m_hwSlow; }
    // Decoded pixels per second the stream needs (0 if not known yet).
    qreal pixelRate() const;
    // The decoder keeps up with the stream (nominal rate known).
    bool decoderLagging() const;
    int output() const { return m_output; }
    enum Output { OutputAuto, OutputGl, OutputGlCopy, OutputQt };
    Q_ENUM(Output)
    QString renderPath() const;

    Q_INVOKABLE QVariantMap config() const;
    Q_INVOKABLE void applyConfig(const QVariantMap &cfg);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void resetSize();   // original (native) video size

    // Seamless change of the played stream: the new stream is opened while
    // the current one keeps playing; the picture switches on its first frame.
    // Falls back to a plain restart (last frame kept) when nothing is shown.
    Q_INVOKABLE void switchStream(const QString &url, const QString &reason = QString());
    QString streamUrl() const { return m_streamUrl; }
    bool overlayVisible() const;
    QString overlayText() const;
    bool switching() const { return m_pending != nullptr; }

    QVariantList profilesVariant() const;
    QString profilePin() const { return m_profilePin; }
    QString profileText() const { return m_profileText; }
    const QList<StreamProfile> &profileList() const { return m_profiles; }
    // Size of the player on the screen in device pixels (called by the tile).
    Q_INVOKABLE void setViewSize(qreal w, qreal h);
    // Re-evaluates the stream choice (settings changed, profiles updated).
    void reevaluateProfile();
    // Profiles from an external catalogue (CAMAI, ONVIF, ...): replaces the
    // profiles of that source, keeps the others.
    void setDiscoveredProfiles(const QString &source, const QList<StreamProfile> &list);

    // Video surface handling (called from QML).
    Q_INVOKABLE void attachGlItem(QQuickItem *item, int index = 0);
    Q_INVOKABLE void attachVideoSink(QVideoSink *sink, int index = 0);
    int surfaceIndex() const { return m_activeSurf; }
    Q_INVOKABLE void detachSink(QObject *surface);

    // Window minimised / closed: release the decoder but keep "active".
    void setSuspended(bool s, bool synchronous = false);

    // Called from GStreamer streaming threads (via PipelineCtx).
    void pushFrame(const QVideoFrame &frame, int surface, quint64 gen);

Q_SIGNALS:
    void configChanged();
    void activeChanged();
    void geometryChanged();
    void posChanged();
    void layoutChanged();
    void orientationChanged();
    void renderPathChanged();
    void orderChanged();
    void statusChanged();
    void videoInfoChanged();
    void statsChanged();
    void persistentChanged();   // something that must be saved changed
    void overlayChanged();
    void streamUrlChanged();
    void surfaceIndexChanged();
    void profilesChanged();
    void profileTextChanged();
    // The prepared stream could not be opened; the current one is kept.
    void switchFailed(const QString &url, const QString &reason);
    void switchDone(const QString &url);

private:
    friend struct PipelineCtx;
    friend struct PipelineCallbacks;

    bool canRun() const;
    void ensureRunning();
    void startPipeline();
    // Builds a pipeline for the URL; pending: output blocked until activation.
    PipelineCtx *buildPipeline(const QString &url, bool pending, QString *error);
    void teardown(bool synchronous = false);
    // Stops one pipeline; the GL surface keeps its last frame. 'after' runs
    // (in the worker) once the pipeline no longer draws on the surface.
    void releasePipeline(PipelineCtx *ctx, bool synchronous, std::function<void()> after = {});
    void activatePending();
    void failPending(const QString &reason);
    void beginWait();
    void endWait();
    void setStage(const QString &stage);
    void onProgress(quint64 gen, const QString &stage);
    bool isActive(quint64 gen) const;
    bool isPending(quint64 gen) const;
    void applyDecoderInfo(const QString &codec, const QString &decoder, bool hw);
    void applyVideoSize(int w, int h, int parN, int parD, int fpsN, int fpsD);
    void resetStats();
    QString chooseUrl(QString *why = nullptr) const;
    void evaluateProfile(bool allowDowngrade);
    void learnProfile();
    void setProfileText(const QString &t);
    void probeUnknownProfiles();
    void setStatus(Status s, const QString &text = QString());
    void setActive(bool a);
    void scheduleGlReady(int index);
    void scheduleRetry(const QString &reason);
    void onTeardownDone();
    void onWatchdog();
    void flushFrame(int surface);
    void clearSurface(int index);

    // Events delivered from streaming threads (queued to the GUI thread).
    void onPipelineError(quint64 gen, const QString &msg);
    void onDecoderSelected(quint64 gen, const QString &codec, const QString &decoder, bool hw);
    void onVideoSize(quint64 gen, int w, int h, int parN, int parD, int fpsN, int fpsD);
    void updateHealth();
    void rebuild(const QString &reason);   // restart keeping the fallbacks
    void checkOutputSpeed();
    void onFirstFrame(quint64 gen);

    GstElement *buildTail(PipelineCtx *ctx, QString *error);
    GstElement *makeAppSrc(const char *caps, bool doTimestamp);
    void pushToAppSrc(const QByteArray &data);
    void stopNetworkSources();
    void requestSnapshot();
    void startMseLd(const QUrl &url);
    void onMseText(quint64 gen, const QString &msg);
    void onMseBinary(quint64 gen, const QByteArray &data);

    // configuration
    QString m_uid;
    QString m_name;
    QString m_url;
    QString m_username;
    QString m_password;
    int m_transport = TransportAuto;
    int m_latency = 200;
    int m_decoder = DecoderAuto;
    bool m_lowLatency = true;
    int m_snapshotInterval = 1000;   // ms, JPEG snapshot polling
    bool m_active = true;
    qint64 m_created = 0;
    qreal m_displayWidth = 480;
    qreal m_posX = 0;
    qreal m_posY = 0;
    qreal m_layoutX = 0;
    qreal m_layoutY = 0;
    qreal m_layoutW = 0;
    qreal m_layoutH = 0;
    int m_order = 0;
    int m_rotation = 0;
    bool m_flipH = false;
    bool m_flipV = false;
    QString m_webUrl;
    qreal m_savedAspect = 16.0 / 9.0;   // last known video aspect (persisted)

    // runtime
    Status m_status = Stopped;
    QString m_statusText;
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    QString m_codec;
    QString m_decoderName;
    bool m_hwDecoding = false;
    qreal m_fps = 0;
    qreal m_inFps = 0;
    qreal m_shownFps = 0;
    qreal m_uploadFps = 0;
    qreal m_bitrate = 0;
    quint64 m_lastBytes = 0;
    quint64 m_lastUploaded = 0;
    qreal m_nominalFps = 0;
    qreal m_frameAge = 0;
    qreal m_renderFps = -1;
    int m_health = HealthNone;
    QString m_healthText;
    quint64 m_lastIn = 0;
    quint64 m_lastShown = 0;
    qint64 m_statsUs = 0;
    qint64 m_lastReconnectMs = 0;
    std::atomic<quint64> m_qtShown{0};   // frames handed to QVideoSink

    bool m_suspended = false;
    bool m_hwFailed = false;     // auto mode: HW decoder failed -> use software
    bool m_needConvert = false;  // decoder output not GL-uploadable -> videoconvert
    int m_output = OutputAuto;   // configured output path
    int m_learned = OutputGl;    // auto mode: path found by measurement (persisted)
    bool m_hwSlow = false;       // auto decoder: HW frames too slow to display -> software (persisted)
    bool m_balancedSw = false;   // auto decoder: moved to CPU because the VPU is saturated (persisted)
    bool copyMode() const;
    int m_slowOutTicks = 0;
    QString m_fallbackNote;
    int m_retry = 0;
    // stream profiles
    QList<StreamProfile> m_profiles;
    QString m_profilePin;               // "" automatic, otherwise URL of the pinned profile
    QString m_profileText;
    qreal m_viewW = 0;
    qreal m_viewH = 0;
    QTimer m_selectTimer;               // debounce of size changes
    QTimer m_downgradeTimer;            // a smaller stream only after the size is stable
    QString m_downTarget;
    QHash<QString, qint64> m_failedUntil;   // URL -> ms epoch (switch failed)
    QSet<QString> m_probed;             // profiles whose parameters were requested
    QString m_lastError;        // shown while reconnecting
    QString m_stage;            // exchange status of the connecting stream
    QString m_pendingStage;     // ... of the prepared stream
    QString m_pendingReason;
    int m_pendingTicks = 0;
    bool m_waitExpired = false;
    QTimer m_waitTimer;
    int m_stallTicks = 0;
    int m_connectTicks = 0;
    quint64 m_lastFrames = 0;
    bool m_gotFrame = false;

    // pipeline
    GstElement *m_pipeline = nullptr;
    PipelineCtx *m_ctx = nullptr;          // active (shown) pipeline
    PipelineCtx *m_pending = nullptr;      // prepared, output blocked
    QString m_streamUrl;                   // URL of the active pipeline
    quint64 m_gen = 0;                     // last generation handed out
    int m_teardowns = 0;         // asynchronous teardowns still running
    int m_stallLimit = 10;       // seconds without frames before reconnect
    GstElement *m_appsrc = nullptr;   // snapshot / MSE-LD input (owned by the pipeline)
    QUrl m_sourceUrl;                 // resolved URL of the running pipeline
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_snapReply;
    QTimer m_snapshotTimer;
    QWebSocket *m_ws = nullptr;
    int m_mseTrack = -1;              // MSE-LD: id of the video track
    bool m_mseInit = false;
    bool m_startPending = false;
    QTimer m_retryTimer;
    QTimer m_watchdog;

    // surfaces (see surfaceIndex)
    struct Surface {
        QPointer<QObject> obj;
        QPointer<QQuickItem> glItem;
        QPointer<QVideoSink> videoSink;
        bool ready = false;
        QMetaObject::Connection conn;
    };
    Surface m_surf[2];
    int m_activeSurf = 0;

    // latest frame per surface for the QVideoSink path (coalesced: never queue up)
    QMutex m_frameMutex;
    QVideoFrame m_latestFrame[2];
    quint64 m_frameGen[2] = {0, 0};
    bool m_framePosted[2] = {false, false};
};
