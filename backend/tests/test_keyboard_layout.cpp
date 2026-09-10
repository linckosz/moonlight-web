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
                           KeyboardMode::SunshineWindows, KeyboardMode::SunshineMacos};

    // ── A key with no character at all ───────────────────────────────────────
    //
    // Arrows, F-keys, Enter, the modifiers: nothing to correct, nothing to
    // choose. The position goes out untouched whatever the host is.
    for (KeyboardMode mode : allModes) {
        const KeyPlan plan = resolveKey(key(kVkUp, "ArrowUp"), mode);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkUp));
        CHECK_EQ(plan.flags, static_cast<char>(0));
    }

    // Opting out in the settings file lands in the same place: the character is
    // ignored and the position goes out, exactly as it did before any of this.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::Positional);
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
        const KeyPlan plan =
            resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::SunshineWindows);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkA));
        CHECK_EQ(plan.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
    }
    {
        // Case is carried by the modifiers, not by the VK: VK_A either way.
        const KeyPlan plan =
            resolveKey(key(kVkQ, "KeyQ", "A", true), KeyboardMode::SunshineWindows);
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkA));
    }
    {
        // A US client's letter agrees with its position, so it is left alone.
        // Sunshine derives the scancode of a non-normalized VK from its OWN
        // thread's layout, where the normalized path uses a fixed table — asking
        // for it here would trade a scancode we can predict for one we cannot,
        // and buy nothing.
        const KeyPlan plan =
            resolveKey(key(kVkQ, "KeyQ", "q", false), KeyboardMode::SunshineWindows);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(plan.flags, static_cast<char>(0));
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
        const KeyPlan plan =
            resolveKey(key(kVk1, "Digit1", "&", true), KeyboardMode::SunshineWindows);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVk1));
        CHECK_EQ(plan.flags, static_cast<char>(0));
    }
    {
        // An accented letter is not a letter either as far as the VK path goes:
        // no US virtual key carries it. Positional, for the same reason.
        const KeyPlan plan =
            resolveKey(key(kVk1, "Digit2", "é", true), KeyboardMode::SunshineWindows);
        CHECK(!plan.isText());
    }

    // ── Sunshine on macOS ────────────────────────────────────────────────────
    //
    // It ignores the non-normalized flag outright, so a letter cannot be both
    // exact and a real key: text is the only exact channel, and it is taken —
    // a macOS GameStream host is a machine people type on.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::SunshineMacos);
        CHECK(plan.isText());
        CHECK_EQ(plan.text.toStdString(), std::string("a"));
    }
    {
        // An agreeing letter, and everything that is not a letter, stay
        // positional there too.
        const KeyPlan agreeing =
            resolveKey(key(kVkQ, "KeyQ", "q", false), KeyboardMode::SunshineMacos);
        CHECK(!agreeing.isText());
        CHECK_EQ(agreeing.keyCode, static_cast<short>(kVkQ));

        const KeyPlan digit =
            resolveKey(key(kVk1, "Digit1", "&", true), KeyboardMode::SunshineMacos);
        CHECK(!digit.isText());
        CHECK_EQ(digit.keyCode, static_cast<short>(kVk1));
    }

    // ── The native host ──────────────────────────────────────────────────────
    //
    // EVERY printable character, agreeing ones included, because the platform
    // layer turns each one back into a real key of the host's OWN layout: when
    // the two layouts match the resolution lands on the very key the viewer
    // pressed, so nothing is lost by asking.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a", true), KeyboardMode::Native);
        CHECK(plan.isText());
        CHECK_EQ(plan.text.toStdString(), std::string("a"));
    }
    {
        const KeyPlan plan = resolveKey(key(kVk1, "Digit1", "&", true), KeyboardMode::Native);
        CHECK(plan.isText());
    }
    {
        // The mirror case, and the reason the native host does not use `nonUs`
        // at all: a US-layout viewer on a French host presses the key marked A,
        // means "q", and diverges from nothing — yet the position types "a".
        // What the position must be compared against is the host's layout, and
        // only the host can make that comparison.
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "q", false), KeyboardMode::Native);
        CHECK(plan.isText());
        CHECK_EQ(plan.text.toStdString(), std::string("q"));
    }

    // ── International keys keep their own rule, in every mode ────────────────
    //
    // They have no US virtual key at all, so they are sent as raw VKs whatever
    // the layout policy says — and a stray character must not override that.
    for (KeyboardMode mode : allModes) {
        const KeyPlan iso = resolveKey(key(0, "IntlBackslash", "<", true), mode);
        CHECK(!iso.isText());
        CHECK_EQ(iso.keyCode, static_cast<short>(0xE2));
        CHECK_EQ(iso.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));

        const KeyPlan ro = resolveKey(key(0, "IntlRo", "\\", true), mode);
        CHECK(!ro.isText());
        CHECK_EQ(ro.keyCode, static_cast<short>(0xC1));
        CHECK_EQ(ro.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
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
        const KeyPlan a = resolveKey(down, KeyboardMode::SunshineWindows);
        const KeyPlan b = resolveKey(up, KeyboardMode::SunshineWindows);
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
            InputMsg::parseHeldKeys(beat, KeyboardMode::SunshineWindows);
        CHECK_EQ(held.size(), 3);
        CHECK_EQ(held[0].keyCode, static_cast<short>(kVkA));
        CHECK_EQ(held[0].flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
        CHECK_EQ(held[1].keyCode, static_cast<short>(kVk1));
        CHECK_EQ(held[2].keyCode, static_cast<short>(kVkUp));

        // Native: every printable character IS a character, so only the key
        // that never had one survives the filter.
        const QVector<IMediaEngine::HeldKey> nativeHeld =
            InputMsg::parseHeldKeys(beat, KeyboardMode::Native);
        CHECK_EQ(nativeHeld.size(), 1);
        CHECK_EQ(nativeHeld[0].keyCode, static_cast<short>(kVkUp));
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
            CHECK(describeKey(key(kVkUp, "ArrowUp"), mode, resolveKey(key(kVkUp, "ArrowUp"), mode))
                      .line.isEmpty());
    }
    {
        // A corrected letter on Sunshine/Windows: exact AND still a real key.
        // The one case where both answers are yes, so no warning.
        const QJsonObject msg = key(kVkQ, "KeyQ", "a", true);
        const InputMsg::KeyDiag diag = describeKey(msg, KeyboardMode::SunshineWindows,
                                                   resolveKey(msg, KeyboardMode::SunshineWindows));
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
        const InputMsg::KeyDiag diag = describeKey(msg, KeyboardMode::SunshineWindows,
                                                   resolveKey(msg, KeyboardMode::SunshineWindows));
        CHECK(diag.warn);
        CHECK(diag.line.contains(QStringLiteral("Notepad: KO")));
        // The key a game sees is named in US terms, which is the vocabulary
        // bindings are written in — the physical key is right even here.
        CHECK(diag.line.contains(QStringLiteral("Game: OK real key, US '1'")));
    }
    {
        // A key that agrees with US is the assumption the protocol has always
        // run on. Reported, not warned: flagging it would put a warning under
        // every keystroke of a US viewer and drown the lines that matter.
        const QJsonObject msg = key(kVkQ, "KeyQ", "q", false);
        const InputMsg::KeyDiag diag = describeKey(msg, KeyboardMode::SunshineWindows,
                                                   resolveKey(msg, KeyboardMode::SunshineWindows));
        CHECK(!diag.warn);
        CHECK(diag.line.contains(QStringLiteral("Notepad: OK 'q'")));
    }
    {
        // Text on a remote host types the character perfectly and presses
        // nothing. Always warned: that half is lost and cannot be recovered.
        const QJsonObject msg = key(kVkQ, "KeyQ", "a", true);
        const InputMsg::KeyDiag diag = describeKey(msg, KeyboardMode::SunshineMacos,
                                                   resolveKey(msg, KeyboardMode::SunshineMacos));
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
            describeKey(msg, KeyboardMode::Native, resolveKey(msg, KeyboardMode::Native));
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
        CHECK(describeKey(msg, KeyboardMode::Native, resolveKey(msg, KeyboardMode::Native))
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
        CHECK_EQ(InputMsg::usKeyLabel(kVk1).toStdString(), std::string("1"));
        CHECK_EQ(InputMsg::usKeyLabel(kVkQ).toStdString(), std::string("Q"));
        CHECK_EQ(InputMsg::usKeyLabel(0xBF).toStdString(), std::string("/"));
        CHECK_EQ(InputMsg::usKeyLabel(0x20).toStdString(), std::string("Space"));
    }
}
