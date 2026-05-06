#pragma once

#include <QByteArray>
#include <cstdint>

// Fixed MVP stream configuration — 1080p60 H.264, 20 Mbps
struct StreamConfig {
    static constexpr int kWidth = 1920;
    static constexpr int kHeight = 1080;
    static constexpr int kFps = 60;
    static constexpr int kBitrateKbps = 20000;
    static constexpr int kPacketSize = 1024;
    static constexpr int kMaxRefFrames = 1;
    static constexpr int kClientRefreshRateX100 = 6000;
    static constexpr int kVideoEncoderSlicesPerFrame = 1;
    static constexpr int kVideoFormat = 0;   // 0=H.264, 1=HEVC, 2=AV1
    static constexpr int kChromaSampling = 0; // 0=4:2:0, 1=4:4:4

    // Audio: stereo Opus, 5ms packets
    static constexpr int kAudioChannels = 2;
    static constexpr int kAudioChannelMask = 3;  // FL | FR
    static constexpr int kAudioPacketDuration = 5;
    static constexpr int kAudioQuality = 0;       // 0=normal, 1=high

    // Generated per session
    QByteArray rikey;   // 16 random bytes (AES key for streams)
    int rikeyid = 0;    // random uint32 (IV prefix)

    void generateKeys();
};
