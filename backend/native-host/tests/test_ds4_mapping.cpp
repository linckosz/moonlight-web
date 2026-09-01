/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 *
 * The DualShock 4 profile's mapping: every button, the D-pad's eight
 * directions, the triggers, and the sticks at minimum, centre and maximum.
 *
 * This is §16's "without hardware" list, and it is worth having in full. A pad
 * mapping fails in ways nobody notices until someone is playing: an inverted Y
 * axis feels like a game setting, a missing D-pad diagonal feels like a bad
 * pad, and a stick that centres one step off drifts in every menu. None of that
 * needs a driver to catch — it is arithmetic, and it is checked here.
 */
#include "native_test_framework.h"

#include "input/Ds4Mapping.h"

using namespace mw::native::input;

namespace {

/// Buttons only — the common case in these checks.
Ds4Report press(uint32_t flags)
{
    return toDs4(flags, 0, 0, 0, 0, 0, 0);
}

} // namespace

void run_ds4_mapping_tests()
{
    SECTION("DS4 — a resting pad");

    {
        const Ds4Report idle = press(0);
        // Centre is exact on all four axes, not "near enough": a resting stick
        // one step off centre is a slow drift in every menu in every game.
        CHECK_EQ(static_cast<int>(idle.thumbLX), 0x80);
        CHECK_EQ(static_cast<int>(idle.thumbLY), 0x80);
        CHECK_EQ(static_cast<int>(idle.thumbRX), 0x80);
        CHECK_EQ(static_cast<int>(idle.thumbRY), 0x80);
        CHECK_EQ(static_cast<int>(idle.triggerL), 0);
        CHECK_EQ(static_cast<int>(idle.triggerR), 0);
        CHECK_EQ(static_cast<int>(idle.special), 0);
        // No button pressed still means a D-pad reading "centred", which is a
        // value (0x8) and not zero.
        CHECK_EQ(static_cast<int>(idle.buttons), static_cast<int>(ds4::kDpadNone));
    }

    SECTION("DS4 — face buttons follow POSITION, not letters");

    {
        // A is the bottom button on both pads, so it is Cross — not the button
        // that happens to share a letter. Getting this wrong puts every on-screen
        // prompt one button away from where the player's thumb goes.
        CHECK((press(pad::kA).buttons & ds4::kCross) != 0);
        CHECK((press(pad::kB).buttons & ds4::kCircle) != 0);
        CHECK((press(pad::kX).buttons & ds4::kSquare) != 0);
        CHECK((press(pad::kY).buttons & ds4::kTriangle) != 0);
        // X is the LEFT button, and on a DualShock the left button is Square.
        // The X-shaped button (Cross) is the bottom one, which is A.
        CHECK((press(pad::kX).buttons & ds4::kCross) == 0);
    }

    SECTION("DS4 — shoulders, thumbs, Share/Options and PS");

    {
        CHECK((press(pad::kLeftShoulder).buttons & ds4::kShoulderLeft) != 0);
        CHECK((press(pad::kRightShoulder).buttons & ds4::kShoulderRight) != 0);
        CHECK((press(pad::kLeftThumb).buttons & ds4::kThumbLeft) != 0);
        CHECK((press(pad::kRightThumb).buttons & ds4::kThumbRight) != 0);
        CHECK((press(pad::kBack).buttons & ds4::kShare) != 0);
        CHECK((press(pad::kStart).buttons & ds4::kOptions) != 0);

        // The PS button is not in the button word at all — it has its own byte,
        // which is exactly the kind of thing a mapping quietly drops.
        const Ds4Report guide = press(pad::kGuide);
        CHECK_EQ(static_cast<int>(guide.special), static_cast<int>(ds4::kSpecialPs));
        CHECK_EQ(static_cast<int>(guide.buttons), static_cast<int>(ds4::kDpadNone));

        // The touchpad click is never claimed: the browser has no touchpad
        // button to send, so reporting one would be inventing input (§6.1).
        CHECK((press(0xFFFFFFFFu).special & ds4::kSpecialTouchpad) == 0);
    }

    SECTION("DS4 — the D-pad is a hat, with all eight directions");

    {
        const auto dpad = [](uint32_t flags) {
            return static_cast<int>(press(flags).buttons & ds4::kDpadMask);
        };

        CHECK_EQ(dpad(pad::kDpadUp), static_cast<int>(ds4::kDpadNorth));
        CHECK_EQ(dpad(pad::kDpadRight), static_cast<int>(ds4::kDpadEast));
        CHECK_EQ(dpad(pad::kDpadDown), static_cast<int>(ds4::kDpadSouth));
        CHECK_EQ(dpad(pad::kDpadLeft), static_cast<int>(ds4::kDpadWest));

        CHECK_EQ(dpad(pad::kDpadUp | pad::kDpadRight), static_cast<int>(ds4::kDpadNorthEast));
        CHECK_EQ(dpad(pad::kDpadDown | pad::kDpadRight), static_cast<int>(ds4::kDpadSouthEast));
        CHECK_EQ(dpad(pad::kDpadDown | pad::kDpadLeft), static_cast<int>(ds4::kDpadSouthWest));
        CHECK_EQ(dpad(pad::kDpadUp | pad::kDpadLeft), static_cast<int>(ds4::kDpadNorthWest));

        // Opposites cancel. A hat cannot say "up and down at once", and picking
        // one of the two would walk the player in a direction they did not ask
        // for — which is worse than standing still.
        CHECK_EQ(dpad(pad::kDpadUp | pad::kDpadDown), static_cast<int>(ds4::kDpadNone));
        CHECK_EQ(dpad(pad::kDpadLeft | pad::kDpadRight), static_cast<int>(ds4::kDpadNone));
        CHECK_EQ(dpad(pad::kDpadUp | pad::kDpadDown | pad::kDpadLeft),
                 static_cast<int>(ds4::kDpadWest));

        // The hat must not leak into the buttons above it, nor they into it.
        const Ds4Report both = press(pad::kDpadUp | pad::kA);
        CHECK_EQ(static_cast<int>(both.buttons & ds4::kDpadMask),
                 static_cast<int>(ds4::kDpadNorth));
        CHECK((both.buttons & ds4::kCross) != 0);
    }

    SECTION("DS4 — triggers: analog through, digital on a threshold");

    {
        // The analog value always travels in full, whatever the digital bit does.
        CHECK_EQ(static_cast<int>(toDs4(0, 5, 200, 0, 0, 0, 0).triggerL), 5);
        CHECK_EQ(static_cast<int>(toDs4(0, 5, 200, 0, 0, 0, 0).triggerR), 200);

        // Below the threshold the digital bit stays clear — a pad resting a few
        // counts above zero must not read as "L2 held" for a whole session.
        CHECK((toDs4(0, kTriggerThreshold - 1, 0, 0, 0, 0, 0).buttons & ds4::kTriggerLeft) == 0);
        CHECK((toDs4(0, kTriggerThreshold, 0, 0, 0, 0, 0).buttons & ds4::kTriggerLeft) != 0);
        CHECK((toDs4(0, 0, 255, 0, 0, 0, 0).buttons & ds4::kTriggerRight) != 0);
    }

    SECTION("DS4 — sticks: ends exact, centre exact, Y inverted");

    {
        // X axes agree with the protocol's direction: left is 0, right is 255.
        CHECK_EQ(static_cast<int>(toStickByte(-32768, false)), 0);
        CHECK_EQ(static_cast<int>(toStickByte(0, false)), 0x80);
        CHECK_EQ(static_cast<int>(toStickByte(32767, false)), 255);

        // Y axes are inverted: the protocol has up positive (XInput's
        // convention), a DualShock has up at 0. Both ends land exactly, and
        // −32768 does NOT wrap around — negating it in 16 bits does not fit,
        // which is where this conversion classically goes wrong.
        CHECK_EQ(static_cast<int>(toStickByte(32767, true)), 0);
        CHECK_EQ(static_cast<int>(toStickByte(0, true)), 0x80);
        CHECK_EQ(static_cast<int>(toStickByte(-32767, true)), 255);
        CHECK_EQ(static_cast<int>(toStickByte(-32768, true)), 255);

        // And through the whole report, on both sticks.
        const Ds4Report pushed = toDs4(0, 0, 0, -32768, 32767, 32767, -32768);
        CHECK_EQ(static_cast<int>(pushed.thumbLX), 0);   // stick left
        CHECK_EQ(static_cast<int>(pushed.thumbLY), 0);   // stick up
        CHECK_EQ(static_cast<int>(pushed.thumbRX), 255); // stick right
        CHECK_EQ(static_cast<int>(pushed.thumbRY), 255); // stick down
    }

    SECTION("DS4 — monotonic, with no step backwards");

    {
        // Walking the axis end to end must never go down: a single reversed step
        // is a stick that stutters at one position, which is invisible in a
        // spot-check and maddening to play on.
        int previous = -1;
        for (int32_t v = -32768; v <= 32767; v += 251) {
            const int current = toStickByte(static_cast<int16_t>(v), false);
            CHECK(current >= previous);
            previous = current;
        }
        CHECK_EQ(previous, 255);
    }
}
