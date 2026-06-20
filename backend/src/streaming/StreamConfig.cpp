/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
