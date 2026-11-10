/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "native_test_framework.h"

#include "capture/CaptureTypes.h"
#include "capture/Orientation.h"
#include "capture/OrientedPointer.h"
#include "capture/linux/CompositorTransform.h"
#include "capture/windows/DisplayRotation.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace mw::native;
using namespace mw::native::capture;

// A picture held turned (Orientation.h), the pointer of a turned scanout
// (OrientedPointer.h) and the compositor's word on a display
// (CompositorTransform.h) — issue #54 on Linux. Pure: every platform runs it.

namespace {

/// All eight: four turns, mirrored or not.
std::vector<Orientation> allOrientations()
{
    std::vector<Orientation> all;
    for (int m = 0; m < 2; ++m)
        for (int k = 0; k < 4; ++k)
            all.push_back(Orientation{k, m == 1});
    return all;
}

/// A picture of width × height whose every pixel says where it is.
std::vector<uint32_t> numbered(int width, int height)
{
    std::vector<uint32_t> p(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            p[static_cast<size_t>(y) * width + x] =
                0x01000000u | (static_cast<uint32_t>(y) << 12) | static_cast<uint32_t>(x);
    return p;
}

/// The picture as a buffer holding it @p o would: each upright pixel put
/// where orientedTexel says, the buffer at orientedSize.
std::vector<uint32_t> hold(const std::vector<uint32_t>& upright, int width, int height,
                           Orientation o, int& heldWidth, int& heldHeight)
{
    orientedSize(width, height, o, heldWidth, heldHeight);
    std::vector<uint32_t> held(static_cast<size_t>(heldWidth) * heldHeight, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            int tx = 0, ty = 0;
            orientedTexel(x, y, width, height, o, tx, ty);
            held[static_cast<size_t>(ty) * heldWidth + tx] =
                upright[static_cast<size_t>(y) * width + x];
        }
    return held;
}

bool sameOrientation(Orientation a, int turns, bool mirrored)
{
    return a == Orientation{turns, mirrored};
}

} // namespace

void run_orientation_tests()
{
    SECTION("Orientation — a picture held turned, stood upright (issue #54, Linux and macOS)");

    // ── wl_output's numbering, which Mutter and PipeWire share ──
    CHECK(orientationFromWayland(0).upright());
    CHECK(sameOrientation(orientationFromWayland(1), 1, false));
    CHECK(sameOrientation(orientationFromWayland(2), 2, false));
    CHECK(sameOrientation(orientationFromWayland(3), 3, false));
    CHECK(sameOrientation(orientationFromWayland(4), 0, true));
    CHECK(sameOrientation(orientationFromWayland(5), 1, true));
    CHECK(sameOrientation(orientationFromWayland(7), 3, true));
    CHECK(orientationFromWayland(8).upright());
    CHECK(orientationFromWayland(1).swapsAxes());
    CHECK(!orientationFromWayland(2).swapsAxes());
    CHECK_EQ(orientationFromWayland(3).degrees(), 270);

    // ── Every orientation held then stood upright gives the picture back ──
    {
        const int w = 5, h = 3;
        const std::vector<uint32_t> upright = numbered(w, h);
        bool texelRoundTrip = true;
        bool copyRoundTrip = true;
        bool sizes = true;
        for (const Orientation o : allOrientations()) {
            int hw = 0, hh = 0;
            const std::vector<uint32_t> held = hold(upright, w, h, o, hw, hh);
            sizes = sizes && hw == (o.swapsAxes() ? h : w) && hh == (o.swapsAxes() ? w : h);
            // Every texel of the held picture is one upright pixel: no gaps.
            for (uint32_t v : held)
                copyRoundTrip = copyRoundTrip && v != 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    int tx = 0, ty = 0, ux = 0, uy = 0;
                    orientedTexel(x, y, w, h, o, tx, ty);
                    uprightPixel(tx, ty, w, h, o, ux, uy);
                    texelRoundTrip = texelRoundTrip && ux == x && uy == y;
                }
            // uprightCopy32 — with its row stride — undoes the hold exactly.
            std::vector<uint32_t> back(static_cast<size_t>(w) * h, 0);
            uprightCopy32(
                reinterpret_cast<const uint8_t*>(held.data()), static_cast<size_t>(hw) * 4, w, h, o,
                reinterpret_cast<uint8_t*>(back.data()), static_cast<size_t>(w) * 4, 0, h);
            copyRoundTrip = copyRoundTrip && back == upright;
        }
        CHECK(sizes);
        CHECK(texelRoundTrip);
        CHECK(copyRoundTrip);
    }

    // ── A quarter turn puts the held top row down the upright right side ──
    {
        int tx = 0, ty = 0;
        // Upright 4x2: its top-right pixel (3,0) is the held picture's (0,0).
        orientedTexel(3, 0, 4, 2, Orientation{1, false}, tx, ty);
        CHECK(tx == 0 && ty == 0);
        orientedTexel(3, 1, 4, 2, Orientation{1, false}, tx, ty);
        CHECK(tx == 1 && ty == 0);
        // And Windows' quarter turn is the same mapping (DisplayRotation.h).
        bool agree = true;
        for (int k = 0; k < 4; ++k)
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 5; ++x) {
                    int ax = 0, ay = 0, bx = 0, by = 0;
                    panelTexel(x, y, 5, 3, k, ax, ay);
                    orientedTexel(x, y, 5, 3, Orientation{k, false}, bx, by);
                    agree = agree && ax == bx && ay == by;
                }
        CHECK(agree);
    }

    // ── compose and inverse follow the pixels ──
    {
        const int w = 4, h = 3;
        bool composeFollows = true;
        bool inverses = true;
        for (const Orientation first : allOrientations())
            for (const Orientation second : allOrientations()) {
                int fw = 0, fh = 0;
                orientedSize(w, h, first, fw, fh);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        int ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
                        orientedTexel(x, y, w, h, first, ax, ay);
                        orientedTexel(ax, ay, fw, fh, second, bx, by);
                        orientedTexel(x, y, w, h, compose(second, first), cx, cy);
                        composeFollows = composeFollows && bx == cx && by == cy;
                    }
            }
        for (const Orientation o : allOrientations())
            inverses =
                inverses && compose(inverse(o), o).upright() && compose(o, inverse(o)).upright();
        CHECK(composeFollows);
        CHECK(inverses);
    }

    // ── The DRM plane's rotation: reflected first, then turned ──
    {
        CHECK(orientationFromDrmRotation(1u << 0).upright());
        CHECK(sameOrientation(orientationFromDrmRotation(1u << 1), 1, false));
        CHECK(sameOrientation(orientationFromDrmRotation(1u << 2), 2, false));
        CHECK(sameOrientation(orientationFromDrmRotation(1u << 3), 3, false));
        CHECK(sameOrientation(orientationFromDrmRotation((1u << 0) | (1u << 4)), 0, true));
        CHECK(sameOrientation(orientationFromDrmRotation((1u << 0) | (1u << 5)), 2, true));
        CHECK(sameOrientation(orientationFromDrmRotation((1u << 0) | (1u << 4) | (1u << 5)), 2,
                              false));
        // No turn, or two: says nothing.
        CHECK(orientationFromDrmRotation(0).upright());
        CHECK(orientationFromDrmRotation((1u << 1) | (1u << 2)).upright());
        // reflect-x and rotate-90 as drm_rect_rotate makes them: (x, y) of a
        // W-wide source goes to (W-1-x, y), then to (y, W-1-x') = (y, x).
        int tx = 0, ty = 0;
        orientedTexel(3, 1, 5, 2, orientationFromDrmRotation((1u << 1) | (1u << 4)), tx, ty);
        CHECK(tx == 1 && ty == 3);
    }

    // ── RandR's rotation: the picture turned counter-clockwise, reflections not taken ──
    {
        bool reflected = true;
        CHECK(orientationFromRandr(1, reflected).upright());
        CHECK(!reflected);
        CHECK(sameOrientation(orientationFromRandr(2, reflected), 1, false));
        CHECK(sameOrientation(orientationFromRandr(4, reflected), 2, false));
        CHECK(sameOrientation(orientationFromRandr(8, reflected), 3, false));
        CHECK(sameOrientation(orientationFromRandr(2 | 16, reflected), 1, false));
        CHECK(reflected);
    }

    // ── The frame's window of its buffer, turned ──
    {
        KmsFrame f;
        f.width = 1080;
        f.height = 1920;
        CHECK_EQ(f.windowWidth(), 1080);
        CHECK_EQ(f.importWidth(), 1080);
        f.orientation = Orientation{1, false};
        CHECK_EQ(f.windowWidth(), 1920);
        CHECK_EQ(f.windowHeight(), 1080);
        CHECK_EQ(f.importWidth(), 1920);
        CHECK_EQ(f.importHeight(), 1080);
        f.bufferWidth = 2560;
        f.bufferHeight = 1440;
        CHECK_EQ(f.importWidth(), 2560);
    }

    // ── The pointer of a turned scanout: image and place stood upright ──
    {
        // A 3x4 sprite, every pixel inked and numbered, on an upright desktop
        // of 10x6 at (4,1). The compositor turns the sprite for the panel and
        // puts it at the top-left of the plane's 8x8 buffer, which it places
        // so that the sprite lands where the desktop's sprite would show.
        const int sw = 3, sh = 4, buf = 8, dw = 10, dh = 6, sx = 4, sy = 1;
        std::vector<uint32_t> sprite = numbered(sw, sh);
        for (uint32_t& p : sprite)
            p |= 0xFF000000u; // alpha: ink everywhere
        bool image = true, place = true, hotspot = true;
        for (const Orientation panel : allOrientations()) {
            int hw = 0, hh = 0;
            const std::vector<uint32_t> heldSprite = hold(sprite, sw, sh, panel, hw, hh);
            HeldPointer held;
            held.width = held.height = buf;
            held.pixels.assign(static_cast<size_t>(buf) * buf * 4, 0);
            for (int y = 0; y < hh; ++y)
                std::memcpy(&held.pixels[static_cast<size_t>(y) * buf * 4],
                            &heldSprite[static_cast<size_t>(y) * hw], static_cast<size_t>(hw) * 4);
            // The sprite's place on the panel: its upright corners, turned.
            int ax = 0, ay = 0, bx = 0, by = 0;
            orientedTexel(sx, sy, dw, dh, panel, ax, ay);
            orientedTexel(sx + sw - 1, sy + sh - 1, dw, dh, panel, bx, by);
            const int planeX = std::min(ax, bx);
            const int planeY = std::min(ay, by);
            // A hotspot known in the plane's pixels: upright sprite pixel (1,2).
            int hx = 0, hy = 0;
            orientedTexel(1, 2, sw, sh, panel, hx, hy);
            held.hotspotKnown = true;
            held.hotspotX = hx;
            held.hotspotY = hy;

            CursorState out;
            UprightPointerCut cut;
            standPointerUpright(held, panel, out, cut);
            // The sprite is back at the image's top-left, inked, upright.
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x) {
                    uint32_t v = 0;
                    std::memcpy(&v, &out.pixels[(static_cast<size_t>(y) * out.width + x) * 4], 4);
                    image = image && v == sprite[static_cast<size_t>(y) * sw + x];
                }
            image = image && out.inkWidth == sw && out.inkHeight == sh;
            hotspot = hotspot && out.hotspotX == 1 && out.hotspotY == 2;
            int ux = 0, uy = 0;
            placeUprightPointer(planeX, planeY, cut, panel, dw, dh, ux, uy);
            place = place && ux == sx && uy == sy;
        }
        CHECK(image);
        CHECK(hotspot);
        CHECK(place);

        // An unknown hotspot stays 0,0: the top-left of the ink stands for it.
        HeldPointer held;
        held.width = held.height = 2;
        held.pixels.assign(16, 0xFF);
        CursorState out;
        UprightPointerCut cut;
        standPointerUpright(held, Orientation{1, false}, out, cut);
        CHECK(out.hotspotX == 0 && out.hotspotY == 0);
        // And an empty shape is cut nowhere.
        held.pixels.assign(16, 0);
        standPointerUpright(held, Orientation{3, false}, out, cut);
        CHECK(cut.cutX == 0 && cut.cutY == 0 && out.inkWidth == 0 && out.width == 2);
    }

    // ── The compositor's word, by output ──
    {
        std::vector<input::WaylandOutput> outputs(2);
        outputs[0].name = "DP-1";
        outputs[1].name = "HDMI-1"; // Mutter's spelling of the kernel's HDMI-A-1
        outputs[1].transform = 3;
        const CompositorTransform hdmi = waylandTransform(outputs, "HDMI-A-1");
        CHECK(hdmi.known);
        CHECK(sameOrientation(hdmi.orientation, 3, false));
        const CompositorTransform dp = waylandTransform(outputs, "DP-1");
        CHECK(dp.known && dp.orientation.upright());
        CHECK(!waylandTransform(outputs, "DP-2").known);
        CHECK(!waylandTransform(outputs, "").known);

        std::vector<input::X11Output> x11(2);
        x11[0].name = "HDMI-A-0";
        x11[0].edid = {1, 2, 3};
        x11[0].x = 1920;
        x11[0].width = 1080;
        x11[0].height = 1920;
        x11[0].rotation = 2; // RR_Rotate_90
        x11[1].name = "DisplayPort-0";
        x11[1].edid = {4, 5, 6};
        x11[1].width = 1920;
        x11[1].height = 1080;
        input::KmsIdentity identity;
        identity.edid = {1, 2, 3};
        identity.width = 1920; // the CRTC scans out X's turned copy, from 0,0
        identity.height = 1080;
        const CompositorTransform turned = x11Transform(x11, identity);
        CHECK(turned.known);
        CHECK(sameOrientation(turned.orientation, 1, false));
        CHECK(turned.haveRect && turned.rect.left == 1920 && turned.rect.width() == 1080 &&
              turned.rect.height() == 1920);
        identity.edid = {9};
        identity.width = 640;
        CHECK(!x11Transform(x11, identity).known);
    }
}
