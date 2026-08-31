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

// Signature fixed by EVT_VIGEM_X360_NOTIFICATION: default calling convention,
// no user context — hence the routing table above.
VOID onX360Notification(PVIGEM_CLIENT, PVIGEM_TARGET target, UCHAR largeMotor, UCHAR smallMotor,
                        UCHAR)
{
    RumbleRoute route;
    {
        std::lock_guard<std::mutex> lock(g_RouteMutex);
        const auto it = g_Routes.find(target);
        if (it == g_Routes.end()) return;
        route = it->second;
    }
    if (!route.sink) return;

    // The protocol carries 16-bit motor amplitudes; XUSB reports 8-bit ones.
    // Scaling by 257 rather than 256 keeps 0xFF mapping to 0xFFFF, so full
    // vibration on the host is full vibration on the pad rather than 0.4 % shy.
    RumbleEvent event;
    event.controllerNumber = route.controllerNumber;
    event.lowFrequencyMotor = static_cast<uint16_t>(largeMotor * 257);
    event.highFrequencyMotor = static_cast<uint16_t>(smallMotor * 257);
    route.sink(event);
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

    std::lock_guard<std::mutex> lock(m_Mutex);
    for (PVIGEM_TARGET& pad : m_Pads) {
        if (!pad) continue;
        // Unregister BEFORE unplugging: a notification arriving after the
        // target is freed would look up a dangling handle.
        vigem_target_x360_unregister_notification(pad);
        {
            std::lock_guard<std::mutex> routes(g_RouteMutex);
            g_Routes.erase(pad);
        }
        vigem_target_remove(m_Client, pad);
        vigem_target_free(pad);
        pad = nullptr;
    }

    vigem_disconnect(m_Client);
    vigem_free(m_Client);
    m_Client = nullptr;
}

int VigemGamepad::connectedCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    int count = 0;
    for (PVIGEM_TARGET pad : m_Pads)
        if (pad) ++count;
    return count;
}

PVIGEM_TARGET VigemGamepad::ensurePad(int slot)
{
    if (!m_Client || slot < 0 || slot >= kMaxPads) return nullptr;
    if (m_Pads[slot]) return m_Pads[slot];

    PVIGEM_TARGET pad = vigem_target_x360_alloc();
    if (!pad) return nullptr;

    const VIGEM_ERROR added = vigem_target_add(m_Client, pad);
    if (!VIGEM_SUCCESS(added)) {
        log::warning(std::string("[native] could not plug in virtual pad ") + std::to_string(slot) +
                     ": " + describeError(added));
        vigem_target_free(pad);
        return nullptr;
    }

    // Register the route before the notification, so a vibration that arrives
    // immediately has somewhere to go.
    if (m_OnRumble) {
        {
            std::lock_guard<std::mutex> routes(g_RouteMutex);
            g_Routes[pad] = RumbleRoute{m_OnRumble, static_cast<uint8_t>(slot)};
        }
        vigem_target_x360_register_notification(m_Client, pad, &onX360Notification);
    }

    m_Pads[slot] = pad;
    log::info("[native] gamepad " + std::to_string(slot) + " plugged in (virtual Xbox 360 pad)");
    return pad;
}

void VigemGamepad::arrive(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    ensurePad(event.controllerNumber);
}

void VigemGamepad::update(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // A client that never announced its pad still gets one. Some browsers only
    // ever send state, and a silent stream of ignored input would be a far
    // worse failure than an extra pad appearing.
    PVIGEM_TARGET pad = ensurePad(event.controllerNumber);
    if (!pad) return;

    XUSB_REPORT report = {};

    // GameStream's button flags were modelled on XInput, so the low 16 bits map
    // across one for one — A_FLAG is 0x1000 and so is XUSB_GAMEPAD_A, and the
    // same holds for every button an Xbox 360 pad has. This is a real identity,
    // not an assumption: the constants are checked against each other below.
    static_assert(static_cast<int>(XUSB_GAMEPAD_A) == 0x1000, "XInput button layout changed");
    static_assert(static_cast<int>(XUSB_GAMEPAD_DPAD_UP) == 0x0001, "XInput button layout changed");
    static_assert(static_cast<int>(XUSB_GAMEPAD_GUIDE) == 0x0400, "XInput button layout changed");
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

    vigem_target_x360_update(m_Client, pad, report);
}

void VigemGamepad::remove(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    const int slot = event.controllerNumber;
    if (slot < 0 || slot >= kMaxPads || !m_Pads[slot]) return;

    PVIGEM_TARGET pad = m_Pads[slot];
    vigem_target_x360_unregister_notification(pad);
    {
        std::lock_guard<std::mutex> routes(g_RouteMutex);
        g_Routes.erase(pad);
    }
    vigem_target_remove(m_Client, pad);
    vigem_target_free(pad);
    m_Pads[slot] = nullptr;
    log::info("[native] gamepad " + std::to_string(slot) + " unplugged");
}

} // namespace mw::native::input
