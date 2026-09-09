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
 * they were read off (see the header's notes), and — more importantly — pin the
 * invariant that keeps the whole thing safe: a keystroke with no character to
 * correct resolves EXACTLY as it did before any of this existed.
 */

namespace {

/// A keydown message as the browser sends it.
QJsonObject key(int vk, const QString& code, const QString& character = QString())
{
    QJsonObject msg;
    msg["type"] = "keydown";
    msg["keyCode"] = vk;
    msg["code"] = code;
    if (!character.isNull()) msg["char"] = character;
    return msg;
}

constexpr int kVkQ = 0x51;
constexpr int kVkA = 0x41;
constexpr int kVk1 = 0x31;

} // namespace

void run_keyboard_layout_tests()
{
    SECTION("KeyboardLayout");

    using InputMsg::KeyPlan;
    using InputMsg::resolveKey;

    // ── The untouched path ───────────────────────────────────────────────────
    //
    // No character means the client's layout agrees with the US layout at this
    // position — every key of a US viewer, and most keys of any other. Whatever
    // the host is, the message must resolve to the plain position it always did.
    for (KeyboardMode mode : {KeyboardMode::Positional, KeyboardMode::Native,
                              KeyboardMode::SunshineWindows, KeyboardMode::SunshineMacos}) {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ"), mode);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(plan.flags, static_cast<char>(0));
    }

    // Opting out in the settings file lands here too: a divergent character is
    // ignored and the position goes out, exactly as before.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a"), KeyboardMode::Positional);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkQ));
        CHECK_EQ(plan.flags, static_cast<char>(0));
    }

    // ── Sunshine on Windows ──────────────────────────────────────────────────
    //
    // A letter becomes the VK of that LETTER, flagged non-normalized so the host
    // injects the VK itself and Windows resolves it through the active layout.
    // It stays a real key press, which is why letters do not go through text.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a"), KeyboardMode::SunshineWindows);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkA));
        CHECK_EQ(plan.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
    }
    {
        // Case is carried by the modifiers, not by the VK: VK_A either way.
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "A"), KeyboardMode::SunshineWindows);
        CHECK_EQ(plan.keyCode, static_cast<short>(kVkA));
    }

    // ── The rule that keeps games working: never trade a key for text ────────
    //
    // A digit is not a letter, and text would type it exactly — but text has no
    // key state, and the divergence above is measured against the US layout,
    // not against the HOST's. On a host whose layout already matches the
    // client's, the position was ALREADY producing "&": correcting it would
    // trade a working key for a stateless one and stop a shooter's weapon slots
    // answering, for a character that was never wrong. So it stays positional.
    {
        const KeyPlan plan = resolveKey(key(kVk1, "Digit1", "&"), KeyboardMode::SunshineWindows);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVk1));
        CHECK_EQ(plan.flags, static_cast<char>(0));
    }
    {
        // An accented letter is not a letter either as far as the VK path goes:
        // no US virtual key carries it. Positional, for the same reason.
        const KeyPlan plan = resolveKey(key(kVk1, "Digit2", "é"), KeyboardMode::SunshineWindows);
        CHECK(!plan.isText());
    }

    // ── Sunshine on macOS ────────────────────────────────────────────────────
    //
    // It ignores the non-normalized flag outright, so a letter cannot be both
    // exact and a real key: text is the only exact channel, and it is taken —
    // a macOS GameStream host is a machine people type on.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a"), KeyboardMode::SunshineMacos);
        CHECK(plan.isText());
        CHECK_EQ(plan.text.toStdString(), std::string("a"));
    }
    {
        // Everything else stays positional there too.
        const KeyPlan plan = resolveKey(key(kVk1, "Digit1", "&"), KeyboardMode::SunshineMacos);
        CHECK(!plan.isText());
        CHECK_EQ(plan.keyCode, static_cast<short>(kVk1));
    }

    // ── The native host ──────────────────────────────────────────────────────
    //
    // Everything goes as a character, because the platform layer turns it back
    // into a real key of the host's own layout — nothing is lost by asking.
    {
        const KeyPlan plan = resolveKey(key(kVkQ, "KeyQ", "a"), KeyboardMode::Native);
        CHECK(plan.isText());
        CHECK_EQ(plan.text.toStdString(), std::string("a"));
    }
    {
        const KeyPlan plan = resolveKey(key(kVk1, "Digit1", "&"), KeyboardMode::Native);
        CHECK(plan.isText());
    }

    // ── International keys keep their own rule, in every mode ────────────────
    //
    // They have no US virtual key at all, so they are sent as raw VKs whatever
    // the layout policy says — and a stray character must not override that.
    for (KeyboardMode mode : {KeyboardMode::Positional, KeyboardMode::Native,
                              KeyboardMode::SunshineWindows, KeyboardMode::SunshineMacos}) {
        const KeyPlan iso = resolveKey(key(0, "IntlBackslash", "<"), mode);
        CHECK(!iso.isText());
        CHECK_EQ(iso.keyCode, static_cast<short>(0xE2));
        CHECK_EQ(iso.flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));

        const KeyPlan ro = resolveKey(key(0, "IntlRo", "\\"), mode);
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
        QJsonObject down = key(kVkQ, "KeyQ", "a");
        QJsonObject up = down;
        up["type"] = "keyup";
        const KeyPlan a = resolveKey(down, KeyboardMode::SunshineWindows);
        const KeyPlan b = resolveKey(up, KeyboardMode::SunshineWindows);
        CHECK_EQ(a.keyCode, b.keyCode);
        CHECK_EQ(a.flags, b.flags);
    }

    // ── The held-input heartbeat ─────────────────────────────────────────────
    //
    // A character key is deliberately absent from it: the watchdog re-presses
    // whatever it sees, and re-pressing a character would type the letter again
    // rather than re-assert a held key. Keys that resolve to a real press stay.
    {
        QJsonObject beat;
        QJsonArray keys;
        keys.append(key(kVkQ, "KeyQ", "a"));   // → a real VK on Windows
        keys.append(key(kVk1, "Digit1", "&")); // → positional on Windows
        keys.append(key(kVkQ, "KeyE"));        // → agreeing key, untouched
        beat["keys"] = keys;

        // Nothing resolves to text on a Windows host, so the whole set is
        // reported — a held movement key still gets re-asserted after a stall.
        const QVector<IMediaEngine::HeldKey> held =
            InputMsg::parseHeldKeys(beat, KeyboardMode::SunshineWindows);
        CHECK_EQ(held.size(), 3);
        CHECK_EQ(held[0].keyCode, static_cast<short>(kVkA));
        CHECK_EQ(held[0].flags, static_cast<char>(SS_KBE_FLAG_NON_NORMALIZED));
        CHECK_EQ(held[1].keyCode, static_cast<short>(kVk1));
        CHECK_EQ(held[2].keyCode, static_cast<short>(kVkQ));

        // On the native host every character IS a character, so only the
        // agreeing key survives the filter.
        const QVector<IMediaEngine::HeldKey> nativeHeld =
            InputMsg::parseHeldKeys(beat, KeyboardMode::Native);
        CHECK_EQ(nativeHeld.size(), 1);
    }
}
