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

#include "mw/native/InputEvent.h"

#include <array>
#include <functional>
#include <mutex>
#include <string>

// Forward-declared so the ViGEm headers — which drag in Windows.h and SetupAPI —
// stay out of every translation unit that merely holds one of these.
struct _VIGEM_CLIENT_T;
struct _VIGEM_TARGET_T;

namespace mw::native::input {

/// Virtual gamepads, through the ViGEmBus driver.
///
/// ── Why a driver at all ─────────────────────────────────────────────────────
///
/// Keyboard and mouse can be injected with a plain API call. A gamepad cannot:
/// there is no user-mode way to make Windows believe a controller is plugged in,
/// because games do not read "input events", they enumerate HID devices and poll
/// them. So a real device has to exist, and only a kernel driver can create one.
///
/// ── Two profiles, and what each costs ───────────────────────────────────────
///
/// ViGEmBus emulates an Xbox 360 pad or a DualShock 4, and this class presents
/// whichever the client says it is holding (InputEvent::controllerType).
///
/// X360 is the default and the fallback for everything unrecognised: XInput is
/// what Windows games overwhelmingly expect, and its layout maps exactly onto
/// what the browser's Gamepad API reports. The price is the buttons that layout
/// does not have — paddles, touchpad click, Share/Capture — which arrive from
/// the protocol and are dropped. That is the same limitation every XInput-based
/// host has.
///
/// DualShock 4 is presented for a PlayStation pad, so the prompts on screen
/// match the pad in the player's hands and the PS button behaves like one. Its
/// own price is the mirror image: a game that speaks only XInput does not see a
/// DS4 at all. Which is why the profile follows the CLIENT's pad rather than a
/// setting — the player holding a DualShock is the one who wants DualShock
/// prompts, and everyone else keeps the profile that works everywhere.
///
/// ── Absent driver is not an error ───────────────────────────────────────────
///
/// start() returning false means "no gamepad this session", never "no session".
/// The overwhelmingly common case is that ViGEmBus is simply not installed, and
/// a desktop stream does not need it.
class VigemGamepad
{
public:
    /// Which device this slot presents to Windows. Fixed when the pad is
    /// plugged in: a live device cannot change what it is, so a client that
    /// swaps pads on one slot has to remove and re-announce it.
    enum class Profile
    {
        X360,       ///< the default, and every unrecognised pad
        DualShock4, ///< a PlayStation pad on the client
    };

    /// The profile for a client's LI_CTYPE_* (InputEvent::controllerType).
    ///
    /// Nintendo is deliberately absent: a Switch Pro pad needs a HID descriptor
    /// ViGEmBus cannot produce (§2.2 of the design), so it gets the profile that
    /// works rather than a refusal.
    static Profile profileFor(uint8_t controllerType);

    /// Xbox 360 pads have four slots on the bus, and XInput exposes exactly
    /// four. A fifth controller has nowhere to go.
    static constexpr int kMaxPads = 4;

    /// Called when a game asks a pad to vibrate. Runs on a ViGEm thread, so it
    /// must not block — it exists to be forwarded to the browser.
    using RumbleSink = std::function<void(const RumbleEvent&)>;

    /// Whether the bus is there, asked of the driver and of nothing else.
    ///
    /// The driver's own answer is the only one worth having: a registry key and
    /// a file on disk both survive a partial uninstall, and both then claim a
    /// bus that vigem_connect() cannot open. Connects and disconnects
    /// immediately — no pad is created, so this is safe to call while a session
    /// holds pads of its own.
    ///
    /// False fills @p error with the driver's own reason, in words a user can
    /// act on.
    static bool busPresent(std::string& error);

    explicit VigemGamepad(RumbleSink onRumble);
    ~VigemGamepad();

    VigemGamepad(const VigemGamepad&) = delete;
    VigemGamepad& operator=(const VigemGamepad&) = delete;

    /// Connect to the driver. False means no driver, with @p error saying so in
    /// words a user can act on.
    bool start(std::string& error);

    /// Unplug every pad and disconnect. A pad left plugged in would outlive the
    /// session as a phantom controller in the Windows game controller list.
    void stop();

    void arrive(const InputEvent& event);
    void update(const InputEvent& event);

    /// Unplug one pad — the client says that controller is gone.
    ///
    /// Centres it first: a pad pulled out mid-input would leave the game holding
    /// its last report, with no device left to release it. Unknown or already
    /// unplugged slots are ignored, so a client repeating a removal costs
    /// nothing.
    void remove(const InputEvent& event);

    /// How many pads are plugged in right now.
    int connectedCount() const;

private:
    struct Pad
    {
        _VIGEM_TARGET_T* target = nullptr;
        Profile profile = Profile::X360;
    };

    /// Plug in slot @p slot if it is not already, and answer whether it is
    /// usable. Called by both arrive() and update(), because a client that never
    /// sent an arrival — an older browser, or one that lost the message — must
    /// still get a working pad rather than silence. Such a pad gets X360, which
    /// is the right guess when nothing was said.
    ///
    /// An already-plugged slot keeps the device it has, whatever @p profile
    /// says: see Profile.
    Pad* ensurePad(int slot, Profile profile);

    /// Unplug one slot. The caller holds m_Mutex.
    void unplug(int slot);

    RumbleSink m_OnRumble;
    _VIGEM_CLIENT_T* m_Client = nullptr;

    /// Guards the pad table against the session thread plugging while a ViGEm
    /// callback thread is delivering rumble for the same slot.
    mutable std::mutex m_Mutex;
    std::array<Pad, kMaxPads> m_Pads{};
};

} // namespace mw::native::input
