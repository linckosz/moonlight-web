#include "StreamConfig.h"

extern "C" {
#include "Limelight.h"
}

#include <QRandomGenerator>

void StreamConfig::generateKeys()
{
    rikey.resize(16);
    for (int i = 0; i < 16; ++i)
        rikey[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    rikeyid = static_cast<int>(QRandomGenerator::global()->generate());
}

int StreamConfig::computeVideoFormats() const
{
    int fmt = 0;

    switch (codec) {
    case VideoCodec::AV1:
        fmt |= VIDEO_FORMAT_MASK_AV1;
        // fall through: AV1 -> HEVC -> H.264 chain
        [[fallthrough]];
    case VideoCodec::HEVC:
        fmt |= VIDEO_FORMAT_MASK_H265;
        [[fallthrough]];
    case VideoCodec::Auto:
        fmt |= VIDEO_FORMAT_MASK_H264;
        break;
    case VideoCodec::H264:
        fmt |= VIDEO_FORMAT_MASK_H264;
        break;
    }

    // Chroma 4:4:4 adds the YUV444 mask for all supported codecs
    if (chroma == ChromaSampling::C444) {
        fmt |= VIDEO_FORMAT_MASK_YUV444;
    }

    // HDR adds 10-bit profiles for H.265 and AV1
    if (hdr == HdrMode::HDR) {
        fmt |= VIDEO_FORMAT_MASK_10BIT;
    }

    return fmt;
}

int StreamConfig::computeColorSpace() const
{
    // 0 = BT.601 SDR, 1 = BT.709 SDR
    // Bit 0 (1): Rec 709, Bit 1 (2): Rec 2020 primaries
    // Bit 2 (4): PQ (ST 2084), Bit 3 (8): full-range encoding
    // BT.2020 + PQ (SMPTE 2084) = 2 | 4 = 6
    if (hdr == HdrMode::HDR) {
        return 6;  // BT.2020 + PQ (HDR10)
    }
    return 1;  // BT.709 SDR
}
