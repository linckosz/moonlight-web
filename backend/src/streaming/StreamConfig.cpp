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
    case VideoCodec::H264: fmt |= VIDEO_FORMAT_MASK_H264; break;
    }

    // Chroma 4:4:4: add the YUV444 profile ONLY for the selected codec.
    // Offering every codec's 444 profile drags the negotiation into AV1 (which
    // moonlight-common-c tests first) and, when the host lacks AV1 444, silently
    // falls back to AV1 4:2:0 — switching codec for nothing. Restricting to the
    // chosen codec keeps the preference and lets the host pick its 444 variant.
    if (chroma == ChromaSampling::C444) {
        switch (codec) {
        case VideoCodec::H264: fmt |= VIDEO_FORMAT_H264_HIGH8_444; break;
        case VideoCodec::AV1: fmt |= VIDEO_FORMAT_AV1_HIGH8_444; break;
        case VideoCodec::Auto:
        case VideoCodec::HEVC: fmt |= VIDEO_FORMAT_H265_REXT8_444; break;
        }
    }

    return fmt;
}

int StreamConfig::computeColorSpace() const
{
    return 1; // BT.709 SDR
}
