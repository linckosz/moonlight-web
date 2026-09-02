/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The input dead-man switch. What it must never do is the interesting part:
 * release a key the client still holds, release before the typematic delay
 * has been beaten, or neutralize a gamepad on a mere hiccup. And it must
 * never run at all for a client that does not heartbeat.
 */
#include "test_framework.h"
#include "streaming/InputWatchdog.h"

#include <QVector>

namespace {

struct FakeWire
{
    QVector<short> keysUp;
    QVector<int> buttonsUp;
    QVector<short> padsZeroed;

    InputWatchdog::Sink sink()
    {
        InputWatchdog::Sink s;
        s.releaseKey = [this](const InputWatchdog::HeldKey& k) { keysUp.append(k.keyCode); };
        s.releaseButton = [this](int b) { buttonsUp.append(b); };
        s.neutralizePad = [this](short c, short) { padsZeroed.append(c); };
        return s;
    }

    void clear()
    {
        keysUp.clear();
        buttonsUp.clear();
        padsZeroed.clear();
    }
};

InputWatchdog::HeldKey key(short code, bool hold = false)
{
    InputWatchdog::HeldKey k;
    k.keyCode = code;
    k.hold = hold;
    return k;
}

} // namespace

void run_input_watchdog_tests()
{
    SECTION("InputWatchdog");

    // ── A client that never heartbeats is left alone ────────────────────────
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        qint64 now = 0;
        wd.setClock([&now]() { return now; });

        wd.noteClientAlive();
        wd.noteKey(key('A'), true);
        CHECK(wd.anythingHeld());
        now = 10000;
        wd.tick();
        CHECK(wire.keysUp.isEmpty());
        CHECK(wd.anythingHeld());
    }

    // ── Short period: plain keys go, hold-flagged ones and pads stay ────────
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        qint64 now = 0;
        wd.setClock([&now]() { return now; });

        wd.sync({}, 0, false); // the handshake
        wd.noteClientAlive();
        wd.noteKey(key('A'), true);
        wd.noteKey(key('W', true), true);
        wd.noteButton(1, true, false);
        wd.notePad(0, 1, false);

        now = InputWatchdog::kStaleMs - 1;
        wd.tick();
        CHECK(wire.keysUp.isEmpty());
        CHECK(wire.buttonsUp.isEmpty());

        now = InputWatchdog::kStaleMs;
        wd.tick();
        CHECK_EQ(wire.keysUp.size(), 1);
        CHECK_EQ(wire.keysUp.value(0), short('A'));
        CHECK_EQ(wire.buttonsUp.size(), 1);
        CHECK_EQ(wire.buttonsUp.value(0), 1);
        CHECK(wire.padsZeroed.isEmpty());
        CHECK(wd.anythingHeld()); // W and the pad

        // Fires once, not on every tick.
        wire.clear();
        now += InputWatchdog::kTickMs;
        wd.tick();
        CHECK(wire.keysUp.isEmpty());

        // Long period: everything else goes, including the pad.
        now = InputWatchdog::kStaleHoldMs;
        wd.tick();
        CHECK_EQ(wire.keysUp.size(), 1);
        CHECK_EQ(wire.keysUp.value(0), short('W'));
        CHECK_EQ(wire.padsZeroed.size(), 1);
        CHECK_EQ(wire.padsZeroed.value(0), short(0));
        CHECK(!wd.anythingHeld());
    }

    // ── A hold-flagged button gets the long period too ──────────────────────
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        qint64 now = 0;
        wd.setClock([&now]() { return now; });

        wd.sync({}, 0, false);
        wd.noteClientAlive();
        wd.noteButton(1, true, true);
        now = InputWatchdog::kStaleMs;
        wd.tick();
        CHECK(wire.buttonsUp.isEmpty());
        now = InputWatchdog::kStaleHoldMs;
        wd.tick();
        CHECK_EQ(wire.buttonsUp.size(), 1);
    }

    // ── Any client message rearms both periods ──────────────────────────────
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        qint64 now = 0;
        wd.setClock([&now]() { return now; });

        wd.sync({}, 0, false);
        wd.noteClientAlive();
        wd.noteKey(key('A'), true);
        now = 200;
        wd.noteClientAlive();
        now = 400; // 200 ms since the last word: nothing yet
        wd.tick();
        CHECK(wire.keysUp.isEmpty());
        now = 460;
        wd.tick();
        CHECK_EQ(wire.keysUp.size(), 1);
    }

    // ── The heartbeat restores what was released, and only that ─────────────
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        qint64 now = 0;
        wd.setClock([&now]() { return now; });

        wd.sync({}, 0, false);
        wd.noteClientAlive();
        wd.noteKey(key('A'), true);
        wd.noteKey(key('B'), true);
        wd.noteButton(1, true, false);
        now = InputWatchdog::kStaleMs;
        wd.tick();
        CHECK_EQ(wire.keysUp.size(), 2);
        CHECK_EQ(wire.buttonsUp.size(), 1);

        // The client still holds A and button 1; B was let go for real.
        wd.noteClientAlive();
        InputWatchdog::SyncDiff diff = wd.sync({key('A')}, 1u << 0, false);
        CHECK_EQ(diff.press.size(), 1);
        CHECK_EQ(diff.press.value(0).keyCode, short('A'));
        CHECK(diff.release.isEmpty());
        CHECK_EQ(diff.buttonsDown.size(), 1);
        CHECK(diff.buttonsUp.isEmpty());

        // A steady heartbeat changes nothing.
        diff = wd.sync({key('A')}, 1u << 0, false);
        CHECK(diff.press.isEmpty());
        CHECK(diff.release.isEmpty());
        CHECK(diff.buttonsDown.isEmpty());
        CHECK(diff.buttonsUp.isEmpty());

        // Drift: the host holds something the client no longer reports.
        diff = wd.sync({}, 0, false);
        CHECK_EQ(diff.release.size(), 1);
        CHECK_EQ(diff.release.value(0).keyCode, short('A'));
        CHECK_EQ(diff.buttonsUp.size(), 1);
        CHECK(!wd.anythingHeld());
    }

    // ── An explicit release does not need the timer ─────────────────────────
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        wd.noteKey(key('A'), true);
        wd.noteKey(key('W', true), true);
        wd.notePad(2, 4, false);
        wd.release(false);
        CHECK_EQ(wire.keysUp.size(), 1);
        CHECK(wire.padsZeroed.isEmpty());
        wd.release(true);
        CHECK_EQ(wire.keysUp.size(), 2);
        CHECK_EQ(wire.padsZeroed.size(), 1);
        CHECK_EQ(wire.padsZeroed.value(0), short(2));
    }

    // ── At rest is loose on purpose ─────────────────────────────────────────
    CHECK(InputWatchdog::padAtRest(0, 0, 0, 4095, -4095, 0, 0));
    CHECK(!InputWatchdog::padAtRest(0, 0, 0, 4096, 0, 0, 0));
    CHECK(!InputWatchdog::padAtRest(1, 0, 0, 0, 0, 0, 0));
    CHECK(!InputWatchdog::padAtRest(0, 1, 0, 0, 0, 0, 0));

    // A pad coming back to rest drops out of the set.
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        wd.notePad(0, 1, false);
        CHECK(wd.anythingHeld());
        wd.notePad(0, 1, true);
        CHECK(!wd.anythingHeld());
    }
}
