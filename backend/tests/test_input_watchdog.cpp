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
    QVector<QString> charsUp;
    QVector<CharKeyHints> charHints;

    InputWatchdog::Sink sink()
    {
        InputWatchdog::Sink s;
        s.releaseKey = [this](const InputWatchdog::HeldKey& k) { keysUp.append(k.keyCode); };
        s.releaseButton = [this](int b) { buttonsUp.append(b); };
        s.neutralizePad = [this](short c, short) { padsZeroed.append(c); };
        s.releaseChar = [this](const QString& ch, const CharKeyHints& hints) {
            charsUp.append(ch);
            charHints.append(hints);
        };
        return s;
    }

    void clear()
    {
        keysUp.clear();
        buttonsUp.clear();
        padsZeroed.clear();
        charsUp.clear();
        charHints.clear();
    }
};

CharKeyHints hintsOf(bool hold = false)
{
    CharKeyHints h;
    h.hold = hold;
    return h;
}

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

    // ── Modifiers get the long period: a stall is no tap of theirs ──────────
    // A Mac on Wi-Fi went silent for 293 ms under a held right Option: let go
    // of at the short period and pressed again by the heartbeat after it, the
    // host saw a second press of it the viewer never made (09/11/2026).
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        qint64 now = 0;
        wd.setClock([&now]() { return now; });

        wd.sync({}, 0, false);
        wd.noteClientAlive();
        const QVector<short> mods = {0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2,
                                     0xA3, 0xA4, 0xA5, 0x5B, 0x5C};
        for (short vk : mods)
            wd.noteKey(key(vk), true);
        wd.noteKey(key('A'), true);

        now = InputWatchdog::kStaleMs;
        wd.tick();
        CHECK_EQ(wire.keysUp.size(), 1);
        CHECK_EQ(wire.keysUp.value(0), short('A'));

        // The link wakes, the viewer still holding them all: nothing to press
        // again but the letter.
        wd.noteClientAlive();
        QVector<InputWatchdog::HeldKey> still;
        for (short vk : mods)
            still.append(key(vk));
        still.append(key('A'));
        InputWatchdog::SyncDiff diff = wd.sync(still, 0, false);
        CHECK_EQ(diff.press.size(), 1);
        CHECK_EQ(diff.press.value(0).keyCode, short('A'));
        CHECK(diff.release.isEmpty());

        // A dead link still lets go of them, at the long period.
        wire.clear();
        now = InputWatchdog::kStaleMs + InputWatchdog::kStaleHoldMs;
        wd.tick();
        CHECK_EQ(wire.keysUp.size(), mods.size() + 1);
        CHECK(!wd.anythingHeld());

        // An explicit release (a policy change, a viewer leaving) takes them
        // at once, as before.
        FakeWire wire2;
        InputWatchdog wd2(wire2.sink());
        wd2.noteKey(key(0x5B), true);
        wd2.release(false);
        CHECK_EQ(wire2.keysUp.size(), 1);
        CHECK(InputWatchdog::isModifierVk(0xA5));
        CHECK(!InputWatchdog::isModifierVk('A'));
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

    // ── The message that ends a silence says how long it was ────────────────
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        qint64 now = 0;
        wd.setClock([&now]() { return now; });

        wd.sync({}, 0, false);
        CHECK_EQ(wd.noteClientAlive(), qint64(0));
        wd.noteKey(key('A'), true);
        // A gap the tick never acted on is not a freeze.
        now = 100;
        CHECK_EQ(wd.noteClientAlive(), qint64(0));
        // One it fired on is: the upstream half of a link freeze.
        now = 100 + InputWatchdog::kStaleMs;
        wd.tick();
        now = 400;
        CHECK_EQ(wd.noteClientAlive(), qint64(300));
        // Reported once, by the message that ended it.
        now = 450;
        CHECK_EQ(wd.noteClientAlive(), qint64(0));
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

    // ── An empty beat lets go of a modifier the client no longer holds ──────
    //
    // The client sends one closing beat when nothing is left held, precisely
    // so this happens: a modifier whose key-up the local OS ate is still down
    // here, and no other message would ever contradict it.
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        wd.sync({key(0x5B)}, 0, false); // the Windows key, held
        InputWatchdog::SyncDiff diff = wd.sync({}, 0, false);
        CHECK_EQ(diff.release.size(), 1);
        CHECK_EQ(diff.release.value(0).keyCode, short(0x5B));
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

    // ── A press of a held key is a repeat, and says so ──────────────────────
    // The GameStream engine drops it: Wolf's inputtino piles every one into
    // the list its repeat thread walks, and a key stuck down on the host.
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        CHECK(!wd.noteKey(key('A'), true));
        CHECK(wd.noteKey(key('A'), true));
        CHECK(wd.noteKey(key('A', true), true));
        CHECK(!wd.noteKey(key('B'), true)); // another key is no repeat of A
        CHECK(!wd.noteKey(key('A'), false));
        CHECK(!wd.noteKey(key('A'), false)); // a release is never one
        CHECK(!wd.noteKey(key('A'), true));

        // Released by the watchdog: the next press goes out again.
        wd.release(true);
        CHECK(!wd.noteKey(key('A'), true));
    }

    // ── A character held goes up as a key does, and never comes back ────────
    //
    // A guest's keyboard taken away while it held a "d" typed as a character:
    // the key stayed down on the host for the 8 s the guest went without one
    // (bench of 05/11/2026). The policy change lets go of it like the rest.
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        CHECK(wd.noteChar(QStringLiteral("d"), hintsOf(), true));
        CHECK(wd.anythingHeld());
        wd.release(true);
        CHECK_EQ(wire.charsUp.size(), 1);
        CHECK(wire.charsUp.value(0) == QStringLiteral("d"));
        CHECK(!wd.anythingHeld());

        // The client's key-up, when it comes, goes no further: no second
        // CharUp reaches the platform, and nothing is left to release.
        CHECK(!wd.noteChar(QStringLiteral("d"), hintsOf(), false));
        wire.clear();
        wd.release(true);
        CHECK(wire.charsUp.isEmpty());

        // Only once: the next press and its release go through as ever.
        CHECK(wd.noteChar(QStringLiteral("d"), hintsOf(), true));
        CHECK(wd.noteChar(QStringLiteral("d"), hintsOf(), false));
        CHECK(!wd.anythingHeld());
    }

    // ── A new press after the release forwards its own key-up ───────────────
    //
    // The guest's key-up never came (its keyboard is still taken away); the
    // owner then presses the same character, and lets go of it.
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        wd.noteChar(QStringLiteral("d"), hintsOf(), true);
        wd.release(true);
        CHECK(wd.noteChar(QStringLiteral("d"), hintsOf(), true));
        CHECK(wd.noteChar(QStringLiteral("d"), hintsOf(), false));
        // A key-up with no press before it is forwarded: only a release of
        // the watchdog's own is swallowed.
        CHECK(wd.noteChar(QStringLiteral("x"), hintsOf(), false));
    }

    // ── A silent link: a plain character goes at the short period ───────────
    //
    // A `hold` one, a movement key typed as a character, at the long one.
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        qint64 now = 0;
        wd.setClock([&now]() { return now; });

        wd.sync({}, 0, false);
        wd.noteClientAlive();
        wd.noteChar(QString(QChar(0x00E9)), hintsOf(), true);
        wd.noteChar(QStringLiteral("z"), hintsOf(true), true);
        now = InputWatchdog::kStaleMs;
        wd.tick();
        CHECK_EQ(wire.charsUp.size(), 1);
        CHECK(wire.charsUp.value(0) == QString(QChar(0x00E9)));
        CHECK(wd.anythingHeld());
        now = InputWatchdog::kStaleHoldMs;
        wd.tick();
        CHECK_EQ(wire.charsUp.size(), 2);
        CHECK(wire.charsUp.value(1) == QStringLiteral("z"));
        CHECK(wire.charHints.value(1).hold);
        CHECK(!wd.anythingHeld());
    }

    // ── The heartbeat neither presses a character nor lets go of one ────────
    //
    // It never lists them (InputMsg::parseHeldKeys): a beat without the
    // character says nothing about it, and a re-press would type it again.
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        wd.noteChar(QStringLiteral("q"), hintsOf(), true);
        const InputWatchdog::SyncDiff diff = wd.sync({}, 0, false);
        CHECK(diff.press.isEmpty());
        CHECK(diff.release.isEmpty());
        CHECK(wire.charsUp.isEmpty());
        CHECK(wd.anythingHeld());
        wd.release(false);
        CHECK_EQ(wire.charsUp.size(), 1);
        CHECK(!wd.anythingHeld());
    }

    // ── The release says what the press said ────────────────────────────────
    {
        FakeWire wire;
        InputWatchdog wd(wire.sink());
        CharKeyHints chord;
        chord.chord = true;
        chord.positionVk = 'Z';
        chord.altGraph = true;
        wd.noteChar(QStringLiteral("z"), chord, true);
        wd.release(true);
        CHECK_EQ(wire.charHints.size(), 1);
        CHECK(wire.charHints.value(0).chord);
        CHECK_EQ(wire.charHints.value(0).positionVk, short('Z'));
        CHECK(wire.charHints.value(0).altGraph);
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
