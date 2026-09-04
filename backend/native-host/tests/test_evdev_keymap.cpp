/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/linux/EvdevKeyMap.h"
#include "input/windows/UsScanCode.h"

#include <cstdio>
#include <set>

using namespace mw::native::input;

// The Linux key map, tested on whatever machine happens to run the suite.
//
// This is the point of writing the codes as literals rather than as KEY_*
// macros: the table is checkable everywhere, and it is checked AGAINST the
// kernel headers by static_assert in UinputInput.cpp on the one platform that
// has them. Here the properties that no header can confirm are checked instead
// — that the map is injective, that it agrees with the Windows table about
// which keys exist, and that the awkward rows really are awkward in the way the
// code claims.
//
// A wrong entry does not fail: it types the wrong letter on somebody's desktop.

void run_evdev_keymap_tests()
{
    SECTION("Linux key map — virtual key to evdev code");

    // ── The rows the code special-cases, spot-checked at both ends ──────────
    CHECK_EQ(evdevKeyCode(0x41), 30); // A   KEY_A
    CHECK_EQ(evdevKeyCode(0x5A), 44); // Z   KEY_Z
    CHECK_EQ(evdevKeyCode(0x51), 16); // Q   KEY_Q — first key of the top row
    CHECK_EQ(evdevKeyCode(0x4D), 50); // M   KEY_M

    // The digit row does not start at zero: 1..9 run 2..10 and 0 closes it.
    // Writing it as a plain range would put every digit one place to the left.
    CHECK_EQ(evdevKeyCode(0x31), 2);  // 1  KEY_1
    CHECK_EQ(evdevKeyCode(0x39), 10); // 9  KEY_9
    CHECK_EQ(evdevKeyCode(0x30), 11); // 0  KEY_0

    // The numpad is laid out bottom-up, so its VK order is not its code order:
    // NUMPAD0 sits BELOW NUMPAD1..3 on the keypad.
    CHECK_EQ(evdevKeyCode(0x60), 82); // KP0  KEY_KP0
    CHECK_EQ(evdevKeyCode(0x61), 79); // KP1  KEY_KP1
    CHECK_EQ(evdevKeyCode(0x69), 73); // KP9  KEY_KP9
    CHECK(evdevKeyCode(0x60) > evdevKeyCode(0x61));

    // F1..F10 are contiguous; F11 and F12 were added later and sit elsewhere.
    CHECK_EQ(evdevKeyCode(0x70), 59); // F1   KEY_F1
    CHECK_EQ(evdevKeyCode(0x79), 68); // F10  KEY_F10
    CHECK_EQ(evdevKeyCode(0x7A), 87); // F11  KEY_F11
    CHECK_EQ(evdevKeyCode(0x7B), 88); // F12  KEY_F12
    CHECK(evdevKeyCode(0x7A) != evdevKeyCode(0x79) + 1);

    // ── Left and right modifiers are distinct ───────────────────────────────
    //
    // Folding them would break AltGr, which is a right-Alt and nothing else:
    // an AZERTY user could not type @ or #.
    CHECK(evdevKeyCode(0xA0) != evdevKeyCode(0xA1)); // shift
    CHECK(evdevKeyCode(0xA2) != evdevKeyCode(0xA3)); // control
    CHECK(evdevKeyCode(0xA4) != evdevKeyCode(0xA5)); // alt / AltGr
    CHECK(evdevKeyCode(0x5B) != evdevKeyCode(0x5C)); // meta

    // The undifferentiated forms a client may still send resolve to the left
    // one — never to nothing, which would drop the modifier entirely.
    CHECK_EQ(evdevKeyCode(0x10), evdevKeyCode(0xA0));
    CHECK_EQ(evdevKeyCode(0x11), evdevKeyCode(0xA2));
    CHECK_EQ(evdevKeyCode(0x12), evdevKeyCode(0xA4));

    // ── Injective, apart from the deliberate aliases above ──────────────────
    //
    // Two virtual keys sharing a code means one of them types the other's
    // character, forever, on that layout. The three aliases are the only
    // intended collisions.
    {
        std::set<uint16_t> seen;
        int collisions = 0;
        for (int vk = 0; vk <= 0xFF; ++vk) {
            if (vk == 0x10 || vk == 0x11 || vk == 0x12) continue; // the aliases
            const uint16_t code = evdevKeyCode(vk);
            if (code == 0) continue;
            if (!seen.insert(code).second) {
                ++collisions;
                std::fprintf(stderr, "  collision: vk 0x%02X -> %u\n", vk, code);
            }
        }
        CHECK_EQ(collisions, 0);
        std::fprintf(stderr, "  %zu distinct evdev codes mapped\n", seen.size());
    }

    // ── The two platform tables name the same keys, with one exception ──────
    //
    // Not the same VALUES — one is a set-1 scancode, the other an evdev code —
    // but the same SET of virtual keys. A key present on Windows and missing
    // here is a key that silently does nothing on Linux, which is the failure
    // worth catching.
    //
    // The exception is Pause (VK 0x13), and it is not an oversight on either
    // side. usScanCode answers 0 for it deliberately: the Windows caller then
    // falls back to MapVirtualKey, because Pause is layout-independent and its
    // scancode sequence is a special case. Linux has no such fallback — uinput
    // takes the code or nothing — so KEY_PAUSE is named outright here.
    //
    // Asserted as exactly one so a SECOND divergence, which would be a real
    // omission, still fails.
    {
        int windowsOnly = 0;
        int linuxOnly = 0;
        for (int vk = 0; vk <= 0xFF; ++vk) {
            const bool onWindows = usScanCode(vk) != 0;
            const bool onLinux = evdevKeyCode(vk) != 0;
            if (onWindows && !onLinux) {
                ++windowsOnly;
                std::fprintf(stderr, "  vk 0x%02X: Windows only\n", vk);
            }
            if (onLinux && !onWindows) {
                ++linuxOnly;
                if (vk != 0x13) std::fprintf(stderr, "  vk 0x%02X: Linux only\n", vk);
            }
        }
        CHECK_EQ(windowsOnly, 0);
        CHECK_EQ(linuxOnly, 1);
        CHECK_EQ(usScanCode(0x13), 0);  // Pause: Windows falls back
        CHECK(evdevKeyCode(0x13) != 0); // Pause: Linux names it
    }

    // Keys Linux does not name at a position are dropped, not guessed at. Zero
    // is the "no such key" answer and 0x00 is not a virtual key at all.
    CHECK_EQ(evdevKeyCode(0x00), 0);
    CHECK_EQ(evdevKeyCode(0xFF), 0);
}
