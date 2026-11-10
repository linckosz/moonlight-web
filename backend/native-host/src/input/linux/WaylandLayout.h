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

/// One output as the Wayland compositor lays it out: its connector name and
/// its LOGICAL rectangle — scaled pixels, in the compositor's own space.
struct WaylandOutput
{
    std::string name; ///< "DP-2", "HDMI-A-1"… the connector, as KMS names it
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    /// wl_output's transform, 0 to 7: how the compositor turns the desktop
    /// onto this output's panel (capture/Orientation.h). Not 0 on a display
    /// turned against its panel, whose scanout is then turned (issue #54).
    uint32_t transform = 0;
};

inline bool waylandDesktopUnion(const std::vector<WaylandOutput>& outputs, int& left, int& top,
                                int& right, int& bottom);

/// The rectangles an absolute pointer needs, chosen from a compositor's layout.
///
/// ── Why KMS is not enough ───────────────────────────────────────────────────
///
/// On X11 every monitor scans out a window of the one root framebuffer, so a
/// CRTC's (x, y) IS its place on the desktop, and the union of the CRTCs is the
/// desktop. A Wayland compositor gives each output its own buffer instead: every
/// CRTC scans out from (0, 0), and the layout — which screen is left of which,
/// at what scale — lives only in the compositor. Read from KMS, a two-monitor
/// Wayland host looks like two screens stacked on the same origin, the desktop
/// comes out the size of the biggest one, and the fraction sent to the absolute
/// device covers the wrong half of what the compositor actually stretches it
/// over: the pointer moves on the screen the viewer is NOT looking at.
///
/// The compositor answers through xdg-output: the logical position and size of
/// each output, in the very space Hyprland, sway, KWin and Mutter spread an
/// absolute device across (their bounding box of all outputs, in logical
/// pixels). That is what WaylandLayout::read collects, and what this picks
/// from.
///
/// Pure arithmetic in the header, like displayPointToDesktop: the choice is the
/// half that can be wrong quietly, and this way every platform's test run
/// exercises it.
///
/// @param outputs   what the compositor reported
/// @param connector the captured display's connector name, as KMS names it
/// @param display   out — the captured display's logical rectangle
/// @param desktop   out — the union of every reported output
/// @return false when no output carries the connector's name, in which case the
///         rectangles are left untouched and the caller keeps what KMS said.
/// A connector name as every compositor would spell it, for matching. The
/// kernel says "HDMI-A-1"; wlroots (Hyprland, sway) and KWin repeat that, but
/// Mutter names the same connector "HDMI-1" — its own type table has one entry
/// for HDMI-A and HDMI-B alike. Lower-cased, and with that middle letter
/// dropped, both spell the same thing; nothing else differs between them.
inline std::string canonicalConnectorName(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name)
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
    if (out.rfind("hdmi-a-", 0) == 0 || out.rfind("hdmi-b-", 0) == 0)
        out.erase(4, 2); // "hdmi-a-1" -> "hdmi-1"
    return out;
}

inline bool pickWaylandRects(const std::vector<WaylandOutput>& outputs,
                             const std::string& connector, int& displayLeft, int& displayTop,
                             int& displayRight, int& displayBottom, int& desktopLeft,
                             int& desktopTop, int& desktopRight, int& desktopBottom)
{
    if (connector.empty()) return false;
    const std::string wanted = canonicalConnectorName(connector);
    const WaylandOutput* mine = nullptr;
    for (const WaylandOutput& out : outputs) {
        if (out.width <= 0 || out.height <= 0) continue;
        if (canonicalConnectorName(out.name) == wanted) {
            mine = &out;
            break;
        }
    }
    if (!mine) return false;

    displayLeft = mine->x;
    displayTop = mine->y;
    displayRight = mine->x + mine->width;
    displayBottom = mine->y + mine->height;
    return waylandDesktopUnion(outputs, desktopLeft, desktopTop, desktopRight, desktopBottom);
}

/// The union of every sized output — the desktop, as the compositor stretches
/// an absolute device over it. False when nothing has a size yet.
inline bool waylandDesktopUnion(const std::vector<WaylandOutput>& outputs, int& left, int& top,
                                int& right, int& bottom)
{
    bool any = false;
    for (const WaylandOutput& out : outputs) {
        if (out.width <= 0 || out.height <= 0) continue;
        const int r = out.x + out.width;
        const int b = out.y + out.height;
        if (!any) {
            left = out.x;
            top = out.y;
            right = r;
            bottom = b;
            any = true;
            continue;
        }
        left = out.x < left ? out.x : left;
        top = out.y < top ? out.y : top;
        right = r > right ? r : right;
        bottom = b > bottom ? b : bottom;
    }
    return any;
}

/// The portal route's display, when the portal said where its monitor sits:
/// the one output whose logical rectangle is exactly that. Nothing to match a
/// name against there — the portal names no connector — but a rectangle is as
/// good a name, and confirms the two sides speak the same space. False when no
/// output, or more than one, has that rectangle.
inline bool findWaylandOutputAt(const std::vector<WaylandOutput>& outputs, int left, int top,
                                int right, int bottom, std::string& name)
{
    const WaylandOutput* found = nullptr;
    for (const WaylandOutput& out : outputs) {
        if (out.x == left && out.y == top && out.x + out.width == right &&
            out.y + out.height == bottom) {
            if (found) return false;
            found = &out;
        }
    }
    if (!found) return false;
    name = found->name;
    return true;
}

/// The compositor's layout, asked over the Wayland socket — without linking to
/// libwayland.
///
/// ── Loaded, not linked ──────────────────────────────────────────────────────
///
/// libwayland-client is opened with dlopen, for the reason X11Pointer opens
/// libX11 that way: one binary for the X desktop, the Wayland desktop and the
/// headless KMS host, with no -dev package needed to build it. The protocol
/// objects it needs — the registry, wl_output and the xdg-output extension —
/// are described here by hand, which is a few static tables: the wire format is
/// libwayland's stable ABI, and pulling wayland-scanner into the build for two
/// interfaces would be the heavier dependency.
///
/// ── Which socket ────────────────────────────────────────────────────────────
///
/// WAYLAND_DISPLAY names it when the host runs inside the user's session, which
/// is the packaged install (the tray, the autostart entry). A host started as a
/// service has no such variable; the sockets of every logged-in user are then
/// tried under /run/user, which is where every compositor puts them, and the
/// first that answers is taken. Nothing is asked of the compositor beyond its
/// list of outputs, which any client may read.
///
/// ── What it is not ──────────────────────────────────────────────────────────
///
/// Not a way to read or move the pointer — Wayland refuses that by design, and
/// this does not try. The connection lives for the length of read() and is
/// closed before it returns.
class WaylandLayout
{
public:
    /// The compositor's outputs, or why they could not be read: no library,
    /// no socket, a compositor without xdg-output. False is ordinary on an X11
    /// or headless host and worth a debug line, not a warning.
    ///
    /// @param socketUsed out — how the compositor was reached, for the log
    static bool read(std::vector<WaylandOutput>& outputs, std::string& socketUsed,
                     std::string& why);
};

} // namespace mw::native::input
