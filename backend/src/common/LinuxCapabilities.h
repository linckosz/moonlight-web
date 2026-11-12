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

#include <cstddef>

/**
 * What this process does with the capabilities the Linux launcher hands it.
 *
 * Two, both for the native engine:
 *  - CAP_SYS_ADMIN: the screen is captured through KMS, and the kernel hands
 *    framebuffer handles only to a process holding it;
 *  - CAP_SYS_NICE: the stream's GPU work — the colour conversion today — runs
 *    above normal priority, which amdgpu, i915 and xe grant to nobody else. Under
 *    a game that saturates the GPU, that halves the conversion's wait (46 → 23 ms
 *    on a Radeon 780M, docs/bench-native-host.md §8o.1).
 *
 * The Linux packages deliver them through `moonlightweb-launch`
 * (backend/packaging/linux), which carries the file capabilities — see that file
 * for why they cannot sit on this binary itself. It hands them to the two
 * processes that capture, the native stream worker and the native probe, which
 * this binary starts through it (captureLauncher()), and to nothing else: the
 * app itself, however it is started, holds neither. A worker or a probe holds
 * each in all of permitted, effective and ambient at main(). That is more than
 * it should keep:
 *
 *  - EFFECTIVE is dropped. The native engine raises one into the effective set
 *    of ITS thread around the one call that checks it, and lowers it again
 *    (native-host/src/platform/linux/ScopedCapability.h). Sunshine's posture
 *    (`cap_sys_admin=p` on its binary, raised per call), reproduced.
 *  - AMBIENT and INHERITABLE are cleared. What the worker spawns — gamescope and
 *    the app it runs in it, systemd-inhibit — would otherwise inherit both.
 *  - PERMITTED keeps them: that is what lets the raise above happen at all.
 *
 * Every function here is a no-op off Linux, and a no-op on a Linux process
 * that was not handed the capabilities (a tree run by hand, the AppImage, the
 * Docker image): the probe then reports the capture as unavailable and says
 * why, and the conversion runs at normal priority and says that too.
 */
namespace mw {

/// Call first thing in main(), before any thread exists (capabilities are
/// per-thread; the main thread's are what every later thread inherits).
/// Returns a one-line description of what was done for the log, or an empty
/// string when the process holds neither capability and nothing changed.
QString confineCapabilities();

/// True when this process may capture through KMS — CAP_SYS_ADMIN in its
/// permitted set, whatever the effective set says right now.
bool hasCaptureCapability();

/// The package's moonlightweb-launch next to this binary, when it carries
/// CAP_SYS_ADMIN as a file capability: what the native stream worker and the
/// native probe are started through, `--stream-worker` / `--native-probe`
/// first. Read from the file itself every time, never assumed. Empty off
/// Linux and wherever no such launcher is (a tree run by hand, the AppImage):
/// those two are then started as this binary, as before, and hold nothing.
QString captureLauncher();

/// Whether the raw `security.capability` attribute of a file (struct
/// vfs_cap_data, revision 1, 2 or 3, little-endian) grants @p cap in its
/// permitted set. Pure, so the parser behind captureLauncher() is tested on
/// every platform.
inline bool fileCapabilityGrants(const unsigned char* data, std::size_t size, int cap)
{
    if (!data || size < 4 || cap < 0) return false;
    const auto le32 = [data](std::size_t at) {
        return static_cast<unsigned>(data[at]) | (static_cast<unsigned>(data[at + 1]) << 8) |
               (static_cast<unsigned>(data[at + 2]) << 16) |
               (static_cast<unsigned>(data[at + 3]) << 24);
    };
    std::size_t words = 0;
    std::size_t needed = 0;
    switch (le32(0) & 0xFF000000u) {
    case 0x01000000u:
        words = 1;
        needed = 12;
        break;
    case 0x02000000u:
        words = 2;
        needed = 20;
        break;
    case 0x03000000u:
        words = 2;
        needed = 24;
        break;
    default: return false;
    }
    const std::size_t word = static_cast<std::size_t>(cap) >> 5;
    if (size < needed || word >= words) return false;
    return ((le32(4 + word * 8) >> (static_cast<unsigned>(cap) & 31)) & 1u) != 0;
}

} // namespace mw
