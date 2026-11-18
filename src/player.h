// One video player: configuration, GStreamer pipeline and runtime state.
//
// Pipeline (RTSP):
//   rtspsrc ! rtpXdepay ! Xparse ! <HW decoder> ! queue(leaky) ! <render tail>
// Render tail, chosen at startup:
//   GL : glupload ! glcolorconvert ! qml6glsink   (zero-copy DMA-BUF import on RK3588)
//   Qt : videoconvert(passthrough) ! appsink -> QVideoSink/VideoOutput (fallback)
#pragma once

#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <QVideoFrame>

#include <gst/gst.h>

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
    Q_PROPERTY(int order READ order NOTIFY orderChanged)
    // --- runtime state ---
    Q_PROPERTY(int status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoInfoChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoInfoChanged)
    Q_PROPERTY(QString codec READ codec NOTIFY videoInfoChanged)
    Q_PROPERTY(QString decoderName READ decoderName NOTIFY videoInfoChanged)
    Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding NOTIFY videoInfoChanged)
    Q_PROPERTY(qreal fps READ fps NOTIFY statsChanged)

public:
    enum Status { Stopped, Connecting, Playing, Reconnecting, Suspended };
    Q_ENUM(Status)
    enum Transport { TransportAuto, TransportTcp, TransportUdp };
    Q_ENUM(Transport)
    enum Decoder { DecoderAuto, DecoderHardware, DecoderSoftware };
    Q_ENUM(Decoder)

    static constexpr qreal kMinWidth = 160;
    static constexpr qreal kMaxWidth = 7680;

    explicit Player(QObject *parent = nullptr);

    // Render path chosen once at startup (true: qml6glsink, false: QVideoSink).
    static void setGlRendering(bool on);
    static bool glRendering();
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
    qreal aspect() const;
    qreal posX() const { return m_posX; }
    void setPosX(qreal x);
    qreal posY() const { return m_posY; }
    void setPosY(qreal y);
    qreal layoutX() const { return m_layoutX; }
    qreal layoutY() const { return m_layoutY; }
    void setLayoutPos(qreal x, qreal y);
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

    Q_INVOKABLE QVariantMap config() const;
    Q_INVOKABLE void applyConfig(const QVariantMap &cfg);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void resetSize();   // original (native) video size

    // Video surface handling (called from QML).
    Q_INVOKABLE void attachGlItem(QQuickItem *item);
    Q_INVOKABLE void attachVideoSink(QVideoSink *sink);
    Q_INVOKABLE void detachSink(QObject *surface);

    // Window minimised / closed: release the decoder but keep "active".
    void setSuspended(bool s, bool synchronous = false);

    // Called from GStreamer streaming threads (via PipelineCtx).
    void pushFrame(const QVideoFrame &frame);

Q_SIGNALS:
    void configChanged();
    void activeChanged();
    void geometryChanged();
    void posChanged();
    void layoutChanged();
    void orderChanged();
    void statusChanged();
    void videoInfoChanged();
    void statsChanged();
    void persistentChanged();   // something that must be saved changed

private:
    friend struct PipelineCtx;
    friend struct PipelineCallbacks;

    bool canRun() const;
    void ensureRunning();
    void startPipeline();
    void teardown(bool synchronous = false);
    void setStatus(Status s, const QString &text = QString());
    void setActive(bool a);
    void scheduleGlReady();
    void scheduleRetry(const QString &reason);
    void onTeardownDone();
    void onWatchdog();
    void flushFrame();
    void clearSurface();

    // Events delivered from streaming threads (queued to the GUI thread).
    void onPipelineError(quint64 gen, const QString &msg);
    void onDecoderSelected(quint64 gen, const QString &codec, const QString &decoder, bool hw);
    void onVideoSize(quint64 gen, int w, int h, int parN, int parD);
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
    int m_order = 0;
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

    bool m_suspended = false;
    bool m_hwFailed = false;     // auto mode: HW decoder failed -> use software
    bool m_needConvert = false;  // decoder output not GL-uploadable -> videoconvert
    int m_retry = 0;
    int m_stallTicks = 0;
    int m_connectTicks = 0;
    quint64 m_lastFrames = 0;
    bool m_gotFrame = false;

    // pipeline
    GstElement *m_pipeline = nullptr;
    PipelineCtx *m_ctx = nullptr;
    quint64 m_gen = 0;
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

    // surface
    QPointer<QObject> m_surface;
    QPointer<QQuickItem> m_glItem;
    QPointer<QVideoSink> m_videoSink;
    bool m_sinkReady = false;
    QMetaObject::Connection m_surfaceConn;

    // latest frame for the QVideoSink path (coalesced: never queue up)
    QMutex m_frameMutex;
    QVideoFrame m_latestFrame;
    bool m_framePosted = false;
};
