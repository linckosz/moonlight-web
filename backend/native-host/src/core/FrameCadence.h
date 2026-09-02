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

/// Holds the capture loop to the stream's frame rate — without ever holding a
/// picture.
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
/// ── Nothing waits: the first present at or after each tick ──────────────────
///
/// The stream's interval is a grid. A present that arrives before the next tick
/// is skipped — converted, so the freshest picture is always in the converter's
/// texture for the still-screen paths, but not encoded. The first present at or
/// after the tick is encoded THE MOMENT IT ARRIVES, and the grid advances by one
/// interval. Nothing is ever kept for a later wake-up.
///
/// The first version of this gate did the opposite: it held the last present of
/// each interval and encoded it when the interval fell due, for a perfectly
/// regular emission. Measured over the Internet, that wait was two thirds of
/// the host's time — 5.4 ms on average, a full 17 ms interval at the p99, for a
/// present that arrived just after a tick and had to sit out the next one. A
/// picture waiting on the host is latency the viewer feels; an emission that is
/// early or late by a present period (6 ms at 165 Hz, nothing when the game
/// itself runs at the stream's rate) is not, and with tearing on the client is
/// invisible altogether. So: latency first, regularity second.
///
/// ── The slack, and how the grid keeps its rate ──────────────────────────────
///
/// A present a little BEFORE the tick — within kSlackFraction of the interval —
/// goes through too, because the next one would arrive a whole present period
/// later for nothing. The grid still advances by exactly one interval from
/// where it was, never "now plus one": that is what keeps the rate at the
/// setting when every admitted present is early or late by a few milliseconds
/// (anchoring on the present itself measured 56 fps for 60 on a 165 Hz panel).
/// Only when the admitted present is LATE by more than one present period of
/// the display — meaning a present was MISSING where the grid expected one:
/// the screen had been still, the loop had stalled, or a game running at the
/// stream's own rate had drifted out of phase with the grid — is the grid
/// re-anchored on it. So the loop never catches up with a burst, a first
/// change after a pause is on the wire at once, and a game at 60 fps under a
/// 60 fps stream locks onto the grid instead of beating against it.
///
/// ── When there is nothing to gate ───────────────────────────────────────────
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
    /// How early a present may be, as a fraction of the interval, and still go
    /// through: a quarter. Two presents can never both be admitted inside one
    /// interval as long as the display is faster than the stream, whatever the
    /// slack, so the slack costs no rate — it only decides how far the emission
    /// may run ahead of the grid before the next present is preferred.
    static constexpr int kSlackDivisor = 4;

    /// @p fps 0 (or negative) disables the gate. @p displayHz is the display's
    /// refresh, which sets how late an admitted present may be before the grid
    /// is re-anchored on it (see above); 0 falls back to half an interval.
    explicit FrameCadence(int fps, int displayHz = 0)
        : m_IntervalUs(fps > 0 ? 1000000 / fps : 0)
        , m_ReanchorUs(displayHz > 0 ? 1000000 / displayHz : m_IntervalUs / 2)
    {}

    bool enabled() const { return m_IntervalUs > 0; }
    int64_t intervalUs() const { return m_IntervalUs; }
    int64_t slackUs() const { return m_IntervalUs / kSlackDivisor; }
    int64_t reanchorUs() const { return m_ReanchorUs; }

    /// A new picture is ready at @p nowUs. True: encode it now. False: skip it
    /// — the next present is the one that will carry the interval.
    bool admit(int64_t nowUs)
    {
        if (!enabled()) return true;
        if (nowUs + slackUs() < m_NextDueUs) {
            m_Skipped++;
            return false;
        }
        if (nowUs - m_NextDueUs > m_ReanchorUs)
            m_NextDueUs = nowUs + m_IntervalUs;
        else
            m_NextDueUs += m_IntervalUs;
        return true;
    }

    /// When the next present will be admitted. Meaningful only when enabled.
    int64_t nextDueUs() const { return m_NextDueUs; }

    /// Presents that were not encoded — what the display produced that the
    /// stream did not carry.
    int64_t skipped() const { return m_Skipped; }

private:
    int64_t m_IntervalUs = 0;
    int64_t m_ReanchorUs = 0;
    int64_t m_NextDueUs = 0;
    int64_t m_Skipped = 0;
};

} // namespace mw::native
