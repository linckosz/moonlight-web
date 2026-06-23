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

#pragma once

#include <QByteArray>
#include <cstdint>

// Video codec preferences — controls RTSP negotiation and WebCodecs config.
//
// The enum values match the bit positions of the corresponding
// VIDEO_FORMAT_MASK_* macros in moonlight-common-c, so computeVideoFormats()
// can build the supportedVideoFormats bitmask from them.

enum class VideoCodec
{
    Auto = -1, // HEVC preferred, H.264 fallback (default)
    H264 = 0,
    HEVC = 1,
    AV1 = 2
};

enum class ChromaSampling
{
    C420 = 0,
    C444 = 1
};

// Stream configuration — 1080p60, HEVC preferred, 20 Mbps
struct StreamConfig
{
    static constexpr int kWidth = 1920;
    static constexpr int kHeight = 1080;
    static constexpr int kFps = 60;
    static constexpr int kBitrateKbps = 20000;
    static constexpr int kPacketSize = 1024;
    static constexpr int kMaxRefFrames = 1;
    static constexpr int kClientRefreshRateX100 = 6000;
    static constexpr int kVideoEncoderSlicesPerFrame = 1;

    // Codec preferences — will be user-selectable later
    VideoCodec codec = VideoCodec::Auto;
    ChromaSampling chroma = ChromaSampling::C420;
    bool hdrEnabled = false;

    // Audio: stereo Opus, 5ms packets
    static constexpr int kAudioChannels = 2;
    static constexpr int kAudioChannelMask = 3; // FL | FR
    static constexpr int kAudioPacketDuration = 5;
    static constexpr int kAudioQuality = 0; // 0=normal, 1=high

    // Generated per session
    QByteArray rikey; // 16 random bytes (AES key for streams)
    int rikeyid = 0;  // random uint32 (IV prefix)

    void generateKeys();

    // Build the supportedVideoFormats bitmask (VIDEO_FORMAT_* flags from Limelight.h)
    // based on the codec/chroma preferences.
    int computeVideoFormats() const;

    // colorSpace value for STREAM_CONFIGURATION.colorSpace. Returns 1 (BT.709 SDR).
    int computeColorSpace() const;
};
