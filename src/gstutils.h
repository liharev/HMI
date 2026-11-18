// Small helpers around GStreamer: element discovery and decoder selection.
#pragma once

#include <gst/gst.h>
#include <QString>
#include <QStringList>

namespace gsu {

enum class Codec { Unknown, H264, H265, Jpeg, Raw };

// Matches Player::decoder values stored in the configuration.
enum class DecoderPref { Auto = 0, Hardware = 1, Software = 2 };

bool hasElement(const char *factory);

// Factory names in priority order. Hardware lists cover Rockchip MPP
// (vendor kernel), V4L2 stateless/stateful (mainline kernel: hantro/rkvdec),
// VA-API (x86) and NVIDIA Jetson, so the same binary runs everywhere.
QStringList hardwareDecoders(Codec c);
QStringList softwareDecoders(Codec c);
QStringList decoderCandidates(Codec c, DecoderPref pref);
bool isHardwareDecoder(const QString &factory);

QString codecName(Codec c);
Codec codecFromRtpEncoding(const QString &encodingName);

// Sets a property from its string form only if the element has it
// (properties differ between GStreamer versions and vendor plugins).
bool setArg(GstElement *e, const char *prop, const QString &value);
bool hasProperty(GstElement *e, const char *prop);

// Human readable list of hardware decoders present on this system.
QString hardwareSummary();

} // namespace gsu
