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
#include <string>

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
///    the frontend's own ladder move resolution or transport. A session may
///    hold a higher floor (SessionConfig::governorFloorPercent): the guests'
///    shared feed keeps ~60 % of its setting, so that one slow guest cannot
///    pull the picture of every other one down to a fifth of it — below that
///    floor the slow guest drops frames on its own, and the intra-refresh
///    repairs what it lost.
///  - **Silence**: no report for kSilenceMs — the feedback channel itself is
///    stuck — reads as overuse once, then nothing until a report comes back.
///  - **Back from the background**: a report flagged `resumed` says the
///    receiver's page was hidden or frozen. Its gaps are frames our own sender
///    evicted while nobody drained them, and its silence was the browser's:
///    the report is not read as overuse, and a standing silence cut is undone
///    at once — the target goes back to where the link had left it — instead
///    of being climbed back in five quiet raises. A cut a real overuse made
///    before the page went away stays.
///  - **Back to a rate the link has proven**: every kGoodHoldMs the target is
///    sampled, and a sample the link then carried for the whole window
///    without a single overuse becomes the link's last good rate. Below it,
///    quiet raises by kFastRaisePercent instead of kRaisePercent; above it,
///    the slow climb stays. Measured on a corporate Wi-Fi (16/09/2026): the
///    link froze for 0.3 to 1.2 s both ways every 20 to 30 s and was clean in
///    between. One freeze is a string of overuse reports while the queue
///    drains — 20 Mbps cut to the 4 Mbps floor in 3.5 s — and the slow climb
///    then kept the picture soft for 30 s on a link long healthy again. The
///    cut still happens at once; only the way back is quicker.
///  - **A probe that fails**: an overuse during a fast climb, or within
///    kProbeGraceMs of its last step, says the link really narrowed. The
///    good rate falls back to where the climb started, so the next climb is
///    a slow one: the rate can overshoot once, not oscillate.
///  - **A queue that overflowed** (the product's rule since 21/10/2026,
///    `ovfgood=0` the bench's "before" — overflowLowersGoodByDefault): a
///    frame the relay shed off a backed-up send buffer is overuse, and the
///    good rate falls to one cut under the rate it overflowed at, however
///    long ago the climb ended. Seen on an N95 host on 19/10/2026: back at
///    30 Mbit/s in 1.5 s at 15:19:05, the buffer overflowed 7.4 s later —
///    past the grace, so the good rate stayed 30 Mbit/s — and again at
///    15:19:25. The fast climb then stops a cut short, and the last step is
///    the slow one: ~2.5 s more from there back to the rate that overflowed.
///    One overflow is an EPISODE: the reports that follow it while the same
///    queue drains (each one kOverflowEpisodeMs or less after the last, or the
///    host's own backlog still standing) cut again but move neither the good
///    rate nor the rate remembered below.
///  - **Near the rate that overflowed, one step at a time** (DebitRemonte2,
///    14/11/2026, same rule): the rate an episode overflowed at is
///    remembered. From one cut under it up to it, the climb takes one
///    kRaisePercent step per kOverflowStepMs, not one per report, and stops
///    there. An episode that starts less than kOverflowRepeatMs after the last
///    overflow doubles the wait, up to kOverflowStepMaxMs; kOverflowForgetMs
///    without an overflow forgets it all, and the ordinary climb takes over
///    again. Seen on the N95 on 13/11/2026 (iPhone, 5G, 18 Mbit/s set): cut to
///    14 400 at 09:01:44, back at 17 502 six seconds later, overflowed again;
///    14 002, back at 16 208, overflowed again; the third one froze the
///    picture 14.8 s. The slow climb is +5 % per report, twice a second:
///    nothing slow about it right under a rate the link just refused.
///  - **The host's own backlog** (DebitRemonte2, same rule): the relay's
///    shed frames reach this governor inside the receiver's reports, and a
///    link that jams the host's way can hold those reports back too — a
///    single silence cut is then all the encoder hears. backlog() is the
///    host's own look at its send buffer: backed up for kBacklogCutAfterMs,
///    the target is cut at most every kBacklogCutEveryMs without waiting for
///    a report, no raise is made until it drains, and the floor is
///    kBacklogFloorKbps meanwhile (a session holding a higher floor keeps
///    it). A drained backlog leaves the target where it is, under the
///    ordinary floor if it went there, and ends an episode the session logs
///    (takeEndedBacklog).
///
/// Pure and clocked by the caller, so it can be tested without a network.
///
/// ── The bench's governor=off ───────────────────────────────────────────────
///
/// `--native-bench` has no receiver: its silence cut the target to 80 % four
/// seconds into every pass, and a ramp= step back up never reached the
/// encoder, a raise needing quiet reports. Started to follow the setting, the
/// governor makes the target the setting, both ways, at once, and reports and
/// silences move nothing: the encoder's rate control is measured against the
/// steps it is given (plan pipeline-video-d3d12-v2, G3).
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
    static constexpr int kFastRaisePercent = 25;
    static constexpr int64_t kGoodHoldMs = 5000;
    static constexpr int64_t kProbeGraceMs = 5000;
    static constexpr int64_t kOverflowEpisodeMs = kHoldAfterCutMs;
    static constexpr int64_t kOverflowStepMs = 5000;
    static constexpr int64_t kOverflowStepMaxMs = 60000;
    static constexpr int64_t kOverflowRepeatMs = 30000;
    static constexpr int64_t kOverflowForgetMs = 60000;
    static constexpr int64_t kBacklogCutAfterMs = 1000;
    static constexpr int64_t kBacklogCutEveryMs = 500;
    static constexpr int kBacklogFloorKbps = kFloorKbps;

    /// One stretch of the host's send buffer backed up past
    /// kBacklogCutAfterMs, for the session's log line: when it started and
    /// drained, the target when it was seen and the lowest it was cut to, and
    /// how many of those cuts were the host's own.
    struct BacklogEpisode
    {
        int64_t startMs = 0;
        int64_t endMs = 0;
        int fromKbps = 0;
        int lowestKbps = 0;
        int cuts = 0;
    };

    /// @p settingKbps the viewer's ceiling. Starts there. @p followSetting:
    /// the bench's governor=off, above. @p floorPercent: the floor as a share
    /// of the setting (kFloorPercent unless the session holds a higher one),
    /// clamped to 0..100; kFloorKbps stays the floor under it.
    void start(int settingKbps, int64_t nowMs, bool followSetting = false,
               int floorPercent = kFloorPercent)
    {
        m_Follow = followSetting;
        m_FloorPercent = floorPercent < 0 ? 0 : floorPercent > 100 ? 100 : floorPercent;
        m_Setting = settingKbps > 0 ? settingKbps : 20000;
        m_Target = m_Setting;
        m_QuietSinceMs = nowMs;
        m_LastReportMs = nowMs;
        m_HoldUntilMs = 0;
        m_SilenceCut = false;
        // Nothing is proven yet: a session's first seconds climb slowly.
        m_GoodKbps = 0;
        rearmGoodSample(nowMs);
        m_ClimbStartKbps = 0;
        m_LastFastRaiseMs = kNever;
        m_LastRaiseFast = false;
        m_LastCutMs = kNever;
        m_OvfKbps = 0;
        m_OvfLastMs = kNever;
        m_OvfStepMs = kNever;
        m_OvfWaitMs = kOverflowStepMs;
        m_Backlogged = false;
        m_EpisodeEnded = false;
        m_AreaSinceMs = nowMs;
        m_AreaLastMs = nowMs;
        m_AreaKbpsMs = 0;
    }

    /// The viewer moved the ceiling (the frontend's ladder, or a new session
    /// setting). The target follows down at once, and up only through quiet.
    void setSetting(int settingKbps)
    {
        if (settingKbps <= 0) return;
        m_Setting = settingKbps;
        if (m_Follow) {
            if (m_Target != m_Setting) m_Changes++;
            m_Target = m_Setting;
            return;
        }
        if (m_Target > m_Setting) m_Target = m_Setting;
        if (m_GoodKbps > m_Setting) m_GoodKbps = m_Setting;
        if (m_GoodSampleKbps > m_Setting) m_GoodSampleKbps = m_Setting;
    }

    /// One report from the receiver at @p nowMs. Returns true when the target
    /// changed.
    bool report(const LinkFeedback& fb, int64_t nowMs)
    {
        account(nowMs);
        m_LastReportMs = nowMs;
        m_LastRaiseFast = false;
        if (m_Follow) return false;
        forgetOverflow(nowMs);
        if (fb.resumed) {
            const bool restore = m_SilenceCut && m_TargetBeforeSilence > m_Target;
            m_SilenceCut = false;
            // Quiet starts over: what this report measured straddles the
            // burst a thawed page receives, and the next one is the first
            // honest look at the link.
            m_QuietSinceMs = nowMs;
            if (!restore) return false;
            m_Target = m_TargetBeforeSilence < m_Setting ? m_TargetBeforeSilence : m_Setting;
            m_HoldUntilMs = 0;
            m_Changes++;
            return true;
        }
        m_SilenceCut = false;
        // SCTP sending again more than retrcut= chunks in a thousand is a
        // link the stream overruns, and quiet wants it under half of that
        // (setRetransCut).
        const bool retransOver = m_RetransCut > 0 && fb.retransPermille >= m_RetransCut;
        const bool retransQuiet = m_RetransCut <= 0 || fb.retransPermille * 2 < m_RetransCut;
        if (retransOver) m_RetransOveruses++;
        // The relay's shed frames, while the rule is on (setOverflowLowersGood).
        const bool overflowed = m_OverflowLowersGood && fb.overflows > 0;
        const bool overuse = fb.owdRiseMs >= kOveruseMs || fb.gaps > 0 || fb.evictions > 0 ||
                             retransOver || overflowed;
        const bool quiet = fb.owdRiseMs < kQuietMs && fb.gaps == 0 && fb.evictions == 0 &&
                           retransQuiet && !overflowed;

        if (overuse) {
            m_QuietSinceMs = nowMs;
            m_HoldUntilMs = nowMs + kHoldAfterCutMs;
            m_Overuses++;
            // A fast climb that ran into a queue: the good rate was not the
            // link's any more.
            if (m_LastFastRaiseMs != kNever && nowMs - m_LastFastRaiseMs < kProbeGraceMs)
                m_GoodKbps = m_ClimbStartKbps;
            m_LastFastRaiseMs = kNever;
            // A queue that overflowed at this rate, the climb long over: the
            // link no longer carries it, whatever it carried before.
            if (overflowed) {
                m_Overflows++;
                noteOverflowAt(nowMs);
            }
            const bool changed = cut(nowMs);
            rearmGoodSample(nowMs);
            return changed;
        }
        // No queue growing: the sample taken at the start of the window was
        // carried all through it.
        if (nowMs - m_GoodSampleMs >= kGoodHoldMs) {
            if (m_GoodSampleKbps > m_GoodKbps) m_GoodKbps = m_GoodSampleKbps;
            rearmGoodSample(nowMs);
        }
        if (!quiet) {
            // Neither: the queue is present but not growing. Hold.
            m_QuietSinceMs = nowMs;
            return false;
        }
        if (nowMs < m_HoldUntilMs) return false;
        if (nowMs - m_QuietSinceMs < kQuietBeforeRaiseMs) return false;
        return raise(nowMs);
    }

    /// Called on the host's own clock between reports. Returns true when the
    /// target changed — only ever because the reports stopped coming.
    bool tick(int64_t nowMs)
    {
        account(nowMs);
        if (m_Follow || m_SilenceCut || nowMs - m_LastReportMs < kSilenceMs) return false;
        m_SilenceCut = true;
        m_TargetBeforeSilence = m_Target;
        m_HoldUntilMs = nowMs + kHoldAfterCutMs;
        m_QuietSinceMs = nowMs;
        m_Silences++;
        m_LastRaiseFast = false;
        m_LastFastRaiseMs = kNever;
        const bool changed = cut(nowMs);
        // A silence proves nothing about the link, either way: the good rate
        // stays, and the window that saw no report starts over.
        rearmGoodSample(nowMs);
        return changed;
    }

    /// The host's own send buffer at @p nowMs: backed up for @p backedUpMs
    /// (0 when drained). Returns true when the target changed — a cut, the
    /// buffer backed up for kBacklogCutAfterMs or more and nothing cut for
    /// kBacklogCutEveryMs. Only under the overflow rule
    /// (setOverflowLowersGood); a drained backlog ends an episode
    /// (takeEndedBacklog). Called on every pass of the session's loop.
    bool backlog(int64_t backedUpMs, int64_t nowMs)
    {
        account(nowMs);
        if (m_Follow || !m_OverflowLowersGood) return false;
        if (backedUpMs < kBacklogCutAfterMs) {
            if (m_Backlogged) {
                m_Backlogged = false;
                m_Episode.endMs = nowMs;
                m_EpisodeEnded = true;
                // Quiet, and the next step near the rate that overflowed,
                // count from the drain.
                m_QuietSinceMs = nowMs;
                m_OvfLastMs = nowMs;
                m_OvfStepMs = nowMs;
            }
            forgetOverflow(nowMs);
            return false;
        }
        if (!m_Backlogged) {
            // As any overuse: a fast climb that ran into it was a probe that
            // failed.
            if (m_LastFastRaiseMs != kNever && nowMs - m_LastFastRaiseMs < kProbeGraceMs)
                m_GoodKbps = m_ClimbStartKbps;
            m_LastFastRaiseMs = kNever;
            m_LastRaiseFast = false;
            noteOverflowAt(nowMs);
            m_Backlogged = true;
            m_Backlogs++;
            m_EpisodeEnded = false;
            m_Episode = BacklogEpisode{};
            m_Episode.startMs = nowMs - backedUpMs;
            m_Episode.fromKbps = m_Target;
            m_Episode.lowestKbps = m_Target;
        } else {
            noteOverflowAt(nowMs); // the same episode, kept open
        }
        m_QuietSinceMs = nowMs;
        if (m_LastCutMs != kNever && nowMs - m_LastCutMs < kBacklogCutEveryMs) return false;
        m_HoldUntilMs = nowMs + kHoldAfterCutMs;
        const bool changed = cut(nowMs);
        if (changed) {
            m_BacklogCuts++;
            m_Episode.cuts++;
            if (m_Target < m_Episode.lowestKbps) m_Episode.lowestKbps = m_Target;
        }
        rearmGoodSample(nowMs);
        return changed;
    }

    /// The backlog that drained last, once: true and @p out filled when one
    /// ended since the last call.
    bool takeEndedBacklog(BacklogEpisode& out)
    {
        if (!m_EpisodeEnded) return false;
        m_EpisodeEnded = false;
        out = m_Episode;
        return true;
    }
    bool backlogged() const { return m_Backlogged; }
    /// Backlogs seen, and the cuts the host made on its own during them.
    int backlogs() const { return m_Backlogs; }
    int backlogCuts() const { return m_BacklogCuts; }
    /// The rate the last overflow episode overflowed at, 0 once forgotten (or
    /// never); and the wait between two steps near it.
    int overflowKbps() const { return m_OvfKbps; }
    int64_t overflowWaitMs() const { return m_OvfWaitMs; }
    /// Overflow episodes, each counted once however many reports it spans.
    int overflowEpisodes() const { return m_OvfEpisodes; }

    /// For a cut's log line, the same on every platform: where the climb will
    /// stop near the rate that overflowed, and how slowly it gets there.
    /// Empty when none is remembered.
    std::string overflowNote() const
    {
        if (m_OvfKbps <= 0) return {};
        return " (overflowed at " + std::to_string(m_OvfKbps) +
               " kbps: back near it one step every " + std::to_string(m_OvfWaitMs / 1000) + " s)";
    }

    /// The log line of a backlog that drained (takeEndedBacklog), after
    /// "[native] link: ".
    std::string drainedLine(const BacklogEpisode& ep) const
    {
        return "send buffer drained after " + std::to_string(ep.endMs - ep.startMs) +
               " ms backed up — from " + std::to_string(ep.fromKbps) + " down to " +
               std::to_string(ep.lowestKbps) + " kbps, " + std::to_string(ep.cuts) +
               " cuts by the host alone; now " + std::to_string(m_Target) + " kbps" +
               overflowNote();
    }

    /// The session's last line on the governor: what the encoder was given on
    /// average up to @p nowMs, and why it was cut.
    std::string summaryLine(int64_t nowMs) const
    {
        return "target " + std::to_string(meanTargetKbps(nowMs)) + " kbps on average of the " +
               std::to_string(m_Setting) + " set, " + std::to_string(m_Changes) + " moves, " +
               std::to_string(m_Overuses) + " overuses (" + std::to_string(m_RetransOveruses) +
               " on SCTP's retransmissions, " + std::to_string(m_Overflows) +
               " on shed frames in " + std::to_string(m_OvfEpisodes) + " overflows), " +
               std::to_string(m_Backlogs) + " send buffers backed up past a second (" +
               std::to_string(m_BacklogCuts) + " cuts by the host alone), " +
               std::to_string(m_Silences) + " silences, last good rate " +
               std::to_string(m_GoodKbps) + " kbps";
    }

    /// `retrcut=` (plan Wi-Fi W2 B): SCTP retransmitting at least @p permille
    /// chunks in a thousand over a report window reads as overuse, and quiet
    /// then wants under half of that. 0: the governor does not look at SCTP
    /// at all, which is where it starts; the Windows session sets
    /// kRetransCutPermille unless the key says otherwise.
    ///
    /// The receiver's delay rise is a minimum over its window: on a Mac in
    /// Wi-Fi a few frames still arrive at once while most wait ~20 ms in
    /// usrsctp behind a window shrunk by each drop, and the rise never reaches
    /// kOveruseMs. The retransmissions are what the host itself sees of it;
    /// at 20 Mbit/s instead of 42 the Mac's drops fell ninefold and its click
    /// came back from 74 to 48.5 ms (W1 bis, 03/10/2026).
    ///
    /// Measured on 03/10/2026 against the Arc of DualRTX, two alternated
    /// rounds each, the page at 240: on a Mac in Wi-Fi the click came back
    /// from 73.6 to 62.6 ms (p90 112 to 86), the browser's socket dropped
    /// 366 datagrams instead of 1,016, a frame's age at p90 went from 86 to
    /// 29 ms, the host's small messages from ~330 to 35 ms at p90, and the
    /// frames thrown away from 182 to 34 a minute, for 23 against 20 Mbit/s
    /// received; on an N95 in Wi-Fi, 109.2 to 100.2 ms. On Ethernet (UM790Pro)
    /// SCTP sent 1 to 8 chunks again in a whole session; once, all 8 fell in
    /// one report window and the rate went from 40 to 32 Mbit/s for 3.5 s.
    /// The click stayed where it was (39.0 ms against 39.8 without).
    void setRetransCut(int permille) { m_RetransCut = permille > 0 ? permille : 0; }
    /// The Windows session's own retrcut=, per thousand.
    static constexpr int kRetransCutPermille = 3;
    int retransCut() const { return m_RetransCut; }
    /// Reports that read as overuse for SCTP's retransmissions alone or with
    /// the rest.
    int retransOveruses() const { return m_RetransOveruses; }

    /// Plan « Gels du lien », G: the relay's shed frames
    /// (LinkFeedback::overflows) are overuse, and lower the good rate to one
    /// cut under the rate they overflowed at. Off, where it starts, the
    /// governor does not read them at all; the session turns it on unless the
    /// bench's ovfgood=0 says otherwise (overflowLowersGoodByDefault).
    void setOverflowLowersGood(bool on) { m_OverflowLowersGood = on; }
    bool overflowLowersGood() const { return m_OverflowLowersGood; }
    /// Reports that carried shed frames, while the rule is on.
    int overflows() const { return m_Overflows; }

    /// The target's mean since start(), weighted by how long each value
    /// lasted, up to @p nowMs: what the encoder was given over the session,
    /// for its last log line (the A/B of ovfgood=). A ceiling the viewer
    /// moved counts from the next report or tick.
    int meanTargetKbps(int64_t nowMs) const
    {
        const int64_t last = nowMs > m_AreaLastMs ? nowMs - m_AreaLastMs : 0;
        const int64_t span = nowMs - m_AreaSinceMs;
        if (span <= 0) return m_Target;
        return static_cast<int>((m_AreaKbpsMs + static_cast<int64_t>(m_Target) * last) / span);
    }

    /// At the setting, the reports coming, and every one of them quiet for at
    /// least @p forMs before @p nowMs: a link with room to spare, for the few
    /// pictures more a press sends (core/InputAdmit.h).
    bool calm(int64_t nowMs, int64_t forMs) const
    {
        return !m_SilenceCut && m_Target >= m_Setting && nowMs - m_LastReportMs < kSilenceMs &&
               nowMs - m_QuietSinceMs >= forMs;
    }

    int targetKbps() const { return m_Target; }
    int settingKbps() const { return m_Setting; }
    int floorKbps() const
    {
        const int pct = static_cast<int>(static_cast<int64_t>(m_Setting) * m_FloorPercent / 100);
        return pct > kFloorKbps ? pct : kFloorKbps;
    }
    int floorPercent() const { return m_FloorPercent; }
    bool limiting() const { return m_Target < m_Setting; }
    int overuses() const { return m_Overuses; }
    int silences() const { return m_Silences; }
    int changes() const { return m_Changes; }
    /// The rate the link last carried through a whole kGoodHoldMs, 0 until
    /// one is proven.
    int goodKbps() const { return m_GoodKbps; }
    /// The last report raised the target by the fast step, back toward the
    /// good rate. For the log line.
    bool lastRaiseFast() const { return m_LastRaiseFast; }

private:
    static constexpr int64_t kNever = INT64_MIN;

    /// The time the target spent at its value since the last call.
    void account(int64_t nowMs)
    {
        if (nowMs <= m_AreaLastMs) return;
        m_AreaKbpsMs += static_cast<int64_t>(m_Target) * (nowMs - m_AreaLastMs);
        m_AreaLastMs = nowMs;
    }

    void rearmGoodSample(int64_t nowMs)
    {
        m_GoodSampleKbps = m_Target;
        m_GoodSampleMs = nowMs;
    }

    /// The send buffer overflowed at @p nowMs: a report's shed frames, or the
    /// host's own backlog. The first of an episode remembers the rate it
    /// overflowed at, lowers the good rate to one cut under it, and sets the
    /// wait between two steps near it — doubled when the last overflow was
    /// under kOverflowRepeatMs ago. The rest of an episode, the same queue
    /// still draining while the rate is cut, only keeps it open.
    void noteOverflowAt(int64_t nowMs)
    {
        const bool continuing =
            m_Backlogged || (m_OvfLastMs != kNever && nowMs - m_OvfLastMs <= kOverflowEpisodeMs);
        if (!continuing) {
            const bool repeat =
                m_OvfKbps > 0 && m_OvfLastMs != kNever && nowMs - m_OvfLastMs < kOverflowRepeatMs;
            if (!repeat)
                m_OvfWaitMs = kOverflowStepMs;
            else
                m_OvfWaitMs =
                    m_OvfWaitMs * 2 > kOverflowStepMaxMs ? kOverflowStepMaxMs : m_OvfWaitMs * 2;
            m_OvfKbps = m_Target;
            m_OvfEpisodes++;
            const int under = m_Target - m_Target * kCutPercent / 100;
            if (m_GoodKbps > under) m_GoodKbps = under;
        }
        m_OvfLastMs = nowMs;
        m_OvfStepMs = nowMs;
    }

    /// kOverflowForgetMs without an overflow: the rate that overflowed no
    /// longer says anything about the link.
    void forgetOverflow(int64_t nowMs)
    {
        if (m_OvfKbps <= 0 || m_Backlogged || nowMs - m_OvfLastMs < kOverflowForgetMs) return;
        m_OvfKbps = 0;
        m_OvfWaitMs = kOverflowStepMs;
    }

    /// The floor while the host's send buffer stays backed up: kFloorKbps,
    /// unless the session holds a higher share of its setting than the
    /// default (the guests' feed), which stays.
    int backlogFloorKbps() const
    {
        return m_FloorPercent > kFloorPercent ? floorKbps() : kBacklogFloorKbps;
    }

    bool cut(int64_t nowMs)
    {
        int next = m_Target - m_Target * kCutPercent / 100;
        const int floor = m_Backlogged ? backlogFloorKbps() : floorKbps();
        if (next < floor) next = floor;
        if (next > m_Setting) next = m_Setting;
        // A target already under the floor — a drained backlog left it there
        // — is not "cut" back up to it.
        if (next >= m_Target) return false;
        m_Target = next;
        m_LastCutMs = nowMs;
        m_Changes++;
        return true;
    }

    bool raise(int64_t nowMs)
    {
        if (m_Backlogged || m_Target >= m_Setting) return false;
        int next;
        // From one cut under the rate the last episode overflowed at up to
        // it: one slow step per wait, and no further while it is remembered.
        const bool nearOverflow =
            m_OvfKbps > 0 && m_Target >= m_OvfKbps - m_OvfKbps * kCutPercent / 100;
        if (nearOverflow) {
            if (m_Target >= m_OvfKbps) return false;
            if (nowMs - m_OvfStepMs < m_OvfWaitMs) return false;
            m_OvfStepMs = nowMs;
            next = m_Target + m_Target * kRaisePercent / 100;
            if (next > m_OvfKbps || next == m_Target) next = m_OvfKbps;
            if (next > m_Setting) next = m_Setting;
            if (next <= m_Target) return false;
            m_Target = next;
            m_Changes++;
            return true;
        }
        if (m_Target < m_GoodKbps) {
            // The first fast step of a climb remembers where it left from.
            if (m_LastFastRaiseMs == kNever) m_ClimbStartKbps = m_Target;
            m_LastFastRaiseMs = nowMs;
            m_LastRaiseFast = true;
            next = m_Target + m_Target * kFastRaisePercent / 100;
            if (next > m_GoodKbps) next = m_GoodKbps;
        } else {
            next = m_Target + m_Target * kRaisePercent / 100;
        }
        if (next > m_Setting) next = m_Setting;
        if (next == m_Target) next = m_Setting; // a rounding stall never sticks
        m_Target = next;
        m_Changes++;
        return true;
    }

    bool m_Follow = false; ///< the bench's governor=off
    int m_RetransCut = 0;  ///< retrcut=, per thousand; 0 off
    int m_RetransOveruses = 0;
    bool m_OverflowLowersGood = false; ///< ovfgood=
    int m_Overflows = 0;
    int m_FloorPercent = kFloorPercent;
    int m_Setting = 20000;
    int m_Target = 20000;
    int64_t m_QuietSinceMs = 0;
    int64_t m_LastReportMs = 0;
    int64_t m_HoldUntilMs = 0;
    bool m_SilenceCut = false;
    /// Where the link had left the target when the reports stopped, for the
    /// receiver that comes back and says the silence was its own.
    int m_TargetBeforeSilence = 0;
    int m_GoodKbps = 0;
    int m_GoodSampleKbps = 0;
    int64_t m_GoodSampleMs = 0;
    /// Where the current fast climb started, and when its last step was:
    /// kNever when no climb is under way or on probation.
    int m_ClimbStartKbps = 0;
    int64_t m_LastFastRaiseMs = kNever;
    bool m_LastRaiseFast = false;
    int64_t m_LastCutMs = kNever;
    /// The rate the last overflow episode overflowed at (0: none, or
    /// forgotten), when an overflow was last seen, when the last step near
    /// it was made (or the episode last seen), and the wait between two.
    int m_OvfKbps = 0;
    int64_t m_OvfLastMs = kNever;
    int64_t m_OvfStepMs = kNever;
    int64_t m_OvfWaitMs = kOverflowStepMs;
    int m_OvfEpisodes = 0;
    /// The host's send buffer backed up past kBacklogCutAfterMs, and the
    /// stretch it is (or was last) in.
    bool m_Backlogged = false;
    bool m_EpisodeEnded = false;
    BacklogEpisode m_Episode;
    int m_Backlogs = 0;
    int m_BacklogCuts = 0;
    int m_Overuses = 0;
    int m_Silences = 0;
    int m_Changes = 0;
    /// The target integrated over time, kbps × ms, since m_AreaSinceMs.
    int64_t m_AreaKbpsMs = 0;
    int64_t m_AreaSinceMs = 0;
    int64_t m_AreaLastMs = 0;
};

} // namespace mw::native::encode
