/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include <cstddef>
#include <cstdint>

// How long the transport has been unable to drain what it was given — the
// question a back-pressure threshold should be asking, and the one a threshold
// in BYTES cannot answer.
//
// ── Why bytes were the wrong unit ───────────────────────────────────────────
//
// The relay used to drop deltas once `bufferedAmount` passed 256 KiB. Two
// things were wrong with that, and they pulled in opposite directions.
//
// It fired far too LATE. `bufferedAmount` counts only what libdatachannel keeps
// after usrsctp has refused a message, and usrsctp's own send buffer defaults
// to 1 MiB (`sctp_sendspace`). So nothing was visible until 1 MiB was already
// queued, and the threshold added a quarter more on top: about 500 ms of
// backlog on a 20 Mbit/s link before anything reacted. Half a second is not
// back-pressure, it is a delay the viewer has already felt.
//
// And any byte threshold low enough to fire in time fires on the WRONG thing. A
// 1440p HEVC keyframe from Sunshine is 200–300 KB; on that same link it
// legitimately occupies the wire for over a hundred milliseconds. A byte
// threshold cannot tell "one big picture is going out normally" from "this link
// is not keeping up", so it would drop the deltas behind every keyframe and ask
// for another keyframe — the IDR storm `MediaTrackRelay` documents, where the
// stream collapses outright.
//
// Time separates the two cleanly. A keyframe on a healthy link makes the buffer
// deep for a moment and then it drains; a link that cannot keep up makes it
// deep and KEEPS it deep. Only the second is worth dropping a frame over.
//
// ── What the old comment got wrong about the danger ─────────────────────────
//
// The relay explained back-pressure as protection against `dc->send()` blocking
// the Qt event loop. It does not block: libdatachannel sets its SCTP socket
// non-blocking (`usrsctp_set_non_blocking`) and, when usrsctp refuses, pushes
// the message onto its own queue instead. The real cost was never a stalled
// thread — it is latency, growing quietly in a buffer nobody was measuring.
class SendBacklog
{
public:
    /// Occupancy at or below which the buffer counts as drained.
    ///
    /// Not zero: a healthy stream at a high bitrate keeps a fragment or two in
    /// flight at all times, and aging from the first byte would call that a
    /// backlog. A couple of 16 KB fragments is the noise floor.
    static constexpr size_t kDrainedBytes = 48 * 1024;

    /// How long the buffer may stay backed up before frames start being
    /// dropped.
    ///
    /// Chosen against the two costs it sits between. Below it lies the drain
    /// time of one legitimate keyframe — 300 KB on a 20 Mbit/s link is 120 ms —
    /// and dropping frames there would cause the storm described above. Above
    /// it lies the latency the viewer feels; the old byte threshold effectively
    /// tolerated ~500 ms, and anything approaching that is a visible lag spike.
    /// 250 ms is twice the keyframe and half the old behaviour.
    static constexpr int64_t kToleranceMs = 250;

    /// Feed the current occupancy.
    ///
    /// @return true when the buffer has been continuously backed up for longer
    ///         than the tolerance — the caller should shed load.
    bool note(size_t bufferedBytes, int64_t nowMs)
    {
        if (bufferedBytes <= kDrainedBytes) {
            m_BackedUpSinceMs = kDrained;
            return false;
        }
        if (m_BackedUpSinceMs == kDrained) {
            // First sample of a new backlog: it has no age yet, so this call
            // never sheds. A burst that drains before the next sample is
            // therefore free, which is the whole point.
            m_BackedUpSinceMs = nowMs;
            return false;
        }
        // A clock that goes backwards (a sample taken before the one that
        // opened the window) would otherwise read as a huge age.
        if (nowMs < m_BackedUpSinceMs) m_BackedUpSinceMs = nowMs;
        return nowMs - m_BackedUpSinceMs > kToleranceMs;
    }

    /// How long the buffer has been backed up, 0 when it is drained. For logs.
    int64_t ageMs(int64_t nowMs) const
    {
        if (m_BackedUpSinceMs == kDrained || nowMs < m_BackedUpSinceMs) return 0;
        return nowMs - m_BackedUpSinceMs;
    }

    bool backedUp() const { return m_BackedUpSinceMs != kDrained; }

    /// Forget the current window — after a successful keyframe, which is the
    /// point the relay treats as a fresh start.
    void reset() { m_BackedUpSinceMs = kDrained; }

private:
    static constexpr int64_t kDrained = INT64_MIN;
    int64_t m_BackedUpSinceMs = kDrained;
};
