/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "convert/CursorBlend.h"
#include "native_test_framework.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace mw::native::convert;

namespace {

/// A 4:2:0 picture filled with one luma and one chroma code, 8 or 10 bit.
struct Picture
{
    int width, height;
    bool tenBit;
    std::vector<uint8_t> y, uv;

    Picture(int w, int h, bool ten, int luma, int chroma)
        : width(w)
        , height(h)
        , tenBit(ten)
    {
        const size_t bpp = ten ? 2 : 1;
        y.resize(static_cast<size_t>(w) * h * bpp);
        uv.resize(static_cast<size_t>(w) * (h / 2) * bpp);
        for (int i = 0; i < w * h; ++i)
            put(y.data(), i, luma);
        for (int i = 0; i < w * (h / 2); ++i)
            put(uv.data(), i, chroma);
    }
    void put(uint8_t* plane, int i, int code)
    {
        if (tenBit) {
            const uint16_t v = static_cast<uint16_t>(code << 6);
            std::memcpy(plane + static_cast<size_t>(i) * 2, &v, 2);
        } else {
            plane[i] = static_cast<uint8_t>(code);
        }
    }
    int luma(int x, int yy) const
    {
        if (tenBit) {
            uint16_t v;
            std::memcpy(&v, y.data() + (static_cast<size_t>(yy) * width + x) * 2, 2);
            return v >> 6;
        }
        return y[static_cast<size_t>(yy) * width + x];
    }
    int chroma(int sample, int row) const // sample = x for Cb, x+1 for Cr
    {
        if (tenBit) {
            uint16_t v;
            std::memcpy(&v, uv.data() + (static_cast<size_t>(row) * width + sample) * 2, 2);
            return v >> 6;
        }
        return uv[static_cast<size_t>(row) * width + sample];
    }
    PlaneViews views()
    {
        PlaneViews p;
        p.y = y.data();
        p.yStride = static_cast<size_t>(width) * (tenBit ? 2 : 1);
        p.uv = uv.data();
        p.uvStride = p.yStride;
        p.width = width;
        p.height = height;
        p.tenBit = tenBit;
        return p;
    }
};

/// The same 4:2:0 picture with the chroma in TWO planes — I420, what the CPU
/// converter writes because that is what OpenH264 reads. 8-bit only: there is
/// no 10-bit CPU path.
struct PlanarPicture
{
    int width, height;
    std::vector<uint8_t> y, u, v;

    PlanarPicture(int w, int h, int luma, int chroma)
        : width(w)
        , height(h)
    {
        y.assign(static_cast<size_t>(w) * h, static_cast<uint8_t>(luma));
        u.assign(static_cast<size_t>(w / 2) * (h / 2), static_cast<uint8_t>(chroma));
        v.assign(static_cast<size_t>(w / 2) * (h / 2), static_cast<uint8_t>(chroma));
    }
    int luma(int x, int yy) const { return y[static_cast<size_t>(yy) * width + x]; }
    int cb(int x, int yy) const { return u[static_cast<size_t>(yy / 2) * (width / 2) + x / 2]; }
    int cr(int x, int yy) const { return v[static_cast<size_t>(yy / 2) * (width / 2) + x / 2]; }
    PlaneViews views()
    {
        PlaneViews p;
        p.y = y.data();
        p.yStride = static_cast<size_t>(width);
        p.uv = u.data();
        p.uvStride = static_cast<size_t>(width / 2);
        p.v = v.data();
        p.vStride = static_cast<size_t>(width / 2);
        p.width = width;
        p.height = height;
        return p;
    }
};

/// A solid premultiplied BGRA square of `size` with `margin` transparent
/// pixels around it (so the canvas is bigger than the ink, like a real arrow).
std::vector<uint8_t> square(int canvas, int margin, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    std::vector<uint8_t> px(static_cast<size_t>(canvas) * canvas * 4, 0);
    for (int y = margin; y < canvas - margin; ++y)
        for (int x = margin; x < canvas - margin; ++x) {
            uint8_t* p = px.data() + (static_cast<size_t>(y) * canvas + x) * 4;
            p[0] = static_cast<uint8_t>(b * a / 255);
            p[1] = static_cast<uint8_t>(g * a / 255);
            p[2] = static_cast<uint8_t>(r * a / 255);
            p[3] = a;
        }
    return px;
}

} // namespace

void run_cursor_blend_tests()
{
    SECTION("CursorBlend — the ink box is the covered pixels, not the canvas");
    {
        const std::vector<uint8_t> px = square(8, 2, 255, 255, 255, 255);
        const PreparedCursor c = prepareCursor(px.data(), 8, 8, BlendTarget::Nv12Bt709);
        CHECK_EQ(c.width, 8);
        CHECK_EQ(c.inkWidth, 6); // columns 2..5 covered → right edge at 5 → 6 wide from the left
        CHECK_EQ(c.inkHeight, 6);
        CHECK(!c.empty());
        const PreparedCursor none = prepareCursor(nullptr, 8, 8, BlendTarget::Nv12Bt709);
        CHECK(none.empty());
    }

    SECTION("CursorBlend — an opaque white square on black NV12 reads 235 / 128");
    {
        const std::vector<uint8_t> px = square(4, 0, 255, 255, 255, 255);
        const PreparedCursor c = prepareCursor(px.data(), 4, 4, BlendTarget::Nv12Bt709);
        Picture pic(32, 32, false, 16, 128);
        PlaneViews v = pic.views();
        const BlendRect r = blendCursor(c, CursorPlacement{8.0f, 8.0f, 1.0f}, v);
        CHECK_EQ(r.x, 8);
        CHECK_EQ(r.y, 8);
        CHECK_EQ(r.width, 4);
        CHECK_EQ(r.height, 4);
        CHECK_EQ(pic.luma(9, 9), 235);
        CHECK_EQ(pic.luma(8, 8), 235);
        CHECK_EQ(pic.luma(7, 9), 16); // untouched outside
        CHECK_EQ(pic.luma(12, 9), 16);
        CHECK_EQ(pic.chroma(8, 4), 128); // white is neutral
        CHECK_EQ(pic.chroma(9, 4), 128);
    }

    SECTION("CursorBlend — half coverage blends halfway, and pure blue tints the chroma");
    {
        const std::vector<uint8_t> px = square(4, 0, 255, 255, 255, 128);
        const PreparedCursor c = prepareCursor(px.data(), 4, 4, BlendTarget::Nv12Bt709);
        Picture pic(16, 16, false, 16, 128);
        PlaneViews v = pic.views();
        blendCursor(c, CursorPlacement{4.0f, 4.0f, 1.0f}, v);
        // 16 × 0.5 + (16 + 219) × 0.5 ≈ 125.5, ± the 8-bit premultiplication.
        CHECK(std::abs(pic.luma(5, 5) - 125) <= 2);

        const std::vector<uint8_t> blue = square(4, 0, 255, 0, 0, 255);
        const PreparedCursor cb = prepareCursor(blue.data(), 4, 4, BlendTarget::Nv12Bt709);
        Picture pic2(16, 16, false, 16, 128);
        PlaneViews v2 = pic2.views();
        blendCursor(cb, CursorPlacement{4.0f, 4.0f, 1.0f}, v2);
        // BT.709 blue: Y ≈ 16 + 219 × 0.0722 ≈ 32, Cb ≈ 128 + 224 × 0.5 = 240, Cr ≈ 118.
        CHECK(std::abs(pic2.luma(5, 5) - 32) <= 1);
        CHECK(std::abs(pic2.chroma(4, 2) - 240) <= 1);
        CHECK(std::abs(pic2.chroma(5, 2) - 118) <= 1);
    }

    SECTION("CursorBlend — magnified ×2 around the top-left, clipped at the picture's edge");
    {
        const std::vector<uint8_t> px = square(4, 0, 255, 255, 255, 255);
        const PreparedCursor c = prepareCursor(px.data(), 4, 4, BlendTarget::Nv12Bt709);
        Picture pic(16, 16, false, 16, 128);
        PlaneViews v = pic.views();
        // 4 px shape × 2 = 8 px, placed so 2 px hang off the right and bottom.
        const BlendRect r = blendCursor(c, CursorPlacement{10.0f, 10.0f, 2.0f}, v);
        CHECK_EQ(r.x, 10);
        CHECK_EQ(r.width, 6);
        CHECK_EQ(r.height, 6);
        CHECK_EQ(pic.luma(15, 15), 235);
        CHECK_EQ(pic.luma(11, 11), 235);
        CHECK_EQ(pic.luma(9, 11), 16);
        // A placement entirely outside touches nothing.
        const BlendRect none = blendCursor(c, CursorPlacement{40.0f, 40.0f, 2.0f}, v);
        CHECK(none.empty());
        const BlendRect neg = blendCursor(c, CursorPlacement{-20.0f, -20.0f, 1.0f}, v);
        CHECK(neg.empty());
    }

    SECTION("CursorBlend — save, blend, restore leaves the picture as it was");
    {
        const std::vector<uint8_t> px = square(6, 1, 0, 255, 0, 255);
        const PreparedCursor c = prepareCursor(px.data(), 6, 6, BlendTarget::Nv12Bt709);
        Picture pic(24, 24, false, 100, 90);
        // A gradient so a restore to the wrong place would show.
        for (int yy = 0; yy < 24; ++yy)
            for (int xx = 0; xx < 24; ++xx)
                pic.put(pic.y.data(), yy * 24 + xx, 40 + xx + yy);
        const std::vector<uint8_t> before = pic.y;
        const std::vector<uint8_t> beforeUv = pic.uv;
        PlaneViews v = pic.views();
        const CursorPlacement place{5.0f, 7.0f, 1.5f};
        PlanePatch patch;
        savePatch(v, blendFootprint(c, place, v), patch);
        CHECK(patch.valid());
        blendCursor(c, place, v);
        CHECK(pic.y != before); // it drew something
        restorePatch(v, patch);
        CHECK(pic.y == before);
        CHECK(pic.uv == beforeUv);
    }

    SECTION("CursorBlend — P010 BT.2020 PQ: white lands near 203-nit reference, black stays black");
    {
        const std::vector<uint8_t> px = square(4, 0, 255, 255, 255, 255);
        const PreparedCursor c = prepareCursor(px.data(), 4, 4, BlendTarget::P010Bt2020Pq);
        Picture pic(16, 16, true, 64, 512);
        PlaneViews v = pic.views();
        blendCursor(c, CursorPlacement{4.0f, 4.0f, 1.0f}, v);
        // PQ(203 nits) = 0.5806 → 64 + 876 × 0.5806 ≈ 573 (BT.2408's reference
        // white). Neutral chroma stays at 512.
        CHECK(std::abs(pic.luma(5, 5) - 573) <= 3);
        CHECK(std::abs(pic.chroma(4, 2) - 512) <= 2);
        CHECK(std::abs(pic.chroma(5, 2) - 512) <= 2);
        CHECK_EQ(pic.luma(3, 5), 64);
        // The 6 low bits of every written word are zero, as P010 defines.
        uint16_t w;
        std::memcpy(&w, pic.y.data() + (5 * 16 + 5) * 2, 2);
        CHECK_EQ(w & 0x3f, 0);

        const std::vector<uint8_t> black = square(4, 0, 0, 0, 0, 255);
        const PreparedCursor cbk = prepareCursor(black.data(), 4, 4, BlendTarget::P010Bt2020Pq);
        Picture pic2(16, 16, true, 500, 512);
        PlaneViews v2 = pic2.views();
        blendCursor(cbk, CursorPlacement{4.0f, 4.0f, 1.0f}, v2);
        CHECK_EQ(pic2.luma(5, 5), 64);
    }

    // ── I420: two chroma planes instead of one interleaved ───────────────────
    //
    // Same colour, different addressing. What these guard is the addressing —
    // that Cb and Cr land in their own planes at half the column, and not the
    // interleaved arithmetic applied to a plane half as wide, which would write
    // past the row and tint the wrong pixels.

    SECTION("CursorBlend — I420: a white square reads the same 235 / 128 as NV12");
    {
        const std::vector<uint8_t> px = square(4, 0, 255, 255, 255, 255);
        const PreparedCursor c = prepareCursor(px.data(), 4, 4, BlendTarget::Nv12Bt709);
        PlanarPicture pic(32, 32, 16, 128);
        PlaneViews v = pic.views();
        CHECK(v.planarChroma());
        const BlendRect r = blendCursor(c, CursorPlacement{8.0f, 8.0f, 1.0f}, v);
        CHECK_EQ(r.x, 8);
        CHECK_EQ(r.width, 4);
        CHECK_EQ(pic.luma(9, 9), 235);
        CHECK_EQ(pic.luma(7, 9), 16); // untouched outside
        CHECK_EQ(pic.luma(12, 9), 16);
        CHECK_EQ(pic.cb(9, 9), 128); // white is neutral
        CHECK_EQ(pic.cr(9, 9), 128);
    }

    SECTION("CursorBlend — I420: pure blue tints Cb and Cr in their own planes");
    {
        const std::vector<uint8_t> blue = square(4, 0, 255, 0, 0, 255);
        const PreparedCursor c = prepareCursor(blue.data(), 4, 4, BlendTarget::Nv12Bt709);
        PlanarPicture pic(16, 16, 16, 128);
        PlaneViews v = pic.views();
        blendCursor(c, CursorPlacement{4.0f, 4.0f, 1.0f}, v);
        CHECK(std::abs(pic.luma(5, 5) - 32) <= 1);
        CHECK(std::abs(pic.cb(5, 5) - 240) <= 1);
        CHECK(std::abs(pic.cr(5, 5) - 118) <= 1);
        // Outside the footprint both planes are untouched — the check that
        // catches a stride mistake, which shows up as a smear to the right.
        CHECK_EQ(pic.cb(15, 5), 128);
        CHECK_EQ(pic.cr(15, 5), 128);
        CHECK_EQ(pic.cb(5, 15), 128);
    }

    SECTION("CursorBlend — I420: save, blend, restore leaves both chroma planes as they were");
    {
        const std::vector<uint8_t> px = square(6, 1, 0, 255, 0, 255);
        const PreparedCursor c = prepareCursor(px.data(), 6, 6, BlendTarget::Nv12Bt709);
        PlanarPicture pic(24, 24, 100, 90);
        for (int yy = 0; yy < 24; ++yy)
            for (int xx = 0; xx < 24; ++xx)
                pic.y[static_cast<size_t>(yy) * 24 + xx] = static_cast<uint8_t>(40 + xx + yy);
        for (int i = 0; i < 12 * 12; ++i) {
            pic.u[static_cast<size_t>(i)] = static_cast<uint8_t>(60 + i % 40);
            pic.v[static_cast<size_t>(i)] = static_cast<uint8_t>(200 - i % 40);
        }
        const std::vector<uint8_t> beforeY = pic.y, beforeU = pic.u, beforeV = pic.v;
        PlaneViews v = pic.views();
        const CursorPlacement place{5.0f, 7.0f, 1.5f};
        PlanePatch patch;
        savePatch(v, blendFootprint(c, place, v), patch);
        CHECK(patch.valid());
        CHECK(!patch.v.empty()); // the Cr plane was saved too
        blendCursor(c, place, v);
        CHECK(pic.y != beforeY);
        CHECK(pic.u != beforeU);
        restorePatch(v, patch);
        CHECK(pic.y == beforeY);
        CHECK(pic.u == beforeU);
        CHECK(pic.v == beforeV);
    }
}
