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

namespace mw::native {

/// Holds the capture loop to the stream's frame rate.
///
/// ── Why the loop needs this at all ──────────────────────────────────────────
///
/// Desktop Duplication wakes the loop on every present of the display, and
/// until this existed every one of them was encoded. On a 165 Hz panel with the
/// stream set to 60 that is 165 frames a second through a rate control that was
/// told 60: the constant bitrate is a budget PER FRAME, so the wire carried 2.75
/// times the configured rate on a moving screen — 63 Mbit/s for 20 set, the
/// moment a window was dragged — and the VBV bounded nothing it was meant to.
/// In a LAN nobody notices. Over the Internet the excess sits in the SCTP queue
/// and is felt as the pointer trailing the hand.
///
/// ── The last present, never the first ───────────────────────────────────────
///
/// The stream's interval is a grid. Inside one interval every present is
/// converted (cheap, GPU-side, and it keeps the freshest picture ready) but only
/// the LAST one is encoded, when the interval is due. Taking the first present
/// and ignoring the rest would send a picture that is on average half an
/// interval older than the one a viewer could have had — the exact opposite of
/// what a subsampling step should cost.
///
/// A present that arrives when nothing is held and the interval is already due
/// goes straight through and the grid restarts on it. That is the still-screen
/// case: after a pause the first change is on the wire at once, not on the next
/// tick of a grid that nothing had been keeping.
///
/// ── When there is nothing to hold ───────────────────────────────────────────
///
/// A stream at the display's own rate — the setting "0" resolves to it — needs
/// no gate: every present is encoded as it comes and the rate control is
/// dimensioned for the refresh rate. The session then constructs this with 0
/// and everything goes through. It is the right choice when the link can carry
/// it.
///
/// Pure and clocked by the caller, so it can be tested without a display.
class FrameCadence
{
public:
    /// How close to due a present may be to go through at once rather than be
    /// held for the remainder. The loop's wait has millisecond resolution, so
    /// holding a picture for less than that would mean waking a millisecond
    /// late for it — the wait itself would cost more than it saves.
    static constexpr int64_t kSlackUs = 1000;

    /// @p fps 0 (or negative) disables the gate.
    explicit FrameCadence(int fps)
        : m_IntervalUs(fps > 0 ? 1000000 / fps : 0)
    {}

    bool enabled() const { return m_IntervalUs > 0; }
    int64_t intervalUs() const { return m_IntervalUs; }

    /// A new picture is ready at @p nowUs.
    ///
    /// True: encode it now. False: it is HELD — the caller keeps it converted,
    /// waits at most waitUs(), and calls release() when the wait has elapsed. A
    /// picture that was already held is superseded by this one and never
    /// encoded; that is the whole point.
    bool admit(int64_t nowUs)
    {
        if (!enabled()) return true;
        if (nowUs + kSlackUs < m_NextDueUs) {
            if (m_Holding) m_Superseded++;
            m_Holding = true;
            return false;
        }
        // Due. Straight through — and what happens to the grid depends on
        // whether it was live. A picture was being held: this present beat the
        // wake-up that would have released it (it arrived within the slack, or
        // just after the due time), so it goes out in the held one's place and
        // the grid advances by its interval exactly as a release would. Nothing
        // was held: the screen had been still, the old grid means nothing, and
        // it is anchored here.
        if (m_Holding) {
            m_Superseded++;
            m_Holding = false;
            advance(nowUs);
        } else {
            m_NextDueUs = nowUs + m_IntervalUs;
        }
        return true;
    }

    /// Whether a picture is being held for the next due time.
    bool holding() const { return m_Holding; }

    /// Microseconds until the held picture is due. Zero or negative: due now.
    /// Meaningless when nothing is held.
    int64_t waitUs(int64_t nowUs) const { return m_NextDueUs - nowUs; }

    /// True when a held picture must be encoded now.
    bool due(int64_t nowUs) const { return m_Holding && waitUs(nowUs) <= 0; }

    /// The held picture is being handed to the encoder at @p nowUs.
    ///
    /// The grid advances by one interval, not to "now plus one": the wake-up
    /// that released the picture is a little late every time, and letting that
    /// lateness accumulate would slowly lower the rate. Only when a whole
    /// interval has been missed — a stall — is the grid re-anchored, so the
    /// loop never catches up with a burst.
    void release(int64_t nowUs)
    {
        m_Holding = false;
        if (enabled()) advance(nowUs);
    }

    /// Forget any held picture — the capture was lost, and with it the texture.
    void drop() { m_Holding = false; }

    /// Presents that were held and then replaced by a newer one — what the
    /// display produced that the stream did not carry.
    int64_t superseded() const { return m_Superseded; }

private:
    void advance(int64_t nowUs)
    {
        m_NextDueUs += m_IntervalUs;
        if (m_NextDueUs <= nowUs) m_NextDueUs = nowUs + m_IntervalUs;
    }

    int64_t m_IntervalUs = 0;
    int64_t m_NextDueUs = 0;
    bool m_Holding = false;
    int64_t m_Superseded = 0;
};

} // namespace mw::native
