/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>
#include <string>

namespace mw::native {

/// One input event to inject into the OS.
///
/// A flat tagged struct rather than JSON on purpose: parsing already happened
/// once, in the relay, and re-serializing here would put back exactly the cost
/// this engine exists to remove. The field names mirror what the existing
/// relays already decode from the browser, so the adapter is a direct
/// translation with no reinterpretation.
///
/// **Threading**: the engine injects on the caller's thread. SendInput,
/// uinput's write() and CGEventPost are all thread-safe and cost microseconds,
/// so there is no queue and no thread hop between the DataChannel callback and
/// the OS (§8). Do not add one.
struct InputEvent
{
    enum class Type
    {
        KeyDown,
        KeyUp,
        Utf8Text, ///< virtual/soft keyboard — a whole string at once
        MouseMoveRelative,
        MouseMoveAbsolute,
        MouseButtonDown,
        MouseButtonUp,
        MouseScrollVertical,
        MouseScrollHorizontal,
        ControllerArrival,
        ControllerState,
        ControllerRemoval,
        LockKeySync, ///< align host NumLock/CapsLock/ScrollLock with the client
    };

    Type type = Type::KeyDown;

    // ── Keyboard ─────────────────────────────────────────────────────────────
    /// Windows virtual-key code, as the browser already sends it. The platform
    /// layer maps it to whatever the local OS wants.
    int16_t keyCode = 0;
    /// Modifier bitmask, matching the browser's existing encoding.
    uint8_t modifiers = 0;
    uint8_t keyFlags = 0;
    /// UTF-8 payload for Type::Utf8Text.
    std::string text;

    // ── Mouse ────────────────────────────────────────────────────────────────
    int16_t deltaX = 0;
    int16_t deltaY = 0;
    /// For MouseMoveAbsolute: position within a reference surface of
    /// referenceWidth × referenceHeight, which the platform layer rescales to
    /// the captured display.
    int16_t positionX = 0;
    int16_t positionY = 0;
    int16_t referenceWidth = 0;
    int16_t referenceHeight = 0;
    /// 1 = left, 2 = middle, 3 = right, 4 = X1, 5 = X2 — the browser's numbering.
    int button = 0;
    /// Wheel amount in 120-unit notches, already quantized upstream.
    int16_t scrollAmount = 0;

    // ── Controller ───────────────────────────────────────────────────────────
    uint8_t controllerNumber = 0;
    uint16_t activeGamepadMask = 0;

    /// What the client says it is holding, as GameStream's LI_CTYPE_*: 0
    /// unknown, 1 Xbox, 2 PlayStation, 3 Nintendo. It decides which virtual pad
    /// the engine presents, and it is a HINT — the browser works it out from a
    /// device name string that varies by OS, driver and connection, so an
    /// unrecognised pad arrives as 0 and gets the profile that works everywhere.
    uint8_t controllerType = 0;
    static constexpr uint8_t kControllerUnknown = 0;
    static constexpr uint8_t kControllerXbox = 1;
    static constexpr uint8_t kControllerPlayStation = 2;
    static constexpr uint8_t kControllerNintendo = 3;

    bool hasRumble = false;
    int32_t buttonFlags = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t leftStickX = 0;
    int16_t leftStickY = 0;
    int16_t rightStickX = 0;
    int16_t rightStickY = 0;

    // ── Lock keys ────────────────────────────────────────────────────────────
    bool numLock = false;
    bool capsLock = false;
    bool scrollLock = false;

    /// True when the client marked this press as one to keep held through a
    /// brief link stall (movement keys in gaming mode). The engine itself does
    /// nothing with it — the shared InputWatchdog above this module does — but
    /// it rides along so the two paths stay symmetrical.
    bool hold = false;

    /// True when this press comes from the client's held-state heartbeat rather
    /// than from the user doing something.
    ///
    /// The distinction is not cosmetic. That heartbeat beats every 100 ms while
    /// anything is held, and its job is to restore what the host's watchdog
    /// released during a stall. Applied blindly it does the opposite: pressing a
    /// key that is ALREADY down is an extra character, and pressing a mouse
    /// button that is already down is a second click — which is how a plain tap
    /// on a trackpad arrived on the host as a double-click. A resync press is
    /// therefore applied only when it actually changes something.
    bool resync = false;
};

/// Rumble the host asked to send back to the client's gamepad. Travels the
/// opposite way from InputEvent, over the same DataChannel.
struct RumbleEvent
{
    uint8_t controllerNumber = 0;
    uint16_t lowFrequencyMotor = 0;
    uint16_t highFrequencyMotor = 0;
};

} // namespace mw::native
