/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "input/windows/UsScanCode.h"

/*
 * The table the native host converts a normalized virtual key back into a
 * physical key with. It is hand-written — Windows' own MapVirtualKeyW answers
 * with the CALLING THREAD's layout, which on a French host sent every AZERTY
 * key out at its QWERTY position — so a single transposed digit here is a key
 * that types the wrong character, with nothing to catch it but a user noticing.
 *
 * These checks are the round trip that matters: a US virtual key must come back
 * as the scancode of the position that carries it on a US board.
 */
void run_us_scancode_tests()
{
    SECTION("UsScanCode");

    using mw::native::input::usScanCode;

    // The top letter row, left to right: QWERTY spells itself out.
    const int qwerty[] = {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'};
    for (int i = 0; i < 10; ++i)
        CHECK_EQ(usScanCode(qwerty[i]), static_cast<uint16_t>(0x10 + i));

    // The home row and the bottom row, same way.
    const int home[] = {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L'};
    for (int i = 0; i < 9; ++i)
        CHECK_EQ(usScanCode(home[i]), static_cast<uint16_t>(0x1E + i));
    const int bottom[] = {'Z', 'X', 'C', 'V', 'B', 'N', 'M'};
    for (int i = 0; i < 7; ++i)
        CHECK_EQ(usScanCode(bottom[i]), static_cast<uint16_t>(0x2C + i));

    // The digit row: 1..9 run in order and 0 closes it rather than opening it.
    for (int i = 1; i <= 9; ++i)
        CHECK_EQ(usScanCode('0' + i), static_cast<uint16_t>(0x02 + i - 1));
    CHECK_EQ(usScanCode('0'), 0x0Bu);

    // The AZERTY case that started this: the key printed A sits where QWERTY
    // has Q, so the browser sends VK_Q and the host must press THAT position —
    // where a French layout then types "a".
    CHECK_EQ(usScanCode(0x51), 0x10u); // VK_Q → the physical A of an AZERTY board
    CHECK_EQ(usScanCode(0x41), 0x1Eu); // VK_A → the physical Q
    CHECK_EQ(usScanCode(0x5A), 0x2Cu); // VK_Z → the physical W
    CHECK_EQ(usScanCode(0x57), 0x11u); // VK_W → the physical Z

    // Function keys: F1..F10 contiguous, F11/F12 sitting elsewhere entirely.
    for (int i = 0; i < 10; ++i)
        CHECK_EQ(usScanCode(0x70 + i), static_cast<uint16_t>(0x3B + i));
    CHECK_EQ(usScanCode(0x7A), 0x57u); // VK_F11
    CHECK_EQ(usScanCode(0x7B), 0x58u); // VK_F12

    // Numpad digits are laid out bottom-up, unlike the digit row.
    const uint16_t numpad[] = {0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};
    for (int i = 0; i < 10; ++i)
        CHECK_EQ(usScanCode(0x60 + i), numpad[i]);

    // Extended keys carry the 0xE0 prefix, which is what tells the navigation
    // cluster apart from the numpad keys sharing its scancodes.
    CHECK_EQ(usScanCode(0x25), 0xE04Bu); // VK_LEFT  vs numpad 4
    CHECK_EQ(usScanCode(0x27), 0xE04Du); // VK_RIGHT vs numpad 6
    CHECK_EQ(usScanCode(0x26), 0xE048u); // VK_UP    vs numpad 8
    CHECK_EQ(usScanCode(0x28), 0xE050u); // VK_DOWN  vs numpad 2
    CHECK_EQ(usScanCode(0x2E), 0xE053u); // VK_DELETE vs numpad .
    CHECK_EQ(usScanCode(0x6E), 0x0053u); // VK_DECIMAL, the same key unextended
    CHECK_EQ(usScanCode(0x6F), 0xE035u); // VK_DIVIDE vs the / of the letter block
    CHECK_EQ(usScanCode(0xBF), 0x0035u); // VK_OEM_2

    // Sided modifiers: only the right-hand ones are extended, and an unsided VK
    // takes the left key the way a keyboard does.
    CHECK_EQ(usScanCode(0x10), 0x002Au); // VK_SHIFT   → left
    CHECK_EQ(usScanCode(0xA1), 0x0036u); // VK_RSHIFT  (not extended: Shift is special)
    CHECK_EQ(usScanCode(0x11), 0x001Du); // VK_CONTROL → left
    CHECK_EQ(usScanCode(0xA3), 0xE01Du); // VK_RCONTROL
    CHECK_EQ(usScanCode(0x12), 0x0038u); // VK_MENU    → left Alt
    CHECK_EQ(usScanCode(0xA5), 0xE038u); // VK_RMENU   → AltGr
    CHECK_EQ(usScanCode(0x5B), 0xE05Bu); // VK_LWIN

    // NumLock's own make code is NOT extended — the flag is added downstream so
    // Windows does not read a bare 0x45 as Pause.
    CHECK_EQ(usScanCode(0x90), 0x0045u);

    // Pause is refused on purpose: its make code is a three-byte sequence no
    // single INPUT can carry, so it has to go out as a virtual key instead.
    CHECK_EQ(usScanCode(0x13), 0u);
    // And so does anything the US layout does not name at all.
    CHECK_EQ(usScanCode(0xA6), 0u); // VK_BROWSER_BACK
    CHECK_EQ(usScanCode(0x00), 0u);
    CHECK_EQ(usScanCode(0xFF), 0u);
}
