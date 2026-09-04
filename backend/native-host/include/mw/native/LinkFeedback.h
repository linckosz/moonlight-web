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

namespace mw::native {

/// What the receiver saw of the link over its last reporting window — the
/// only view of the link that is worth anything (design §9.3).
///
/// ── Why the host cannot see this itself ─────────────────────────────────────
///
/// On the host, a congested link looks like nothing: `send()` returns at once
/// into a megabyte of SCTP buffer, `bufferedAmount` counts only what overflowed
/// that, and the frames leave the process on time. The queue builds inside the
/// transport and on the routers, and the first place it can be measured is the
/// arrival side — every frame carries the host's present time, so the receiver
/// knows exactly how much later than usual each one came. That rise is the
/// queue, in milliseconds, before a single packet is lost. Loss and frame gaps
/// arrive later and mean the queue has already overflowed.
struct LinkFeedback
{
    /// How much later frames arrive than at the best of the session, in
    /// milliseconds: the minimum over the window of (arrival − host present)
    /// minus the same minimum over the whole session. Clock offsets cancel out
    /// in the subtraction; only queueing remains. 0 is a link with nothing
    /// waiting in it.
    int owdRiseMs = 0;

    /// Frames the receiver never got in the window (holes in the frame
    /// numbering), and on RTP transports the packets it lost.
    int gaps = 0;

    /// Frames the host's own sender evicted from its queue in the window,
    /// because the link had not taken the previous ones. Filled by the host.
    int evictions = 0;

    /// Frames the receiver decoded in the window per second, as a sanity
    /// check on the rest; 0 when unknown.
    int receivedFps = 0;
};

} // namespace mw::native
