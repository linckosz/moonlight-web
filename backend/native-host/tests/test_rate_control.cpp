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

#include "encode/RateControl.h"
#include "encode/RateGovernor.h"
#include "mw/native/EncoderTuning.h"
#include "native_test_framework.h"

using mw::native::LinkFeedback;
using mw::native::encode::RateGovernor;

using mw::native::EncoderTuning;
using mw::native::encode::EffectiveCadence;
using mw::native::encode::intraRefreshCountFrames;
using mw::native::encode::intraRefreshPeriodFrames;
using mw::native::encode::kIntraRefreshMaxFrames;
using mw::native::encode::kIntraRefreshMinFrames;
using mw::native::encode::kIntraRefreshSeconds;
using mw::native::encode::kStillBoost;
using mw::native::encode::kStillMaxKbps;
using mw::native::encode::kVaapiMinQp;
using mw::native::encode::kVbvFrameRateFloor;
using mw::native::encode::LinkOccupancy;
using mw::native::encode::RefineConvergence;
using mw::native::encode::stillBitrateKbps;
using mw::native::encode::vaapiMinQp;
using mw::native::encode::vbvBitsPerFrame;

void run_rate_control_tests()
{
    constexpr uint32_t k55Mbps = 55000000u;

    SECTION("RateControl — at or below 60 fps the VBV is exactly one frame");
    {
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 60), k55Mbps / 60);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 30), k55Mbps / 30);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 20), k55Mbps / 20);
    }

    SECTION("RateControl — above 60 fps the budget stops shrinking");
    {
        const uint32_t floor = k55Mbps / static_cast<uint32_t>(kVbvFrameRateFloor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 61), floor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 120), floor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 144), floor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 165), floor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 240), floor);
        // The plain division AmfEncoder::setBitrate used to do, and what it cost:
        // a budget 2.4× tighter at 144 Hz than the one init() had chosen.
        CHECK(vbvBitsPerFrame(k55Mbps, 144) > 2 * (k55Mbps / 144));
    }

    SECTION("RateControl — an unknown frame rate reads as 60");
    {
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 0), k55Mbps / 60);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, -1), k55Mbps / 60);
    }

    SECTION("RateControl — a re-applied bitrate lands on the same VBV as init()");
    {
        // The still-screen burst raises the bitrate and puts it back through
        // setBitrate(); the budget it restores must be the one the session
        // started with, on every encoder, at every refresh rate.
        constexpr int kRates[] = {30, 60, 120, 144, 165, 240};
        for (int fps : kRates) {
            const uint32_t atInit = vbvBitsPerFrame(k55Mbps, fps);
            const uint32_t restored = vbvBitsPerFrame(k55Mbps, fps);
            CHECK_EQ(restored, atInit);
        }
    }

    SECTION("RateControl — the still-screen budget is three times the stream, capped");
    {
        CHECK_EQ(kStillBoost, 3);
        CHECK_EQ(stillBitrateKbps(20000), 20000 * kStillBoost);
        CHECK_EQ(stillBitrateKbps(55000), 55000 * kStillBoost);
        CHECK_EQ(stillBitrateKbps(200000), kStillMaxKbps);
        CHECK_EQ(stillBitrateKbps(0), 0);
        CHECK_EQ(stillBitrateKbps(-5), -5);
    }

    SECTION("RateControl — one boosted pass is at most 50 ms of link, at any bitrate");
    {
        // The multiplier is a latency: the longest one refinement pass can
        // occupy the link, hence the longest a motion frame right behind it
        // can wait. boost / 60 s, so 50 ms — whatever the stream's rate.
        constexpr int kRates[] = {5000, 20000, 55000, 150000};
        for (int kbps : kRates) {
            const uint32_t passBits =
                vbvBitsPerFrame(static_cast<uint32_t>(stillBitrateKbps(kbps)) * 1000u, 144);
            LinkOccupancy link;
            link.sent(0, passBits / 8, kbps);
            CHECK(link.backlogUs(0) <= 50000);
            CHECK(link.backlogUs(0) >= 49000);
            CHECK(!link.drainedAt(49000));
            CHECK(link.drainedAt(50000));
        }
    }

    SECTION("LinkOccupancy — bytes occupy the link for size / rate, back to back");
    {
        LinkOccupancy link;
        CHECK(link.drainedAt(0));
        CHECK_EQ(link.backlogUs(0), 0);

        // 250 KB at 20 Mbps is 100 ms.
        link.sent(1000, 250000, 20000);
        CHECK_EQ(link.backlogUs(1000), 100000);
        CHECK(!link.drainedAt(100999));
        CHECK(link.drainedAt(101000));

        // A second frame while the first is still going queues behind it,
        // not on top of it.
        link.sent(51000, 250000, 20000);
        CHECK_EQ(link.backlogUs(51000), 150000);
        CHECK(link.drainedAt(201000));

        // Once drained, a new frame starts from now, not from the old end.
        link.sent(500000, 25000, 20000);
        CHECK_EQ(link.backlogUs(500000), 10000);
    }

    SECTION("RefineConvergence — a low-bitrate burst ends on two tiny passes");
    {
        // What the 1 Mbps sessions always did: five passes, the last two of
        // almost nothing. Size alone is enough, with or without a QP.
        RefineConvergence conv;
        using V = RefineConvergence::Verdict;
        CHECK(conv.notePass(30000, -1) == V::Continue);
        CHECK(conv.notePass(9000, -1) == V::Continue);
        CHECK(conv.notePass(3000, -1) == V::Continue);
        CHECK(conv.notePass(500, -1) == V::Continue); // 4th pass: first quiet one
        CHECK(conv.notePass(400, -1) == V::Converged);
        CHECK_EQ(conv.passes, 5);
    }

    SECTION("RefineConvergence — under CBR the QP plateau ends it, size never would");
    {
        // 55 Mbps at 165 fps: every pass padded to ~120 KB, QP falling until
        // the residual is gone, then flat. Two flat passes after the minimum.
        RefineConvergence conv;
        using V = RefineConvergence::Verdict;
        CHECK(conv.notePass(120000, 30) == V::Continue);
        CHECK(conv.notePass(120000, 25) == V::Continue);
        CHECK(conv.notePass(120000, 20) == V::Continue);
        CHECK(conv.notePass(120000, 15) == V::Continue);  // still falling
        CHECK(conv.notePass(120000, 15) == V::Continue);  // flat once
        CHECK(conv.notePass(120000, 16) == V::Converged); // rising counts as flat
        CHECK_EQ(conv.passes, 6);
    }

    SECTION("RefineConvergence — a QP that resumes falling resets the streak");
    {
        RefineConvergence conv;
        using V = RefineConvergence::Verdict;
        for (int i = 0; i < 4; i++)
            CHECK(conv.notePass(120000, 30 - i) == V::Continue);
        CHECK(conv.notePass(120000, 27) == V::Continue); // flat once
        CHECK(conv.notePass(120000, 22) == V::Continue); // fell again: streak reset
        CHECK(conv.notePass(120000, 22) == V::Continue); // flat once
        CHECK(conv.notePass(120000, 21) == V::Capped);   // 8th, still falling: the ceiling
    }

    SECTION("RefineConvergence — no QP and big passes run into the cap");
    {
        RefineConvergence conv;
        using V = RefineConvergence::Verdict;
        for (int i = 1; i < RefineConvergence::kMaxPasses; i++)
            CHECK(conv.notePass(120000, -1) == V::Continue);
        CHECK(conv.notePass(120000, -1) == V::Capped);
        CHECK_EQ(conv.passes, RefineConvergence::kMaxPasses);
    }

    SECTION("RefineConvergence — the cap is at most 400 ms of link");
    {
        // Eight passes of one boosted VBV each, at the stream's own rate.
        constexpr int kRates[] = {5000, 20000, 55000};
        for (int kbps : kRates) {
            const uint32_t passBits =
                vbvBitsPerFrame(static_cast<uint32_t>(stillBitrateKbps(kbps)) * 1000u, 60);
            LinkOccupancy link;
            for (int i = 0; i < RefineConvergence::kMaxPasses; i++)
                link.sent(0, passBits / 8, kbps);
            CHECK(link.backlogUs(0) <= 400000);
        }
    }

    SECTION("RefineConvergence — reset forgets everything");
    {
        RefineConvergence conv;
        conv.notePass(120000, 20);
        conv.notePass(120000, 20);
        conv.reset();
        CHECK_EQ(conv.passes, 0);
        CHECK_EQ(conv.quiet, 0);
        CHECK_EQ(conv.lastQp, -1);
    }

    SECTION("IntraRefresh — one sweep is two seconds at the effective rate");
    {
        CHECK_EQ(kIntraRefreshSeconds, 2);
        CHECK_EQ(intraRefreshPeriodFrames(30), 60);
        CHECK_EQ(intraRefreshPeriodFrames(60), 120); // what the three paths hard-coded
        CHECK_EQ(intraRefreshPeriodFrames(120), 240);
        CHECK_EQ(intraRefreshPeriodFrames(144), 288);
        CHECK_EQ(intraRefreshPeriodFrames(165), 330);
        CHECK_EQ(intraRefreshPeriodFrames(240), 480);
    }

    SECTION("IntraRefresh — an unknown rate reads as 60, like the VBV");
    {
        CHECK_EQ(intraRefreshPeriodFrames(0), 120);
        CHECK_EQ(intraRefreshPeriodFrames(-1), 120);
    }

    SECTION("IntraRefresh — the bounds only catch nonsense");
    {
        CHECK_EQ(intraRefreshPeriodFrames(5), kIntraRefreshMinFrames);
        CHECK_EQ(intraRefreshPeriodFrames(1000), kIntraRefreshMaxFrames);
        // Every real stream rate lands strictly inside them.
        constexpr int kRates[] = {24, 30, 60, 90, 120, 144, 165, 240};
        for (int fps : kRates) {
            CHECK(intraRefreshPeriodFrames(fps) >= kIntraRefreshMinFrames);
            CHECK(intraRefreshPeriodFrames(fps) <= kIntraRefreshMaxFrames);
        }
    }

    SECTION("IntraRefresh — the NVENC wave is half the period, always shorter than it");
    {
        constexpr int kRates[] = {0, 24, 30, 60, 120, 144, 165, 240};
        for (int fps : kRates) {
            CHECK_EQ(intraRefreshCountFrames(fps), intraRefreshPeriodFrames(fps) / 2);
            CHECK(intraRefreshCountFrames(fps) < intraRefreshPeriodFrames(fps));
            CHECK(intraRefreshCountFrames(fps) > 0);
        }
    }

    SECTION("IntraRefresh — AMD's per-slot count rounds down, so the sweep never runs short");
    {
        // What AmfEncoder computes: blocks in the picture divided by the
        // period, at least one. A 1080p HEVC picture has 510 CTBs; at 240 fps
        // one per frame sweeps in 510 frames, a little over two seconds, never
        // under.
        const int period240 = intraRefreshPeriodFrames(240);
        const int ctbs1080p = ((1920 + 63) / 64) * ((1080 + 63) / 64);
        CHECK_EQ(ctbs1080p, 510);
        const int perSlot = ctbs1080p / period240 > 0 ? ctbs1080p / period240 : 1;
        CHECK(ctbs1080p / perSlot >= period240);
        // At 60 fps the same picture takes 4 CTBs per frame: 128 frames, still
        // never shorter than the 120 asked for.
        const int period60 = intraRefreshPeriodFrames(60);
        const int perSlot60 = ctbs1080p / period60;
        CHECK_EQ(perSlot60, 4);
        CHECK(ctbs1080p / perSlot60 >= period60);
    }

    SECTION("LinkOccupancy — an unknown rate models nothing");
    {
        LinkOccupancy link;
        link.sent(0, 1000000, 0);
        link.sent(0, 1000000, -1);
        CHECK(link.drainedAt(0));
    }

    SECTION("LinkOccupancy — CBR motion frames keep the link level");
    {
        // One interval of link time, sent once per interval: the backlog
        // never grows past one frame, so the gate is invisible to motion and
        // the first refinement pass after it waits at most one interval.
        LinkOccupancy link;
        const int kbps = 55000;
        const size_t frameBytes = vbvBitsPerFrame(kbps * 1000u, 60) / 8;
        int64_t now = 0;
        for (int i = 0; i < 600; i++) {
            link.sent(now, frameBytes, kbps);
            CHECK(link.backlogUs(now) <= 16667);
            now += 16667;
        }
        CHECK(link.drainedAt(now));
    }

    SECTION("EffectiveCadence — a 60 fps game under a 165 fps stream gets the whole budget");
    {
        // The bench case: 40 Mbit/s set, frames at 60/s, encoder told 165. The
        // rate lowers after two agreeing windows and the encoder is handed
        // 165/60 of the bitrate — the wire still carries 40.
        EffectiveCadence c;
        c.start(165, 0);
        CHECK_EQ(c.currentFps, 165);
        CHECK_EQ(c.scaledKbps(40000), 40000);
        CHECK(!c.evaluate(60)); // first window: noted, not acted on
        CHECK_EQ(c.currentFps, 165);
        CHECK(c.evaluate(60)); // second: lowered
        CHECK_EQ(c.currentFps, 60);
        CHECK(c.scaling());
        CHECK_EQ(c.scaledKbps(40000), 40000 * 165 / 60);
        // Wire rate is unchanged: scaled bitrate × frames = configured bitrate.
        CHECK_EQ(static_cast<int64_t>(c.scaledKbps(40000)) * 60 / 165, 40000);
    }

    SECTION("EffectiveCadence — raising is immediate, lowering waits");
    {
        EffectiveCadence c;
        c.start(165, 0);
        c.evaluate(60);
        c.evaluate(60);
        CHECK_EQ(c.currentFps, 60);
        // The game jumps to 120: every frame is now too big, act at once.
        CHECK(c.evaluate(120));
        CHECK_EQ(c.currentFps, 120);
        // One slow window is a hitch, not a new rate.
        CHECK(!c.evaluate(40));
        CHECK_EQ(c.currentFps, 120);
        CHECK(!c.evaluate(118)); // back to normal: the streak is forgotten
        CHECK(!c.evaluate(40));
        CHECK_EQ(c.currentFps, 120);
        CHECK(c.evaluate(40));
        CHECK_EQ(c.currentFps, 40);
    }

    SECTION("EffectiveCadence — hysteresis: hovering around a rate moves nothing");
    {
        EffectiveCadence c;
        c.start(165, 0);
        c.evaluate(60);
        c.evaluate(60);
        constexpr int kHovering[] = {55, 65, 58, 68, 52, 69};
        for (int fps : kHovering) {
            CHECK(!c.evaluate(fps));
            CHECK_EQ(c.currentFps, 60);
        }
        CHECK_EQ(c.changes, 1);
    }

    SECTION("EffectiveCadence — retarget: the gate moved, the budget follows at once");
    {
        // Encoder built for 60. The client's screen changes and the gate now
        // admits 72 a second: each frame must get 60/72 of its budget so the
        // wire still carries the setting.
        EffectiveCadence c;
        c.start(60, 0);
        CHECK(c.retarget(72));
        CHECK_EQ(c.configuredFps, 72);
        CHECK_EQ(c.encoderFps, 60);
        CHECK_EQ(c.currentFps, 72);
        CHECK(c.scaling());
        CHECK_EQ(c.scaledKbps(36000), 36000 * 60 / 72);
        CHECK_EQ(static_cast<int64_t>(c.scaledKbps(36000)) * 72 / 60, 36000);
        // Frames measured above the new gate never inflate past it.
        CHECK(!c.evaluate(90));
        CHECK_EQ(c.currentFps, 72);
        // Back to 60: no scaling, same as never having moved.
        CHECK(c.retarget(60));
        CHECK(!c.scaling());
        CHECK_EQ(c.scaledKbps(36000), 36000);
        // Same value again: nothing to do.
        CHECK(!c.retarget(60));
        CHECK(!c.retarget(0));

        // Down to 55 on a 60 encoder: bigger frames, fewer of them.
        CHECK(c.retarget(55));
        CHECK_EQ(c.scaledKbps(36000), 36000 * 60 / 55);
        // A game at 30 under the 55 gate still lowers after two windows, from
        // the encoder's 60 — not from 55.
        c.evaluate(30);
        CHECK(c.evaluate(30));
        CHECK_EQ(c.currentFps, 30);
        CHECK_EQ(c.scaledKbps(36000), 36000 * 60 / 30);
    }

    SECTION("EffectiveCadence — bounded by 30 below and the setting above");
    {
        EffectiveCadence c;
        c.start(60, 0);
        c.evaluate(5);
        CHECK(c.evaluate(5));
        CHECK_EQ(c.currentFps, EffectiveCadence::kMinFps);
        CHECK_EQ(c.scaledKbps(20000), 20000 * 60 / 30);
        // Frames faster than the setting (the cadence gate lets a few through
        // early) never inflate the budget above the viewer's own statement.
        CHECK(c.evaluate(200));
        CHECK_EQ(c.currentFps, 60);
        CHECK(!c.scaling());
        CHECK_EQ(c.scaledKbps(20000), 20000);
    }

    SECTION("EffectiveCadence — a rate that goes up is caught within a quarter second");
    {
        // Settled at 60 (two windows), then the game jumps to 165: the budget
        // must come back down before a whole second of oversized frames has
        // gone out — the quarter-second sub-window catches it.
        EffectiveCadence c;
        c.start(165, 0);
        c.evaluate(60);
        c.evaluate(60);
        CHECK_EQ(c.currentFps, 60);
        int64_t now = 0;
        int64_t raisedAt = -1;
        for (int i = 1; i <= 165 && raisedAt < 0; i++) {
            now = i * 6061;
            if (c.noteFrame(now)) raisedAt = now;
        }
        CHECK(raisedAt > 0);
        CHECK(raisedAt <= 300000); // well under a second
        CHECK_EQ(c.currentFps, 165);
    }

    SECTION("RateGovernor — a rising delay cuts at once, quiet raises slowly");
    {
        RateGovernor g;
        g.start(40000, 0);
        CHECK_EQ(g.targetKbps(), 40000);
        CHECK(!g.limiting());
        LinkFeedback rising;
        rising.owdRiseMs = 45;
        CHECK(g.report(rising, 500));
        CHECK_EQ(g.targetKbps(), 32000);
        CHECK(g.limiting());
        // Inside the two-second hold, quiet does nothing yet.
        LinkFeedback quiet;
        CHECK(!g.report(quiet, 1000));
        CHECK(!g.report(quiet, 2000));
        CHECK_EQ(g.targetKbps(), 32000);
        // Quiet since the cut, three seconds of it: the first raise, 5 %.
        CHECK(!g.report(quiet, 3000));
        CHECK(g.report(quiet, 4000));
        CHECK_EQ(g.targetKbps(), 33600);
        // And on, one step per report, up to the setting and no further:
        // 33 600 → 35 280 → 37 044 → 38 896 → 40 000.
        int64_t t = 4500;
        int steps = 0;
        while (g.limiting() && steps < 100) {
            g.report(quiet, t);
            t += 500;
            steps++;
        }
        CHECK_EQ(g.targetKbps(), 40000);
        CHECK_EQ(steps, 4); // gradual, not a jump back
        CHECK(!g.report(quiet, t));
    }

    SECTION("RateGovernor — loss and evictions are overuse whatever the delay says");
    {
        RateGovernor g;
        g.start(20000, 0);
        LinkFeedback gap;
        gap.gaps = 1;
        CHECK(g.report(gap, 500));
        CHECK_EQ(g.targetKbps(), 16000);
        LinkFeedback evicted;
        evicted.evictions = 2;
        CHECK(g.report(evicted, 1000)); // a second overuse inside the hold cuts again
        CHECK_EQ(g.targetKbps(), 12800);
    }

    SECTION("RateGovernor — calm: at the setting, every report quiet for the time asked");
    {
        RateGovernor g;
        g.start(20000, 0);
        LinkFeedback quiet;
        CHECK(!g.calm(1000, 2000));
        g.report(quiet, 1500);
        CHECK(g.calm(2000, 2000));
        // A queue there but not growing: no cut, and no calm for two seconds.
        LinkFeedback standing;
        standing.owdRiseMs = 15;
        CHECK(!g.report(standing, 2500));
        CHECK(!g.limiting());
        CHECK(!g.calm(4000, 2000));
        g.report(quiet, 4000);
        CHECK(g.calm(4500, 2000));
        // A cut: not calm until the target is back at the setting.
        LinkFeedback rising;
        rising.owdRiseMs = 45;
        CHECK(g.report(rising, 5000));
        CHECK(!g.calm(5500, 0));
        g.report(quiet, 7500);
        CHECK(!g.calm(8000, 2000));
        // Reports that stopped coming: not calm either.
        RateGovernor s;
        s.start(20000, 0);
        s.report(quiet, 100);
        CHECK(s.calm(3000, 2000));
        CHECK(!s.calm(4200, 2000));
    }

    SECTION("RateGovernor — SCTP's retransmissions cut only under retrcut=");
    {
        // The Windows session's own, measured (plan Wi-Fi W2 B).
        CHECK_EQ(RateGovernor::kRetransCutPermille, 3);
        // Unset, the governor does not look at SCTP, whatever it says.
        RateGovernor off;
        off.start(40000, 0);
        CHECK_EQ(off.retransCut(), 0);
        LinkFeedback retrans;
        retrans.retransPermille = 30;
        CHECK(!off.report(retrans, 500));
        CHECK_EQ(off.targetKbps(), 40000);
        CHECK_EQ(off.retransOveruses(), 0);

        // retrcut=5: 5 in a thousand or more is overuse, with no delay rise.
        RateGovernor g;
        g.start(40000, 0);
        g.setRetransCut(5);
        LinkFeedback four;
        four.retransPermille = 4;
        CHECK(!g.report(four, 500));
        CHECK_EQ(g.targetKbps(), 40000);
        LinkFeedback five;
        five.retransPermille = 5;
        CHECK(g.report(five, 1000));
        CHECK_EQ(g.targetKbps(), 32000);
        CHECK_EQ(g.retransOveruses(), 1);
        // Under the threshold but not under half of it: held, never raised.
        LinkFeedback three;
        three.retransPermille = 3;
        for (int64_t t = 1500; t <= 8000; t += 500)
            CHECK(!g.report(three, t));
        CHECK_EQ(g.targetKbps(), 32000);
        // Under half of it (2 * 2 < 5): quiet, and after three seconds a raise.
        LinkFeedback two;
        two.retransPermille = 2;
        // Quiet counts from the last report that was not (8000).
        CHECK(!g.report(two, 8500));
        CHECK(!g.report(two, 10500));
        CHECK(g.report(two, 11000));
        CHECK(g.targetKbps() > 32000);
        // A negative or zero cut is off.
        g.setRetransCut(-3);
        CHECK_EQ(g.retransCut(), 0);
    }

    SECTION("RateGovernor — never below the floor, never above the setting");
    {
        RateGovernor g;
        g.start(10000, 0);
        CHECK_EQ(g.floorKbps(), RateGovernor::kFloorKbps); // 20 % of 10 000 is under it
        LinkFeedback bad;
        bad.owdRiseMs = 200;
        for (int i = 0; i < 40; i++)
            g.report(bad, i * 500);
        CHECK_EQ(g.targetKbps(), RateGovernor::kFloorKbps);
        RateGovernor big;
        big.start(100000, 0);
        CHECK_EQ(big.floorKbps(), 20000);
        for (int i = 0; i < 40; i++)
            big.report(bad, i * 500);
        CHECK_EQ(big.targetKbps(), 20000);
    }

    SECTION("RateGovernor — a session's own floor: the guests' feed holds 60 % of its setting");
    {
        // The shared feed of a native host's guests (SessionConfig::
        // governorFloorPercent): the slowest guest pulls the rate down, never
        // under 60 %; under that it drops frames alone.
        RateGovernor g;
        g.start(10000, 0, false, 60);
        CHECK_EQ(g.floorPercent(), 60);
        CHECK_EQ(g.floorKbps(), 6000);
        LinkFeedback bad;
        bad.owdRiseMs = 200;
        CHECK(g.report(bad, 500));
        CHECK_EQ(g.targetKbps(), 8000);
        CHECK(g.report(bad, 1000));
        CHECK_EQ(g.targetKbps(), 6400);
        CHECK(g.report(bad, 1500));
        CHECK_EQ(g.targetKbps(), 6000); // 5 120 would be under the floor
        CHECK(!g.report(bad, 2000));    // at the floor: nothing left to cut
        CHECK_EQ(g.targetKbps(), 6000);
        // kFloorKbps still under a small setting's share.
        RateGovernor small;
        small.start(3000, 0, false, 60);
        CHECK_EQ(small.floorKbps(), RateGovernor::kFloorKbps);
        // Out of range is clamped, and the default is unchanged.
        RateGovernor odd;
        odd.start(10000, 0, false, 150);
        CHECK_EQ(odd.floorPercent(), 100);
        CHECK_EQ(odd.floorKbps(), 10000);
        RateGovernor plain;
        plain.start(10000, 0);
        CHECK_EQ(plain.floorPercent(), RateGovernor::kFloorPercent);
    }

    SECTION("RateGovernor — a queue that is present but not growing holds");
    {
        RateGovernor g;
        g.start(40000, 0);
        LinkFeedback rising;
        rising.owdRiseMs = 40;
        g.report(rising, 500);
        CHECK_EQ(g.targetKbps(), 32000);
        LinkFeedback middling;
        middling.owdRiseMs = 20; // between quiet and overuse
        for (int i = 0; i < 20; i++)
            CHECK(!g.report(middling, 3000 + i * 500));
        CHECK_EQ(g.targetKbps(), 32000);
    }

    SECTION("RateGovernor — the ceiling moving down takes the target with it");
    {
        RateGovernor g;
        g.start(40000, 0);
        g.setSetting(15000);
        CHECK_EQ(g.targetKbps(), 15000);
        CHECK(!g.limiting());
        g.setSetting(40000); // back up: the target stays where the link left it
        CHECK_EQ(g.targetKbps(), 15000);
        CHECK(g.limiting());
    }

    SECTION("RateGovernor — silence from the receiver is one cut, not a slide");
    {
        RateGovernor g;
        g.start(40000, 0);
        LinkFeedback quiet;
        g.report(quiet, 500);
        CHECK(!g.tick(3000));
        CHECK(g.tick(4600));
        CHECK_EQ(g.targetKbps(), 32000);
        CHECK(!g.tick(9000)); // still silent: no further cut
        CHECK(!g.tick(20000));
        CHECK_EQ(g.targetKbps(), 32000);
        // A report resumes the ordinary rules — quiet all this time, so the
        // first one already raises.
        CHECK(g.report(quiet, 20500));
        CHECK_EQ(g.targetKbps(), 33600);
        CHECK_EQ(g.silences(), 1);
    }

    SECTION("RateGovernor — a receiver back from the background undoes the silence cut");
    {
        RateGovernor g;
        g.start(40000, 0);
        LinkFeedback quiet;
        g.report(quiet, 500);
        CHECK(g.tick(4600));
        CHECK_EQ(g.targetKbps(), 32000);
        // Its first report names the frames it missed while hidden — evicted
        // by our own sender — and a delay that straddles the thaw burst, and
        // says it was away: not overuse, and the precautionary cut is lifted
        // at once rather than climbed back in five raises.
        LinkFeedback back;
        back.resumed = true;
        back.gaps = 12;
        back.owdRiseMs = 400;
        CHECK(g.report(back, 20500));
        CHECK_EQ(g.targetKbps(), 40000);
        CHECK(!g.limiting());
        CHECK_EQ(g.overuses(), 0);
        // The next report is an ordinary one: a queue it measures is real.
        LinkFeedback rising;
        rising.owdRiseMs = 50;
        CHECK(g.report(rising, 21000));
        CHECK_EQ(g.targetKbps(), 32000);

        // A cut a real overuse made before the page went away stays: only the
        // silence cut is undone.
        RateGovernor h;
        h.start(40000, 0);
        h.report(rising, 500);
        CHECK_EQ(h.targetKbps(), 32000);
        CHECK(h.tick(4600));
        CHECK_EQ(h.targetKbps(), 25600);
        CHECK(h.report(back, 20500));
        CHECK_EQ(h.targetKbps(), 32000);
        // No silence cut standing: "resumed" changes nothing, and is still
        // not read as overuse.
        CHECK(!h.report(back, 21000));
        CHECK_EQ(h.targetKbps(), 32000);
        CHECK_EQ(h.overuses(), 1);
    }

    SECTION("RateGovernor — a freeze on a proven link climbs back fast");
    {
        // The corporate Wi-Fi of 16/09/2026: a link that carried the setting,
        // then froze, and a string of overuse reports while its queue drained.
        RateGovernor g;
        g.start(20000, 0);
        LinkFeedback quiet;
        for (int64_t t = 500; t <= 6000; t += 500)
            g.report(quiet, t);
        CHECK_EQ(g.goodKbps(), 20000);
        LinkFeedback frozen;
        frozen.owdRiseMs = 200;
        for (int64_t t = 6500; t <= 10000; t += 500)
            g.report(frozen, t);
        CHECK_EQ(g.targetKbps(), 4000); // the floor, as before
        CHECK_EQ(g.goodKbps(), 20000);  // a freeze proves nothing against it
        // Hold and quiet as before: nothing until 13 000.
        CHECK(!g.report(quiet, 10500));
        CHECK(!g.report(quiet, 12500));
        CHECK(g.report(quiet, 13000));
        CHECK(g.lastRaiseFast());
        CHECK_EQ(g.targetKbps(), 5000);
        // 5 000 → 6 250 → 7 812 → 9 765 → 12 206 → 15 257 → 19 071 → 20 000.
        int64_t t = 13500;
        int steps = 1;
        while (g.limiting() && steps < 100) {
            g.report(quiet, t);
            t += 500;
            steps++;
        }
        CHECK_EQ(g.targetKbps(), 20000);
        CHECK_EQ(steps, 8); // four seconds, where 5 % steps took thirty
    }

    SECTION("RateGovernor — a fast climb that runs into a queue falls back to a slow one");
    {
        RateGovernor g;
        g.start(20000, 0);
        LinkFeedback quiet;
        for (int64_t t = 500; t <= 6000; t += 500)
            g.report(quiet, t);
        LinkFeedback frozen;
        frozen.owdRiseMs = 200;
        for (int64_t t = 6500; t <= 10000; t += 500)
            g.report(frozen, t);
        CHECK(g.report(quiet, 13000));
        CHECK(g.report(quiet, 13500));
        CHECK_EQ(g.targetKbps(), 6250);
        // The link really narrowed: overuse in the middle of the climb.
        LinkFeedback rising;
        rising.owdRiseMs = 45;
        CHECK(g.report(rising, 14000));
        CHECK_EQ(g.targetKbps(), 5000);
        CHECK_EQ(g.goodKbps(), 4000); // where the climb had started
        // The next climb is the slow one.
        CHECK(!g.report(quiet, 16500));
        CHECK(g.report(quiet, 17000));
        CHECK(!g.lastRaiseFast());
        CHECK_EQ(g.targetKbps(), 5250);
    }

    SECTION("RateGovernor — shed frames move nothing while the rule is off (ovfgood=0)");
    {
        RateGovernor g;
        g.start(30000, 0);
        CHECK(!g.overflowLowersGood());
        LinkFeedback quiet;
        for (int64_t t = 500; t <= 6000; t += 500)
            g.report(quiet, t);
        LinkFeedback shed;
        shed.overflows = 3;
        CHECK(!g.report(shed, 6500));
        CHECK_EQ(g.targetKbps(), 30000);
        CHECK_EQ(g.goodKbps(), 30000);
        CHECK_EQ(g.overuses(), 0);
        CHECK_EQ(g.overflows(), 0);
    }

    SECTION("RateGovernor — the product's rule: a queue that overflowed lowers the good rate");
    {
        // The N95 of 19/10/2026: 30 Mbit/s proven, then the send buffer
        // overflowed at that rate long after the last climb.
        RateGovernor g;
        g.start(30000, 0);
        g.setOverflowLowersGood(true);
        LinkFeedback quiet;
        for (int64_t t = 500; t <= 14500; t += 500)
            g.report(quiet, t);
        CHECK_EQ(g.goodKbps(), 30000);
        LinkFeedback shed;
        shed.overflows = 1;
        CHECK(g.report(shed, 15000)); // overuse on its own, no delay needed
        CHECK_EQ(g.targetKbps(), 24000);
        CHECK_EQ(g.goodKbps(), 24000); // one cut under the rate that overflowed
        CHECK_EQ(g.overflows(), 1);
        LinkFeedback rising;
        rising.owdRiseMs = 45;
        CHECK(g.report(rising, 15500));
        CHECK_EQ(g.targetKbps(), 19200);
        CHECK_EQ(g.goodKbps(), 24000); // a delay alone still proves nothing
        // Back fast to the good rate, then the slow steps above it — one per
        // five seconds since the overflow, near the rate that overflowed
        // (DebitRemonte2): 19 200 → 24 000, then 25 200 at 20 000 → 26 460 →
        // 27 783 → 29 172 → 30 000 at 40 000.
        CHECK(!g.report(quiet, 18000));
        CHECK(g.report(quiet, 18500));
        CHECK(g.lastRaiseFast());
        CHECK_EQ(g.targetKbps(), 24000);
        CHECK(!g.report(quiet, 19000));
        CHECK(!g.lastRaiseFast());
        CHECK_EQ(g.targetKbps(), 24000);
        CHECK(g.report(quiet, 20000));
        CHECK_EQ(g.targetKbps(), 25200);
        int raises = 0;
        int64_t last = 0;
        for (int64_t t = 20500; t <= 45000; t += 500) {
            if (g.report(quiet, t)) {
                raises++;
                last = t;
            }
        }
        CHECK_EQ(g.targetKbps(), 30000);
        CHECK_EQ(raises, 4);
        CHECK_EQ(last, 40000);
        // Without the key, the same reports went from 24 000 straight back.
        RateGovernor off;
        off.start(30000, 0);
        for (int64_t u = 500; u <= 14500; u += 500)
            off.report(quiet, u);
        off.report(rising, 15000);
        off.report(rising, 15500);
        off.report(quiet, 18500);
        off.report(quiet, 19000);
        CHECK_EQ(off.targetKbps(), 30000);
    }

    SECTION("RateGovernor — ovfgood=: the probe's own rule still holds inside the grace");
    {
        RateGovernor g;
        g.start(20000, 0);
        g.setOverflowLowersGood(true);
        LinkFeedback quiet;
        for (int64_t t = 500; t <= 6000; t += 500)
            g.report(quiet, t);
        LinkFeedback frozen;
        frozen.owdRiseMs = 200;
        for (int64_t t = 6500; t <= 10000; t += 500)
            g.report(frozen, t);
        CHECK(g.report(quiet, 13000));
        CHECK(g.report(quiet, 13500));
        CHECK(g.report(quiet, 14000));
        CHECK_EQ(g.targetKbps(), 7812);
        LinkFeedback shed;
        shed.overflows = 2;
        CHECK(g.report(shed, 14500));
        CHECK_EQ(g.goodKbps(), 4000); // where the climb started, under 6 250
    }

    SECTION("RateGovernor — near the rate that overflowed, one step per wait (the N95 of 13/11)");
    {
        // iPhone in 5G, 18 Mbit/s set: cut to 14 400, back at 17 502 six
        // seconds later and overflowed again, three times in 18 s.
        RateGovernor g;
        g.start(18000, 0);
        g.setOverflowLowersGood(true);
        LinkFeedback quiet;
        for (int64_t t = 500; t <= 6000; t += 500)
            g.report(quiet, t);
        CHECK_EQ(g.goodKbps(), 18000);
        LinkFeedback shed;
        shed.overflows = 1;
        CHECK(g.report(shed, 6500));
        CHECK_EQ(g.targetKbps(), 14400);
        CHECK_EQ(g.goodKbps(), 14400);
        CHECK_EQ(g.overflowKbps(), 18000);
        CHECK_EQ(g.overflowWaitMs(), RateGovernor::kOverflowStepMs);
        // Nothing before five seconds, where the old climb was back at 17 502.
        for (int64_t t = 7000; t < 11500; t += 500)
            CHECK(!g.report(quiet, t));
        CHECK_EQ(g.targetKbps(), 14400);
        CHECK(g.report(quiet, 11500));
        CHECK_EQ(g.targetKbps(), 15120);
        // Then one step per five seconds: 15 876, 16 669, 17 502, 18 000.
        int raises = 0;
        int64_t first = 0;
        for (int64_t t = 12000; t <= 32000; t += 500) {
            if (g.report(quiet, t)) {
                if (first == 0) first = t;
                raises++;
            }
        }
        CHECK_EQ(raises, 4);
        CHECK_EQ(first, 16500);
        CHECK_EQ(g.targetKbps(), 18000);
        // Overflowed again within 30 s: the wait doubles.
        CHECK(g.report(shed, 33000));
        CHECK_EQ(g.targetKbps(), 14400);
        CHECK_EQ(g.overflowWaitMs(), 2 * RateGovernor::kOverflowStepMs);
        CHECK_EQ(g.overflowEpisodes(), 2);
        for (int64_t t = 33500; t < 43000; t += 500)
            CHECK(!g.report(quiet, t));
        CHECK(g.report(quiet, 43000));
        CHECK_EQ(g.targetKbps(), 15120);
    }

    SECTION("RateGovernor — one overflow is one episode, however many reports it spans");
    {
        RateGovernor g;
        g.start(18000, 0);
        g.setOverflowLowersGood(true);
        LinkFeedback quiet;
        for (int64_t t = 500; t <= 6000; t += 500)
            g.report(quiet, t);
        LinkFeedback shed;
        shed.overflows = 1;
        CHECK(g.report(shed, 6500));
        CHECK(g.report(shed, 7000));
        CHECK(g.report(shed, 7500));
        CHECK_EQ(g.targetKbps(), 9216);
        // The queue draining at lower rates is the same overflow: the good
        // rate and the rate remembered stay where it began.
        CHECK_EQ(g.goodKbps(), 14400);
        CHECK_EQ(g.overflowKbps(), 18000);
        CHECK_EQ(g.overflowEpisodes(), 1);
        CHECK_EQ(g.overflows(), 3);
        CHECK_EQ(g.overflowWaitMs(), RateGovernor::kOverflowStepMs);
        // Fast back to one cut under it, then the wait from the last report.
        CHECK(!g.report(quiet, 10000));
        CHECK(g.report(quiet, 10500));
        CHECK(g.lastRaiseFast());
        CHECK_EQ(g.targetKbps(), 11520);
        CHECK(g.report(quiet, 11000));
        CHECK_EQ(g.targetKbps(), 14400);
        CHECK(!g.report(quiet, 11500));
        CHECK(!g.report(quiet, 12000));
        CHECK(g.report(quiet, 12500));
        CHECK_EQ(g.targetKbps(), 15120);
    }

    SECTION("RateGovernor — a minute without an overflow forgets it");
    {
        RateGovernor g;
        g.start(30000, 0);
        g.setOverflowLowersGood(true);
        LinkFeedback rising;
        rising.owdRiseMs = 45;
        CHECK(g.report(rising, 500));
        CHECK_EQ(g.targetKbps(), 24000);
        LinkFeedback shed;
        shed.overflows = 1;
        CHECK(g.report(shed, 3000)); // overflowed at 24 000, under the setting
        CHECK_EQ(g.targetKbps(), 19200);
        CHECK_EQ(g.overflowKbps(), 24000);
        LinkFeedback quiet;
        for (int64_t t = 3500; t <= 62500; t += 500)
            g.report(quiet, t);
        // Back at the rate that overflowed, and no further while remembered.
        CHECK_EQ(g.targetKbps(), 24000);
        CHECK(g.limiting());
        CHECK_EQ(g.overflowKbps(), 24000);
        // Sixty seconds after it: forgotten, the ordinary climb again.
        CHECK(g.report(quiet, 63000));
        CHECK_EQ(g.overflowKbps(), 0);
        CHECK_EQ(g.targetKbps(), 25200);
        CHECK(g.report(quiet, 63500));
        CHECK_EQ(g.targetKbps(), 26460);
        // An overflow long after the last: the first wait, not a doubled one.
        CHECK(g.report(shed, 64000));
        CHECK_EQ(g.overflowWaitMs(), RateGovernor::kOverflowStepMs);
        CHECK_EQ(g.overflowEpisodes(), 2);
    }

    SECTION("RateGovernor — the host's own backlog cuts without a report, down to 2 Mbit/s");
    {
        RateGovernor g;
        g.start(18000, 0);
        g.setOverflowLowersGood(true);
        CHECK_EQ(g.floorKbps(), 3600);
        CHECK(!g.backlog(900, 1000)); // under a second: the relay's own business
        CHECK(!g.backlogged());
        CHECK(g.backlog(1200, 1300));
        CHECK(g.backlogged());
        CHECK_EQ(g.targetKbps(), 14400);
        CHECK_EQ(g.overflowKbps(), 18000);
        CHECK(!g.backlog(1400, 1500)); // a cut under half a second ago
        // A cut every half second, past the ordinary floor down to 2 Mbit/s.
        for (int64_t t = 1800; t <= 8000; t += 500)
            g.backlog(t - 100, t);
        CHECK_EQ(g.targetKbps(), RateGovernor::kBacklogFloorKbps);
        CHECK(!g.backlog(8200, 8300)); // nothing left to cut
        // A report with no frame in its window reads quiet: it raises nothing.
        LinkFeedback quiet;
        CHECK(!g.report(quiet, 8400));
        CHECK_EQ(g.targetKbps(), RateGovernor::kBacklogFloorKbps);
        // Drained: the episode, once.
        CHECK(!g.backlog(0, 8500));
        CHECK(!g.backlogged());
        RateGovernor::BacklogEpisode ep;
        CHECK(g.takeEndedBacklog(ep));
        CHECK(!g.takeEndedBacklog(ep));
        CHECK_EQ(ep.startMs, 100);
        CHECK_EQ(ep.endMs, 8500);
        CHECK_EQ(ep.fromKbps, 18000);
        CHECK_EQ(ep.lowestKbps, RateGovernor::kBacklogFloorKbps);
        CHECK_EQ(ep.cuts, 10);
        CHECK_EQ(g.backlogCuts(), 10);
        CHECK_EQ(g.backlogs(), 1);
        CHECK_EQ(g.overflowEpisodes(), 1);
        // Quiet counts from the drain; the climb starts under the ordinary
        // floor, where the backlog left it.
        CHECK(!g.report(quiet, 11000));
        CHECK(g.report(quiet, 11500));
        CHECK_EQ(g.targetKbps(), 2100);
        // An overuse down there does not "cut" it back up to that floor.
        LinkFeedback rising;
        rising.owdRiseMs = 45;
        CHECK(!g.report(rising, 12000));
        CHECK_EQ(g.targetKbps(), 2100);
    }

    SECTION("RateGovernor — a report's shed frames during a backlog are the same episode");
    {
        RateGovernor g;
        g.start(18000, 0);
        g.setOverflowLowersGood(true);
        CHECK(g.backlog(1200, 1300));
        LinkFeedback shed;
        shed.overflows = 1;
        CHECK(g.report(shed, 1600)); // a report still cuts on its own
        CHECK_EQ(g.targetKbps(), 11520);
        CHECK(!g.backlog(1500, 1800)); // but the host waits half a second after any cut
        CHECK(g.backlog(1900, 2100));
        CHECK_EQ(g.targetKbps(), 9216);
        CHECK_EQ(g.overflowEpisodes(), 1);
        CHECK_EQ(g.overflowKbps(), 18000);
        CHECK_EQ(g.goodKbps(), 0); // nothing proven yet, and nothing proven since
    }

    SECTION("RateGovernor — the backlog rule: off with ovfgood=0, a session's own floor kept");
    {
        RateGovernor off;
        off.start(18000, 0);
        CHECK(!off.backlog(5000, 6000));
        CHECK(!off.backlogged());
        CHECK_EQ(off.targetKbps(), 18000);
        RateGovernor follow;
        follow.start(18000, 0, /*followSetting=*/true);
        follow.setOverflowLowersGood(true);
        CHECK(!follow.backlog(5000, 6000));
        CHECK_EQ(follow.targetKbps(), 18000);
        // The guests' feed holds 60 % of its setting, backlog or not.
        RateGovernor feed;
        feed.start(10000, 0, false, 60);
        feed.setOverflowLowersGood(true);
        for (int64_t t = 1000; t <= 5000; t += 500)
            feed.backlog(t, t);
        CHECK_EQ(feed.targetKbps(), 6000);
    }

    SECTION("RateGovernor — the target's mean over time");
    {
        RateGovernor g;
        g.start(20000, 0);
        CHECK_EQ(g.meanTargetKbps(0), 20000);
        LinkFeedback rising;
        rising.owdRiseMs = 45;
        g.report(rising, 1000); // 20 000 for 1 s, then 16 000
        CHECK_EQ(g.meanTargetKbps(3000), 17333);
        g.tick(3000);
        CHECK_EQ(g.meanTargetKbps(3000), 17333);
        CHECK_EQ(g.meanTargetKbps(5000), 16800);
    }

    SECTION("RateGovernor — a session's first seconds prove nothing");
    {
        RateGovernor g;
        g.start(20000, 0);
        LinkFeedback rising;
        rising.owdRiseMs = 45;
        CHECK(g.report(rising, 500));
        CHECK_EQ(g.goodKbps(), 0);
        LinkFeedback quiet;
        CHECK(!g.report(quiet, 3000));
        CHECK(g.report(quiet, 3500));
        CHECK(!g.lastRaiseFast());
        CHECK_EQ(g.targetKbps(), 16800);
    }

    SECTION("RateGovernor — a lowered ceiling lowers the good rate with it");
    {
        RateGovernor g;
        g.start(20000, 0);
        LinkFeedback quiet;
        for (int64_t t = 500; t <= 5000; t += 500)
            g.report(quiet, t);
        CHECK_EQ(g.goodKbps(), 20000);
        g.setSetting(10000);
        CHECK_EQ(g.goodKbps(), 10000);
        CHECK_EQ(g.targetKbps(), 10000);
    }

    SECTION("RateGovernor — the bench's governor=off: the setting, both ways, at once");
    {
        RateGovernor g;
        g.start(20000, 0, /*followSetting=*/true);
        // No receiver: the silence cuts nothing.
        CHECK(!g.tick(10000));
        CHECK_EQ(g.targetKbps(), 20000);
        // Down and back up, each on the spot — a ramp= step reaches the encoder.
        g.setSetting(5000);
        CHECK_EQ(g.targetKbps(), 5000);
        g.setSetting(20000);
        CHECK_EQ(g.targetKbps(), 20000);
        CHECK_EQ(g.changes(), 2);
        // A report of overuse moves nothing either.
        LinkFeedback lossy;
        lossy.gaps = 3;
        lossy.owdRiseMs = 80;
        CHECK(!g.report(lossy, 11000));
        CHECK_EQ(g.targetKbps(), 20000);
        CHECK_EQ(g.overuses(), 0);
    }

    SECTION("EffectiveCadence — the clock closes windows of one second");
    {
        EffectiveCadence c;
        c.start(165, 1000);
        // 60 frames spread over the first second: the window closes on the
        // 61st frame, just past the second, and reads 60 fps. The quick look
        // never fires — 60 is below the 165 the encoder is dimensioned for.
        bool changed = false;
        for (int i = 1; i <= 61; i++)
            changed = c.noteFrame(1000 + i * 16667) || changed;
        CHECK(!changed); // first window only notes
        for (int i = 62; i <= 122; i++)
            changed = c.noteFrame(1000 + i * 16667) || changed;
        CHECK(changed);
        CHECK_EQ(c.currentFps, 60);
    }

    SECTION("VA-API's QP floor — 18 by default (§32.28), the bench's vaminqp= over it, -1 none");
    {
        CHECK_EQ(kVaapiMinQp, 18);
        CHECK_EQ(vaapiMinQp(0), 18);
        CHECK_EQ(vaapiMinQp(26), 26);
        CHECK_EQ(vaapiMinQp(-1), 0);
        CHECK_EQ(vaapiMinQp(EncoderTuning{}.vaapiMinQp), 18);
    }
}
