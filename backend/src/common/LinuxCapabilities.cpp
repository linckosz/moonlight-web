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
#include <sys/xattr.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QFile>

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

    bool permitted(int cap) const { return data[index(cap)].permitted & bit(cap); }

    static unsigned index(int cap) { return static_cast<unsigned>(cap) >> 5; }
    static unsigned bit(int cap) { return 1u << (static_cast<unsigned>(cap) & 31); }
};

/// The capabilities the launcher hands over, and what each one is for.
struct Handed
{
    int cap;
    const char* note;
};
constexpr Handed kHanded[] = {
    {CAP_SYS_ADMIN, "CAP_SYS_ADMIN held (permitted): the screen can be captured through KMS"},
    {CAP_SYS_NICE, "CAP_SYS_NICE held (permitted): the stream's GPU work can run above normal "
                   "priority"},
};

} // namespace

namespace mw {

QString confineCapabilities()
{
    Caps caps;
    if (!caps.get()) return {};

    QString note;
    bool wasEffective = false;
    bool any = false;
    for (const Handed& handed : kHanded) {
        if (!caps.permitted(handed.cap)) continue;
        const unsigned i = Caps::index(handed.cap), b = Caps::bit(handed.cap);
        any = true;
        wasEffective = wasEffective || (caps.data[i].effective & b);
        caps.data[i].effective &= ~b;
        caps.data[i].inheritable &= ~b;
        if (!note.isEmpty()) note += QStringLiteral("; ");
        note += QString::fromLatin1(handed.note);
    }
    if (!any) return {};

    const bool set = caps.set();
    // Lower only ours: a systemd unit may have granted CAP_NET_BIND_SERVICE the
    // same way, and that one is meant to stay.
    bool lowered = true;
    for (const Handed& handed : kHanded) {
        if (caps.permitted(handed.cap))
            lowered = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER, handed.cap, 0, 0) == 0 && lowered;
    }

    if (set && wasEffective) note += QStringLiteral("; dropped from the effective set");
    if (lowered) note += QStringLiteral("; withheld from every child process");
    if (!set) note += QStringLiteral("; capset failed, the effective set is unchanged");
    return note;
}

QString workerLauncher()
{
    const QString path =
        QCoreApplication::applicationDirPath() + QStringLiteral("/moonlightweb-launch");
    const QByteArray file = QFile::encodeName(path);
    if (::access(file.constData(), X_OK) != 0) return {};
    unsigned char raw[64];
    const ssize_t size = ::getxattr(file.constData(), "security.capability", raw, sizeof raw);
    if (size <= 0) return {};
    return fileCapabilityGrants(raw, static_cast<std::size_t>(size), CAP_SYS_NICE) ? path
                                                                                   : QString();
}

} // namespace mw

#else // not Linux

namespace mw {

QString confineCapabilities()
{
    return {};
}

QString workerLauncher()
{
    return {};
}

} // namespace mw

#endif
