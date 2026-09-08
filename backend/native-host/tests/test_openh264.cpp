/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include <cstdio>

// Only built for Windows and Linux (CMakeLists.txt): macOS uses VideoToolbox,
// which has its own software fall-back inside the same API, so there is no
// OpenH264Encoder to link against there — mirrors test_win32_cursor.cpp's
// pattern for a test whose subject does not exist on every platform.
#ifdef MW_NATIVE_OPENH264
#include "encode/OpenH264Encoder.h"

#include <chrono>
#include <cstring>
#include <vector>

using namespace mw::native;
using namespace mw::native::encode;

namespace {

/// A synthetic I420 picture with something to encode: a gradient that moves
/// with the frame number, so consecutive pictures differ the way a desktop
/// being scrolled does.
struct TestPicture
{
    int width, height;
    std::vector<uint8_t> planes;

    TestPicture(int w, int h)
        : width(w)
        , height(h)
        , planes(static_cast<size_t>(w) * h * 3 / 2)
    {}

    void paint(int frame)
    {
        uint8_t* y = planes.data();
        for (int r = 0; r < height; ++r)
            for (int c = 0; c < width; ++c)
                y[static_cast<size_t>(r) * width + c] =
                    static_cast<uint8_t>((c + frame * 3) ^ (r >> 2));
        uint8_t* u = y + static_cast<size_t>(width) * height;
        uint8_t* v = u + static_cast<size_t>(width / 2) * (height / 2);
        std::memset(u, 128, static_cast<size_t>(width / 2) * (height / 2));
        std::memset(v, 128 + (frame & 15), static_cast<size_t>(width / 2) * (height / 2));
    }

    I420Picture view() const
    {
        I420Picture p;
        p.y = planes.data();
        p.u = p.y + static_cast<size_t>(width) * height;
        p.v = p.u + static_cast<size_t>(width / 2) * (height / 2);
        p.strideY = width;
        p.strideU = p.strideV = width / 2;
        p.width = width;
        p.height = height;
        return p;
    }
};

/// Whether the bitstream starts with an Annex B start code and carries an SPS:
/// what the browser's first keyframe has to look like (StreamView configures
/// its decoder from it).
bool looksLikeAnnexBWithSps(const uint8_t* data, size_t size)
{
    if (size < 5) return false;
    const bool startCode =
        data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1));
    if (!startCode) return false;
    for (size_t i = 0; i + 4 < size && i < 64; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1F) == 7)
            return true;
    }
    return false;
}

} // namespace
#endif // MW_NATIVE_OPENH264

void run_openh264_tests()
{
    SECTION("OpenH264 — the CPU encoder of last resort");

#ifndef MW_NATIVE_OPENH264
    std::fprintf(stderr, "  skipped: not built on this platform (VideoToolbox covers macOS)\n");
#else
    // ── Init, first picture is a keyframe with SPS/PPS in band ───────────────
    {
        OpenH264Encoder enc;
        std::string err;
        EncoderTuning tuning;
        CHECK(enc.init(640, 360, 60, 4000, 2, tuning, err));
        CHECK(err.empty());
        CHECK_EQ(enc.threads(), 2);

        TestPicture pic(640, 360);
        pic.paint(0);
        EncoderOutput out;
        CHECK(enc.encode(pic.view(), false, 0, out, err));
        CHECK(out.size > 0);
        CHECK(out.keyframe);
        CHECK(looksLikeAnnexBWithSps(out.data, out.size));
        enc.releaseOutput();

        // ── Deltas follow, and stay deltas without a periodic keyframe ──────
        int keyframes = 0;
        size_t deltaBytes = 0;
        for (int f = 1; f <= 30; ++f) {
            pic.paint(f);
            CHECK(enc.encode(pic.view(), false, static_cast<uint32_t>(f), out, err));
            CHECK(out.size > 0); // no frame skipping, ever
            if (out.keyframe) ++keyframes;
            deltaBytes += out.size;
            enc.releaseOutput();
        }
        CHECK_EQ(keyframes, 0);

        // ── A keyframe on request, and only then ─────────────────────────────
        pic.paint(31);
        CHECK(enc.encode(pic.view(), true, 31, out, err));
        CHECK(out.keyframe);
        CHECK(looksLikeAnnexBWithSps(out.data, out.size));
        enc.releaseOutput();

        // ── The bitrate moves without a restart ─────────────────────────────
        CHECK(enc.setBitrate(1000, err));
        CHECK(enc.setBitrate(8000, err));

        enc.stop();
    }

    // ── Odd sizes are refused with a message, not a crash ────────────────────
    {
        OpenH264Encoder enc;
        std::string err;
        EncoderTuning tuning;
        CHECK(!enc.init(641, 360, 60, 4000, 1, tuning, err));
        CHECK(!err.empty());
    }

    // ── Thread count: asked, capped, and picked from the machine ─────────────
    {
        OpenH264Encoder enc;
        std::string err;
        EncoderTuning tuning;
        CHECK(enc.init(320, 180, 30, 1000, 16, tuning, err));
        CHECK(enc.threads() <= 4);
        enc.stop();
        CHECK(enc.init(320, 180, 30, 1000, 0, tuning, err));
        CHECK(enc.threads() >= 1);
        CHECK(enc.threads() <= 4);
        enc.stop();
    }

    // ── What this machine does at 1080p: an inventory, not an assertion ──────
    {
        OpenH264Encoder enc;
        std::string err;
        EncoderTuning tuning;
        if (enc.init(1920, 1080, 60, 20000, 0, tuning, err)) {
            TestPicture pic(1920, 1080);
            EncoderOutput out;
            double totalMs = 0;
            size_t totalBytes = 0;
            const int frames = 30;
            for (int f = 0; f < frames; ++f) {
                pic.paint(f);
                const auto t0 = std::chrono::steady_clock::now();
                CHECK(enc.encode(pic.view(), false, static_cast<uint32_t>(f), out, err));
                totalMs +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
                totalBytes += out.size;
                enc.releaseOutput();
            }
            std::fprintf(
                stderr,
                "  openh264 1080p60 20 Mbit/s: %.2f ms/frame mean on %d thread(s), %zu KB/frame\n",
                totalMs / frames, enc.threads(), totalBytes / frames / 1024);
            enc.stop();
        }
    }
#endif // MW_NATIVE_OPENH264
}
