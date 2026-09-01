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

#include "../../capture/windows/IWindowsCapture.h"
#include "../IInputSink.h"
#include "VigemGamepad.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>

namespace mw::native::input {

/// Keyboard and mouse injection through SendInput.
///
/// ── Scancodes, not virtual keys ─────────────────────────────────────────────
///
/// Games read the keyboard through Raw Input or DirectInput, and both of those
/// see SCANCODES. A virtual-key injection is invisible to them: the menu of a
/// game would respond while the game itself would not, which is the confusing
/// half-working failure this avoids. So a press is mapped VK → scancode and
/// injected with KEYEVENTF_SCANCODE.
///
/// The exception is the browser's NON_NORMALIZED flag, which says "this key had
/// no US-layout equivalent, take the code as it is". There, injecting a
/// scancode would resolve through the wrong layout, so the virtual key goes in
/// directly and the host's active layout decides.
///
/// ── Not usable from session 0 ───────────────────────────────────────────────
///
/// SendInput targets the calling thread's desktop. A Windows service in session
/// 0 has no interactive desktop, so injection silently does nothing there. That
/// is a property of the OS, not of this class — the console-session launcher is
/// what solves it, and the probe already refuses a session with no interactive
/// desktop.
class Win32Input final : public IInputSink
{
public:
    /// @param displayRect  where the captured display sits on the desktop.
    ///                     Absolute pointer positions from the browser are
    ///                     relative to THAT display, not to the whole desktop:
    ///                     the client is looking at one screen and its top-left
    ///                     is not the desktop's.
    /// @param onRumble     where a game's vibration request goes. Optional: with
    ///                     no sink the pad still works, it just does not shake.
    explicit Win32Input(const capture::DesktopRect& displayRect,
                        VigemGamepad::RumbleSink onRumble = {});
    ~Win32Input() override;

    bool start(std::string& error) override;
    void stop() override;
    void inject(const InputEvent& event) override;
    void setDisplayRect(int left, int top, int right, int bottom) override;

private:
    void injectKey(const InputEvent& event, bool down);
    void injectText(const std::string& utf8);
    void injectMouseMove(int deltaX, int deltaY);
    void injectMousePosition(const InputEvent& event);
    /// Applies the press if — and only if — it changes the button's state.
    void injectMouseButton(int button, bool down);
    /// Sends it regardless. For the release-everything path, which has already
    /// decided what is down.
    void sendMouseButton(int button, bool down);
    void injectScroll(int amount, bool horizontal);
    void syncLockKeys(const InputEvent& event);

    /// Release everything still recorded as held. Called by stop().
    void releaseAll();

    /// Not const: the display can be re-resolved under a running session (see
    /// setDisplayRect). Written and read under the caller's own serialisation.
    capture::DesktopRect m_DisplayRect;

    /// Null when ViGEmBus is absent — the overwhelmingly common case, and not a
    /// failure. Keyboard and mouse are unaffected.
    std::unique_ptr<VigemGamepad> m_Gamepad;
    VigemGamepad::RumbleSink m_OnRumble;

    /// What we have pressed and not yet released, so a session that ends
    /// mid-keypress does not leave the host holding a key down forever. Guarded
    /// because inject() runs on the network thread while stop() runs on the
    /// session's — the only shared state in this class, and small enough that a
    /// plain mutex costs nothing next to the SendInput call it wraps.
    std::mutex m_HeldMutex;
    std::set<int> m_HeldKeys;    ///< virtual-key codes
    std::set<int> m_HeldButtons; ///< browser button numbers, 1..5

    /// Events applied since start(), and which InputEvent::Type values have
    /// been seen at least once (one bit each). Only ever read for the log
    /// lines, hence relaxed ordering — diagnostics, not synchronisation.
    std::atomic<uint64_t> m_Injected{0};
    std::atomic<uint32_t> m_SeenTypes{0};

    bool m_Started = false;
};

} // namespace mw::native::input
