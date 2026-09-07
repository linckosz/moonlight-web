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

#include "UinputInput.h"

#include "../../core/Log.h"
#include "EvdevKeyMap.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mw::native::input {
namespace {

// ── The table is a kernel ABI, and this is where that claim is checked ──────
//
// EvdevKeyMap.h writes its codes as literals so it compiles and can be tested
// on a machine with no Linux headers — every machine this engine has been
// developed on. That is only safe because the codes are fixed ABI, so the claim
// is verified here rather than trusted: a sample spanning every group of the
// table is compared against <linux/input-event-codes.h> at compile time.
//
// A drift breaks the Linux build instead of typing the wrong letter on
// somebody's desktop.
static_assert(evdevKeyCode(0x41) == KEY_A, "letters");
static_assert(evdevKeyCode(0x5A) == KEY_Z, "letters");
static_assert(evdevKeyCode(0x30) == KEY_0, "digit row wraps at zero");
static_assert(evdevKeyCode(0x31) == KEY_1, "digit row");
static_assert(evdevKeyCode(0x39) == KEY_9, "digit row");
static_assert(evdevKeyCode(0x70) == KEY_F1, "function row");
static_assert(evdevKeyCode(0x79) == KEY_F10, "function row");
static_assert(evdevKeyCode(0x7B) == KEY_F12, "function row beyond F10");
static_assert(evdevKeyCode(0x60) == KEY_KP0, "numpad is laid out bottom-up");
static_assert(evdevKeyCode(0x69) == KEY_KP9, "numpad");
static_assert(evdevKeyCode(0x0D) == KEY_ENTER, "typewriter keys");
static_assert(evdevKeyCode(0x20) == KEY_SPACE, "typewriter keys");
static_assert(evdevKeyCode(0xDC) == KEY_BACKSLASH, "OEM keys");
static_assert(evdevKeyCode(0xA0) == KEY_LEFTSHIFT, "modifiers, left and right");
static_assert(evdevKeyCode(0xA5) == KEY_RIGHTALT, "modifiers, left and right");
static_assert(evdevKeyCode(0x5B) == KEY_LEFTMETA, "modifiers");
static_assert(evdevKeyCode(0x25) == KEY_LEFT, "arrows");
static_assert(evdevKeyCode(0x28) == KEY_DOWN, "arrows");
static_assert(evdevKeyCode(0x2E) == KEY_DELETE, "the block above the arrows");
static_assert(evdevKeyCode(0x90) == KEY_NUMLOCK, "lock keys");
static_assert(evdevKeyCode(0xE2) == KEY_102ND, "the 102-key extra");

/// The browser's button numbering (1 left, 2 middle, 3 right, 4/5 side) to
/// evdev's. Not contiguous and not in the same order, which is exactly the kind
/// of thing that silently swaps middle-click and right-click.
uint16_t buttonCode(int button)
{
    switch (button) {
    case 1: return BTN_LEFT;
    case 2: return BTN_MIDDLE;
    case 3: return BTN_RIGHT;
    case 4: return BTN_SIDE;
    case 5: return BTN_EXTRA;
    default: return 0;
    }
}

/// The absolute pointer's coordinate space. 0..32767 is the tablet convention,
/// and using a fixed range rather than the display's pixels means a resolution
/// change does not need the device recreated.
constexpr int32_t kAbsMax = 32767;

std::string errnoText()
{
    char buffer[128] = {};
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    return strerror_r(errno, buffer, sizeof(buffer));
#else
    ::strerror_r(errno, buffer, sizeof(buffer));
    return buffer;
#endif
}

} // namespace

UinputInput::~UinputInput()
{
    stop();
}

bool UinputInput::createDevice(bool absolute, int& fd, std::string& error)
{
    fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open /dev/uinput (" + errnoText() +
                "). The packaged install ships a udev rule that grants it; a host running from a "
                "tarball needs one, or to run as root.";
        return false;
    }

    if (absolute) {
        ::ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_X);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_Y);
        // Buttons on the absolute device too: a compositor that follows the
        // pointer here must be able to see the click that goes with it.
        ::ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (uint16_t b : {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA})
            ::ioctl(fd, UI_SET_KEYBIT, b);
    } else {
        ::ioctl(fd, UI_SET_EVBIT, EV_KEY);
        // Every key the map can produce. Announcing the whole KEY_ range would
        // also work and would have the device claim keys it can never send.
        for (int vk = 0; vk <= 0xFF; ++vk) {
            const uint16_t code = evdevKeyCode(vk);
            if (code != 0) ::ioctl(fd, UI_SET_KEYBIT, code);
        }
        for (uint16_t b : {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA})
            ::ioctl(fd, UI_SET_KEYBIT, b);

        ::ioctl(fd, UI_SET_EVBIT, EV_REL);
        ::ioctl(fd, UI_SET_RELBIT, REL_X);
        ::ioctl(fd, UI_SET_RELBIT, REL_Y);
        ::ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
        ::ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
    }

    uinput_setup setup = {};
    setup.id.bustype = BUS_VIRTUAL;
    // Not a real vendor/product pair: nothing should recognise these as
    // hardware, and a made-up VID/PID is how a device announces that.
    setup.id.vendor = 0x1D6B;  // Linux Foundation
    setup.id.product = 0x0104; // arbitrary, distinct from the gamepad's
    setup.id.version = 1;
    std::snprintf(setup.name, sizeof(setup.name), "%s",
                  absolute ? "MoonlightWeb Pointer" : "MoonlightWeb Keyboard");

    if (absolute) {
        uinput_abs_setup abs = {};
        abs.absinfo.minimum = 0;
        abs.absinfo.maximum = kAbsMax;
        abs.code = ABS_X;
        ::ioctl(fd, UI_ABS_SETUP, &abs);
        abs.code = ABS_Y;
        ::ioctl(fd, UI_ABS_SETUP, &abs);
    }

    if (::ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ::ioctl(fd, UI_DEV_CREATE) < 0) {
        error = "the kernel refused the virtual input device (" + errnoText() + ")";
        ::close(fd);
        fd = -1;
        return false;
    }
    return true;
}

bool UinputInput::start(std::string& error)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Keyboard >= 0) return true;

    if (!createDevice(false, m_Keyboard, error)) return false;
    if (!createDevice(true, m_Absolute, error)) {
        ::ioctl(m_Keyboard, UI_DEV_DESTROY);
        ::close(m_Keyboard);
        m_Keyboard = -1;
        return false;
    }

    // Best effort, and never a reason to fail: without it relative motion is
    // exactly what it was before, which is right on the single-screen hosts
    // that are the common case.
    m_X11.open();

    log::info("[native] input: uinput keyboard and pointer created");
    return true;
}

void UinputInput::stop()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Lift everything still held BEFORE the devices go: a key released after
    // its device is destroyed is a key that stays down for the user.
    if (m_Keyboard >= 0) {
        for (uint16_t code : m_HeldKeys)
            emit(m_Keyboard, EV_KEY, code, 0);
        for (uint16_t code : m_HeldButtons)
            emit(m_Keyboard, EV_KEY, code, 0);
        if (!m_HeldKeys.empty() || !m_HeldButtons.empty()) emitSyn(m_Keyboard);
    }
    m_HeldKeys.clear();
    m_HeldButtons.clear();

    for (int* fd : {&m_Keyboard, &m_Absolute}) {
        if (*fd < 0) continue;
        ::ioctl(*fd, UI_DEV_DESTROY);
        ::close(*fd);
        *fd = -1;
    }

    m_X11.close();
    m_WarpLogged = false;
}

void UinputInput::emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    if (fd < 0) return;
    input_event event = {};
    event.type = type;
    event.code = code;
    event.value = value;
    // Timestamps are the kernel's to fill; writing our own is ignored.
    const ssize_t written = ::write(fd, &event, sizeof(event));
    (void)written; // a full pipe drops the event, which is not worth a log line
}

void UinputInput::emitSyn(int fd)
{
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

void UinputInput::injectKey(const InputEvent& event, bool down)
{
    const uint16_t code = evdevKeyCode(event.keyCode);
    if (code == 0) return;
    emit(m_Keyboard, EV_KEY, code, down ? 1 : 0);
    emitSyn(m_Keyboard);
    if (down)
        m_HeldKeys.insert(code);
    else
        m_HeldKeys.erase(code);
}

void UinputInput::injectButton(const InputEvent& event, bool down)
{
    const uint16_t code = buttonCode(event.button);
    if (code == 0) return;
    emit(m_Keyboard, EV_KEY, code, down ? 1 : 0);
    emitSyn(m_Keyboard);
    if (down)
        m_HeldButtons.insert(code);
    else
        m_HeldButtons.erase(code);
}

void UinputInput::setDisplayRect(int left, int top, int right, int bottom)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_RectLeft = left;
    m_RectTop = top;
    m_RectWidth = right - left;
    m_RectHeight = bottom - top;
}

void UinputInput::bringPointerOntoDisplay()
{
    // The caller holds m_Mutex, which is also what serialises Xlib here.
    if (!m_X11.isOpen() || m_RectWidth <= 0 || m_RectHeight <= 0) return;

    int x = 0;
    int y = 0;
    if (!m_X11.position(x, y)) return;

    // The rectangle comes from KMS and the position from X. They agree on any
    // ordinary desktop, where the X root is laid out over the same CRTCs — and
    // where they do not agree, the worst case is a pointer parked on an edge
    // rather than a pointer lost off-screen.
    int warpX = 0;
    int warpY = 0;
    if (!clampIntoRect(m_RectLeft, m_RectTop, m_RectLeft + m_RectWidth, m_RectTop + m_RectHeight, x,
                       y, warpX, warpY))
        return;

    if (!m_X11.warp(warpX, warpY)) return;

    const std::string where = "pointer was at " + std::to_string(x) + "," + std::to_string(y) +
                              ", off the captured display — brought back to " +
                              std::to_string(warpX) + "," + std::to_string(warpY);
    // Once at info, then quietly: see m_WarpLogged.
    if (!m_WarpLogged) {
        m_WarpLogged = true;
        log::info("[native] input: " + where);
    } else {
        log::debug("[native] input: " + where);
    }
}

void UinputInput::inject(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Keyboard < 0) return;

    using Type = InputEvent::Type;
    switch (event.type) {
    case Type::KeyDown: injectKey(event, true); break;
    case Type::KeyUp: injectKey(event, false); break;
    case Type::MouseButtonDown: injectButton(event, true); break;
    case Type::MouseButtonUp: injectButton(event, false); break;

    case Type::MouseMoveRelative:
        // A delta moves the pointer from wherever it IS, and on a multi-monitor
        // host that may be a screen the viewer is not looking at — where they
        // can swipe forever without the cursor ever coming into view. So it is
        // brought home first, and the delta applied from there.
        //
        // Only when a delta arrives, never at session start: opening a stream
        // should not touch the host's mouse until the viewer moves it.
        if (event.deltaX != 0 || event.deltaY != 0) bringPointerOntoDisplay();
        if (event.deltaX != 0) emit(m_Keyboard, EV_REL, REL_X, event.deltaX);
        if (event.deltaY != 0) emit(m_Keyboard, EV_REL, REL_Y, event.deltaY);
        if (event.deltaX != 0 || event.deltaY != 0) emitSyn(m_Keyboard);
        break;

    case Type::MouseMoveAbsolute: {
        if (m_Absolute < 0) break;
        // The client sends a position inside a reference surface of its own
        // size; the device's space is a fixed 0..32767. Scaling straight
        // between them means a resolution change costs nothing here — which is
        // why the rectangle is only consulted as a fallback.
        const int refW = event.referenceWidth > 0 ? event.referenceWidth : m_RectWidth;
        const int refH = event.referenceHeight > 0 ? event.referenceHeight : m_RectHeight;
        if (refW <= 0 || refH <= 0) break;
        int32_t x = static_cast<int32_t>((static_cast<int64_t>(event.positionX) * kAbsMax) / refW);
        int32_t y = static_cast<int32_t>((static_cast<int64_t>(event.positionY) * kAbsMax) / refH);
        x = x < 0 ? 0 : (x > kAbsMax ? kAbsMax : x);
        y = y < 0 ? 0 : (y > kAbsMax ? kAbsMax : y);
        emit(m_Absolute, EV_ABS, ABS_X, x);
        emit(m_Absolute, EV_ABS, ABS_Y, y);
        emitSyn(m_Absolute);
        break;
    }

    // The wire carries 120-unit notches, as Windows does. evdev's REL_WHEEL is
    // in whole notches, so the amount is divided rather than forwarded — sent
    // raw, one notch of the wheel would scroll a hundred and twenty lines.
    case Type::MouseScrollVertical:
        if (event.scrollAmount != 0) {
            emit(m_Keyboard, EV_REL, REL_WHEEL, event.scrollAmount / 120);
            emitSyn(m_Keyboard);
        }
        break;
    case Type::MouseScrollHorizontal:
        if (event.scrollAmount != 0) {
            emit(m_Keyboard, EV_REL, REL_HWHEEL, event.scrollAmount / 120);
            emitSyn(m_Keyboard);
        }
        break;

    // Not this sink's business: the gamepad has its own device
    // (UinputGamepad), and text and lock-key sync need a layout-aware path that
    // does not exist on Linux yet. Ignored rather than approximated.
    case Type::Utf8Text:
    case Type::LockKeySync:
    case Type::ControllerArrival:
    case Type::ControllerState:
    case Type::ControllerRemoval: break;
    }
}

} // namespace mw::native::input
