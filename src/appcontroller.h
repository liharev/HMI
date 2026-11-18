// Application controller: windows, persistence, screens, single instance IPC.
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

class QQmlEngine;
class QQmlComponent;
class QQuickWindow;
class QLocalServer;
class VmsWindow;

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
    QLocalServer *m_server = nullptr;
    bool m_quitting = false;
};
