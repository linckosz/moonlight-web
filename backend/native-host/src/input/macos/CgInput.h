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

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

// Keyboard and mouse injection on macOS, through Quartz events (CGEventPost).
//
// ── The one route there is ──────────────────────────────────────────────────
//
// macOS has no uinput and no SendInput; a process that wants to move the
// pointer posts a Quartz event at the HID tap, and the window server treats it
// like the real thing. Thread-safe and microseconds per event, which is what
// IInputSink asks for.
//
// ── Two things macOS makes the sender's job ─────────────────────────────────
//
// 1. Modifiers are FLAGS, not keys. A Shift press is posted as a flags-changed
//    event carrying the new modifier state, and every key or mouse event that
//    follows has to carry the modifiers currently held in its own flags field —
//    the window server does not remember them for us. So the held modifiers
//    are tracked here and stamped onto every event.
// 2. Double clicks are declared, not detected. A second press within the
//    double-click interval has to say "click count 2" or it is two single
//    clicks; the OS will not infer it. Same for a drag: a move with a button
//    held is a "dragged" event, not a "moved" one.
//
// ── Permission ──────────────────────────────────────────────────────────────
//
// Posting events needs Accessibility (TCC). Without it the calls succeed and
// nothing happens — the failure mode that reads as a frozen session from the
// browser — so start() checks, asks the OS to prompt once, and says in the log
// exactly which System Settings switch is missing.

namespace mw::native::input {

class CgInput final : public IInputSink
{
public:
    CgInput() = default;
    ~CgInput() override;

    CgInput(const CgInput&) = delete;
    CgInput& operator=(const CgInput&) = delete;

    bool start(std::string& error) override;
    void stop() override;
    void inject(const InputEvent& event) override;
    /// The captured display's rectangle in POINTS (CoreGraphics' global space).
    void setDisplayRect(int left, int top, int right, int bottom) override;

private:
    void injectKey(const InputEvent& event, bool down);
    void injectText(const std::string& utf8);
    void injectMouseMove(int deltaX, int deltaY);
    void injectMousePosition(const InputEvent& event);
    void injectMouseButton(int button, bool down);
    void injectScroll(int amount, bool horizontal);
    void syncLockKeys(const InputEvent& event);
    void releaseAll();

    /// Post a pointer event at @p x, @p y (points), typed for the buttons
    /// currently held. The caller holds m_Mutex.
    void postPointer(double x, double y, int deltaX, int deltaY);
    void postButton(int button, bool down, double x, double y);
    void moveTo(double x, double y, int deltaX, int deltaY);

    /// Guards everything below: inject() comes from the libdatachannel thread
    /// while stop() runs on another.
    std::mutex m_Mutex;
    bool m_Started = false;

    std::set<int> m_HeldKeys;    ///< virtual keys held, for release at stop()
    std::set<int> m_HeldButtons; ///< browser button numbers held
    uint64_t m_Modifiers = 0;    ///< CGEventFlags of the modifiers held

    /// Where the pointer is, in points — our own account, moved by every
    /// event we post. Read from the OS once at start.
    double m_X = 0;
    double m_Y = 0;

    /// The display's rectangle in points; relative motion is clamped to it and
    /// absolute positions are mapped onto it.
    int m_Left = 0;
    int m_Top = 0;
    int m_Right = 0;
    int m_Bottom = 0;

    /// For the click count the OS wants declared: which button was last
    /// pressed, when, and how many times in a row.
    int m_LastButton = 0;
    int64_t m_LastPressUs = 0;
    int m_ClickCount = 0;

    std::atomic<uint32_t> m_SeenTypes{0};
    std::atomic<uint64_t> m_Injected{0};
};

} // namespace mw::native::input
