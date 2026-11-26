// Stream profiles of one camera (several resolutions / frame rates of the
// same picture) and the choice of the profile that fits a player best.
#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

#include <functional>

struct StreamProfile
{
    QString url;
    QString label;        // "720p", "stream_0", free text
    QString kind;         // "preview" (encoded on demand), "stream" (always running), ""
    QString source;       // "manual", "probe", "camai", "onvif", "flussonic"
    QString codec;        // "h264", "h265", "jpeg", "" (unknown)
    int width = 0;
    int height = 0;
    double fps = 0;
    int bitrateKbps = 0;
    int audio = -1;       // -1 unknown, 0 no audio track, 1 has audio

    bool known() const { return width > 0 && height > 0; }
    QString describe() const;     // "1280×720 · 15 к/с · H.264 · 900 кбит/с"

    QJsonObject toJson() const;
    static StreamProfile fromJson(const QJsonObject &o);
    QVariantMap toVariant() const;
    static StreamProfile fromVariant(const QVariantMap &m);
};

struct ProfileRequest
{
    double boxWidth = 0;          // device pixels of the player area (rotation applied)
    double boxHeight = 0;
    double minFps = 10;           // lighter profiles below this rate are avoided
    bool needAudio = false;
    QSet<QString> excluded;       // URLs that failed recently
};

namespace profiles {

// Index of the best profile: the lightest one (decoded pixels per second,
// software-only codecs count triple) that is not smaller than the player
// (10 % tolerance). If none is large enough, the largest one. Equal costs:
// an always running stream wins over an on-demand preview (no encoder start).
// -1 if the list has no usable profile.
int select(const QList<StreamProfile> &list, const ProfileRequest &req, QString *why = nullptr);

// Decoded pixels per second (with the software penalty).
double cost(const StreamProfile &p);

int indexOf(const QList<StreamProfile> &list, const QString &url);

// Reads the parameters of a stream (resolution, frame rate, codec, audio)
// without decoding it, in a worker thread. The callback runs in the thread of
// 'context' unless the context was destroyed meanwhile.
void probe(const QString &url, const QString &user, const QString &password, QObject *context,
           std::function<void(const StreamProfile &, const QString &error)> done, int timeoutSec = 12);

} // namespace profiles
