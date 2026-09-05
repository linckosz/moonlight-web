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

#include "LinuxCapabilities.h"

#if defined(__linux__)
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstring>

// Linux 4.3; older kernel headers may lack the names, the numbers are ABI.
#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_RAISE 2
#define PR_CAP_AMBIENT_LOWER 3
#endif

namespace {

// Raw capget/capset on the CALLING THREAD. No libcap: nothing to link, nothing
// to bundle, and libcap's capset would sync every thread, which is not wanted.
struct Caps
{
    __user_cap_header_struct header{};
    __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3]{};

    bool get()
    {
        std::memset(&header, 0, sizeof header);
        std::memset(data, 0, sizeof data);
        header.version = _LINUX_CAPABILITY_VERSION_3;
        return syscall(SYS_capget, &header, data) == 0;
    }
    bool set() { return syscall(SYS_capset, &header, data) == 0; }
};

constexpr unsigned kIndex = CAP_SYS_ADMIN >> 5;
constexpr unsigned kBit = 1u << (CAP_SYS_ADMIN & 31);

} // namespace

namespace mw {

QString confineCapabilities()
{
    Caps caps;
    if (!caps.get() || !(caps.data[kIndex].permitted & kBit)) return {};

    const bool wasEffective = caps.data[kIndex].effective & kBit;
    caps.data[kIndex].effective &= ~kBit;
    caps.data[kIndex].inheritable |= kBit;
    const bool set = caps.set();
    // Lower only ours: a systemd unit may have granted CAP_NET_BIND_SERVICE the
    // same way, and that one is meant to stay.
    const bool lowered = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER, CAP_SYS_ADMIN, 0, 0) == 0;

    QString note =
        QStringLiteral("CAP_SYS_ADMIN held (permitted): the screen can be captured through KMS");
    if (set && wasEffective) note += QStringLiteral("; dropped from the effective set");
    if (lowered) note += QStringLiteral("; withheld from child processes except the native worker");
    if (!set) note += QStringLiteral("; capset failed, the effective set is unchanged");
    return note;
}

bool hasCaptureCapability()
{
    Caps caps;
    return caps.get() && (caps.data[kIndex].permitted & kBit);
}

void raiseCaptureCapabilityForChild()
{
    Caps caps;
    if (!caps.get() || !(caps.data[kIndex].permitted & kBit)) return;
    caps.data[kIndex].inheritable |= kBit;
    if (!caps.set()) return;
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SYS_ADMIN, 0, 0);
}

} // namespace mw

#else // not Linux

namespace mw {

QString confineCapabilities()
{
    return {};
}

bool hasCaptureCapability()
{
    return false;
}

void raiseCaptureCapabilityForChild() {}

} // namespace mw

#endif
