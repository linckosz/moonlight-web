/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/HidDescriptor.h"
#include "input/SystemKeyboard.h"
#include "input/SystemMouse.h"

#include <set>
#include <vector>

using namespace mw::native::input;
using namespace mw::native::input::systemkeyboard;

// The host's own keyboard on a Windows host with no keyboard (S1 for the
// keyboard): one fixed descriptor, the driver's second exception to the
// passthrough's refusal of keyboards, the reports that drive it, and the rules
// a security review set on them (L1, L3, L5, L7).

void run_system_keyboard_tests()
{
    SECTION("System keyboard — the descriptor");
    {
        const std::vector<uint8_t>& d = descriptor();
        const hid::Parsed p = hid::parse(d);
        CHECK(p.ok);
        // No report id, no output report (no LEDs), no feature report.
        CHECK_EQ(p.reportIds.size(), static_cast<size_t>(1));
        CHECK_EQ(static_cast<int>(p.reportIds[0]), 0);
        CHECK_EQ(hid::reportBytes(p, hid::Kind::Input, 0), kReportBytes);
        CHECK_EQ(hid::reportBytes(p, hid::Kind::Output, 0), static_cast<size_t>(0));
        CHECK_EQ(hid::reportBytes(p, hid::Kind::Feature, 0), static_cast<size_t>(0));
        CHECK_EQ(p.collections.size(), static_cast<size_t>(1));
        CHECK_EQ(p.collections[0].usage, 0x10006u); // Generic Desktop, Keyboard
    }

    SECTION("System keyboard — the passthrough still refuses it");
    {
        // What a page sends goes through validate(), which refuses a keyboard;
        // HidParking only ever carries what passed it, so this device can
        // never be parked, and a page can never send these bytes (L1, L2).
        CHECK(!hid::validate(descriptor()).empty());
    }

    SECTION("System keyboard — only these exact bytes");
    {
        const std::vector<uint8_t>& d = descriptor();
        CHECK(isDescriptor(d.data(), d.size()));
        CHECK(!isDescriptor(nullptr, d.size()));
        CHECK(!isDescriptor(d.data(), 0));
        CHECK(!isDescriptor(d.data(), d.size() - 1));
        std::vector<uint8_t> longer = d;
        longer.push_back(0xC0);
        CHECK(!isDescriptor(longer.data(), longer.size()));
        int refused = 0;
        for (size_t i = 0; i < d.size(); ++i) {
            std::vector<uint8_t> changed = d;
            changed[i] ^= 0x01;
            if (!isDescriptor(changed.data(), changed.size())) ++refused;
        }
        CHECK_EQ(refused, static_cast<int>(d.size()));
        // Neither exception is the other's.
        const std::vector<uint8_t>& mouse = systemmouse::descriptor();
        CHECK(!isDescriptor(mouse.data(), mouse.size()));
        CHECK(!systemmouse::isDescriptor(d.data(), d.size()));
    }

    SECTION("System keyboard — scancodes to usages");
    {
        CHECK_EQ(static_cast<int>(keyOfScan(0x1E).usage), 0x04); // A
        CHECK_EQ(static_cast<int>(keyOfScan(0x2C).usage), 0x1D); // Z
        CHECK_EQ(static_cast<int>(keyOfScan(0x26).usage), kUsageL);
        CHECK_EQ(static_cast<int>(keyOfScan(0x02).usage), 0x1E); // 1
        CHECK_EQ(static_cast<int>(keyOfScan(0x0B).usage), 0x27); // 0
        CHECK_EQ(static_cast<int>(keyOfScan(0x01).usage), 0x29); // Esc
        CHECK_EQ(static_cast<int>(keyOfScan(0x1C).usage), 0x28); // Enter
        CHECK_EQ(static_cast<int>(keyOfScan(0x39).usage), 0x2C); // Space
        CHECK_EQ(static_cast<int>(keyOfScan(0x3B).usage), 0x3A); // F1
        CHECK_EQ(static_cast<int>(keyOfScan(0x44).usage), 0x43); // F10
        CHECK_EQ(static_cast<int>(keyOfScan(0x57).usage), 0x44); // F11
        CHECK_EQ(static_cast<int>(keyOfScan(0x58).usage), 0x45); // F12
        CHECK_EQ(static_cast<int>(keyOfScan(0x56).usage), 0x64); // the ISO key
        CHECK_EQ(static_cast<int>(keyOfScan(0x45).usage), 0x53); // NumLock
        CHECK_EQ(static_cast<int>(keyOfScan(0x46).usage), kUsageScrollLock);
        CHECK_EQ(static_cast<int>(keyOfScan(0x53).usage), kUsageKeypadDecimal);
        // An extended key is not the keypad's it shares a scancode with.
        CHECK_EQ(static_cast<int>(keyOfScan(0xE053).usage), kUsageDelete);
        CHECK_EQ(static_cast<int>(keyOfScan(0x48).usage), 0x60);   // Keypad 8
        CHECK_EQ(static_cast<int>(keyOfScan(0xE048).usage), 0x52); // Up
        CHECK_EQ(static_cast<int>(keyOfScan(0xE05D).usage), 0x65); // Application

        CHECK_EQ(static_cast<int>(keyOfScan(0x1D).modifier), kLeftCtrl);
        CHECK_EQ(static_cast<int>(keyOfScan(0xE01D).modifier), kRightCtrl);
        CHECK_EQ(static_cast<int>(keyOfScan(0x2A).modifier), kLeftShift);
        CHECK_EQ(static_cast<int>(keyOfScan(0x36).modifier), kRightShift);
        CHECK_EQ(static_cast<int>(keyOfScan(0x38).modifier), kLeftAlt);
        CHECK_EQ(static_cast<int>(keyOfScan(0xE038).modifier), kRightAlt);
        CHECK_EQ(static_cast<int>(keyOfScan(0xE05B).modifier), kLeftWin);
        CHECK_EQ(static_cast<int>(keyOfScan(0xE05C).modifier), kRightWin);
        // A modifier is no usage of the array.
        CHECK_EQ(static_cast<int>(keyOfScan(0x1D).usage), 0);

        // Nothing for what it cannot say.
        CHECK(!keyOfScan(0).valid());
        CHECK(!keyOfScan(0x54).valid());
        CHECK(!keyOfScan(0x59).valid());
        CHECK(!keyOfScan(0xFF).valid());
        CHECK(!keyOfScan(0xE100).valid()); // Pause's sequence
        CHECK(!keyOfScan(0xE020).valid()); // a media key: Mute
        CHECK(!keyOfScan(0xE05E).valid()); // ACPI Power

        // Every key it can say is in the descriptor's reach, and no two
        // positions share a usage.
        std::set<int> seen;
        bool inReach = true;
        bool distinct = true;
        for (int prefix : {0, 0xE0}) {
            for (int code = 0; code < 256; ++code) {
                const Key key = keyOfScan(static_cast<uint16_t>((prefix << 8) | code));
                if (key.usage == 0) continue;
                inReach = inReach && key.usage <= kMaxUsage;
                distinct = seen.insert(key.usage).second && distinct;
            }
        }
        CHECK(inReach);
        CHECK(distinct);
    }

    SECTION("System keyboard — reports");
    {
        Report r = reportOf(0, {});
        bool empty = true;
        for (uint8_t b : r)
            empty = empty && b == 0;
        CHECK(empty);

        r = reportOf(kLeftShift, {0x04, 0x05});
        CHECK_EQ(static_cast<int>(r[0]), kLeftShift);
        CHECK_EQ(static_cast<int>(r[1]), 0);
        CHECK_EQ(static_cast<int>(r[2]), 0x04);
        CHECK_EQ(static_cast<int>(r[3]), 0x05);
        CHECK_EQ(static_cast<int>(r[4]), 0);

        // Six at most, and nothing out of reach: Power, F13, Mute.
        r = reportOf(0, {4, 5, 6, 7, 8, 9, 10});
        CHECK_EQ(static_cast<int>(r[7]), 9);
        r = reportOf(0, {0x66, 0x68, 0x7F, 0xFF, 0x04});
        CHECK_EQ(static_cast<int>(r[2]), 0x04);
        CHECK_EQ(static_cast<int>(r[3]), 0);

        // A usage is said once.
        r = reportOf(0, {0x04, 0x04});
        CHECK_EQ(static_cast<int>(r[2]), 0x04);
        CHECK_EQ(static_cast<int>(r[3]), 0);
    }

    SECTION("System keyboard — the combinations Windows keeps (L5)");
    {
        // Delete, both of them, under any Ctrl and any Alt.
        for (uint8_t ctrl : {kLeftCtrl, kRightCtrl}) {
            for (uint8_t alt : {kLeftAlt, kRightAlt}) {
                const uint8_t both = static_cast<uint8_t>(ctrl | alt);
                CHECK(withheld(both, kUsageDelete));
                CHECK(withheld(both, kUsageKeypadDecimal));
                const Report r = reportOf(both, {kUsageDelete, kUsageKeypadDecimal, 0x04});
                CHECK_EQ(static_cast<int>(r[0]), both);
                CHECK_EQ(static_cast<int>(r[2]), 0x04);
                CHECK_EQ(static_cast<int>(r[3]), 0);
            }
        }
        CHECK(!withheld(kLeftCtrl, kUsageDelete));
        CHECK(!withheld(kLeftAlt, kUsageDelete));
        CHECK(!withheld(0, kUsageDelete));

        // L under either Windows key.
        CHECK(withheld(kLeftWin, kUsageL));
        CHECK(withheld(kRightWin, kUsageL));
        CHECK(!withheld(kLeftCtrl, kUsageL));
        CHECK(!withheld(0, kUsageL));

        // Scroll Lock under the right Ctrl only.
        CHECK(withheld(kRightCtrl, kUsageScrollLock));
        CHECK(!withheld(kLeftCtrl, kUsageScrollLock));

        // Whatever the order: a key already held goes when the modifier comes,
        // since the report is worked out from the whole state each time.
        Report r = reportOf(0, {kUsageL});
        CHECK_EQ(static_cast<int>(r[2]), kUsageL);
        r = reportOf(kLeftWin, {kUsageL});
        CHECK_EQ(static_cast<int>(r[2]), 0);
        // Modifiers held by SendInput count: Windows adds the two up.
        r = reportOf(0, {kUsageL}, kRightWin);
        CHECK_EQ(static_cast<int>(r[0]), 0);
        CHECK_EQ(static_cast<int>(r[2]), 0);
        r = reportOf(kLeftAlt, {kUsageDelete}, kLeftCtrl);
        CHECK_EQ(static_cast<int>(r[0]), kLeftAlt);
        CHECK_EQ(static_cast<int>(r[2]), 0);
    }

    SECTION("System keyboard — who goes through it (L3, L6, L7)");
    {
        Standing s;
        s.mode = -1;
        s.allowElevated = true;
        s.gamingMode = true;
        s.onConsole = true;
        s.onDefault = true;
        CHECK(usable(s));
        CHECK(mayMake(s, 0));

        // A real keyboard, or a doubt: not made. The bench's forcing lifts that.
        CHECK(!mayMake(s, 1));
        CHECK(!mayMake(s, -1));
        Standing forced = s;
        forced.mode = 1;
        CHECK(mayMake(forced, 1));
        CHECK(mayMake(forced, -1));
        Standing never = s;
        never.mode = 0;
        CHECK(!usable(never));
        CHECK(!mayMake(never, 0));

        // A viewer held back from administrator windows: never, forced or not.
        Standing guest = s;
        guest.allowElevated = false;
        CHECK(!usable(guest));
        CHECK(!mayMake(guest, 0));
        guest.mode = 1;
        CHECK(!usable(guest));
        CHECK(!mayMake(guest, 0));

        // Another session on the console: never, forced or not.
        Standing away = s;
        away.onConsole = false;
        CHECK(!usable(away));
        CHECK(!mayMake(away, 0));
        away.mode = 1;
        CHECK(!usable(away));
        CHECK(!mayMake(away, 0));

        // A key out of gaming mode goes through SendInput, forced or not.
        Standing desktop = s;
        desktop.gamingMode = false;
        CHECK(!usable(desktop));
        CHECK(!mayMake(desktop, 0));
        desktop.mode = 1;
        CHECK(!usable(desktop));

        // The secure desktop, or one that cannot be read: SendInput, forced or
        // not. A SYSTEM worker still reaches it that way.
        Standing secure = s;
        secure.onDefault = false;
        CHECK(!usable(secure));
        CHECK(!mayMake(secure, 0));
        secure.mode = 1;
        CHECK(!usable(secure));
        CHECK(!mayMake(secure, 1));

        // Nothing said: closed.
        CHECK(!usable(Standing{}));
    }
}
