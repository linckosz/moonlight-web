/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include <QString>

/**
 * What this process does with CAP_SYS_ADMIN on Linux.
 *
 * The native engine captures the screen through KMS, and the kernel hands
 * framebuffer handles only to a process with CAP_SYS_ADMIN. The Linux packages
 * deliver it through `moonlightweb-launch` (backend/packaging/linux), which
 * carries the file capability and execs this binary with the capability in its
 * AMBIENT set — see that file for why the capability cannot sit on this binary
 * itself. So at main() this process may hold CAP_SYS_ADMIN in all of permitted,
 * effective and ambient. That is more than it should keep:
 *
 *  - EFFECTIVE is dropped. The HTTP server, the signalling, the relays run
 *    without it; native-host/src/capture/linux/KmsCapture.cpp raises it back
 *    into the effective set of ITS thread around the one ioctl that needs it,
 *    and lowers it again. Sunshine's posture (`cap_sys_admin=p` on its binary,
 *    raised per call), reproduced.
 *  - AMBIENT is lowered. Everything this process spawns — xdg-open and the
 *    browser behind it, the gio helper, a package installer — would otherwise
 *    inherit CAP_SYS_ADMIN silently. Only the native stream worker gets it,
 *    explicitly, through raiseCaptureCapabilityForChild().
 *  - PERMITTED and INHERITABLE keep it: that is what lets the two raises above
 *    happen at all.
 *
 * Every function here is a no-op off Linux, and a no-op on a Linux process
 * that was not handed the capability (a tree run by hand, the AppImage, the
 * Docker image): the probe then reports the capture as unavailable and says
 * why.
 */
namespace mw {

/// Call first thing in main(), before any thread exists (capabilities are
/// per-thread; the main thread's are what every later thread inherits).
/// Returns a one-line description of what was done for the log, or an empty
/// string when the process holds no CAP_SYS_ADMIN and nothing changed.
QString confineCapabilities();

/// True when this process may capture through KMS — CAP_SYS_ADMIN in its
/// permitted set, whatever the effective set says right now.
bool hasCaptureCapability();

/// For QProcess::setChildProcessModifier on the NATIVE stream worker: runs in
/// the forked child before exec, and puts CAP_SYS_ADMIN back into the ambient
/// set so the worker starts with it. Raw syscalls only — the child is between
/// fork and exec, where nothing may allocate or lock. Does nothing when the
/// parent was never handed the capability.
void raiseCaptureCapabilityForChild();

} // namespace mw
