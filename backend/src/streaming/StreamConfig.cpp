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

    // Base H.264 profile: High (0x0001) only — NOT VIDEO_FORMAT_MASK_H264 (0x000F),
    // which also covers H264_HIGH8_444 (0x0004). Advertising the full mask lets
    // Sunshine negotiate H.264 High 4:4:4 (profile_idc 244, avc1.f4002a) even when
    // 4:4:4 was not requested — undecodable by WebCodecs on Chrome (Win/Android/macOS;
    // only iOS VideoToolbox accepts it). The 4:4:4 profile is added back below only
    // when chroma == C444.
    switch (codec) {
    case VideoCodec::AV1:
        // Base: AV1 Main8 + HEVC Main + H.264 High (higher profiles added conditionally below)
        fmt |= VIDEO_FORMAT_AV1_MAIN8 | VIDEO_FORMAT_H265 | VIDEO_FORMAT_H264;
        break;
    case VideoCodec::Auto:
    case VideoCodec::HEVC:
        // Base: HEVC Main + H.264 High (higher profiles added conditionally below)
        fmt |= VIDEO_FORMAT_H265 | VIDEO_FORMAT_H264;
        break;
    case VideoCodec::H264: fmt |= VIDEO_FORMAT_H264; break;
    }

    // Chroma 4:4:4: add the YUV444 profile ONLY for the selected codec.
    if (chroma == ChromaSampling::C444) {
        switch (codec) {
        case VideoCodec::H264: fmt |= VIDEO_FORMAT_H264_HIGH8_444; break;
        case VideoCodec::AV1: fmt |= VIDEO_FORMAT_AV1_HIGH8_444; break;
        case VideoCodec::Auto:
        case VideoCodec::HEVC: fmt |= VIDEO_FORMAT_H265_REXT8_444; break;
        }
    }

    // HDR profiles: add 10-bit and 4:4:4 10-bit variants for the chosen codec.
    if (hdrEnabled) {
        switch (codec) {
        case VideoCodec::H264:
            // H.264 10-bit is not standard GameStream; ignore
            break;
        case VideoCodec::AV1:
            fmt |= VIDEO_FORMAT_AV1_MAIN10;
            if (chroma == ChromaSampling::C444) fmt |= VIDEO_FORMAT_AV1_HIGH10_444;
            break;
        case VideoCodec::Auto:
        case VideoCodec::HEVC:
            fmt |= VIDEO_FORMAT_H265_MAIN10;
            if (chroma == ChromaSampling::C444) fmt |= VIDEO_FORMAT_H265_REXT10_444;
            break;
        }
    }

    return fmt;
}

int StreamConfig::computeColorSpace() const
{
    return hdrEnabled ? 6 : 1; // 6=BT.2020+PQ(HDR10), 1=BT.709(SDR)
}
