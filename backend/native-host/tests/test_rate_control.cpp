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
#include "native_test_framework.h"

using mw::native::LinkFeedback;
using mw::native::encode::RateGovernor;

using mw::native::encode::EffectiveCadence;
using mw::native::encode::intraRefreshCountFrames;
using mw::native::encode::intraRefreshPeriodFrames;
using mw::native::encode::kIntraRefreshMaxFrames;
using mw::native::encode::kIntraRefreshMinFrames;
using mw::native::encode::kIntraRefreshSeconds;
using mw::native::encode::kStillBoost;
using mw::native::encode::kStillMaxKbps;
using mw::native::encode::kVbvFrameRateFloor;
using mw::native::encode::LinkOccupancy;
using mw::native::encode::RefineConvergence;
using mw::native::encode::stillBitrateKbps;
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
}
