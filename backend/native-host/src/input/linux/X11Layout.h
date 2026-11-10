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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mw::native::input {

/// One output as the X server lays it out: RandR's name for it, the rectangle
/// of the root window its CRTC shows, and what ties it to a KMS connector.
struct X11Output
{
    std::string name; ///< "HDMI-A-0", "HDMI-1-0", "DP-1"… as the X driver spells it
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> edid; ///< the monitor's EDID as the driver read it; empty: none
    uint32_t connectorId = 0;  ///< the KMS connector the driver says it drives; 0: not said
    /// RandR's rotation of its CRTC: RR_Rotate_0 (1) unless the screen is
    /// turned against its panel (capture/Orientation.h). The rectangle above
    /// is already the turned one.
    unsigned rotation = 1;
};

/// The captured display as KMS knows it — what an X output is recognised by.
struct KmsIdentity
{
    std::vector<uint8_t> edid;
    uint32_t connectorId = 0;
    int x = 0; ///< the CRTC's rectangle
    int y = 0;
    int width = 0;
    int height = 0;
};

/// Which of X's outputs shows the captured connector, or -1.
///
/// ── Why KMS is not enough under X either ────────────────────────────────────
///
/// On one GPU a CRTC scans out a window of the root, so its (x, y) is its place
/// on the desktop — but only the CRTCs of the GPU X renders on. A monitor on a
/// second GPU (a PRIME output sink: X copies its part of the root into a buffer
/// of that GPU) scans out from (0, 0) of that copy, and nothing in KMS says
/// where the copy came from. The union of one card's outputs is not the
/// desktop either: X stretches an absolute device over its whole root, every
/// GPU's monitors included. Measured on the UM790Pro (30/09/2026, X11, the
/// M27Q on a GTX 1050 at 0,0 and the Radeon's screen at 2560,0): the centre of
/// the Radeon's picture put the pointer at 2241,720 of the 4480×1440 root.
///
/// ── Recognised by what the driver read, not by its name ────────────────────
///
/// X drivers name outputs their own way — the amdgpu driver counts from 0
/// ("HDMI-A-0" is the kernel's HDMI-A-1, and its "HDMI-A-1" is another
/// connector), modesetting from 1 ("HDMI-1"), and both add the GPU for an
/// output sink ("HDMI-A-1-0"). A name matched across them lands on the next
/// connector. What they share is what they read from the kernel: the EDID
/// blob, byte for byte, and — amdgpu and modesetting both publish it — the
/// connector's id. So, in that order: the one output with the same EDID;
/// among several (two monitors of one model), the one with the same connector
/// id; and failing both, the one at the CRTC's own rectangle, which is right on
/// the GPU X renders on and the last resort only. Ambiguity is no answer: the
/// caller then keeps what KMS said.
///
/// Pure, in the header, like pickWaylandRects: every platform's run tests it.
///
/// @param how out — how it was recognised, for the log
inline int findX11Output(const std::vector<X11Output>& outputs, const KmsIdentity& kms,
                         std::string& how)
{
    std::vector<int> candidates;
    for (int i = 0; i < static_cast<int>(outputs.size()); ++i)
        if (outputs[i].width > 0 && outputs[i].height > 0) candidates.push_back(i);

    bool byEdid = false;
    if (!kms.edid.empty()) {
        std::vector<int> same;
        for (int i : candidates)
            if (outputs[i].edid == kms.edid) same.push_back(i);
        if (same.size() == 1) {
            how = "its EDID";
            return same.front();
        }
        if (!same.empty()) {
            candidates = same;
            byEdid = true;
        }
    }

    if (kms.connectorId != 0) {
        int found = -1;
        int count = 0;
        for (int i : candidates)
            if (outputs[i].connectorId == kms.connectorId) {
                found = i;
                ++count;
            }
        if (count == 1) {
            how = byEdid ? "its EDID and connector id" : "its connector id";
            return found;
        }
    }

    int found = -1;
    int count = 0;
    for (int i : candidates)
        if (outputs[i].x == kms.x && outputs[i].y == kms.y && outputs[i].width == kms.width &&
            outputs[i].height == kms.height) {
            found = i;
            ++count;
        }
    if (count == 1) {
        how = byEdid ? "its EDID and place on the root" : "its place on the root";
        return found;
    }
    return -1;
}

/// The one output at exactly this rectangle of the root, or -1 — for the portal
/// route, whose monitor has no connector to recognise but a place the portal
/// names, in the root's coordinates on an X11 session.
inline int findX11OutputAt(const std::vector<X11Output>& outputs, int left, int top, int right,
                           int bottom)
{
    int found = -1;
    for (int i = 0; i < static_cast<int>(outputs.size()); ++i) {
        const X11Output& out = outputs[i];
        if (out.x == left && out.y == top && out.x + out.width == right &&
            out.y + out.height == bottom) {
            if (found >= 0) return -1;
            found = i;
        }
    }
    return found;
}

/// The one output of exactly this size, or -1 — for a portal that said how big
/// its monitor is and not where.
inline int findX11OutputSized(const std::vector<X11Output>& outputs, int width, int height)
{
    int found = -1;
    for (int i = 0; i < static_cast<int>(outputs.size()); ++i) {
        if (outputs[i].width != width || outputs[i].height != height) continue;
        if (found >= 0) return -1;
        found = i;
    }
    return found;
}

/// The X server's layout, asked over its own connection — without linking to
/// libX11 or libXrandr.
///
/// Loaded with dlopen for X11Pointer's reasons: one binary for the X desktop,
/// the Wayland desktop and the headless host. The same session rules too: no
/// DISPLAY, no X server; a Wayland session's DISPLAY is Xwayland's, whose root
/// is not the space the compositor stretches an absolute device over (the
/// Wayland layout answers there). The connection lives for the length of
/// read(); an X error on it (an output unplugged between two requests) is
/// counted, not the default handler's exit().
class X11Layout
{
public:
    /// Every output that shows something (a CRTC, a size), and the root's
    /// size — the desktop, as X stretches an absolute device across it. False,
    /// with why, when there is no X server to ask; ordinary on Wayland and on
    /// a headless host.
    ///
    /// @param displayUsed out — the DISPLAY asked, for the log
    static bool read(std::vector<X11Output>& outputs, int& rootWidth, int& rootHeight,
                     std::string& displayUsed, std::string& why);
};

} // namespace mw::native::input
