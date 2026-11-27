// Application controller: windows, persistence, screens, single instance IPC.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include "diagnostics.h"

#include <atomic>
#include <memory>

class QQmlEngine;
class QQmlComponent;
class QQuickWindow;
class QLocalServer;
class VmsWindow;
class Player;


class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<QObject *> windows READ windowsList NOTIFY windowsChanged)
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)
    Q_PROPERTY(bool glSink READ glSink CONSTANT)
    Q_PROPERTY(QString renderBackend READ renderBackend CONSTANT)
    Q_PROPERTY(QString hardwareDecoders READ hardwareDecoders CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    Q_PROPERTY(QString productName READ productName CONSTANT)
    Q_PROPERTY(QString glRenderer READ glRenderer NOTIFY glInfoChanged)
    Q_PROPERTY(bool softwareRendering READ softwareRendering NOTIFY glInfoChanged)
    Q_PROPERTY(QVariantList builtinWallpapers READ builtinWallpapers CONSTANT)
    Q_PROPERTY(QString decoderBalance READ decoderBalance NOTIFY decoderBalanceChanged)
    Q_PROPERTY(Diagnostics *diag READ diag CONSTANT)
    // Application settings ("Настройки программы").
    // Waiting for the video stream: the last frame is kept this long before
    // the exchange status is shown over the player.
    Q_PROPERTY(int streamWaitMs READ streamWaitMs WRITE setStreamWaitMs NOTIFY settingsChanged)
    // Automatic choice of the camera stream by the player size, and the
    // lowest frame rate the choice may go to.
    Q_PROPERTY(bool autoQuality READ autoQuality WRITE setAutoQuality NOTIFY settingsChanged)
    Q_PROPERTY(double minTileFps READ minTileFps WRITE setMinTileFps NOTIFY settingsChanged)
    // Smooth playback: frames shown by their timestamps after a jitter buffer.
    Q_PROPERTY(bool smoothPlayback READ smoothPlayback WRITE setSmoothPlayback NOTIFY settingsChanged)
    Q_PROPERTY(int smoothBufferMs READ smoothBufferMs WRITE setSmoothBufferMs NOTIFY settingsChanged)
    // Output device for the sound ("" = system default) and the devices found.
    Q_PROPERTY(QString audioDevice READ audioDevice WRITE setAudioDevice NOTIFY settingsChanged)
    Q_PROPERTY(QStringList audioDevices READ audioDevices NOTIFY audioDevicesChanged)

public:
    AppController(QQmlEngine *engine, bool glSink, const QString &configPath, QObject *parent = nullptr);
    ~AppController() override;

    QList<QObject *> windowsList() const;
    QVariantList screens() const;
    bool glSink() const { return m_glSink; }
    QString renderBackend() const;
    QString hardwareDecoders() const;
    QString version() const;
    QString configPath() const { return m_configPath; }
    QString productName() const { return QStringLiteral("CAMAI VMS"); }
    QString glRenderer() const { return m_glRenderer; }
    bool softwareRendering() const;
    QVariantList builtinWallpapers() const;
    QString decoderBalance() const { return m_balanceText; }
    Diagnostics *diag() const { return m_diag; }
    int streamWaitMs() const;
    void setStreamWaitMs(int ms);
    bool autoQuality() const;
    void setAutoQuality(bool on);
    double minTileFps() const;
    void setMinTileFps(double fps);
    bool smoothPlayback() const;
    void setSmoothPlayback(bool on);
    int smoothBufferMs() const;
    void setSmoothBufferMs(int ms);
    QString audioDevice() const;
    void setAudioDevice(const QString &name);
    QStringList audioDevices() const { return m_audioDevices; }
    Q_INVOKABLE void refreshAudioDevices();
    // Reads resolution / frame rate / codec / audio of a stream (async);
    // the result comes with probeFinished(id, ...).
    Q_INVOKABLE int probeStream(const QString &url, const QString &user, const QString &password);
    // Channels of a CAMAI device (async, result in camaiChannelsReady).
    Q_INVOKABLE int camaiChannels(const QString &service, const QString &user, const QString &password);
    Q_INVOKABLE QString camaiNormalize(const QString &service) const;
    // Streams offered by a camera / server: kind "onvif" (address = camera
    // host[:port]) or "flussonic" (address = any URL of the stream).
    Q_INVOKABLE int discoverStreams(const QString &kind, const QString &address,
                                    const QString &user, const QString &password);
    QList<Player *> allPlayers() const;
    QString decoderEnvironment() const;   // kernel + available hardware decoders
    bool balanceOff() const { return m_balanceOff; }
    Q_INVOKABLE void openDiagnostics();

    void load();
    bool listen(const QString &serverName);

    Q_INVOKABLE VmsWindow *newWindow();
    Q_INVOKABLE void openWindow(VmsWindow *w);
    Q_INVOKABLE void closeWindow(VmsWindow *w);           // window "X" pressed
    Q_INVOKABLE void deleteWindow(VmsWindow *w);
    Q_INVOKABLE void moveToScreen(VmsWindow *w, const QString &screenName);
    Q_INVOKABLE void setFullScreen(VmsWindow *w, bool on);
    Q_INVOKABLE void toggleFullScreen(VmsWindow *w);
    Q_INVOKABLE int openWindowCount() const;
    Q_INVOKABLE void save();
    Q_INVOKABLE void quit();

Q_SIGNALS:
    void windowsChanged();
    void screensChanged();
    void glInfoChanged();
    void decoderBalanceChanged();
    void settingsChanged();
    void audioDevicesChanged();
    void probeFinished(int id, const QVariantMap &profile, const QString &error);
    void camaiChannelsReady(int id, const QVariantList &channels, const QString &error);
    void discoverFinished(int id, const QVariantList &profiles, const QString &error);

private:
    void addWindow(VmsWindow *w);
    void createView(VmsWindow *w);
    void destroyView(VmsWindow *w);
    void trackView(VmsWindow *w, QQuickWindow *view);
    void scheduleSave();
    void shutdown();
    QColor nextColor() const;

    QQmlEngine *m_engine;
    QQmlComponent *m_windowComponent = nullptr;
    bool m_glSink;
    QString m_configPath;
    QList<VmsWindow *> m_windows;
    QHash<VmsWindow *, QPointer<QQuickWindow>> m_views;
    QTimer m_saveTimer;
    QTimer m_fpsTimer;
    QElapsedTimer m_fpsClock;
    QHash<VmsWindow *, std::shared_ptr<std::atomic<int>>> m_frameCounters;
    QString m_glRenderer;
    // Hardware decoder / CPU load balancing.
    void balanceDecoders();
    qreal readCpuLoad();
    QTimer m_balanceTimer;
    qint64 m_lastBalanceMs = 0;
    quint64 m_cpuIdle = 0;
    quint64 m_cpuTotal = 0;
    qreal m_hwCapacity = 0;      // decoded pixels/s the VPU managed when saturated (persisted)
    bool m_verifyMove = false;   // check whether the last move to software helped
    bool m_balanceOff = false;   // moving did not help: the bottleneck is the source/network
    QString m_balanceText;
    Diagnostics *m_diag = nullptr;
    QPointer<QQuickWindow> m_diagView;
    QLocalServer *m_server = nullptr;
    bool m_quitting = false;
    int m_probeId = 0;
    QStringList m_audioDevices;
    void settingsUpdated();
    // Window not visible (minimised, not shown by the compositor, display
    // switched off): its players stop after a delay.
    void updateWindowVisibility(VmsWindow *w);
    bool eventFilter(QObject *obj, QEvent *ev) override;
};
