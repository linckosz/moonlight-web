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

/// Virtual Xbox 360 pads, through the ViGEmBus driver.
///
/// ── Why a driver at all ─────────────────────────────────────────────────────
///
/// Keyboard and mouse can be injected with a plain API call. A gamepad cannot:
/// there is no user-mode way to make Windows believe a controller is plugged in,
/// because games do not read "input events", they enumerate HID devices and poll
/// them. So a real device has to exist, and only a kernel driver can create one.
///
/// ── Xbox 360, and what that costs ───────────────────────────────────────────
///
/// ViGEmBus can emulate an X360 pad or a DualShock 4. The X360 one is chosen
/// because XInput is what Windows games overwhelmingly expect, and because its
/// layout maps exactly onto what the browser's Gamepad API reports.
///
/// The price is the buttons that layout does not have. Paddles, touchpad click
/// and the Share/Capture button arrive from the protocol and are dropped here —
/// an X360 pad has nowhere to put them. That is a real limitation, not an
/// oversight, and it is the same one every XInput-based host has.
///
/// ── Absent driver is not an error ───────────────────────────────────────────
///
/// start() returning false means "no gamepad this session", never "no session".
/// The overwhelmingly common case is that ViGEmBus is simply not installed, and
/// a desktop stream does not need it.
class VigemGamepad
{
public:
    /// Xbox 360 pads have four slots on the bus, and XInput exposes exactly
    /// four. A fifth controller has nowhere to go.
    static constexpr int kMaxPads = 4;

    /// Called when a game asks a pad to vibrate. Runs on a ViGEm thread, so it
    /// must not block — it exists to be forwarded to the browser.
    using RumbleSink = std::function<void(const RumbleEvent&)>;

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
    void remove(const InputEvent& event);

    /// How many pads are plugged in right now.
    int connectedCount() const;

private:
    /// Plug in slot @p slot if it is not already, and answer whether it is
    /// usable. Called by both arrive() and update(), because a client that never
    /// sent an arrival — an older browser, or one that lost the message — must
    /// still get a working pad rather than silence.
    _VIGEM_TARGET_T* ensurePad(int slot);

    RumbleSink m_OnRumble;
    _VIGEM_CLIENT_T* m_Client = nullptr;

    /// Guards the pad table against the session thread plugging while a ViGEm
    /// callback thread is delivering rumble for the same slot.
    mutable std::mutex m_Mutex;
    std::array<_VIGEM_TARGET_T*, kMaxPads> m_Pads{};
};

} // namespace mw::native::input
