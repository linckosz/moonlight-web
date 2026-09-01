/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "encode/windows/VplApi.h"
#include "encode/windows/VplSession.h"
#endif

#include <cstdio>
#include <string>

// Only the Windows headers above declare the namespace: elsewhere the body of
// this file is a single "skipped" line, and opening mw::native would not
// compile.
#if defined(_WIN32)
using namespace mw::native;
#endif

// The Intel encoder cannot be exercised without Intel hardware, and there is
// none on the bench it was written on. What CAN be checked here is the part
// most likely to be silently wrong: the encode parameters.
//
// These are not a substitute for running it. They cover the arithmetic and the
// invariants — the failures that would otherwise show up as "the bitrate
// setting does nothing" or "the picture is subtly cropped" on someone else's
// machine, long after the code was written.

void run_vpl_params_tests()
{
    SECTION("oneVPL — encode parameters (no Intel hardware needed)");

#if !defined(_WIN32)
    std::fprintf(stderr, "  skipped: Windows only\n");
#else
    // ── The runtime is absent here, and must say so plainly ─────────────────
    {
        const encode::VplApi* api = encode::VplApi::instance();
        std::fprintf(stderr, "  oneVPL runtime: %s\n",
                     api->available() ? "present" : api->unavailableReason().c_str());
        // Whatever the answer, it must be self-consistent: unavailable always
        // carries a reason, available never leaves one behind.
        if (api->available())
            CHECK(api->unavailableReason().empty());
        else
            CHECK(!api->unavailableReason().empty());
    }

    // ── Latency settings, which are the whole point of this configuration ───
    {
        mfxVideoParam p = {};
        CHECK(encode::fillEncodeParams(p, Codec::H264, 1920, 1080, 60, 20000));

        CHECK_EQ(p.mfx.CodecId, static_cast<mfxU32>(MFX_CODEC_AVC));
        // In video memory: the surface comes from our own conversion pass, and
        // any other IOPattern would mean a copy through system memory.
        CHECK_EQ(p.IOPattern, static_cast<mfxU16>(MFX_IOPATTERN_IN_VIDEO_MEMORY));
        // One frame in, one frame out. A deeper pipeline trades latency for
        // throughput, which is the opposite of the trade made here.
        CHECK_EQ(p.AsyncDepth, static_cast<mfxU16>(1));
        // No B-frames: one would reference a picture not yet sent.
        CHECK_EQ(p.mfx.GopRefDist, static_cast<mfxU16>(1));
        CHECK_EQ(p.mfx.RateControlMethod, static_cast<mfxU16>(MFX_RATECONTROL_CBR));
        // No periodic keyframe: a bitrate spike is what provokes the loss that
        // asks for the next one.
        CHECK(p.mfx.GopPicSize >= 0xFFFF);
    }

    // ── Bitrate: the 16-bit field, and the multiplier that rescues it ───────
    //
    // TargetKbps is mfxU16, so anything at or above 65535 kbps needs
    // BRCParamMultiplier. MoonlightWeb offers up to 150 Mbps, so this is not a
    // theoretical edge — without the multiplier a 100 Mbps request would wrap
    // to a fraction of itself and look like the setting being ignored.
    {
        mfxVideoParam low = {};
        CHECK(encode::fillEncodeParams(low, Codec::H264, 1920, 1080, 60, 20000));
        CHECK_EQ(low.mfx.BRCParamMultiplier, static_cast<mfxU16>(1));
        CHECK_EQ(low.mfx.TargetKbps, static_cast<mfxU16>(20000));

        // The maximum the UI offers.
        mfxVideoParam high = {};
        CHECK(encode::fillEncodeParams(high, Codec::Hevc, 3840, 2160, 60, 150000));
        CHECK(high.mfx.BRCParamMultiplier > 1);
        const int effective = static_cast<int>(high.mfx.TargetKbps) * high.mfx.BRCParamMultiplier;
        std::fprintf(stderr, "  150000 kbps -> %u x %u = %d kbps\n", high.mfx.TargetKbps,
                     high.mfx.BRCParamMultiplier, effective);
        // Within one multiplier step of what was asked for, and never zero.
        CHECK(high.mfx.TargetKbps > 0);
        CHECK(effective > 140000);
        CHECK(effective <= 150000 + high.mfx.BRCParamMultiplier);
        // CBR means the peak is the target.
        CHECK_EQ(high.mfx.MaxKbps, high.mfx.TargetKbps);
        // And the buffer must survive the division too — a zero-sized buffer
        // is refused by the runtime.
        CHECK(high.mfx.BufferSizeInKB > 0);
    }

    // ── Geometry: aligned surface, exact crop ───────────────────────────────
    //
    // Intel hardware wants 16-aligned surface dimensions; the crop is what the
    // picture really is. Getting this backwards shows up as a few pixels of
    // garbage at the edge, or as a stream quietly smaller than the display.
    {
        // 1080 is not a multiple of 16 — the case that catches the mistake.
        mfxVideoParam p = {};
        CHECK(encode::fillEncodeParams(p, Codec::Hevc, 1920, 1080, 60, 20000));

        CHECK_EQ(p.mfx.FrameInfo.CropW, static_cast<mfxU16>(1920));
        CHECK_EQ(p.mfx.FrameInfo.CropH, static_cast<mfxU16>(1080));
        CHECK_EQ(p.mfx.FrameInfo.Width, static_cast<mfxU16>(1920));  // already aligned
        CHECK_EQ(p.mfx.FrameInfo.Height, static_cast<mfxU16>(1088)); // 1080 -> 1088
        CHECK(p.mfx.FrameInfo.Width >= p.mfx.FrameInfo.CropW);
        CHECK(p.mfx.FrameInfo.Height >= p.mfx.FrameInfo.CropH);
        CHECK_EQ(p.mfx.FrameInfo.FourCC, static_cast<mfxU32>(MFX_FOURCC_NV12));
        CHECK_EQ(p.mfx.FrameInfo.FrameRateExtN, static_cast<mfxU32>(60));
        CHECK_EQ(p.mfx.FrameInfo.FrameRateExtD, static_cast<mfxU32>(1));
    }

    // ── An odd, unaligned size still comes out legal ────────────────────────
    {
        mfxVideoParam p = {};
        CHECK(encode::fillEncodeParams(p, Codec::Av1, 2560, 1440, 165, 55000));
        CHECK_EQ(p.mfx.CodecId, static_cast<mfxU32>(MFX_CODEC_AV1));
        CHECK_EQ(p.mfx.FrameInfo.Width, static_cast<mfxU16>(2560));
        CHECK_EQ(p.mfx.FrameInfo.Height, static_cast<mfxU16>(1440));
        CHECK_EQ(p.mfx.FrameInfo.FrameRateExtN, static_cast<mfxU32>(165));
        CHECK(p.mfx.BufferSizeInKB > 0);
    }

    // ── Degenerate inputs are corrected, not propagated ─────────────────────
    {
        mfxVideoParam p = {};
        CHECK(encode::fillEncodeParams(p, Codec::H264, 1280, 720, 0, 0));
        // Zero fps and zero bitrate would divide by zero downstream.
        CHECK_EQ(p.mfx.FrameInfo.FrameRateExtN, static_cast<mfxU32>(60));
        CHECK(p.mfx.TargetKbps > 0);
        CHECK(p.mfx.BufferSizeInKB > 0);
    }
#endif
}
