/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "streaming/StreamConfig.h"

extern "C" {
#include "Limelight.h"
}

void run_stream_config_tests()
{
    SECTION("StreamConfig");

    // Defaults.
    StreamConfig cfg;
    CHECK(cfg.codec == VideoCodec::Auto);
    CHECK(cfg.chroma == ChromaSampling::C420);
    CHECK_EQ(cfg.hdrEnabled, false);
    CHECK_EQ(cfg.computeColorSpace(), 1); // BT.709 SDR

    // Auto/HEVC base: HEVC Main + H.264 High.
    CHECK_EQ(cfg.computeVideoFormats(), VIDEO_FORMAT_H265 | VIDEO_FORMAT_H264);

    // H.264-only.
    cfg.codec = VideoCodec::H264;
    CHECK_EQ(cfg.computeVideoFormats(), VIDEO_FORMAT_H264);

    // AV1 base.
    cfg.codec = VideoCodec::AV1;
    CHECK_EQ(cfg.computeVideoFormats(),
             VIDEO_FORMAT_AV1_MAIN8 | VIDEO_FORMAT_H265 | VIDEO_FORMAT_H264);

    // HEVC + 4:4:4 adds the REXT8 444 profile.
    cfg.codec = VideoCodec::HEVC;
    cfg.chroma = ChromaSampling::C444;
    CHECK_EQ(cfg.computeVideoFormats(),
             VIDEO_FORMAT_H265 | VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265_REXT8_444);

    // HDR colorspace + extra HEVC 10-bit profile.
    cfg.chroma = ChromaSampling::C420;
    cfg.hdrEnabled = true;
    CHECK_EQ(cfg.computeColorSpace(), 6); // BT.2020 PQ
    CHECK((cfg.computeVideoFormats() & VIDEO_FORMAT_H265_MAIN10) != 0);

    // generateKeys produces a 16-byte AES key and (almost surely) varies.
    StreamConfig a, b;
    a.generateKeys();
    b.generateKeys();
    CHECK_EQ(a.rikey.size(), 16);
    CHECK_EQ(b.rikey.size(), 16);
    CHECK(a.rikey != b.rikey);
}
