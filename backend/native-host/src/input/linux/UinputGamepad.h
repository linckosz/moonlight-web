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
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mw::native::input {

/// Virtual gamepads on Linux, through uinput.
///
/// ── Why this platform is the easy one ───────────────────────────────────────
///
/// Windows needs a signed kernel driver to make a gamepad exist (see
/// VigemGamepad). Linux ships that ability in the kernel: /dev/uinput creates
/// an input device with whatever buttons and axes you declare, and evdev hands
/// it to games exactly like a real pad. No driver to install, no signature to
/// buy, nothing to redistribute.
///
/// ── Two profiles, and what actually differs ─────────────────────────────────
///
/// On evdev, an Xbox pad and a DualShock report the SAME button and axis codes
/// — BTN_SOUTH is the bottom button on both. What differs is the device's
/// identity: its name and its USB vendor/product ids. That is what SDL matches
/// on to decide whether to draw "A" or "✕", so presenting the right identity is
/// the whole of the DualShock 4 profile here. There is no report to re-map, and
/// Ds4Mapping is deliberately not used on this platform.
///
/// ── Permissions, which is the real obstacle ─────────────────────────────────
///
/// /dev/uinput is root-only by default. The install ships a udev rule granting
/// the group that runs MoonlightWeb, so the server never needs to keep root
/// (§9). start() failing on EACCES means exactly that rule is missing, and it
/// says so — the caller degrades to "no gamepad this session" and carries on,
/// with keyboard and mouse untouched.
///
/// ── Not reached yet ─────────────────────────────────────────────────────────
///
/// Nothing constructs this today: the native engine has no Linux session to
/// route input from (no capture, no encoder). It is written, compiled and
/// shaped like its Windows counterpart so that the day that session lands, the
/// gamepad is not the part that has to be invented.
class UinputGamepad
{
public:
    /// The same ceiling as Windows, and for the same reason: the protocol caps
    /// controllers at four and §13 wants a hard bound on what a remote client
    /// can make us allocate. uinput itself would happily create more.
    static constexpr int kMaxPads = 4;

    using RumbleSink = std::function<void(const RumbleEvent&)>;

    /// Which device this slot presents. Fixed when the pad is created — a live
    /// evdev node cannot change its identity, so a client swapping pads on one
    /// slot has to remove and re-announce it.
    enum class Profile
    {
        X360,
        DualShock4,
    };

    /// The profile for a client's LI_CTYPE_* (InputEvent::controllerType).
    static Profile profileFor(uint8_t controllerType);

    /// Whether a virtual pad can be created here, asked of /dev/uinput itself.
    ///
    /// Opens and closes it without creating a device. The two answers worth
    /// telling apart both come back in @p error: the node is missing (the
    /// uinput module is not loaded) or it refuses us (the udev rule is not
    /// installed, or we are not in its group).
    static bool devicePresent(std::string& error);

    explicit UinputGamepad(RumbleSink onRumble);
    ~UinputGamepad();

    UinputGamepad(const UinputGamepad&) = delete;
    UinputGamepad& operator=(const UinputGamepad&) = delete;

    /// Check that uinput is usable. False means no gamepad this session, never
    /// no session — with @p error saying so in words a user can act on.
    bool start(std::string& error);

    /// Destroy every pad and stop the feedback thread. A device left behind
    /// would outlive the session in /dev/input, and the next game would find a
    /// controller nobody is holding.
    void stop();

    void arrive(const InputEvent& event);
    void update(const InputEvent& event);
    void remove(const InputEvent& event);

    int connectedCount() const;

private:
    struct Pad
    {
        int fd = -1;
        Profile profile = Profile::X360;
        /// Rumble effects the game uploaded, by the id the kernel gave them.
        /// A game uploads once and then plays that id, so the magnitudes have
        /// to be remembered from the upload to be of any use at play time.
        std::map<int16_t, RumbleEvent> effects;
    };

    /// Create slot @p slot if it is not already there. Returns null when uinput
    /// refused, which is not fatal — the session simply has no pad in that slot.
    Pad* ensurePad(int slot, Profile profile);

    /// Destroy one slot. The caller holds m_Mutex.
    void destroyPad(int slot);

    /// Poll every live pad for the kernel's force-feedback traffic, and turn it
    /// into RumbleEvents. Runs until m_Stopping is set and the wake-up fd fires.
    ///
    /// Its own thread because uinput delivers force feedback by making US answer
    /// an ioctl: the kernel asks the device it created to accept an effect, and
    /// nobody else is going to answer for it.
    void feedbackLoop();

    /// Handle one force-feedback message on @p pad. Called only by the feedback
    /// thread, under m_Mutex — which is why a vibration to report is APPENDED to
    /// @p pending rather than sent: the sink goes off to a relay and a
    /// DataChannel, and handing it our lock is how a rumble deadlocks against
    /// the session thread creating a pad.
    void handleFeedback(Pad& pad, uint16_t type, uint16_t code, int32_t value,
                        std::vector<RumbleEvent>& pending);

    RumbleSink m_OnRumble;
    bool m_Started = false;

    /// Guards the pad table between the session thread and the feedback thread.
    mutable std::mutex m_Mutex;
    std::array<Pad, kMaxPads> m_Pads{};

    std::thread m_Feedback;
    std::atomic<bool> m_Stopping{false};
    /// eventfd the destructor writes to, so the poll() in the feedback thread
    /// returns at once instead of waiting out its timeout.
    int m_WakeFd = -1;
};

} // namespace mw::native::input
