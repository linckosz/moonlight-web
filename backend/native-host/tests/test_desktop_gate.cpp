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

#include "audio/SoundHold.h"
#include "core/DesktopGate.h"
#include "native_test_framework.h"

#ifdef MW_NATIVE_TESTS_HAVE_OPUS
#include "audio/OpusEncoder.h"

#include <opus.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using mw::native::DesktopGate;
using mw::native::GateCloses;
using mw::native::LockNotice;
using mw::native::LockRead;
using mw::native::audio::holdFrame;

namespace {

constexpr int64_t kSecond = 1000000;

/// A gate opened by a first good read, as a stream starts on an unlocked PC.
void openAtStart(DesktopGate& gate)
{
    gate.read(LockRead::Unlocked, true, 0);
}

/// No picture handed out yet: a value no generation takes.
constexpr uint64_t kNoPicture = ~uint64_t(0);

/// One 5 ms stereo frame of a loud 440 Hz tone, from sample @p from on.
std::vector<float> tone(int from)
{
    std::vector<float> frame(240 * 2);
    for (int i = 0; i < 240; ++i) {
        const float v =
            0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 *
                                               static_cast<double>(from + i) / 48000.0));
        frame[static_cast<size_t>(2 * i)] = v;
        frame[static_cast<size_t>(2 * i + 1)] = v;
    }
    return frame;
}

float peakOf(const std::vector<float>& frame)
{
    float peak = 0.0f;
    for (float v : frame)
        peak = std::fabs(v) > peak ? std::fabs(v) : peak;
    return peak;
}

} // namespace

void run_desktop_gate_tests()
{
    SECTION("DesktopGate — closed at the start, open after the first good read only (D5)");
    {
        DesktopGate gate;
        CHECK(!gate.open());
        CHECK_EQ(gate.generation(), uint64_t(0));
        // Nothing but a read opens it: neither the user's desktop seen by a
        // path, nor a notice.
        CHECK(!gate.admit(true));
        CHECK(!gate.admitPicture(0, true));
        const DesktopGate::Change n = gate.notice(LockNotice::Unlock, true);
        CHECK(!n.opened);
        CHECK(n.readNow);
        CHECK(!gate.open());
        const DesktopGate::Change r = gate.read(LockRead::Unlocked, true, 0);
        CHECK(r.opened);
        CHECK(gate.open());
        CHECK(gate.why() == DesktopGate::Why::Unlocked);
        CHECK(gate.admit(true));
    }
    {
        // A stream started on a locked PC: nothing until it is unlocked.
        DesktopGate gate;
        CHECK(!gate.read(LockRead::Locked, true, 0).opened);
        CHECK(!gate.open());
        CHECK(!gate.read(LockRead::Unknown, true, 0).opened);
        CHECK(!gate.open());
    }

    SECTION("DesktopGate — an unknown state holds it (C3, C7)");
    {
        DesktopGate gate;
        openAtStart(gate);
        const DesktopGate::Change c = gate.read(LockRead::Unknown, true, kSecond);
        CHECK(c.closed);
        CHECK(!gate.open());
        CHECK(gate.why() == DesktopGate::Why::Unknown);
        // Still unknown: still held, whatever the desktop.
        CHECK(!gate.read(LockRead::Unknown, true, 2 * kSecond).opened);
        CHECK(!gate.admit(true));
        // Unknown counts as a lock: the first good read waits out the settle.
        CHECK(!gate.read(LockRead::Unlocked, true, 3 * kSecond).opened);
        CHECK(gate.read(LockRead::Unlocked, true, 4 * kSecond).opened);
    }

    SECTION("DesktopGate — the desktop back while the session is locked holds it (C7)");
    {
        // Win+L on Windows 11: the secure desktop for a moment, then the lock
        // screen on the user's own desktop.
        DesktopGate gate;
        openAtStart(gate);
        bool closedNow = false;
        CHECK(!gate.admit(false, &closedNow));
        CHECK(closedNow);
        CHECK(gate.why() == DesktopGate::Why::Desktop);
        // The user's desktop is back, with the lock screen on it.
        CHECK(!gate.admit(true));
        CHECK(!gate.read(LockRead::Locked, true, kSecond).opened);
        CHECK(!gate.open());
        CHECK(!gate.admit(true));
        CHECK(!gate.admitPicture(gate.generation(), true));
    }

    SECTION("DesktopGate — a lock announced on the user's desktop holds it (C7)");
    {
        DesktopGate gate;
        openAtStart(gate);
        const uint64_t before = gate.generation();
        const DesktopGate::Change c = gate.notice(LockNotice::Lock, true);
        CHECK(c.closed);
        CHECK(c.readNow);
        CHECK(!gate.open());
        CHECK(gate.why() == DesktopGate::Why::Locked);
        CHECK_EQ(gate.generation(), before + 1);
        // The desktop is the user's the whole time: nothing reopens it.
        CHECK(!gate.admit(true));
        CHECK(!gate.admitPicture(before, true));
        CHECK(!gate.admitPicture(gate.generation(), true));
        // A second lock notice changes nothing.
        CHECK(!gate.notice(LockNotice::Lock, true).closed);
        CHECK_EQ(gate.generation(), before + 1);
    }

    SECTION("DesktopGate — a lost unlock notice: the read resumes it (C7)");
    {
        DesktopGate gate;
        openAtStart(gate);
        gate.notice(LockNotice::Lock, true);
        // No unlock notice ever comes; the reads every 250 ms do.
        CHECK(!gate.read(LockRead::Locked, true, 1 * kSecond).opened);
        CHECK(!gate.read(LockRead::Unlocked, true, 2 * kSecond).opened); // settling
        CHECK(!gate.read(LockRead::Unlocked, true, 2 * kSecond + 750000).opened);
        const DesktopGate::Change c = gate.read(LockRead::Unlocked, true, 3 * kSecond);
        // Opened: the caller sends a fresh picture as a key frame.
        CHECK(c.opened);
        CHECK(gate.open());
        CHECK(gate.admit(true));
    }

    SECTION("DesktopGate — the settle restarts when the session locks again");
    {
        DesktopGate gate;
        openAtStart(gate);
        gate.notice(LockNotice::Lock, true);
        CHECK(!gate.read(LockRead::Unlocked, true, 1 * kSecond).opened);
        // Locked again before the second was up.
        CHECK(!gate.read(LockRead::Locked, true, 1 * kSecond + 500000).opened);
        CHECK(!gate.read(LockRead::Unlocked, true, 2 * kSecond).opened);
        CHECK(!gate.read(LockRead::Unlocked, true, 2 * kSecond + 999999).opened);
        CHECK(gate.read(LockRead::Unlocked, true, 3 * kSecond).opened);
    }

    SECTION("DesktopGate — the desktop alone holds it without the settle (UAC)");
    {
        DesktopGate gate;
        openAtStart(gate);
        CHECK(!gate.admit(false));
        CHECK(!gate.open());
        // Unlocked, but the secure desktop is still up: held.
        const DesktopGate::Change away = gate.read(LockRead::Unlocked, false, kSecond);
        CHECK(!away.opened);
        CHECK(!away.closed); // already closed by the path
        // The desktop is back: open at the first read, no lock to wait out.
        CHECK(gate.read(LockRead::Unlocked, true, kSecond + 250000).opened);
        // The watcher's own read of the desktop closes it as well.
        const DesktopGate::Change w = gate.read(LockRead::Unlocked, false, 2 * kSecond);
        CHECK(w.closed);
        CHECK(gate.why() == DesktopGate::Why::Desktop);
    }

    SECTION("DesktopGate — a notice about another session is ignored (C3, C7, Q1)");
    {
        DesktopGate gate;
        openAtStart(gate);
        const uint64_t openGen = gate.generation();
        // Its lock: nothing closes; this session is read again.
        DesktopGate::Change c = gate.notice(LockNotice::Lock, false);
        CHECK(!c.closed);
        CHECK(c.readNow);
        CHECK(gate.open());
        CHECK_EQ(gate.generation(), openGen);
        // Its unlock, while this session is locked: nothing opens.
        gate.notice(LockNotice::Lock, true);
        const uint64_t held = gate.generation();
        c = gate.notice(LockNotice::Unlock, false);
        CHECK(!c.opened);
        CHECK(c.readNow);
        CHECK(!gate.open());
        CHECK_EQ(gate.generation(), held);
        // The read it asked for says what this session is: still locked.
        CHECK(!gate.read(LockRead::Locked, true, kSecond).opened);
        CHECK(!gate.open());
        // Any other notice of this session: a read, nothing more.
        c = gate.notice(LockNotice::Other, true);
        CHECK(c.readNow);
        CHECK(!c.closed);
        CHECK(!c.opened);
    }

    SECTION("DesktopGate — input is let go of once per close (C4, C7)");
    {
        DesktopGate gate;
        GateCloses input;
        // Nothing was held before the first opening.
        CHECK(!input.releaseDue(gate.generation()));
        openAtStart(gate);
        CHECK(!input.releaseDue(gate.generation()));
        // The lock: due once, whoever sees it first.
        gate.notice(LockNotice::Lock, true);
        CHECK(input.releaseDue(gate.generation()));
        CHECK(!input.releaseDue(gate.generation()));
        // Opened again: nothing new to let go of.
        gate.read(LockRead::Unlocked, true, 0);
        gate.read(LockRead::Unlocked, true, 2 * kSecond);
        CHECK(gate.open());
        CHECK(!input.releaseDue(gate.generation()));
        // Closed and opened between two events: still due, once.
        gate.admit(false);
        gate.read(LockRead::Unlocked, true, 3 * kSecond);
        CHECK(gate.open());
        CHECK(input.releaseDue(gate.generation()));
        CHECK(!input.releaseDue(gate.generation()));
    }

    SECTION("DesktopGate — a picture goes only from the generation it was taken in (C5)");
    {
        DesktopGate gate;
        // Taken before the first opening: never.
        const uint64_t atStart = gate.generation();
        openAtStart(gate);
        CHECK(!gate.admitPicture(atStart, true));
        const uint64_t taken = gate.generation();
        CHECK(gate.admitPicture(taken, true));
        // Locked: neither the picture taken before, nor one taken now.
        gate.notice(LockNotice::Lock, true);
        CHECK(!gate.admitPicture(taken, true));
        const uint64_t duringLock = gate.generation();
        CHECK(!gate.admitPicture(duringLock, true));
        // Open again: still neither — only a picture taken from now on.
        gate.read(LockRead::Unlocked, true, 0);
        gate.read(LockRead::Unlocked, true, 2 * kSecond);
        CHECK(gate.open());
        CHECK(!gate.admitPicture(taken, true));
        CHECK(!gate.admitPicture(duringLock, true));
        CHECK(gate.admitPicture(gate.generation(), true));
        // A picture whose desktop is not the user's closes the gate.
        bool closedNow = false;
        const uint64_t now = gate.generation();
        CHECK(!gate.admitPicture(now, false, &closedNow));
        CHECK(closedNow);
        CHECK(!gate.admitPicture(now, true));
    }

    SECTION("DesktopGate — the sound goes with the picture, silence while held (SR 13/11)");
    {
        DesktopGate gate;
        uint64_t sent = kNoPicture;
        // Closed at the start: silence.
        CHECK(!gate.soundMayGo(sent));
        // Open, but no picture out yet: still silence.
        openAtStart(gate);
        CHECK(!gate.soundMayGo(sent));
        // The first picture went: the sound goes with it.
        sent = gate.generation();
        CHECK(gate.soundMayGo(sent));
        // The audio path, frame by frame: it passes untouched while it may go.
        std::vector<float> frame = tone(0);
        const std::vector<float> loud = frame;
        CHECK(!holdFrame(frame.data(), frame.size(), !gate.soundMayGo(sent)));
        CHECK(frame == loud);
        // Locked: silence at once, before any picture says so.
        gate.notice(LockNotice::Lock, true);
        CHECK(!gate.soundMayGo(sent));
        CHECK(holdFrame(frame.data(), frame.size(), !gate.soundMayGo(sent)));
        CHECK_EQ(peakOf(frame), 0.0f);
        // Open again: silence until a picture of the new generation went out.
        gate.read(LockRead::Unlocked, true, 0);
        gate.read(LockRead::Unlocked, true, 2 * kSecond);
        CHECK(gate.open());
        CHECK(!gate.soundMayGo(sent));
        sent = gate.generation();
        CHECK(gate.soundMayGo(sent));
        frame = loud;
        CHECK(!holdFrame(frame.data(), frame.size(), !gate.soundMayGo(sent)));
        CHECK(frame == loud);
        // The desktop gone (UAC): silence too.
        gate.admit(false);
        CHECK(!gate.soundMayGo(sent));
    }

#ifdef MW_NATIVE_TESTS_HAVE_OPUS
    SECTION("DesktopGate — held frames decode to silence, the track keeps its clock (Opus)");
    {
        mw::native::audio::OpusEncoder encoder;
        std::string error;
        CHECK(encoder.open(error, 240));
        int decodeErr = 0;
        OpusDecoder* decoder = opus_decoder_create(48000, 2, &decodeErr);
        CHECK(decoder != nullptr);
        if (decoder) {
            std::vector<uint8_t> packet;
            std::vector<float> decoded(240 * 2);
            // Held: a packet every frame, each one silence.
            int packets = 0;
            float heldPeak = 0.0f;
            for (int f = 0; f < 20; ++f) {
                std::vector<float> frame = tone(f * 240);
                holdFrame(frame.data(), frame.size(), true);
                const size_t n = encoder.encode(frame.data(), packet);
                if (n == 0) continue;
                ++packets;
                CHECK_EQ(opus_decode_float(decoder, packet.data(), static_cast<opus_int32>(n),
                                           decoded.data(), 240, 0),
                         240);
                heldPeak = std::max(heldPeak, peakOf(decoded));
            }
            CHECK_EQ(packets, 20);
            CHECK(heldPeak < 0.001f);
            // Open: the tone comes through.
            float openPeak = 0.0f;
            for (int f = 20; f < 40; ++f) {
                std::vector<float> frame = tone(f * 240);
                holdFrame(frame.data(), frame.size(), false);
                const size_t n = encoder.encode(frame.data(), packet);
                if (n == 0) continue;
                CHECK_EQ(opus_decode_float(decoder, packet.data(), static_cast<opus_int32>(n),
                                           decoded.data(), 240, 0),
                         240);
                openPeak = std::max(openPeak, peakOf(decoded));
            }
            CHECK(openPeak > 0.2f);
            opus_decoder_destroy(decoder);
        }
    }
#endif

    SECTION("DesktopGate — no settle when it is set to none");
    {
        DesktopGate gate(0);
        openAtStart(gate);
        gate.notice(LockNotice::Lock, true);
        CHECK(gate.read(LockRead::Unlocked, true, 1).opened);
    }

    SECTION("DesktopGate — paths closing while the watcher opens: the state stays whole");
    {
        DesktopGate gate(0);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> closes{0};
        std::vector<std::thread> paths;
        for (int i = 0; i < 3; ++i)
            paths.emplace_back([&] {
                while (!stop.load()) {
                    bool closedNow = false;
                    gate.admit(false, &closedNow);
                    if (closedNow) closes.fetch_add(1);
                    std::this_thread::yield();
                }
            });
        uint64_t opens = 0;
        for (int i = 0; i < 20000; ++i)
            if (gate.read(LockRead::Unlocked, true, i).opened) ++opens;
        stop.store(true);
        for (std::thread& t : paths)
            t.join();
        gate.admit(false); // the last word is a close, whoever had it
        const uint64_t g = gate.generation();
        // Every change was one step, each close one open's: no step lost,
        // none made twice.
        CHECK(!DesktopGate::isOpen(g));
        CHECK_EQ(g, 2 * opens);
        CHECK(closes.load() == opens || closes.load() + 1 == opens);
    }
}
