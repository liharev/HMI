// CAMAI device integration (REST, see RTSP_PREVIEW_INTEGRATION.md):
//   GET /api/v1/preview  catalogue of channels and their stream profiles
//   GET /api/v1/status   state of the inputs (signal / splash) and encoders
// One CamaiService per device, shared by all players showing its channels.
#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include <functional>

#include "streamprofile.h"

class CamaiService : public QObject
{
    Q_OBJECT
public:
    struct ChannelState
    {
        bool known = false;
        bool signal = true;
        bool splash = false;
        QString description;
    };

    // Shared instance for a device (base URL such as http://192.168.53.168:8080).
    static CamaiService *get(const QString &service, const QString &user, const QString &password);
    // Normalised base URL ("192.168.53.168" -> "http://192.168.53.168:8080").
    static QString normalize(const QString &service);

    QString baseUrl() const { return m_base; }
    void setRtspHost(const QString &hostPort);   // replaces host[:port] of rtsp_uri (proxies), "" = as given

    // Players register to get catalogue / status updates (status is polled
    // only while somebody is registered).
    void addUser(QObject *user);
    void removeUser(QObject *user);

    bool catalogKnown() const { return m_catalogKnown; }
    QList<StreamProfile> profiles(int channel) const { return m_catalog.value(channel); }
    QList<int> channels() const;
    ChannelState state(int channel) const { return m_state.value(channel); }
    QString lastError() const { return m_error; }

    // Reads the catalogue again (e.g. a profile answered RTSP 404).
    void refreshCatalog();

    // One-shot request for the "add camera" dialog: list of channels as
    // QVariantMaps {id, signal, splash, description, profiles:[...]}.
    static void fetchChannels(const QString &service, const QString &user, const QString &password,
                              QObject *context, std::function<void(const QVariantList &, const QString &)> done);

    // Profiles of a catalogue channel (JSON layout of /api/v1/preview).
    static QList<StreamProfile> parseProfiles(const QJsonArray &profiles, const QString &rtspHost);

Q_SIGNALS:
    void catalogChanged();
    void statusChanged();

private:
    CamaiService(const QString &base, const QString &user, const QString &password);
    void get(const QString &path, std::function<void(const QByteArray &, const QString &)> done);
    void pollStatus();

    QString m_base;
    QString m_user;
    QString m_password;
    QString m_rtspHost;
    QNetworkAccessManager m_nam;
    QTimer m_statusTimer;
    QTimer m_catalogTimer;
    bool m_catalogKnown = false;
    bool m_catalogBusy = false;
    bool m_statusBusy = false;
    qint64 m_lastCatalogMs = 0;
    QHash<int, QList<StreamProfile>> m_catalog;
    QHash<int, ChannelState> m_state;
    QList<QPointer<QObject>> m_users;
    QString m_error;
};
