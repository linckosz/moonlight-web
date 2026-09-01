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

#include "VigemGamepad.h"

#include "../../core/Log.h"
#include "../Ds4Mapping.h"

// Windows.h first, and not incidentally: the ViGEm headers are written against
// the Win32 type names (USHORT, FORCEINLINE, …) and do not include it
// themselves, so on their own they fail to parse.
#include <windows.h>

#include <ViGEmClient.h>

#include <map>

namespace mw::native::input {
namespace {

/// Who to tell when a pad vibrates.
///
/// ViGEm's notification callback is a bare function pointer with no user
/// context, so the only way back to the object that owns the pad is a lookup on
/// the target handle. Kept file-scope and small: at most four entries per
/// session, and sessions are one per worker process.
struct RumbleRoute
{
    VigemGamepad::RumbleSink sink;
    uint8_t controllerNumber;
};

std::mutex g_RouteMutex;
std::map<PVIGEM_TARGET, RumbleRoute> g_Routes;

/// Forward one vibration request to whoever owns @p target.
///
/// The protocol carries 16-bit motor amplitudes; both drivers report 8-bit
/// ones. Scaling by 257 rather than 256 keeps 0xFF mapping to 0xFFFF, so full
/// vibration on the host is full vibration on the pad rather than 0.4 % shy.
void routeRumble(PVIGEM_TARGET target, UCHAR largeMotor, UCHAR smallMotor)
{
    RumbleRoute route;
    {
        std::lock_guard<std::mutex> lock(g_RouteMutex);
        const auto it = g_Routes.find(target);
        if (it == g_Routes.end()) return;
        route = it->second;
    }
    if (!route.sink) return;

    RumbleEvent event;
    event.controllerNumber = route.controllerNumber;
    event.lowFrequencyMotor = static_cast<uint16_t>(largeMotor * 257);
    event.highFrequencyMotor = static_cast<uint16_t>(smallMotor * 257);
    route.sink(event);
}

// Signatures fixed by EVT_VIGEM_X360_NOTIFICATION / EVT_VIGEM_DS4_NOTIFICATION:
// default calling convention, no user context — hence the routing table above.
// They differ only in their last argument, which is an LED number on one and a
// lightbar colour on the other; neither travels back to a browser, which has no
// light to set.
VOID onX360Notification(PVIGEM_CLIENT, PVIGEM_TARGET target, UCHAR largeMotor, UCHAR smallMotor,
                        UCHAR)
{
    routeRumble(target, largeMotor, smallMotor);
}

VOID onDs4Notification(PVIGEM_CLIENT, PVIGEM_TARGET target, UCHAR largeMotor, UCHAR smallMotor,
                       DS4_LIGHTBAR_COLOR)
{
    routeRumble(target, largeMotor, smallMotor);
}

const char* describeError(VIGEM_ERROR error)
{
    switch (error) {
    case VIGEM_ERROR_BUS_NOT_FOUND: return "the ViGEmBus driver is not installed on this machine";
    case VIGEM_ERROR_BUS_ACCESS_FAILED: return "the ViGEmBus driver refused access";
    case VIGEM_ERROR_BUS_VERSION_MISMATCH:
        return "the installed ViGEmBus driver is too old for this build";
    case VIGEM_ERROR_NO_FREE_SLOT: return "the virtual bus has no free slot";
    case VIGEM_ERROR_TARGET_UNINITIALIZED: return "the virtual pad was not initialised";
    case VIGEM_ERROR_ALREADY_CONNECTED: return "that virtual pad is already plugged in";
    default: return "the virtual gamepad bus refused the request";
    }
}

} // namespace

bool VigemGamepad::busPresent(std::string& error)
{
    PVIGEM_CLIENT client = vigem_alloc();
    if (!client) {
        error = "out of memory allocating the virtual gamepad client";
        return false;
    }

    const VIGEM_ERROR result = vigem_connect(client);
    if (!VIGEM_SUCCESS(result)) {
        error = describeError(result);
        vigem_free(client);
        return false;
    }

    // Nothing was plugged in, so there is nothing to unplug — disconnect drops
    // the handle on the bus and leaves any session's pads untouched.
    vigem_disconnect(client);
    vigem_free(client);
    error.clear();
    return true;
}

VigemGamepad::VigemGamepad(RumbleSink onRumble)
    : m_OnRumble(std::move(onRumble))
{}

VigemGamepad::~VigemGamepad()
{
    stop();
}

bool VigemGamepad::start(std::string& error)
{
    if (m_Client) return true;

    m_Client = vigem_alloc();
    if (!m_Client) {
        error = "out of memory allocating the virtual gamepad client";
        return false;
    }

    const VIGEM_ERROR result = vigem_connect(m_Client);
    if (!VIGEM_SUCCESS(result)) {
        // Overwhelmingly this is "the driver is not installed", which is not a
        // fault — most desktop streams never want a gamepad. The caller logs it
        // and carries on without one.
        error = describeError(result);
        vigem_free(m_Client);
        m_Client = nullptr;
        return false;
    }
    return true;
}

void VigemGamepad::stop()
{
    if (!m_Client) return;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (int slot = 0; slot < kMaxPads; ++slot) unplug(slot);
    }

    vigem_disconnect(m_Client);
    vigem_free(m_Client);
    m_Client = nullptr;
}

int VigemGamepad::connectedCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    int count = 0;
    for (const Pad& pad : m_Pads)
        if (pad.target) ++count;
    return count;
}

VigemGamepad::Profile VigemGamepad::profileFor(uint8_t controllerType)
{
    if (controllerType == InputEvent::kControllerPlayStation) return Profile::DualShock4;
    return Profile::X360;
}

void VigemGamepad::unplug(int slot)
{
    if (slot < 0 || slot >= kMaxPads) return;
    Pad& pad = m_Pads[slot];
    if (!pad.target) return;

    // Unregister BEFORE unplugging: a notification arriving after the target is
    // freed would look up a dangling handle.
    if (pad.profile == Profile::DualShock4)
        vigem_target_ds4_unregister_notification(pad.target);
    else
        vigem_target_x360_unregister_notification(pad.target);
    {
        std::lock_guard<std::mutex> routes(g_RouteMutex);
        g_Routes.erase(pad.target);
    }
    vigem_target_remove(m_Client, pad.target);
    vigem_target_free(pad.target);
    pad.target = nullptr;
    pad.profile = Profile::X360;
}

VigemGamepad::Pad* VigemGamepad::ensurePad(int slot, Profile profile)
{
    if (!m_Client || slot < 0 || slot >= kMaxPads) return nullptr;
    if (m_Pads[slot].target) return &m_Pads[slot];

    PVIGEM_TARGET target = profile == Profile::DualShock4 ? vigem_target_ds4_alloc()
                                                          : vigem_target_x360_alloc();
    if (!target) return nullptr;

    const VIGEM_ERROR added = vigem_target_add(m_Client, target);
    if (!VIGEM_SUCCESS(added)) {
        log::warning(std::string("[native] could not plug in virtual pad ") + std::to_string(slot) +
                     ": " + describeError(added));
        vigem_target_free(target);
        return nullptr;
    }

    // Register the route before the notification, so a vibration that arrives
    // immediately has somewhere to go.
    if (m_OnRumble) {
        {
            std::lock_guard<std::mutex> routes(g_RouteMutex);
            g_Routes[target] = RumbleRoute{m_OnRumble, static_cast<uint8_t>(slot)};
        }
        if (profile == Profile::DualShock4)
            vigem_target_ds4_register_notification(m_Client, target, &onDs4Notification);
        else
            vigem_target_x360_register_notification(m_Client, target, &onX360Notification);
    }

    m_Pads[slot].target = target;
    m_Pads[slot].profile = profile;
    log::info("[native] gamepad " + std::to_string(slot) + " plugged in (virtual " +
              (profile == Profile::DualShock4 ? "DualShock 4" : "Xbox 360") + " pad)");
    return &m_Pads[slot];
}

void VigemGamepad::arrive(const InputEvent& event)
{
    const int slot = event.controllerNumber;
    if (slot < 0 || slot >= kMaxPads) return;

    std::lock_guard<std::mutex> lock(m_Mutex);

    const Profile wanted = profileFor(event.controllerType);
    // A slot re-announced with a different kind of pad: replace the device
    // rather than keep the old one. Windows cannot change what a live device
    // IS, so a player who put down an Xbox pad and picked up a DualShock would
    // otherwise keep playing on a virtual X360 until the session ended.
    if (m_Pads[slot].target && m_Pads[slot].profile != wanted) {
        log::info("[native] gamepad " + std::to_string(slot) +
                  " changed type — replacing the virtual pad");
        unplug(slot);
    }

    ensurePad(slot, wanted);
}

void VigemGamepad::update(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // A client that never announced its pad still gets one. Some browsers only
    // ever send state, and a silent stream of ignored input would be a far
    // worse failure than an extra pad appearing.
    // A client that never announced its pad gets an X360 one: nothing said what
    // it is, and that is the profile that works everywhere.
    Pad* pad = ensurePad(event.controllerNumber, Profile::X360);
    if (!pad) return;

    // These two mappings are checked against the drivers' own headers rather
    // than trusted. The XInput one is an identity — GameStream modelled its
    // flags on XInput, so A_FLAG is 0x1000 and so is XUSB_GAMEPAD_A — and the
    // DualShock one is written out in Ds4Mapping.cpp against the constants
    // asserted here. A driver header that ever renumbers itself fails the build
    // instead of silently mis-mapping every button.
    static_assert(static_cast<int>(XUSB_GAMEPAD_A) == 0x1000, "XInput button layout changed");
    static_assert(static_cast<int>(XUSB_GAMEPAD_DPAD_UP) == 0x0001, "XInput button layout changed");
    static_assert(static_cast<int>(XUSB_GAMEPAD_GUIDE) == 0x0400, "XInput button layout changed");
    static_assert(static_cast<int>(DS4_BUTTON_CROSS) == ds4::kCross, "DS4 button layout changed");
    static_assert(static_cast<int>(DS4_BUTTON_THUMB_RIGHT) == ds4::kThumbRight,
                  "DS4 button layout changed");
    static_assert(static_cast<int>(DS4_BUTTON_DPAD_NONE) == ds4::kDpadNone,
                  "DS4 D-pad encoding changed");
    static_assert(static_cast<int>(DS4_SPECIAL_BUTTON_PS) == ds4::kSpecialPs,
                  "DS4 special button layout changed");

    if (pad->profile == Profile::DualShock4) {
        const Ds4Report mapped =
            toDs4(static_cast<uint32_t>(event.buttonFlags), event.leftTrigger, event.rightTrigger,
                  event.leftStickX, event.leftStickY, event.rightStickX, event.rightStickY);

        DS4_REPORT report = {};
        report.bThumbLX = mapped.thumbLX;
        report.bThumbLY = mapped.thumbLY;
        report.bThumbRX = mapped.thumbRX;
        report.bThumbRY = mapped.thumbRY;
        report.wButtons = mapped.buttons;
        report.bSpecial = mapped.special;
        report.bTriggerL = mapped.triggerL;
        report.bTriggerR = mapped.triggerR;
        vigem_target_ds4_update(m_Client, pad->target, report);
        return;
    }

    XUSB_REPORT report = {};
    report.wButtons = static_cast<USHORT>(event.buttonFlags & 0xFFFF);

    // The bits above 16 — paddles, touchpad click, the Share button — are
    // Sunshine extensions with no place on an Xbox 360 pad. Dropped knowingly:
    // there is no button to press.

    report.bLeftTrigger = event.leftTrigger;
    report.bRightTrigger = event.rightTrigger;
    report.sThumbLX = event.leftStickX;
    report.sThumbLY = event.leftStickY;
    report.sThumbRX = event.rightStickX;
    report.sThumbRY = event.rightStickY;

    vigem_target_x360_update(m_Client, pad->target, report);
}

void VigemGamepad::remove(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    const int slot = event.controllerNumber;
    if (slot < 0 || slot >= kMaxPads || !m_Pads[slot].target) return;

    // Centre everything before unplugging. A pad pulled out mid-input would
    // otherwise leave the game holding its last report — a trigger down, a stick
    // shoved — with no device left to ever release it. Best-effort: the unplug
    // is what actually matters.
    if (m_Pads[slot].profile == Profile::DualShock4) {
        DS4_REPORT neutral = {};
        DS4_REPORT_INIT(&neutral);
        vigem_target_ds4_update(m_Client, m_Pads[slot].target, neutral);
    } else {
        const XUSB_REPORT neutral = {};
        vigem_target_x360_update(m_Client, m_Pads[slot].target, neutral);
    }

    unplug(slot);
    log::info("[native] gamepad " + std::to_string(slot) + " unplugged");
}

} // namespace mw::native::input
