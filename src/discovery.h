// Discovery of the streams a camera / server offers, as stream profiles:
//  - ONVIF (Media service): GetProfiles + GetStreamUri, WS-UsernameToken
//    (PasswordDigest) with the camera clock offset, HTTP Digest by Qt;
//  - Flussonic: media_info.json of a stream; a transcoded stream has several
//    video tracks (v1, v2, ...), each one becomes a profile (HLS track URL).
#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

#include "streamprofile.h"

namespace discovery {

using Done = std::function<void(const QList<StreamProfile> &, const QString &error)>;

// address: "192.168.1.64", "192.168.1.64:8000" or the device service URL.
void onvif(const QString &address, const QString &user, const QString &password, QObject *context, Done done);

// streamUrl: any Flussonic URL of the stream (HLS, MPEG-TS, RTSP, ...).
void flussonic(const QString &streamUrl, const QString &user, const QString &password, QObject *context, Done done);

// Flussonic URL of one video track, derived from a URL of the stream.
QString flussonicTrackUrl(const QString &streamUrl, const QString &trackId);

} // namespace discovery
