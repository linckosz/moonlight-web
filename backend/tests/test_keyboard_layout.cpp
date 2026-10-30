/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "streaming/InputMessageCodec.h"

#include <QJsonArray>
#include <QJsonObject>

/*
 * The policy that decides what the host actually receives for one keystroke.
 *
 * The protocol carries no keyboard layout, so there is no single mechanism that
 * works everywhere: what a host can honour depends on what its own injection
 * path does with a virtual key. These checks pin the four answers to the source
 * they were read off (see the header's notes), and pin the two rules that keep
 * the whole thing safe — never trade a real key press for text, and never ask a
 * Sunshine host to re-derive a scancode it was already deriving from a fixed
 * table.
 */

namespace {

/// A keydown message as the browser sends it. `character` is what the client's
/// layout produced; `nonUs` says the US layout puts something else there.
QJsonObject key(int vk, const QString& code, const QString& character = QString(),
                bool nonUs = false)
{
    QJsonObject msg;
    msg["type"] = "keydown";
    msg["keyCode"] = vk;
    msg["code"] = code;
    if (!character.isNull()) {
        msg["char"] = character;
        msg["nonUs"] = nonUs;
    }
    return msg;
}

constexpr int kVkQ = 0x51;
constexpr int kVkA = 0x41;
constexpr int kVk1 = 0x31;
constexpr int kVkUp = 0x26;

} // namespace

void run_keyboard_layout_tests()
{
    SECTION("KeyboardLayout");

    using InputMsg::KeyPlan;
    using InputMsg::resolveKey;

    const auto allModes = {KeyboardMode::Positional, KeyboardMode::Native,
                           KeyboardMode::SunshineWindows};

    // ── A key with no character at all ───────────────────────────────────────
    //
    // Arrows, F-keys, Enter, the modifiers: nothing to correct, nothing to
    // choose. The position goes out untouched whatever the host is.
    for (KeyboardMode mode : allModes) {
        const KeyPlan plan = resolveKey(key(kVkUp, "ArrowUp"), mode, LayoutFidelity::Always);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkUp));
        CHECK_EQ(plan.flags, static_cast<char>(0));
    }

    // A `char` no key produces — a string — is dropped, the key going by its
    // position: typing a string is Utf8Text's, with its own bounds.
    {
        const KeyPlan one =
            resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::Native, LayoutFidelity::Always);
        CHECK(one.isText());
        const KeyPlan many = resolveKey(key(kVkQ, "KeyQ", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", true),
                                        KeyboardMode::Native, LayoutFidelity::Always);
        CHECK(!many.isText());
        CHECK_EQ(many.keyCode, static_cast<short>(kVkQ));
    }

    // Opting out in the settings file lands in the same place: the character is
    // ignored and the position goes out, exactly as it did before any of this.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::Positional,
                                        LayoutFidelity::Always);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(plan.flags, static_cast<char>(0));
    }

    // ── Sunshine on Windows ──────────────────────────────────────────────────
    //
    // A DIVERGENT letter becomes the VK of that letter, flagged non-normalized
    // so the host injects the VK itself and Windows resolves it through the
    // active layout. It stays a real key press, which is why letters never go
    // through text here.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::SunshineWindows,
                                        LayoutFidelity::Always);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkA));
        CHECK_EQ(plan.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
    }
    {
        // Case is carried by the modifiers, not by the VK: VK_A either way.
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "A", true), KeyboardMode::SunshineWindows,
                                        LayoutFidelity::Always);
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkA));
    }
    {
        // A US client's letter agrees with its position, and still goes as its
        // VK, non-normalized (Bruno's decision of 22/10/2026). Left alone, it
        // was the mirror case: on an AZERTY Sunshine the key marked A, meaning
        // "a", typed "q". The price — Sunshine derives that scancode from its
        // own thread's layout — is a thread whose layout differs from the
        // foreground window's, which is rare.
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "q", false),
                                        KeyboardMode::SunshineWindows, LayoutFidelity::Always);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(plan.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
    }

    // ── The rule that keeps games working: never trade a key for text ────────
    //
    // A digit is not a letter, and text would type it exactly — but text has no
    // key state, and divergence is measured against the US layout, not against
    // the HOST's. On a host whose layout already matches the client's, the
    // position was ALREADY producing "&": correcting it would trade a working
    // key for a stateless one and stop a shooter's weapon slots answering, for
    // a character that was never wrong. So it stays positional.
    {
        const KeyPlan plan = resolveKey(key(kVk1, "Digit1", "&", true),
                                        KeyboardMode::SunshineWindows, LayoutFidelity::Always);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVk1));
        CHECK_EQ(plan.flags, static_cast<char>(0));
    }
    {
        // An accented letter is not a letter either as far as the VK path goes:
        // no US virtual key carries it. Positional, for the same reason.
        const KeyPlan plan = resolveKey(key(kVk1, "Digit2", "é", true),
                                        KeyboardMode::SunshineWindows, LayoutFidelity::Always);
        CHECK(!plan.isText());
    }

    // ── Sunshine on macOS ────────────────────────────────────────────────────
    //
    // There is no mode for it, and the session maps a macOS Sunshine host to
    // Positional on purpose. It used to have one — "letters as text", since a
    // Mac ignores the non-normalized flag and text was the only exact channel.
    // Then an AZERTY viewer on a QWERTY Mac running Sunshine 2026.516 watched
    // "q" and "a" vanish while "p" typed: that Sunshine's unicode() logs
    // "Unicode input not yet implemented for MacOS" and drops the packet, and
    // nothing on the wire says which Sunshine is listening. A letter that goes
    // out as text is either right or gone; as a position it is at worst
    // swapped, which is what every Moonlight client has always done there.
    //
    // What is pinned: the exact keystrokes that vanished leave as positions,
    // never as text, under the mode a macOS host now gets.
    {
        const KeyPlan a = resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::Positional,
                                     LayoutFidelity::Always);
        CHECK(!a.isText());
        CHECK_EQ(a.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(a.flags, static_cast<char>(0));

        const KeyPlan q = resolveKey(key(kVkA, "KeyA", "q", true), KeyboardMode::Positional,
                                     LayoutFidelity::Always);
        CHECK(!q.isText());
        CHECK_EQ(q.keyCode, static_cast<short>(kVkA));
        CHECK_EQ(q.flags, static_cast<char>(0));
    }

    // ── The native host ──────────────────────────────────────────────────────
    //
    // EVERY printable character, agreeing ones included, because the platform
    // layer turns each one back into a real key of the host's OWN layout: when
    // the two layouts match the resolution lands on the very key the viewer
    // pressed, so nothing is lost by asking.
    {
        const KeyPlan plan =
            resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::Native, LayoutFidelity::Always);
        CHECK(plan.isText());
        CHECK_EQ(plan.text.toStdString(), std::string("a"));
    }
    {
        const KeyPlan plan = resolveKey(key(kVk1, "Digit1", "&", true), KeyboardMode::Native,
                                        LayoutFidelity::Always);
        CHECK(plan.isText());
    }
    {
        // The mirror case, and the reason the native host does not use `nonUs`
        // at all: a US-layout viewer on a French host presses the key marked A,
        // means "q", and diverges from nothing — yet the position types "a".
        // What the position must be compared against is the host's layout, and
        // only the host can make that comparison.
        const KeyPlan plan =
            resolveKey(key(kVkQ, "KeyQ", "q", false), KeyboardMode::Native, LayoutFidelity::Always);
        CHECK(plan.isText());
        CHECK_EQ(plan.text.toStdString(), std::string("q"));
    }

    // ── A dead key ───────────────────────────────────────────────────────────
    //
    // AltGr+2 on AZERTY is the tilde, held back until the next key. The native
    // host gets the composed "~" with that key, so the dead key itself sends
    // nothing: pressed as its position, it typed "é" first (12/10/2026, "é~"
    // for one tilde on a French Ubuntu host). Neither does the watchdog re-press
    // it. A Sunshine host keeps the position: it composes on its own side.
    {
        QJsonObject msg = key(kVk1 + 1, "Digit2");
        msg["dead"] = true;
        const KeyPlan native = resolveKey(msg, KeyboardMode::Native, LayoutFidelity::Always);
        CHECK(native.skip);
        CHECK(!native.isText());

        for (KeyboardMode mode : {KeyboardMode::Positional, KeyboardMode::SunshineWindows}) {
            const KeyPlan plan = resolveKey(msg, mode, LayoutFidelity::Always);
            CHECK(!plan.skip);
            CHECK_EQ(plan.keyCode, static_cast<short>(kVk1 + 1));
        }

        QJsonObject beat;
        beat["keys"] = QJsonArray{msg};
        CHECK(
            InputMsg::parseHeldKeys(beat, KeyboardMode::Native, LayoutFidelity::Always).isEmpty());
        CHECK_EQ(
            InputMsg::parseHeldKeys(beat, KeyboardMode::SunshineWindows, LayoutFidelity::Always)
                .size(),
            1);
    }

    // ── International keys: a raw VK wherever a position goes ────────────────
    //
    // They have no US virtual key at all, so a position is their raw VK,
    // non-normalized: on Sunshine, under "never", in gaming mode.
    for (KeyboardMode mode : {KeyboardMode::Positional, KeyboardMode::SunshineWindows}) {
        const KeyPlan iso =
            resolveKey(key(0, "IntlBackslash", "<", true), mode, LayoutFidelity::Always);
        CHECK(!iso.isText());
        CHECK_EQ(iso.keyCode, static_cast<short>(0xE2));
        CHECK_EQ(iso.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));

        const KeyPlan ro = resolveKey(key(0, "IntlRo", "\\", true), mode, LayoutFidelity::Always);
        CHECK(!ro.isText());
        CHECK_EQ(ro.keyCode, static_cast<short>(0xC1));
        CHECK_EQ(ro.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
    }
    {
        const KeyPlan never = resolveKey(key(0, "IntlBackslash", "@", true), KeyboardMode::Native,
                                         LayoutFidelity::Never);
        CHECK(!never.isText());
        CHECK_EQ(never.keyCode, static_cast<short>(0xE2));

        QJsonObject game = key(0, "IntlBackslash", "@", true);
        game["gaming"] = true;
        const KeyPlan play = resolveKey(game, KeyboardMode::Native, LayoutFidelity::Auto);
        CHECK(!play.isText());
        CHECK_EQ(play.keyCode, static_cast<short>(0xE2));
        CHECK_EQ(play.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
    }

    // …and their character on the native host at the desktop, as any key's. A
    // Mac's ISO keyboard reports its top-left key, the @ and # of a French Mac,
    // as IntlBackslash: sent as that position, a French host typed "<" for "@"
    // (24/10/2026).
    for (LayoutFidelity f : {LayoutFidelity::Auto, LayoutFidelity::Always}) {
        const KeyPlan at = resolveKey(key(0, "IntlBackslash", "@", true), KeyboardMode::Native, f);
        CHECK_EQ(at.text.toStdString(), std::string("@"));
        CHECK_EQ(at.keyCode, static_cast<short>(0xE2));

        const KeyPlan ro = resolveKey(key(0, "IntlRo", "\\", true), KeyboardMode::Native, f);
        CHECK_EQ(ro.text.toStdString(), std::string("\\"));
    }
    {
        // A dead key there keeps its position: nothing to type yet.
        QJsonObject dead = key(0, "IntlBackslash", "^", true);
        dead["dead"] = true;
        const KeyPlan plan = resolveKey(dead, KeyboardMode::Native, LayoutFidelity::Always);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(0xE2));
    }

    // ── Press and release must resolve the same way ──────────────────────────
    //
    // Sunshine identifies a held key by (keyCode, flags). A press resolved one
    // way and a release resolved the other are two different keys to it, so the
    // press never comes back up and the host holds it forever. The client
    // replays the press's character on the release; this is the codec half of
    // that contract.
    {
        QJsonObject down = key(kVkQ, "KeyQ", "a", true);
        QJsonObject up = down;
        up["type"] = "keyup";
        const KeyPlan a = resolveKey(down, KeyboardMode::SunshineWindows, LayoutFidelity::Always);
        const KeyPlan b = resolveKey(up, KeyboardMode::SunshineWindows, LayoutFidelity::Always);
        CHECK_EQ(a.keyCode, b.keyCode);
        CHECK_EQ(a.flags, b.flags);
    }

    // ── The held-input heartbeat ─────────────────────────────────────────────
    //
    // A key that resolves to TEXT is left out of it: the watchdog re-presses
    // whatever it sees, and re-pressing a character would type the letter again
    // rather than re-assert a held key. Everything that resolves to a real key
    // stays in, so a held movement key is still re-asserted after a stall.
    {
        QJsonObject beat;
        QJsonArray keys;
        keys.append(key(kVkQ, "KeyQ", "a", true));   // letter, divergent
        keys.append(key(kVk1, "Digit1", "&", true)); // digit, divergent
        keys.append(key(kVkUp, "ArrowUp"));          // no character at all
        beat["keys"] = keys;

        // Windows: nothing resolves to text, so the whole set is reported.
        const QVector<IMediaEngine::HeldKey> held =
            InputMsg::parseHeldKeys(beat, KeyboardMode::SunshineWindows, LayoutFidelity::Always);
        CHECK_EQ(held.size(), 3);
        CHECK_EQ(held[0].keyCode, static_cast<short>(kVkA));
        CHECK_EQ(held[0].flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
        CHECK_EQ(held[1].keyCode, static_cast<short>(kVk1));
        CHECK_EQ(held[2].keyCode, static_cast<short>(kVkUp));

        // Native: every printable character IS a character, so only the key
        // that never had one survives the filter.
        const QVector<IMediaEngine::HeldKey> nativeHeld =
            InputMsg::parseHeldKeys(beat, KeyboardMode::Native, LayoutFidelity::Always);
        CHECK_EQ(nativeHeld.size(), 1);
        CHECK_EQ(nativeHeld[0].keyCode, static_cast<short>(kVkUp));
    }

    // ── The heartbeat of a guest who may only watch ──────────────────────────
    //
    // The watchdog presses whatever a heartbeat lists that it does not hold
    // yet, so a heartbeat is input like any key: what the guest's policy
    // forbids comes out of it, on every transport (all three read it through
    // readHeldState). Until 01/10/2026 webrtc-media and WSS did not filter it,
    // and a viewer could press keys and click on the host.
    {
        QJsonObject beat;
        QJsonArray keys;
        keys.append(key(kVkUp, "ArrowUp"));
        beat["keys"] = keys;
        beat["buttons"] = 1;
        beat["buttonsHold"] = true;

        InputMsg::Policy viewer;
        viewer.keyboardMouse = false;
        viewer.gamepad = false;
        const InputMsg::HeldState none =
            InputMsg::readHeldState(beat, KeyboardMode::Native, LayoutFidelity::Always, viewer);
        CHECK_EQ(none.keys.size(), 0);
        CHECK_EQ(none.buttons, 0u);

        // A guest allowed the keyboard keeps their held keys and buttons.
        const InputMsg::HeldState full = InputMsg::readHeldState(
            beat, KeyboardMode::Native, LayoutFidelity::Always, InputMsg::Policy{});
        CHECK_EQ(full.keys.size(), 1);
        CHECK_EQ(full.buttons, 1u);
        CHECK(full.buttonsHold);
    }

    // ── The diagnostic line ──────────────────────────────────────────────────
    //
    // A keystroke that comes out wrong looks identical from the browser however
    // it went wrong. `keyboard_debug` turns on one line per press saying which
    // link broke, and the line answers two questions that fail separately: the
    // character a text field shows (Notepad) and the physical key a game
    // reading raw scancodes sees (Game).
    //
    // What is pinned here is the VERDICTS, not the wording: a line that claims
    // OK where the character is not guaranteed is worse than no line at all,
    // because it sends whoever reads it looking somewhere else.
    using InputMsg::describeKey;

    {
        // Nothing about an arrow depends on a layout. Diagnosing it would put a
        // line under every keystroke of a game and bury the ones that matter.
        for (KeyboardMode mode : allModes)
            CHECK(describeKey(key(kVkUp, "ArrowUp"), mode,
                              resolveKey(key(kVkUp, "ArrowUp"), mode, LayoutFidelity::Always))
                      .line.isEmpty());
    }
    {
        // A corrected letter on Sunshine/Windows: exact AND still a real key.
        // The one case where both answers are yes, so no warning.
        const QJsonObject msg = key(kVkQ, "KeyQ", "a", true);
        const InputMsg::KeyDiag diag =
            describeKey(msg, KeyboardMode::SunshineWindows,
                        resolveKey(msg, KeyboardMode::SunshineWindows, LayoutFidelity::Always));
        CHECK(!diag.warn);
        CHECK(diag.line.contains(QStringLiteral("Notepad: OK 'a'")));
        CHECK(diag.line.contains(QStringLiteral("Game: OK real key")));
        CHECK(diag.line.contains(QStringLiteral("non-normalized")));
    }
    {
        // A divergent digit left positional: the host's layout decides, and
        // nothing here can read it. Warned, because this is the case the
        // fidelity feature exists for and could not fix.
        const QJsonObject msg = key(kVk1, "Digit1", "&", true);
        const InputMsg::KeyDiag diag =
            describeKey(msg, KeyboardMode::SunshineWindows,
                        resolveKey(msg, KeyboardMode::SunshineWindows, LayoutFidelity::Always));
        CHECK(diag.warn);
        CHECK(diag.line.contains(QStringLiteral("Notepad: KO")));
        // The key a game sees is named in US terms, which is the vocabulary
        // bindings are written in — the physical key is right even here.
        CHECK(diag.line.contains(QStringLiteral("Game: OK real key, US '1'")));
    }
    {
        // A key that agrees with US is the assumption the protocol has always
        // run on. Reported, not warned: flagging it would put a warning under
        // every keystroke of a US viewer and drown the lines that matter. A
        // digit, since a letter now goes as its VK on Sunshine for Windows.
        const QJsonObject msg = key(kVk1, "Digit1", "1", false);
        const InputMsg::KeyDiag diag =
            describeKey(msg, KeyboardMode::SunshineWindows,
                        resolveKey(msg, KeyboardMode::SunshineWindows, LayoutFidelity::Always));
        CHECK(!diag.warn);
        CHECK(diag.line.contains(QStringLiteral("Notepad: OK '1'")));
    }
    {
        // Text on a remote host types the character perfectly and presses
        // nothing. Always warned: that half is lost and cannot be recovered.
        // No policy produces such a plan any more (the macOS one that did was
        // withdrawn), so the plan is built by hand: the diagnostic's contract
        // is pinned on its own, not through whichever policy happens to reach
        // the branch today.
        const QJsonObject msg = key(kVkQ, "KeyQ", "a", true);
        KeyPlan text;
        text.text = QStringLiteral("a");
        const InputMsg::KeyDiag diag = describeKey(msg, KeyboardMode::SunshineWindows, text);
        CHECK(diag.warn);
        CHECK(diag.line.contains(QStringLiteral("Notepad: OK 'a'")));
        CHECK(diag.line.contains(QStringLiteral("Game: KO")));
    }
    {
        // The native host resolves the character in its own real layout and
        // reports what it actually did on the next line. A verdict here would
        // be a guess competing with an answer, so there is none — and no
        // warning either, since nothing has been found wrong.
        const QJsonObject msg = key(kVkQ, "KeyQ", "a", true);
        const InputMsg::KeyDiag diag =
            describeKey(msg, KeyboardMode::Native,
                        resolveKey(msg, KeyboardMode::Native, LayoutFidelity::Always));
        CHECK(!diag.warn);
        CHECK(diag.line.contains(QStringLiteral("host verdict below")));
        CHECK(!diag.line.contains(QStringLiteral("OK")));
        CHECK(!diag.line.contains(QStringLiteral("KO")));
    }
    {
        // The line names the physical key the client pressed, which the native
        // host never learns — it is handed a character, not a position. That is
        // what makes the two lines worth reading together.
        const QJsonObject msg = key(kVkQ, "KeyQ", "a", true);
        CHECK(describeKey(msg, KeyboardMode::Native,
                          resolveKey(msg, KeyboardMode::Native, LayoutFidelity::Always))
                  .line.contains(QStringLiteral("KeyQ")));
    }
    {
        // Off by default, and nothing turns it on but the settings file.
        CHECK(!InputMsg::debugEnabled());
        InputMsg::setDebug(true);
        CHECK(InputMsg::debugEnabled());
        InputMsg::setDebug(false);
        CHECK(!InputMsg::debugEnabled());
    }
    {
        // Never a line while the secure desktop has the keyboard, switch or no
        // switch: the lock screen's PIN went into the worker log key by key
        // once the SYSTEM worker could reach it (28/09/2026).
        static int lines = 0;
        static bool secure = false;
        const QtMessageHandler previous =
            qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& text) {
                if (text.startsWith(QStringLiteral("[KBD]"))) ++lines;
            });
        const QJsonObject msg = key(kVk1, "Digit1", "1");
        const KeyPlan plan = resolveKey(msg, KeyboardMode::SunshineWindows, LayoutFidelity::Always);
        InputMsg::setDebug(true);
        InputMsg::logKey(msg, KeyboardMode::SunshineWindows, plan, true);
        CHECK_EQ(lines, 1);
        InputMsg::setQuietProbe([]() { return secure; });
        secure = true;
        InputMsg::logKey(msg, KeyboardMode::SunshineWindows, plan, true);
        CHECK_EQ(lines, 1);
        secure = false;
        InputMsg::logKey(msg, KeyboardMode::SunshineWindows, plan, true);
        CHECK_EQ(lines, 2);
        InputMsg::setQuietProbe(nullptr);
        InputMsg::setDebug(false);
        qInstallMessageHandler(previous);
    }
    {
        // The modifier mask is read off the message, not off the key plan, so
        // it is the same whether the key goes out as a position or as a
        // character. The character path used to drop it, and the macOS host —
        // the only one that has to synthesise the modifier flags rather than
        // let the OS hold them — had nothing left to correct its own idea of
        // them with.
        QJsonObject msg = key(kVkQ, "KeyQ", "a", true);
        msg["metaKey"] = true;
        msg["shiftKey"] = true;
        CHECK(resolveKey(msg, KeyboardMode::Native, LayoutFidelity::Always).isText());
        CHECK_EQ(static_cast<int>(InputMsg::modifierMask(msg)), 0x08 | 0x01);
        // And "nothing is held" is a statement too, not an absence.
        CHECK_EQ(static_cast<int>(InputMsg::modifierMask(key(kVkQ, "KeyQ", "a", true))), 0);
    }
    {
        CHECK_EQ(InputMsg::usKeyLabel(kVk1).toStdString(), std::string("1"));
        CHECK_EQ(InputMsg::usKeyLabel(kVkQ).toStdString(), std::string("Q"));
        CHECK_EQ(InputMsg::usKeyLabel(0xBF).toStdString(), std::string("/"));
        CHECK_EQ(InputMsg::usKeyLabel(0x20).toStdString(), std::string("Space"));
    }

    // ── The owner chose the positions ────────────────────────────────────────
    //
    // keyboard_layout_fidelity off: every key by its position, on every host,
    // the native one included; and a dead key goes as its position too — the
    // host composes it in its own layout, as with Moonlight.
    {
        const KeyPlan a =
            resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::Native, LayoutFidelity::Never);
        CHECK(!a.isText());
        CHECK_EQ(a.keyCode, static_cast<short>(kVkQ));
        QJsonObject dead = key(kVk1 + 1, "Digit2");
        dead["dead"] = true;
        const KeyPlan d = resolveKey(dead, KeyboardMode::Native, LayoutFidelity::Never);
        CHECK(!d.skip);
        CHECK_EQ(d.keyCode, static_cast<short>(kVk1 + 1));
        const KeyPlan s = resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::SunshineWindows,
                                     LayoutFidelity::Never);
        CHECK_EQ(s.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(s.flags, static_cast<char>(0));
    }

    // ── A chord of the touch keyboard's bar ──────────────────────────────────
    //
    // Ctrl latched on the bar, then z on the phone's keyboard. It names a
    // shortcut, so it is resolved by its character whatever the owner chose for
    // the physical keyboard: sent as the US position of Z, it was Ctrl+W on an
    // AZERTY host, which closed the tab, and Shift+a typed Q (22/10/2026).
    {
        QJsonObject z = key(0x5A, "", "z");
        z["chord"] = true;
        z["ctrlKey"] = true;
        for (LayoutFidelity f :
             {LayoutFidelity::Never, LayoutFidelity::Always, LayoutFidelity::Auto}) {
            // The native host presses the key carrying it, the US position
            // riding along for a layout that has none.
            const KeyPlan native = resolveKey(z, KeyboardMode::Native, f);
            CHECK(native.isText());
            CHECK(native.chord);
            CHECK_EQ(native.text.toStdString(), std::string("z"));
            CHECK_EQ(native.keyCode, static_cast<short>(0x5A));
            // Sunshine for Windows: the letter's own VK, non-normalized, which
            // its active layout puts on the right key.
            const KeyPlan windows = resolveKey(z, KeyboardMode::SunshineWindows, f);
            CHECK(!windows.isText());
            CHECK_EQ(windows.keyCode, static_cast<short>(0x5A));
            CHECK_EQ(windows.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
            // Any other host: the position, as before.
            const KeyPlan other = resolveKey(z, KeyboardMode::Positional, f);
            CHECK(!other.isText());
            CHECK_EQ(other.keyCode, static_cast<short>(0x5A));
            CHECK_EQ(other.flags, static_cast<char>(0));
        }
        // A digit chord on Sunshine stays at its position: Ctrl+1 is VK_1 on
        // any layout as far as a shortcut is concerned.
        QJsonObject one = key(kVk1, "", "1");
        one["chord"] = true;
        const KeyPlan digit = resolveKey(one, KeyboardMode::SunshineWindows, LayoutFidelity::Never);
        CHECK_EQ(digit.keyCode, static_cast<short>(kVk1));
        CHECK_EQ(digit.flags, static_cast<char>(0));
        // Never re-pressed by the watchdog on the native host: it would type
        // the character again.
        QJsonObject beat;
        beat["keys"] = QJsonArray{z};
        CHECK(InputMsg::parseHeldKeys(beat, KeyboardMode::Native, LayoutFidelity::Never).isEmpty());
        // The diagnostic says it is a chord.
        const KeyPlan native = resolveKey(z, KeyboardMode::Native, LayoutFidelity::Never);
        CHECK(describeKey(z, KeyboardMode::Native, native).line.contains(QStringLiteral("chord")));
    }
    {
        // The session's line names the owner's choice.
        CHECK(QString(InputMsg::layoutFidelityName(LayoutFidelity::Auto)).startsWith("auto"));
        CHECK(QString(InputMsg::layoutFidelityName(LayoutFidelity::Always)).startsWith("always"));
        CHECK(QString(InputMsg::layoutFidelityName(LayoutFidelity::Never)).startsWith("never"));
    }

    // ── "auto": the client's characters at the desktop, positions in a game ──
    //
    // keyboard_layout_fidelity's default since 22/10/2026 (Bruno's rule): in
    // the desktop mouse mode a key types what the client's layout put on it,
    // in gaming mode it plays by its place — ZQSD and the weapon slots stay
    // where they are, whatever the two keyboards. The client marks the press
    // (`gaming`); a key without the mark is a desktop key.
    {
        QJsonObject desk = key(kVkQ, "KeyQ", "a", true);
        desk["gaming"] = false;
        QJsonObject game = desk;
        game["gaming"] = true;

        const KeyPlan nativeDesk = resolveKey(desk, KeyboardMode::Native, LayoutFidelity::Auto);
        CHECK(nativeDesk.isText());
        CHECK_EQ(nativeDesk.text.toStdString(), std::string("a"));
        const KeyPlan nativeGame = resolveKey(game, KeyboardMode::Native, LayoutFidelity::Auto);
        CHECK(!nativeGame.isText());
        CHECK_EQ(nativeGame.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(nativeGame.flags, static_cast<char>(0));

        // Sunshine for Windows: every letter as its VK at the desktop, the
        // position in a game.
        const KeyPlan winDesk =
            resolveKey(desk, KeyboardMode::SunshineWindows, LayoutFidelity::Auto);
        CHECK_EQ(winDesk.keyCode, static_cast<short>(kVkA));
        CHECK_EQ(winDesk.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
        const KeyPlan winGame =
            resolveKey(game, KeyboardMode::SunshineWindows, LayoutFidelity::Auto);
        CHECK_EQ(winGame.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(winGame.flags, static_cast<char>(0));

        // "always" keeps the characters in a game; "never" the positions at
        // the desktop.
        CHECK(resolveKey(game, KeyboardMode::Native, LayoutFidelity::Always).isText());
        CHECK(!resolveKey(desk, KeyboardMode::Native, LayoutFidelity::Never).isText());

        // A dead key in a game goes by its position: the host composes it in
        // its own layout, as with Moonlight. At the desktop it is left out.
        QJsonObject dead = key(kVk1 + 1, "Digit2");
        dead["dead"] = true;
        dead["gaming"] = true;
        const KeyPlan deadGame = resolveKey(dead, KeyboardMode::Native, LayoutFidelity::Auto);
        CHECK(!deadGame.skip);
        CHECK_EQ(deadGame.keyCode, static_cast<short>(kVk1 + 1));
        dead["gaming"] = false;
        CHECK(resolveKey(dead, KeyboardMode::Native, LayoutFidelity::Auto).skip);
        // Under "always" too: the client does not hold the next key back in
        // gaming mode, so the host has to compose them, or the accent is lost.
        dead["gaming"] = true;
        const KeyPlan deadAlways = resolveKey(dead, KeyboardMode::Native, LayoutFidelity::Always);
        CHECK(!deadAlways.skip);
        CHECK_EQ(deadAlways.keyCode, static_cast<short>(kVk1 + 1));

        // A chord stays a chord in a game: Ctrl+z still means undo.
        QJsonObject z = key(0x5A, "", "z");
        z["chord"] = true;
        z["gaming"] = true;
        CHECK(resolveKey(z, KeyboardMode::Native, LayoutFidelity::Auto).chord);

        // The heartbeat replays each key with its own press's mode: the game's
        // key is re-asserted by position, the desktop's character is not
        // re-pressed (it would type again).
        QJsonObject beat;
        beat["keys"] = QJsonArray{desk, game};
        const QVector<IMediaEngine::HeldKey> held =
            InputMsg::parseHeldKeys(beat, KeyboardMode::Native, LayoutFidelity::Auto);
        CHECK_EQ(held.size(), 1);
        CHECK(held.size() == 1 && held[0].keyCode == static_cast<short>(kVkQ));

        // And the diagnostic says which mode the key came from.
        CHECK(describeKey(game, KeyboardMode::Native, nativeGame)
                  .line.endsWith(QStringLiteral("| mode: gaming")));
        CHECK(describeKey(desk, KeyboardMode::Native, nativeDesk)
                  .line.endsWith(QStringLiteral("| mode: desktop")));
    }
    // ── The numeric keypad ───────────────────────────────────────────────────
    //
    // The same on every layout, and keys of their own: its 1 typed as a
    // character came out as the main row's 1 — Shift+& on a French host —
    // which Alt+0233 codes and Blender's views never see. By position always.
    {
        const struct
        {
            int vk;
            const char* code;
            const char* character;
        } pad[] = {{0x61, "Numpad1", "1"},
                   {0x6E, "NumpadDecimal", "."},
                   {0x6A, "NumpadMultiply", "*"},
                   {0x6F, "NumpadDivide", "/"}};
        for (const auto& k : pad) {
            const QJsonObject msg = key(k.vk, k.code, k.character, false);
            for (KeyboardMode mode : allModes) {
                for (LayoutFidelity f : {LayoutFidelity::Auto, LayoutFidelity::Always}) {
                    const KeyPlan plan = resolveKey(msg, mode, f);
                    CHECK(!plan.isText() && !plan.skip);
                    CHECK_EQ(plan.keyCode, static_cast<short>(k.vk));
                    CHECK_EQ(plan.flags, static_cast<char>(0));
                }
            }
        }
        // And the watchdog keeps them, like any key pressed by position.
        QJsonObject beat;
        beat["keys"] = QJsonArray{key(0x61, "Numpad1", "1", false)};
        CHECK_EQ(InputMsg::parseHeldKeys(beat, KeyboardMode::Native, LayoutFidelity::Auto).size(),
                 1);
        // The key tapped in a dead key's place, which no layout assigns.
        CHECK_EQ(InputMsg::kMaskVk, static_cast<short>(0xE8));
    }
    {
        // As settings.json spells the choice; anything else is the default.
        CHECK(InputMsg::parseLayoutFidelity("always") == LayoutFidelity::Always);
        CHECK(InputMsg::parseLayoutFidelity("never") == LayoutFidelity::Never);
        CHECK(InputMsg::parseLayoutFidelity("auto") == LayoutFidelity::Auto);
        CHECK(InputMsg::parseLayoutFidelity("") == LayoutFidelity::Auto);
        CHECK(InputMsg::parseLayoutFidelity("true") == LayoutFidelity::Auto);
    }

    // ── The touch keyboard's text ────────────────────────────────────────────
    //
    // A positional host — Wolf, Sunshine on Linux — types text through IBus,
    // and a game with no input method got no letter at all from a phone
    // (issue #44). There, what a US key carries goes as that key, Shift in the
    // modifier byte; only the rest stays text, in order.
    {
        using InputMsg::planText;
        const auto steps = planText(QStringLiteral("aZ1!?é😀 x"), KeyboardMode::Positional);
        CHECK_EQ(steps.size(), 8);
        if (steps.size() == 8) {
            CHECK(!steps[0].isText() && steps[0].keyCode == 0x41 && !steps[0].shift);
            CHECK(!steps[1].isText() && steps[1].keyCode == 0x5A && steps[1].shift);
            CHECK(!steps[2].isText() && steps[2].keyCode == 0x31 && !steps[2].shift);
            CHECK(!steps[3].isText() && steps[3].keyCode == 0x31 && steps[3].shift);
            CHECK(!steps[4].isText() && steps[4].keyCode == 0xBF && steps[4].shift);
            // The accent and the emoji (a surrogate pair) stay one run of text.
            CHECK(steps[5].text == QStringLiteral("é😀"));
            CHECK(!steps[6].isText() && steps[6].keyCode == 0x20 && !steps[6].shift);
            CHECK(!steps[7].isText() && steps[7].keyCode == 0x58 && !steps[7].shift);
        }
        // Every printable ASCII character has its US key.
        for (char16_t u = 0x20; u < 0x7F; ++u) {
            short vk = 0;
            bool shift = false;
            CHECK(InputMsg::usKeyOf(QChar(u), vk, shift));
        }
        // Hosts that inject text exactly keep it, untouched.
        for (KeyboardMode mode : {KeyboardMode::Native, KeyboardMode::SunshineWindows}) {
            const auto kept = planText(QStringLiteral("aZ1!"), mode);
            CHECK(kept.size() == 1 && kept[0].text == QStringLiteral("aZ1!"));
        }
        CHECK(planText(QString(), KeyboardMode::Positional).isEmpty());

        // What is named back to the page: only the text a positional host
        // types through its input method — never a character sent as a key,
        // never the text of a host that injects it itself.
        using InputMsg::inputMethodText;
        CHECK(inputMethodText(steps, KeyboardMode::Positional) == QStringLiteral("é😀"));
        CHECK(inputMethodText(planText(QStringLiteral("abc 1!"), KeyboardMode::Positional),
                              KeyboardMode::Positional)
                  .isEmpty());
        for (KeyboardMode mode : {KeyboardMode::Native, KeyboardMode::SunshineWindows})
            CHECK(inputMethodText(planText(QStringLiteral("é"), mode), mode).isEmpty());
    }

    // ── The touch keyboard's text on this PC's layout ────────────────────────
    //
    // With the owner's box ticked and the host on this machine, each character
    // goes on the key this machine's layout puts it on: an AZERTY's a is the
    // US Q, é its 2, € AltGr+E, ê the dead circumflex then e. Only what the
    // layout cannot type is left to text (issue #44, 29/10/2026).
    {
        using InputMsg::planText;
        struct AzertySample : mw::native::HostKeyboardLayout
        {
            bool find(char32_t cp, mw::native::LayoutChar& out) const override
            {
                mw::native::LayoutChar typed;
                typed.count = 1;
                switch (cp) {
                case U'a': typed.keys[0] = {0x51, false, false}; break;
                case U'A': typed.keys[0] = {0x51, true, false}; break;
                case U'1': typed.keys[0] = {0x31, true, false}; break;
                case U' ': typed.keys[0] = {0x20, false, false}; break;
                case U'é': typed.keys[0] = {0x32, false, false}; break; // é
                case U'€': typed.keys[0] = {0x45, false, true}; break;  // €
                case U'ê':                                              // ê
                    typed.keys[0] = {0xDD, false, false};
                    typed.keys[1] = {0x45, false, false};
                    typed.count = 2;
                    break;
                default: return false;
                }
                out = typed;
                return true;
            }
            std::string name() const override { return "fr azerty"; }
        };
        const AzertySample azerty;

        const auto steps =
            planText(QStringLiteral("aA1 é€ê😀z"), KeyboardMode::Positional, &azerty);
        CHECK_EQ(steps.size(), 9);
        if (steps.size() == 9) {
            CHECK(steps[0].keyCode == 0x51 && !steps[0].shift && !steps[0].altGr);
            CHECK(steps[1].keyCode == 0x51 && steps[1].shift);
            CHECK(steps[2].keyCode == 0x31 && steps[2].shift);
            CHECK(steps[3].keyCode == 0x20);
            CHECK(steps[4].keyCode == 0x32 && !steps[4].shift && !steps[4].altGr);
            CHECK(steps[5].keyCode == 0x45 && !steps[5].shift && steps[5].altGr);
            CHECK(steps[6].keyCode == 0xDD && !steps[6].isText());
            CHECK(steps[7].keyCode == 0x45 && !steps[7].altGr);
            // What the layout lacks stays one run of text, the surrogate pair
            // whole, and z with it: the US key would not be z on this host.
            CHECK(steps[8].text == QStringLiteral("😀z"));
        }
        // é € ê went as keys: only what the layout lacks is named back.
        CHECK(InputMsg::inputMethodText(steps, KeyboardMode::Positional) == QStringLiteral("😀z"));
        CHECK(InputMsg::inputMethodText(
                  planText(QStringLiteral("é€ê"), KeyboardMode::Positional, &azerty),
                  KeyboardMode::Positional)
                  .isEmpty());
        // Hosts that inject text exactly keep it, layout or not.
        for (KeyboardMode mode : {KeyboardMode::Native, KeyboardMode::SunshineWindows}) {
            const auto kept = planText(QStringLiteral("aé"), mode, &azerty);
            CHECK(kept.size() == 1 && kept[0].text == QStringLiteral("aé"));
        }
    }

    // ── AltGr on a positional host ───────────────────────────────────────────
    //
    // Chrome on Windows reports AltGr as Ctrl+Alt. Wolf presses what the
    // modifier byte names around the key, and Ctrl plus the left Alt took the
    // host's AltGr away: AltGr+E typed no € on a French Wolf (issue #44). The
    // right Alt is held as a key of its own, so the byte drops both there.
    {
        QJsonObject euro = key(0x45, "KeyE", QStringLiteral("€"), true);
        euro["ctrlKey"] = true;
        euro["altKey"] = true;
        euro["shiftKey"] = true;
        euro["altGraph"] = true;
        CHECK_EQ(InputMsg::positionModifiers(euro, KeyboardMode::Positional),
                 static_cast<char>(MODIFIER_SHIFT));
        // Every other host keeps the byte as it was.
        for (KeyboardMode mode : {KeyboardMode::Native, KeyboardMode::SunshineWindows})
            CHECK_EQ(InputMsg::positionModifiers(euro, mode), InputMsg::modifierMask(euro));
        // A real Ctrl+Alt shortcut, no AltGr, keeps its modifiers everywhere.
        euro["altGraph"] = false;
        CHECK_EQ(InputMsg::positionModifiers(euro, KeyboardMode::Positional),
                 InputMsg::modifierMask(euro));
    }

    // ── The right Alt, and the left Ctrl Windows puts under an AltGr ─────────
    //
    // The right Alt comes as VK_RMENU, AltGr to a Linux host; the native host
    // keeps VK_MENU, the Alt its AltGr symbols let go of and put back (held as
    // the right one, the Alt put back stayed down). A Windows client's AltGr
    // also holds the left Ctrl Windows adds to it, marked by the client: a
    // positional host does not hold it, as a key or in a key's byte, or
    // AltGr+E typed no € on a French Wolf (issue #44, 29/10/2026).
    {
        const QJsonObject rightAlt = key(0xA5, "AltRight");
        CHECK_EQ(InputMsg::resolveKey(rightAlt, KeyboardMode::Native, LayoutFidelity::Auto).keyCode,
                 static_cast<short>(0x12));
        for (KeyboardMode mode : {KeyboardMode::Positional, KeyboardMode::SunshineWindows})
            CHECK_EQ(InputMsg::resolveKey(rightAlt, mode, LayoutFidelity::Auto).keyCode,
                     static_cast<short>(0xA5));

        QJsonObject ghost = key(0x11, "ControlLeft");
        ghost["altGrCtrl"] = true;
        ghost["ctrlKey"] = true;
        ghost["altKey"] = true;
        QJsonObject alt = key(0xA5, "AltRight");
        alt["altGrCtrl"] = false;
        alt["ctrlKey"] = true;
        alt["altKey"] = true;
        QJsonObject beat;
        beat["keys"] = QJsonArray{ghost, alt};

        const QVector<IMediaEngine::HeldKey> wolf =
            InputMsg::parseHeldKeys(beat, KeyboardMode::Positional, LayoutFidelity::Auto);
        CHECK_EQ(wolf.size(), 1);
        if (wolf.size() == 1) {
            CHECK_EQ(wolf[0].keyCode, static_cast<short>(0xA5));
            CHECK_EQ(wolf[0].modifiers & MODIFIER_CTRL, 0);
            CHECK((wolf[0].modifiers & MODIFIER_ALT) != 0);
        }
        // Windows hosts keep both, Ctrl in the byte too.
        const QVector<IMediaEngine::HeldKey> native =
            InputMsg::parseHeldKeys(beat, KeyboardMode::Native, LayoutFidelity::Auto);
        CHECK_EQ(native.size(), 2);
        if (native.size() == 2) CHECK((native[1].modifiers & MODIFIER_CTRL) != 0);

        // A Ctrl of the viewer's own, held with it, stays in the byte.
        beat["keys"] = QJsonArray{ghost, alt, key(0x11, "ControlRight")};
        const QVector<IMediaEngine::HeldKey> both =
            InputMsg::parseHeldKeys(beat, KeyboardMode::Positional, LayoutFidelity::Auto);
        CHECK_EQ(both.size(), 2);
        if (both.size() == 2) CHECK((both[0].modifiers & MODIFIER_CTRL) != 0);
    }
}
