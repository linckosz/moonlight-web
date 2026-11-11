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

#include <functional>
#include <string>

namespace mw::native::platform {

/**
 * The desktop that currently has the input — and how a thread joins it.
 *
 * One window station, `winsta0`, holds several desktops. `Default` is the one
 * with the taskbar and the windows; `Winlogon` is the SECURE desktop, where the
 * UAC prompt, the lock screen and the Ctrl+Alt+Suppr screen live; a screen
 * saver gets a third. Windows switches between them, and both halves of a
 * stream are per-thread-desktop:
 *
 *  - Desktop Duplication duplicates the desktop the CALLING thread is attached
 *    to. Across a switch it answers DXGI_ERROR_ACCESS_LOST, and a thread still
 *    sitting on `Default` cannot open a new duplication until the switch is
 *    undone — which is why a locked screen is a wait rather than a picture.
 *  - SendInput injects into the desktop the CALLING thread is attached to.
 *    From `Default`, nothing reaches a UAC prompt.
 *
 * So the two threads that matter follow the switch instead: `attachThread()`
 * before opening a duplication, and again before injecting.
 *
 * None of this works below SYSTEM — OpenInputDesktop on `Winlogon` is refused
 * outright — which is what the launcher service exists for. Every entry point
 * here is therefore a no-op that reports false when `runningAsSystem()` is
 * false, and the engine behaves exactly as it did before.
 */

/// Whether this process runs as LocalSystem, the one case in which the secure
/// desktop can be reached at all. Decided once.
bool runningAsSystem();

/// Attach the CALLING thread to the desktop that has the input right now.
///
/// Cheap and idempotent: when the thread is already on it, nothing happens but
/// the name comparison. The previous desktop handle, if this function opened
/// it, is closed only after the switch has succeeded — a thread is never left
/// pointing at a closed desktop.
///
/// @param name  filled with the desktop's name ("Default", "Winlogon", …) when
///              not null, whether or not the thread moved.
/// @return true when the thread is on the input desktop. False means it stayed
///         where it was: not SYSTEM, the desktop refused to open, or the thread
///         owns a window or a hook (SetThreadDesktop will not move such a
///         thread — which is precisely why input has a thread of its own).
bool attachThread(std::string* name = nullptr);

/// The input desktop's name without moving anything, or empty when it cannot be
/// read. For deciding whether a re-attach is due.
std::string inputDesktopName();

/// Press Ctrl+Alt+Suppr on the host.
///
/// The one key combination no injected input can ever produce: Windows reserves
/// it in the kernel, and SendInput cannot forge it by design. `sas.dll`'s
/// SendSAS is the documented way in, and Windows honours it from a service in
/// session 0 only, under a machine policy (SoftwareSASGeneration) that lets
/// services press it: from the worker, a SYSTEM process in the console
/// session, the call opens nothing and says nothing (29/09/2026). So the press
/// goes through the sender the host application set — its launcher service —
/// and SendSAS is called here only when none was. Asked by a SYSTEM worker
/// only, the one that can follow the screen that opens. @p error says why
/// when it returns false.
bool sendSecureAttention(std::string& error);

/// NativeHost::setSecureAttentionSender.
void setSecureAttentionSender(std::function<bool(std::string& error)> sender);

struct SignInLook;

/// Enter sign-in screen mode (platform/SignInScreen.h) for the rest of the
/// process. One way: nothing leaves it. The worker calls it from its command
/// line, before anything else runs (NativeHost::enterSignInScreenOnly).
///
/// From then on attachThread() moves a thread to `Winlogon` only, and never
/// to another desktop: a thread that cannot follow stays where it is, and the
/// checks below end the stream.
void enterSignInScreenOnly();

/// Whether this process is in sign-in screen mode.
bool signInScreenOnly();

/// In sign-in screen mode: whether a picture (@p forPicture, captured from the
/// CALLING thread's desktop) or an input may go through now. Latching: once a
/// check has failed, every later one fails with the first reason, so the
/// stream cannot carry on past it. @p why gets the reason when it returns
/// false; @p look, when not null, what was seen (the bench's log). True, and
/// nothing read, outside the mode.
bool signInScreenStillShown(bool forPicture, std::string* why, SignInLook* look = nullptr);

} // namespace mw::native::platform
