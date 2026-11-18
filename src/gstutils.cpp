#include "gstutils.h"

namespace gsu {

bool hasElement(const char *factory)
{
    GstElementFactory *f = gst_element_factory_find(factory);
    if (!f)
        return false;
    gst_object_unref(f);
    return true;
}

QStringList hardwareDecoders(Codec c)
{
    switch (c) {
    case Codec::H264:
        return {"mppvideodec", "v4l2slh264dec", "v4l2h264dec", "nvv4l2decoder", "vah264dec"};
    case Codec::H265:
        return {"mppvideodec", "v4l2slh265dec", "v4l2h265dec", "nvv4l2decoder", "vah265dec"};
    case Codec::Jpeg:
        return {"mppjpegdec", "v4l2jpegdec", "nvjpegdec", "vajpegdec"};
    default:
        return {};
    }
}

QStringList softwareDecoders(Codec c)
{
    switch (c) {
    case Codec::H264:
        return {"avdec_h264", "openh264dec"};
    case Codec::H265:
        return {"avdec_h265", "libde265dec"};
    case Codec::Jpeg:
        return {"jpegdec", "avdec_mjpeg"};
    default:
        return {};
    }
}

QStringList decoderCandidates(Codec c, DecoderPref pref)
{
    switch (pref) {
    case DecoderPref::Hardware:
        return hardwareDecoders(c);
    case DecoderPref::Software:
        return softwareDecoders(c);
    case DecoderPref::Auto:
    default:
        return hardwareDecoders(c) + softwareDecoders(c);
    }
}

bool isHardwareDecoder(const QString &factory)
{
    // Developer aid for testing the fallbacks on machines without a VPU.
    static const bool fakeHw = qEnvironmentVariableIsSet("HMI_TEST_FAKE_HW");
    if (fakeHw && factory.startsWith(QLatin1String("avdec")))
        return true;
    for (Codec c : {Codec::H264, Codec::H265, Codec::Jpeg})
        if (hardwareDecoders(c).contains(factory))
            return true;
    return false;
}

QString codecName(Codec c)
{
    switch (c) {
    case Codec::H264: return QStringLiteral("H.264");
    case Codec::H265: return QStringLiteral("H.265");
    case Codec::Jpeg: return QStringLiteral("MJPEG");
    case Codec::Raw:  return QStringLiteral("RAW");
    default:          return QString();
    }
}

Codec codecFromRtpEncoding(const QString &enc)
{
    const QString e = enc.toUpper();
    if (e == QLatin1String("H264"))
        return Codec::H264;
    if (e == QLatin1String("H265") || e == QLatin1String("HEVC"))
        return Codec::H265;
    if (e == QLatin1String("JPEG") || e == QLatin1String("MJPEG"))
        return Codec::Jpeg;
    return Codec::Unknown;
}

bool hasProperty(GstElement *e, const char *prop)
{
    return e && g_object_class_find_property(G_OBJECT_GET_CLASS(e), prop) != nullptr;
}

bool setArg(GstElement *e, const char *prop, const QString &value)
{
    if (!hasProperty(e, prop))
        return false;
    gst_util_set_object_arg(G_OBJECT(e), prop, value.toUtf8().constData());
    return true;
}

QString hardwareSummary()
{
    QStringList found;
    QStringList all;
    for (Codec c : {Codec::H264, Codec::H265, Codec::Jpeg})
        for (const QString &f : hardwareDecoders(c))
            if (!all.contains(f))
                all << f;
    for (const QString &f : all)
        if (hasElement(f.toUtf8().constData()))
            found << f;
    return found.isEmpty() ? QStringLiteral("нет (программное декодирование)")
                           : found.join(QStringLiteral(", "));
}

} // namespace gsu
