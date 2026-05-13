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
        // Base: AV1 Main8 + HEVC Main + H.264 (higher profiles added conditionally below)
        fmt |= VIDEO_FORMAT_AV1_MAIN8 | VIDEO_FORMAT_H265 | VIDEO_FORMAT_MASK_H264;
        break;
    case VideoCodec::Auto:
    case VideoCodec::HEVC:
        // Base: HEVC Main + H.264 (higher profiles added conditionally below)
        fmt |= VIDEO_FORMAT_H265 | VIDEO_FORMAT_MASK_H264;
        break;
    case VideoCodec::H264:
        fmt |= VIDEO_FORMAT_MASK_H264;
        break;
    }

    // HDR adds 10-bit profiles (HEVC Main10, HEVC RExt10, AV1 Main10, AV1 High10)
    if (hdr == HdrMode::HDR) {
        fmt |= VIDEO_FORMAT_MASK_10BIT;
    }

    // Chroma 4:4:4 adds YUV444 profiles for all codecs
    if (chroma == ChromaSampling::C444) {
        fmt |= VIDEO_FORMAT_MASK_YUV444;
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
