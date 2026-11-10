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

#include "../../input/linux/WaylandLayout.h"
#include "../../input/linux/X11Layout.h"
#include "../CaptureTypes.h"
#include "../Orientation.h"

#include <string>
#include <vector>

// How the compositor shows the desktop on one panel, for the capture that
// reads the panel's scanout (KmsCapture; issue #54 on Linux).
//
// Only the compositor knows. It turns the desktop into the scanout itself, and
// KMS sees a buffer of the panel's shape with nothing to say it is turned. So
// it is asked the way the pointer's layout is (LinuxSession's readInputRects):
// X's RandR on an X11 session, the output found by what the driver read from
// KMS (findX11Output); else the Wayland compositor's wl_output, found by the
// connector's name. Both say it the same way (Orientation.h).
//
// The choice of output is pure, in the header, so every platform's run tests
// it; reading the layouts is Linux's.

namespace mw::native::capture {

struct CompositorTransform
{
    bool known = false;
    Orientation orientation;
    /// Where it was read, for the log: "RandR's HDMI-A-0 (by its EDID)",
    /// "wl_output HDMI-1".
    std::string source;
    /// The display's rectangle on X's root, where X answered: what its damage
    /// is watched over — the CRTC of a turned X screen scans out a buffer of
    /// its own, at the panel's origin, and says nothing of where it is.
    bool haveRect = false;
    DesktopRect rect;
    /// X reflected the screen as well, which is not undone
    /// (orientationFromRandr).
    bool reflectionIgnored = false;
};

/// From X's layout: the output that shows the captured connector.
inline CompositorTransform x11Transform(const std::vector<input::X11Output>& outputs,
                                        const input::KmsIdentity& identity)
{
    CompositorTransform t;
    std::string how;
    const int index = input::findX11Output(outputs, identity, how);
    if (index < 0) return t;
    const input::X11Output& out = outputs[static_cast<size_t>(index)];
    t.known = true;
    t.orientation = orientationFromRandr(out.rotation, t.reflectionIgnored);
    t.source = "RandR's " + out.name + ", by " + how;
    t.haveRect = true;
    t.rect = DesktopRect{out.x, out.y, out.x + out.width, out.y + out.height};
    return t;
}

/// From a Wayland compositor's layout: the output named as the connector,
/// however the compositor spells it (canonicalConnectorName).
inline CompositorTransform waylandTransform(const std::vector<input::WaylandOutput>& outputs,
                                            const std::string& connector)
{
    CompositorTransform t;
    if (connector.empty()) return t;
    const std::string wanted = input::canonicalConnectorName(connector);
    for (const input::WaylandOutput& out : outputs) {
        if (input::canonicalConnectorName(out.name) != wanted) continue;
        t.known = true;
        t.orientation = orientationFromWayland(out.transform);
        t.source = "wl_output " + out.name;
        return t;
    }
    return t;
}

#if defined(__linux__)
/// The layouts, each read at most once — the probe asks for every display of
/// the machine in a row. Asked of whoever answers, X first and then Wayland:
/// the order the pointer's layout is read in. Unknown, and the capture then
/// goes by its plane alone, when neither answers or recognises the display: a
/// host started with no session to ask, a bare console.
class CompositorLayouts
{
public:
    CompositorTransform transformFor(const input::KmsIdentity& identity,
                                     const std::string& connector)
    {
        std::string used;
        std::string why;
        if (!m_X11Read) {
            m_X11Read = true;
            int rootWidth = 0;
            int rootHeight = 0;
            m_HaveX11 = input::X11Layout::read(m_X11, rootWidth, rootHeight, used, why);
        }
        if (m_HaveX11) {
            CompositorTransform t = x11Transform(m_X11, identity);
            if (t.known) return t;
        }
        if (!m_WaylandRead) {
            m_WaylandRead = true;
            m_HaveWayland = input::WaylandLayout::read(m_Wayland, used, why);
        }
        if (m_HaveWayland) return waylandTransform(m_Wayland, connector);
        return CompositorTransform{};
    }

private:
    bool m_X11Read = false;
    bool m_HaveX11 = false;
    std::vector<input::X11Output> m_X11;
    bool m_WaylandRead = false;
    bool m_HaveWayland = false;
    std::vector<input::WaylandOutput> m_Wayland;
};

/// One display's, read afresh.
inline CompositorTransform readCompositorTransform(const input::KmsIdentity& identity,
                                                   const std::string& connector)
{
    CompositorLayouts layouts;
    return layouts.transformFor(identity, connector);
}
#endif

} // namespace mw::native::capture
