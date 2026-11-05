/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/HidDescriptor.h"
#include "input/SystemMouse.h"

#include <vector>

using namespace mw::native::input;

// The host's own mouse on a Windows host with no pointing device (option B,
// porte S1): one fixed descriptor, the driver's only exception to the
// passthrough's refusal of pointing devices, and the reports that drive it.

namespace {

int16_t read16(const systemmouse::Report& r, size_t at)
{
    return static_cast<int16_t>(static_cast<uint16_t>(r[at] | (r[at + 1] << 8)));
}

} // namespace

void run_system_mouse_tests()
{
    SECTION("System mouse — the descriptor");
    {
        const std::vector<uint8_t>& d = systemmouse::descriptor();
        const hid::Parsed p = hid::parse(d);
        CHECK(p.ok);
        // No report id: the report is the bytes alone.
        CHECK_EQ(p.reportIds.size(), static_cast<size_t>(1));
        CHECK_EQ(static_cast<int>(p.reportIds[0]), 0);
        CHECK_EQ(hid::reportBytes(p, hid::Kind::Input, 0), systemmouse::kReportBytes);
        CHECK_EQ(hid::reportBytes(p, hid::Kind::Output, 0), static_cast<size_t>(0));
        CHECK_EQ(hid::reportBytes(p, hid::Kind::Feature, 0), static_cast<size_t>(0));
        CHECK(!p.collections.empty());
        CHECK_EQ(p.collections[0].usage, 0x10002u); // Generic Desktop, Mouse
    }

    SECTION("System mouse — the passthrough still refuses it");
    {
        // What a page sends goes through validate(), which refuses a mouse:
        // the exception lives in the driver's byte comparison, nowhere else.
        CHECK(!hid::validate(systemmouse::descriptor()).empty());
    }

    SECTION("System mouse — only these exact bytes");
    {
        const std::vector<uint8_t>& d = systemmouse::descriptor();
        CHECK(systemmouse::isDescriptor(d.data(), d.size()));
        CHECK(!systemmouse::isDescriptor(nullptr, d.size()));
        CHECK(!systemmouse::isDescriptor(d.data(), 0));
        CHECK(!systemmouse::isDescriptor(d.data(), d.size() - 1));
        std::vector<uint8_t> longer = d;
        longer.push_back(0xC0);
        CHECK(!systemmouse::isDescriptor(longer.data(), longer.size()));
        // Any byte changed: refused, every one of them.
        int refused = 0;
        for (size_t i = 0; i < d.size(); ++i) {
            std::vector<uint8_t> changed = d;
            changed[i] ^= 0x01;
            if (!systemmouse::isDescriptor(changed.data(), changed.size())) ++refused;
        }
        CHECK_EQ(refused, static_cast<int>(d.size()));
    }

    SECTION("System mouse — buttons");
    {
        CHECK_EQ(static_cast<int>(systemmouse::buttonBit(1)), 0x01); // left
        CHECK_EQ(static_cast<int>(systemmouse::buttonBit(3)), 0x02); // right
        CHECK_EQ(static_cast<int>(systemmouse::buttonBit(2)), 0x04); // middle
        CHECK_EQ(static_cast<int>(systemmouse::buttonBit(4)), 0x08);
        CHECK_EQ(static_cast<int>(systemmouse::buttonBit(5)), 0x10);
        CHECK_EQ(static_cast<int>(systemmouse::buttonBit(0)), 0);
        CHECK_EQ(static_cast<int>(systemmouse::buttonBit(6)), 0);
    }

    SECTION("System mouse — reports");
    {
        // A button alone: one report, nothing moves.
        auto r = systemmouse::reports(0x03, 0, 0);
        CHECK_EQ(r.size(), static_cast<size_t>(1));
        CHECK_EQ(static_cast<int>(r[0][0]), 0x03);
        CHECK_EQ(read16(r[0], 1), 0);
        CHECK_EQ(read16(r[0], 3), 0);
        CHECK_EQ(static_cast<int>(r[0][5]), 0);
        CHECK_EQ(static_cast<int>(r[0][6]), 0);

        // A move, little-endian and signed, with the wheels.
        r = systemmouse::reports(0x01, -5, 300, -2, 1);
        CHECK_EQ(r.size(), static_cast<size_t>(1));
        CHECK_EQ(read16(r[0], 1), -5);
        CHECK_EQ(read16(r[0], 3), 300);
        CHECK_EQ(static_cast<int>(static_cast<int8_t>(r[0][5])), -2);
        CHECK_EQ(static_cast<int>(static_cast<int8_t>(r[0][6])), 1);

        // Only the five buttons: the padding bits stay clear.
        r = systemmouse::reports(0xFF, 0, 0);
        CHECK_EQ(static_cast<int>(r[0][0]), 0x1F);

        // A step beyond one report: split, the whole of it delivered, the
        // buttons in every piece.
        r = systemmouse::reports(0x02, 70000, -40000, 300, 0);
        CHECK_EQ(r.size(), static_cast<size_t>(3));
        int sumX = 0, sumY = 0, sumWheel = 0;
        bool buttonsEverywhere = true;
        bool withinReach = true;
        for (const auto& one : r) {
            sumX += read16(one, 1);
            sumY += read16(one, 3);
            sumWheel += static_cast<int8_t>(one[5]);
            buttonsEverywhere = buttonsEverywhere && one[0] == 0x02;
            withinReach = withinReach && read16(one, 1) >= -systemmouse::kMaxDelta &&
                          read16(one, 3) >= -systemmouse::kMaxDelta;
        }
        CHECK_EQ(sumX, 70000);
        CHECK_EQ(sumY, -40000);
        CHECK_EQ(sumWheel, 300);
        CHECK(buttonsEverywhere);
        // -32768 is outside the logical range the descriptor declares.
        CHECK(withinReach);
    }

    SECTION("System mouse — wheel notches");
    {
        systemmouse::Notches n;
        CHECK_EQ(n.take(120), 1);
        CHECK_EQ(n.take(-240), -2);
        // A trackpad's small steps add up to a notch, and no more.
        CHECK_EQ(n.take(40), 0);
        CHECK_EQ(n.take(40), 0);
        CHECK_EQ(n.take(40), 1);
        CHECK_EQ(n.take(0), 0);
        // What was left over is dropped when the wheel turns back.
        CHECK_EQ(n.take(100), 0);
        CHECK_EQ(n.take(-100), 0);
        CHECK_EQ(n.take(-20), -1);
        n.reset();
        CHECK_EQ(n.take(119), 0);
        n.reset();
        CHECK_EQ(n.take(1), 0);
    }

    SECTION("System mouse — while the console is its own session");
    {
        const uint8_t left = systemmouse::buttonBit(1);
        const uint8_t right = systemmouse::buttonBit(3);

        // On the console: what reports() says, and nothing to do about it.
        systemmouse::ConsoleGuard guard;
        auto s = guard.step(true, 0, 5, -3);
        CHECK(!s.aside);
        CHECK(!s.changed);
        CHECK_EQ(s.forgotten, 0);
        CHECK_EQ(s.reports.size(), size_t(1));
        CHECK_EQ(read16(s.reports[0], 1), 5);
        CHECK_EQ(read16(s.reports[0], 3), -3);

        // Off it, with nothing down: not one byte, the caller told, once.
        s = guard.step(false, 0, 5, 5, 1, 1);
        CHECK(s.aside);
        CHECK(s.changed);
        CHECK(s.reports.empty());
        CHECK_EQ(s.forgotten, 0);
        s = guard.step(false, 0, 5, 5);
        CHECK(s.aside);
        CHECK(!s.changed);
        CHECK(s.reports.empty());

        // A press off it goes nowhere near the device, and is handed back.
        s = guard.step(false, left, 0, 0);
        CHECK(s.aside);
        CHECK(s.reports.empty());
        CHECK_EQ(s.forgotten, left);

        // Back on it: the device again, the change said.
        s = guard.step(true, left, 1, 0);
        CHECK(!s.aside);
        CHECK(s.changed);
        CHECK_EQ(s.reports.size(), size_t(1));
        CHECK_EQ(s.reports[0][0], left);

        // Held when the console leaves: said up once, with no movement in it,
        // and never again.
        s = guard.step(false, left | right, 40, 40, 2, 2);
        CHECK(s.aside);
        CHECK_EQ(s.forgotten, static_cast<uint8_t>(left | right));
        CHECK_EQ(s.reports.size(), size_t(1));
        CHECK_EQ(s.reports[0][0], 0);
        CHECK_EQ(read16(s.reports[0], 1), 0);
        CHECK_EQ(read16(s.reports[0], 3), 0);
        CHECK_EQ(s.reports[0][5], 0);
        CHECK_EQ(s.reports[0][6], 0);
        s = guard.step(false, 0, 40, 40);
        CHECK(s.reports.empty());

        // The viewer's own release, as the console leaves: the caller has
        // already dropped the button, and the device still has to hear it.
        systemmouse::ConsoleGuard released;
        CHECK_EQ(released.step(true, left, 0, 0).reports.size(), size_t(1));
        s = released.step(false, 0, 0, 0);
        CHECK(s.aside);
        CHECK_EQ(s.forgotten, 0);
        CHECK_EQ(s.reports.size(), size_t(1));
        CHECK_EQ(s.reports[0][0], 0);

        // One button held, the console gone as another is pressed: one release
        // for both, both handed back, and the device silent after it.
        systemmouse::ConsoleGuard two;
        CHECK_EQ(two.step(true, left, 0, 0).reports.size(), size_t(1));
        s = two.step(false, left | right, 0, 0);
        CHECK(s.aside);
        CHECK(s.changed);
        CHECK_EQ(s.forgotten, static_cast<uint8_t>(left | right));
        CHECK_EQ(s.reports.size(), size_t(1));
        CHECK_EQ(s.reports[0][0], 0);
        s = two.step(false, right, 3, 3);
        CHECK(s.aside);
        CHECK(!s.changed);
        CHECK(s.reports.empty());
        CHECK_EQ(s.forgotten, right);
    }
}
