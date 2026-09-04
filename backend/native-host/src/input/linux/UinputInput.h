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

#include "../IInputSink.h"

#include <mutex>
#include <set>
#include <string>

// Keyboard and mouse injection on Linux, through uinput.
//
// ── Why uinput and not the display server ───────────────────────────────────
//
// XTEST works on X11 and nowhere else; Wayland has no injection protocol at all
// and is not going to grow one. uinput creates a REAL input device in the
// kernel, below both — so the same code drives X11, every Wayland compositor,
// and a bare console, and the compositor applies the user's own keyboard layout
// on top exactly as it would for a physical keyboard.
//
// It is also what the virtual gamepad already uses (UinputGamepad), so a Linux
// host ends up presenting three ordinary devices rather than three special
// cases.
//
// ── Two devices, not one ────────────────────────────────────────────────────
//
// A device that reports both relative and absolute pointer axes is read
// differently by different compositors — some pick one and ignore the other,
// some treat it as a tablet. Splitting them is what every other project doing
// this settled on, and it costs one more file descriptor.
//
// ── Permissions ─────────────────────────────────────────────────────────────
//
// /dev/uinput is root-only by default. The Linux packages already ship the udev
// rule and the modules-load.d entry that the gamepad needed (§8.3), so a host
// installed from them has access; one running from a tarball does not, and
// start() says so plainly rather than failing at the first keystroke.

namespace mw::native::input {

class UinputInput final : public IInputSink
{
public:
    UinputInput() = default;
    ~UinputInput() override;

    UinputInput(const UinputInput&) = delete;
    UinputInput& operator=(const UinputInput&) = delete;

    bool start(std::string& error) override;
    void stop() override;
    void inject(const InputEvent& event) override;
    void setDisplayRect(int left, int top, int right, int bottom) override;

private:
    /// Create one uinput device. @p absolute selects the pointer that reports
    /// positions rather than deltas.
    bool createDevice(bool absolute, int& fd, std::string& error);

    /// One event, then the SYN_REPORT that makes the kernel deliver the batch.
    /// Without the SYN nothing is seen at all — the single most common way to
    /// get a uinput device that exists and does nothing.
    void emit(int fd, uint16_t type, uint16_t code, int32_t value);
    void emitSyn(int fd);

    void injectKey(const InputEvent& event, bool down);
    void injectButton(const InputEvent& event, bool down);

    int m_Keyboard = -1; ///< keys + relative pointer + wheel
    int m_Absolute = -1; ///< absolute pointer only

    /// Everything currently held down, so stop() can lift it. A session that
    /// ends mid-keypress would otherwise leave the host with a key stuck and
    /// nobody left to release it — the same reason Win32Input keeps its own.
    std::set<uint16_t> m_HeldKeys;
    std::set<uint16_t> m_HeldButtons;

    /// Guards the descriptors and the held sets. inject() is documented as
    /// callable from any thread, and on the native path it really is called
    /// from the libdatachannel thread while stop() runs on another.
    std::mutex m_Mutex;

    /// The captured display's rectangle, for absolute positions. The absolute
    /// device is created with a fixed 0..32767 range — the convention a tablet
    /// uses — so this only scales the incoming reference surface onto it.
    int m_RectWidth = 0;
    int m_RectHeight = 0;
};

} // namespace mw::native::input
