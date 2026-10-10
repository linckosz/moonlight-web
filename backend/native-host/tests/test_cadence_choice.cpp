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

#include "core/CadenceChoice.h"
#include "native_test_framework.h"

#include <string>

using mw::native::CadenceChoice;
using mw::native::CadenceInputs;
using mw::native::EncoderTuning;
using mw::native::FrameCadence;

namespace {

CadenceInputs inputs(int settingFps, int displayMilliHz, int clientMilliHz, bool clientVsync)
{
    CadenceInputs in;
    in.settingFps = settingFps;
    in.displayMilliHz = displayMilliHz;
    in.clientMilliHz = clientMilliHz;
    in.clientVsync = clientVsync;
    return in;
}

/// Presents at @p presentHz for one second; how many the gate lets through.
int admittedInOneSecond(FrameCadence gate, int presentHz)
{
    int admitted = 0;
    for (int i = 0; i < presentHz; ++i)
        if (gate.admit(static_cast<int64_t>(i) * 1000000 / presentHz)) admitted++;
    return admitted;
}

bool contains(const std::string& s, const char* part)
{
    return s.find(part) != std::string::npos;
}

} // namespace

void run_cadence_choice_tests()
{
    using Mode = EncoderTuning::Cadence;

    // Without the key, the choice and its line are exactly what they were
    // before they moved out of WindowsSession — word for word, since the
    // bench's parsers read these lines.
    SECTION("CadenceChoice — no key: a client that tears gets the setting");
    {
        const CadenceChoice c = mw::native::chooseCadence(inputs(60, 165000, 60000, false));
        CHECK_EQ(c.fps, 60);
        CHECK(c.gate.enabled());
        CHECK(!c.gate.isCeiling());
        CHECK_EQ(c.line, std::string("[native] cadence: 60 fps stream on a 165 Hz display — the "
                                     "first present of each interval is encoded, at once; client "
                                     "at 60 Hz, tearing — nothing to align on"));
    }

    SECTION("CadenceChoice — no key: a vsync client gets a divisor of its refresh");
    {
        const CadenceChoice c = mw::native::chooseCadence(inputs(60, 165000, 144000, true));
        CHECK_EQ(c.fps, 72);
        CHECK(c.gate.enabled());
        CHECK_EQ(c.gate.intervalUs(), 2 * 1000000000LL / 144000);
        CHECK_EQ(c.line,
                 std::string("[native] cadence: 72 fps stream for a 144 Hz client presenting on "
                             "vsync (60 set, every 2nd refresh) on a 165 Hz display — the first "
                             "present of each interval is encoded, at once"));
    }

    SECTION("CadenceChoice — no key: Auto's ceiling keeps a 144 Hz client at 120");
    {
        CadenceInputs in = inputs(120, 165000, 144000, true);
        in.maxFps = 120;
        const CadenceChoice c = mw::native::chooseCadence(in);
        CHECK_EQ(c.fps, 120);
        CHECK_EQ(c.line, std::string("[native] cadence: 120 fps stream on a 165 Hz display — the "
                                     "first present of each interval is encoded, at once; client "
                                     "at 144 Hz, no divisor within a fifth of the setting"));
    }

    SECTION("CadenceChoice — no key: the decoder's cap lowers the rate");
    {
        CadenceInputs in = inputs(120, 120000, 120000, false);
        in.clientCapFps = 90;
        const CadenceChoice c = mw::native::chooseCadence(in);
        CHECK_EQ(c.fps, 90);
        CHECK_EQ(c.line, std::string("[native] cadence: 90 fps stream on a 120 Hz display — the "
                                     "first present of each interval is encoded, at once; client "
                                     "at 120 Hz, tearing — nothing to align on (no more than 90 "
                                     "fps: what its decoder keeps up with)"));
    }

    SECTION("CadenceChoice — no key: at the display's own rate the gate is a ceiling");
    {
        const CadenceChoice c = mw::native::chooseCadence(inputs(60, 60000, 0, false));
        CHECK_EQ(c.fps, 60);
        CHECK(c.gate.isCeiling());
        CHECK_EQ(c.line, std::string("[native] cadence: 60 fps stream on a 60 Hz display — every "
                                     "refresh is encoded, presents beyond it no faster than the "
                                     "stream"));
    }

    SECTION("CadenceChoice — cadence=host: every present, the client's ceilings unapplied");
    {
        CadenceInputs in = inputs(60, 240000, 60000, true);
        in.maxFps = 60;
        in.clientCapFps = 45;
        in.mode = Mode::Host;
        const CadenceChoice c = mw::native::chooseCadence(in);
        CHECK_EQ(c.fps, 240);
        CHECK(!c.gate.enabled());
        CHECK_EQ(admittedInOneSecond(c.gate, 240), 240);
        CHECK_EQ(c.line,
                 std::string("[native] cadence: the host's rate (cadence=host) — 240 fps stream "
                             "on a 240 Hz display — every present is encoded; client at 60 Hz, on "
                             "vsync (not applied: 60 set, no more than 60 chosen for this client, "
                             "no more than 45 asked by its decoder)"));
    }

    SECTION("CadenceChoice — cadence=host: a display faster, equal or slower than the client");
    {
        CadenceInputs in = inputs(60, 500000, 120000, false);
        in.mode = Mode::Host;
        CHECK_EQ(mw::native::chooseCadence(in).fps, 500);
        in.displayMilliHz = 120000;
        CHECK_EQ(mw::native::chooseCadence(in).fps, 120);
        in.displayMilliHz = 59940;
        const CadenceChoice slower = mw::native::chooseCadence(in);
        CHECK_EQ(slower.fps, 60);
        CHECK(!slower.gate.enabled());
        // The setting names no rate the host ignored when it equals it.
        CHECK(!contains(slower.line, "set"));
        CHECK(contains(slower.line, "client at 120 Hz, tearing"));
        // A display whose rate is unknown: the setting, still no gate.
        in.displayMilliHz = 0;
        const CadenceChoice unknown = mw::native::chooseCadence(in);
        CHECK_EQ(unknown.fps, 60);
        CHECK(!unknown.gate.enabled());
    }

    SECTION("CadenceChoice — cadence=host-ceiling: every refresh, a runaway source held");
    {
        CadenceInputs in = inputs(60, 165000, 60000, false);
        in.mode = Mode::HostCeiling;
        const CadenceChoice c = mw::native::chooseCadence(in);
        CHECK_EQ(c.fps, 165);
        CHECK(c.gate.isCeiling());
        CHECK_EQ(admittedInOneSecond(c.gate, 165), 165);
        // 400 presents a second held near the display's rate (+ a fiftieth).
        const int runaway = admittedInOneSecond(c.gate, 400);
        CHECK(runaway >= 165 && runaway <= 172);
        CHECK(contains(c.line, "(cadence=host-ceiling) — 165 fps stream on a 165 Hz display — "
                               "every refresh is encoded"));
    }

    SECTION("CadenceChoice — cadence=host-guarded: no gate, the credit named");
    {
        CadenceInputs in = inputs(60, 240000, 60000, false);
        in.mode = Mode::HostGuarded;
        const CadenceChoice c = mw::native::chooseCadence(in);
        CHECK_EQ(c.fps, 240);
        CHECK(!c.gate.enabled());
        CHECK(contains(c.line, "(cadence=host-guarded)"));
        CHECK(contains(c.line, "skipped while the client's decode queue holds more than a frame"));
    }

    SECTION("CadenceChoice — cadence=deadline: today's cadence until the client's grid");
    {
        // A 120 Hz client on vsync, 120 set, a 240 Hz display: the engine's own
        // choice stands (the gate its fallback), the grid's part named.
        CadenceInputs in = inputs(120, 240000, 120000, true);
        const CadenceChoice own = mw::native::chooseCadence(in);
        in.mode = Mode::Deadline;
        const CadenceChoice c = mw::native::chooseCadence(in);
        CHECK_EQ(c.fps, own.fps);
        CHECK_EQ(c.gate.intervalUs(), own.gate.intervalUs());
        CHECK(contains(c.line, own.line.c_str()));
        CHECK(contains(c.line, "one picture per refresh of the client once it says when they "
                               "are (cadence=deadline)"));
    }

    SECTION("CadenceChoice — the key in the tuning's description");
    {
        EncoderTuning t;
        CHECK(t.isDefault());
        t.cadence = Mode::Host;
        CHECK(!t.isDefault());
        CHECK_EQ(t.describe(), std::string("cadence=host"));
        t.cadence = Mode::HostCeiling;
        CHECK_EQ(t.describe(), std::string("cadence=host-ceiling"));
        t.cadence = Mode::HostGuarded;
        CHECK_EQ(t.describe(), std::string("cadence=host-guarded"));
        t.cadence = Mode::Deadline;
        CHECK_EQ(t.describe(), std::string("cadence=deadline"));
    }

    // The link's own hold (plan Wi-Fi W2 C), the same hold as the guarded
    // cadence's, asked of the relay's send queue instead of the client.
    SECTION("linkhold= and sctpbuf= — off by default, named when set");
    {
        EncoderTuning t;
        CHECK_EQ(t.linkHoldMs, 0);
        CHECK_EQ(t.sctpBufferKb, 0);
        t.sctpBufferKb = 48;
        CHECK(!t.isDefault());
        CHECK_EQ(t.describe(), std::string("sctpbuf=48"));
        t.linkHoldMs = 4;
        CHECK_EQ(t.describe(), std::string("sctpbuf=48 linkhold=4"));
        t.sctpBufferKb = 0;
        CHECK(!t.isDefault());
        CHECK_EQ(t.describe(), std::string("linkhold=4"));
    }

    // retrcut= is the engine's own unless said (plan Wi-Fi W2 B): a bench's
    // retrcut=0, the governor blind to SCTP, is a key like any other.
    SECTION("retrcut= — the engine's own unless said, 0 named");
    {
        EncoderTuning t;
        CHECK_EQ(t.retransCutPermille, -1);
        CHECK(t.isDefault());
        CHECK_EQ(t.describe(), std::string());
        t.retransCutPermille = 0;
        CHECK(!t.isDefault());
        CHECK_EQ(t.describe(), std::string("retrcut=0"));
        t.retransCutPermille = 5;
        CHECK_EQ(t.describe(), std::string("retrcut=5"));
    }

    // usrsctp's max burst (plan Wi-Fi W2.5): libdatachannel's 10 unless said,
    // 0 a key like any other (no limit).
    SECTION("sctpburst= — libdatachannel's unless said, 0 named");
    {
        EncoderTuning t;
        CHECK_EQ(t.sctpMaxBurst, -1);
        CHECK(t.isDefault());
        t.sctpMaxBurst = 0;
        CHECK(!t.isDefault());
        CHECK_EQ(t.describe(), std::string("sctpburst=0"));
        t.sctpMaxBurst = 32;
        CHECK_EQ(t.describe(), std::string("sctpburst=32"));
        // And its stream scheduler (W2.3), never set unless said.
        EncoderTuning s;
        CHECK_EQ(s.sctpScheduler, -1);
        s.sctpScheduler = 4;
        CHECK(!s.isDefault());
        CHECK_EQ(s.describe(), std::string("sctpss=4"));
        // And its path MTU (W2.6): libdatachannel's 1280 unless said.
        EncoderTuning m;
        CHECK_EQ(m.sctpMtu, 0);
        CHECK(m.isDefault());
        m.sctpMtu = 1500;
        CHECK(!m.isDefault());
        CHECK_EQ(m.describe(), std::string("sctpmtu=1500"));
    }

    // The audio log (plan audio + DSCP, A0): off unless said, named when set.
    SECTION("audiolog= — off by default, named when set");
    {
        EncoderTuning t;
        CHECK(!t.audioLog);
        CHECK(t.isDefault());
        t.audioLog = true;
        CHECK(!t.isDefault());
        CHECK_EQ(t.describe(), std::string("audiolog=1"));
    }

    // The Opus frame (plan audio + DSCP, A3 L2): 5 ms unless said.
    SECTION("audioframe= — 240 samples by default, 480 / 960 when set, named");
    {
        EncoderTuning t;
        CHECK_EQ(t.audioFrameMs, 0);
        CHECK_EQ(t.audioFrameSamples(), 240);
        t.audioFrameMs = 10;
        CHECK_EQ(t.audioFrameSamples(), 480);
        CHECK(!t.isDefault());
        CHECK_EQ(t.describe(), std::string("audioframe=10"));
        t.audioFrameMs = 20;
        CHECK_EQ(t.audioFrameSamples(), 960);
    }

    // ScreenCaptureKit's queue (plan « attente », AM2): the product's unless
    // said, and 0 is a value of its own for the interval (none at all).
    SECTION("sckdepth= / sckinterval= — default unless set, an interval of 0 named");
    {
        EncoderTuning t;
        CHECK(t.isDefault());
        t.sckMinIntervalUs = 0;
        CHECK(!t.isDefault());
        CHECK_EQ(t.describe(), std::string("sckinterval=0"));
        t.sckQueueDepth = 1;
        CHECK_EQ(t.describe(), std::string("sckdepth=1 sckinterval=0"));
    }

    // A client whose decoder falls silent under the reference repairs asks for
    // none (/start's ref_invalidation): the bench's dpb=1, on every encoder.
    SECTION("refuseReferenceRepairs — one reference for that client, a bench's dpb kept");
    {
        EncoderTuning own;
        CHECK(mw::native::refuseReferenceRepairs(own));
        CHECK_EQ(own.dpbFrames, 1);
        CHECK_EQ(own.describe(), std::string("dpb=1"));
        // Asked twice (a relaunch): nothing more to change.
        CHECK(!mw::native::refuseReferenceRepairs(own));
        CHECK_EQ(own.dpbFrames, 1);
        EncoderTuning bench;
        bench.dpbFrames = 6;
        CHECK(!mw::native::refuseReferenceRepairs(bench));
        CHECK_EQ(bench.dpbFrames, 6);
        // Every other client never calls it: the engine's own stays default.
        EncoderTuning other;
        CHECK(other.isDefault());
        CHECK_EQ(other.dpbFrames, 0);
    }
}
