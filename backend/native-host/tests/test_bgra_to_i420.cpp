/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "convert/BgraToI420.h"

#include <cstdlib>
#include <vector>

using namespace mw::native::convert;

namespace {

struct Planes
{
    int w, h;
    std::vector<uint8_t> y, u, v;
    Planes(int width, int height)
        : w(width)
        , h(height)
        , y(static_cast<size_t>(width) * height)
        , u(static_cast<size_t>(width / 2) * (height / 2))
        , v(static_cast<size_t>(width / 2) * (height / 2))
    {}
};

BgraToI420Params params(const std::vector<uint8_t>& src, size_t pitch, int sw, int sh, Planes& out,
                        bool rgb = false)
{
    BgraToI420Params p;
    p.src = src.data();
    p.srcPitch = pitch;
    p.srcWidth = sw;
    p.srcHeight = sh;
    p.rgbOrder = rgb;
    p.y = out.y.data();
    p.u = out.u.data();
    p.v = out.v.data();
    p.dstWidth = out.w;
    p.dstHeight = out.h;
    p.strideY = out.w;
    p.strideUV = out.w / 2;
    return p;
}

std::vector<uint8_t> solid(int w, int h, size_t pitch, uint8_t b, uint8_t g, uint8_t r)
{
    std::vector<uint8_t> px(pitch * h, 0);
    for (int row = 0; row < h; ++row)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = px.data() + pitch * row + static_cast<size_t>(x) * 4;
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = 255;
        }
    return px;
}

} // namespace

void run_bgra_to_i420_tests()
{
    SECTION("BGRA → I420 on the CPU — BT.709 limited range");

    // ── White and black land on the studio-range ends ─────────────────────────
    {
        Planes out(8, 4);
        const std::vector<uint8_t> white = solid(8, 4, 8 * 4, 255, 255, 255);
        bgraToI420Rows(params(white, 32, 8, 4, out), 0, 4);
        CHECK(out.y[0] >= 234 && out.y[0] <= 236);
        CHECK(out.u[0] == 128);
        CHECK(out.v[0] == 128);

        const std::vector<uint8_t> black = solid(8, 4, 8 * 4, 0, 0, 0);
        bgraToI420Rows(params(black, 32, 8, 4, out), 0, 4);
        CHECK_EQ(out.y[0], 16);
        CHECK(out.u[0] == 128);
        CHECK(out.v[0] == 128);
    }

    // ── Pure red: low luma, Cr high, Cb low — and RGB order agrees ───────────
    {
        Planes out(8, 4);
        const std::vector<uint8_t> red = solid(8, 4, 32, 0, 0, 255);
        bgraToI420Rows(params(red, 32, 8, 4, out), 0, 4);
        // BT.709: Y ≈ 63, Cb ≈ 102, Cr ≈ 240 in limited range.
        CHECK(out.y[0] >= 58 && out.y[0] <= 68);
        CHECK(out.u[0] < 110);
        CHECK(out.v[0] > 230);

        // The same red written R,G,B,X must give the same answer with rgbOrder.
        Planes out2(8, 4);
        const std::vector<uint8_t> redRgb = solid(8, 4, 32, 255, 0, 0); // B slot carries R
        bgraToI420Rows(params(redRgb, 32, 8, 4, out2, true), 0, 4);
        CHECK_EQ(out2.y[0], out.y[0]);
        CHECK_EQ(out2.v[0], out.v[0]);
    }

    // ── A pitch wider than the row is honoured ───────────────────────────────
    {
        Planes out(4, 2);
        std::vector<uint8_t> px(64 * 2, 0); // pitch 64 for a 16-byte row
        for (int x = 0; x < 4; ++x) {
            px[static_cast<size_t>(x) * 4 + 1] = 255; // green, row 0
            px[64 + static_cast<size_t>(x) * 4 + 1] = 255;
        }
        bgraToI420Rows(params(px, 64, 4, 2, out), 0, 2);
        CHECK(out.y[0] > 150); // green is bright
        CHECK(out.u[0] < 128 && out.v[0] < 128);
    }

    // ── Row bands compose: two halves equal one whole ────────────────────────
    {
        const int w = 32, h = 16;
        std::vector<uint8_t> px(static_cast<size_t>(w) * 4 * h);
        std::srand(7);
        for (uint8_t& b : px)
            b = static_cast<uint8_t>(std::rand());
        Planes whole(w, h), split(w, h);
        bgraToI420Rows(params(px, w * 4, w, h, whole), 0, h);
        bgraToI420Rows(params(px, w * 4, w, h, split), 0, h / 2);
        bgraToI420Rows(params(px, w * 4, w, h, split), h / 2, h);
        CHECK(whole.y == split.y);
        CHECK(whole.u == split.u);
        CHECK(whole.v == split.v);
    }

    // ── Scaling: a 2:1 downscale of a flat picture stays flat and in range ───
    {
        Planes out(8, 4);
        const std::vector<uint8_t> grey = solid(16, 8, 16 * 4, 128, 128, 128);
        bgraToI420Rows(params(grey, 64, 16, 8, out), 0, 4);
        // 128 sRGB grey: Y = 128*219/255 + 16 ≈ 126.
        for (uint8_t v : out.y)
            CHECK(v >= 124 && v <= 128);
        for (uint8_t v : out.u)
            CHECK_EQ(v, 128);
    }

    // ── Scaling: a vertical edge lands where the geometry says ───────────────
    {
        // Left half white, right half black, 32 wide; downscaled to 16 the edge
        // must be at column 8, with at most a one-pixel bilinear ramp.
        const int sw = 32, sh = 4;
        std::vector<uint8_t> px(static_cast<size_t>(sw) * 4 * sh, 0);
        for (int row = 0; row < sh; ++row)
            for (int x = 0; x < sw / 2; ++x) {
                uint8_t* p =
                    px.data() + static_cast<size_t>(row) * sw * 4 + static_cast<size_t>(x) * 4;
                p[0] = p[1] = p[2] = 255;
            }
        Planes out(16, 2);
        bgraToI420Rows(params(px, sw * 4, sw, sh, out), 0, 2);
        CHECK(out.y[6] > 220);
        CHECK(out.y[9] < 30);
    }
}
