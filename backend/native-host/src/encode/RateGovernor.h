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

#include "mw/native/LinkFeedback.h"

#include <cstdint>

namespace mw::native::encode {

/// Turns what the receiver reports about the link into the bitrate the stream
/// should be encoded at — plan v2, E3; design §9.3.
///
/// ── The idea ────────────────────────────────────────────────────────────────
///
/// The viewer's bitrate setting is a ceiling, not a promise the link keeps at
/// every moment. When the link narrows — Wi-Fi contention, a neighbour's
/// download, the 4G cell filling up — frames keep being encoded at the
/// setting, queue up in the transport, and every one of them arrives later
/// than the last: that is the pointer trailing the hand, seconds before a
/// single packet is lost. The receiver sees the queue as a rise in one-way
/// delay (LinkFeedback::owdRiseMs) and says so twice a second; this governor
/// answers by lowering the encoder's target at once, and raises it back only
/// when the link has been quiet for a while. A queue that is drained by
/// sending less is a queue the viewer never feels.
///
/// The same idea as WebRTC's delay-based congestion control, kept to the bare
/// bones on purpose: this has one flow, one direction, a receiver that stamps
/// every frame, and an encoder that changes rate between two frames.
///
/// ── The rules ───────────────────────────────────────────────────────────────
///
///  - **Overuse**: a delay rise past kOveruseMs (a queue of two frame intervals
///    at 60 fps), any frame gap, or any eviction on the host's own sender.
///    The target drops by kCutPercent at once, and no raise is attempted for
///    kHoldAfterCutMs. A second overuse inside the hold cuts again.
///  - **Quiet**: a delay rise under kQuietMs, no gaps, no evictions. After
///    kQuietBeforeRaiseMs of it the target climbs by kRaisePercent per report,
///    never above the setting. Raising is slow and cutting is fast because a
///    cut costs sharpness for a second and an overrun costs the viewer's hand.
///  - **Floor**: kFloorPercent of the setting, and never under kFloorKbps. Below
///    that the picture is not worth sending; the stream is better off letting
///    the frontend's own ladder move resolution or transport.
///  - **Silence**: no report for kSilenceMs — the feedback channel itself is
///    stuck — reads as overuse once, then nothing until a report comes back.
///
/// Pure and clocked by the caller, so it can be tested without a network.
class RateGovernor
{
public:
    static constexpr int kOveruseMs = 30;
    static constexpr int kQuietMs = 10;
    static constexpr int kCutPercent = 20;
    static constexpr int kRaisePercent = 5;
    static constexpr int kFloorPercent = 20;
    static constexpr int kFloorKbps = 2000;
    static constexpr int64_t kHoldAfterCutMs = 2000;
    static constexpr int64_t kQuietBeforeRaiseMs = 3000;
    static constexpr int64_t kSilenceMs = 4000;

    /// @p settingKbps the viewer's ceiling. Starts there.
    void start(int settingKbps, int64_t nowMs)
    {
        m_Setting = settingKbps > 0 ? settingKbps : 20000;
        m_Target = m_Setting;
        m_QuietSinceMs = nowMs;
        m_LastReportMs = nowMs;
        m_HoldUntilMs = 0;
        m_SilenceCut = false;
    }

    /// The viewer moved the ceiling (the frontend's ladder, or a new session
    /// setting). The target follows down at once, and up only through quiet.
    void setSetting(int settingKbps)
    {
        if (settingKbps <= 0) return;
        m_Setting = settingKbps;
        if (m_Target > m_Setting) m_Target = m_Setting;
    }

    /// One report from the receiver at @p nowMs. Returns true when the target
    /// changed.
    bool report(const LinkFeedback& fb, int64_t nowMs)
    {
        m_LastReportMs = nowMs;
        m_SilenceCut = false;
        const bool overuse = fb.owdRiseMs >= kOveruseMs || fb.gaps > 0 || fb.evictions > 0;
        const bool quiet = fb.owdRiseMs < kQuietMs && fb.gaps == 0 && fb.evictions == 0;

        if (overuse) {
            m_QuietSinceMs = nowMs;
            m_HoldUntilMs = nowMs + kHoldAfterCutMs;
            m_Overuses++;
            return cut();
        }
        if (!quiet) {
            // Neither: the queue is present but not growing. Hold.
            m_QuietSinceMs = nowMs;
            return false;
        }
        if (nowMs < m_HoldUntilMs) return false;
        if (nowMs - m_QuietSinceMs < kQuietBeforeRaiseMs) return false;
        return raise();
    }

    /// Called on the host's own clock between reports. Returns true when the
    /// target changed — only ever because the reports stopped coming.
    bool tick(int64_t nowMs)
    {
        if (m_SilenceCut || nowMs - m_LastReportMs < kSilenceMs) return false;
        m_SilenceCut = true;
        m_HoldUntilMs = nowMs + kHoldAfterCutMs;
        m_QuietSinceMs = nowMs;
        m_Silences++;
        return cut();
    }

    int targetKbps() const { return m_Target; }
    int settingKbps() const { return m_Setting; }
    int floorKbps() const
    {
        const int pct = m_Setting * kFloorPercent / 100;
        return pct > kFloorKbps ? pct : kFloorKbps;
    }
    bool limiting() const { return m_Target < m_Setting; }
    int overuses() const { return m_Overuses; }
    int silences() const { return m_Silences; }
    int changes() const { return m_Changes; }

private:
    bool cut()
    {
        int next = m_Target - m_Target * kCutPercent / 100;
        const int floor = floorKbps();
        if (next < floor) next = floor;
        if (next > m_Setting) next = m_Setting;
        if (next == m_Target) return false;
        m_Target = next;
        m_Changes++;
        return true;
    }

    bool raise()
    {
        if (m_Target >= m_Setting) return false;
        int next = m_Target + m_Target * kRaisePercent / 100;
        if (next > m_Setting) next = m_Setting;
        if (next == m_Target) next = m_Setting; // a rounding stall never sticks
        m_Target = next;
        m_Changes++;
        return true;
    }

    int m_Setting = 20000;
    int m_Target = 20000;
    int64_t m_QuietSinceMs = 0;
    int64_t m_LastReportMs = 0;
    int64_t m_HoldUntilMs = 0;
    bool m_SilenceCut = false;
    int m_Overuses = 0;
    int m_Silences = 0;
    int m_Changes = 0;
};

} // namespace mw::native::encode
