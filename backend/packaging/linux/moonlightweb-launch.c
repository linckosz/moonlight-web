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

/*
 * moonlightweb-launch — hands CAP_SYS_ADMIN to MoonlightWeb, then becomes it.
 *
 * Why a launcher exists at all. Capturing the screen through KMS needs
 * CAP_SYS_ADMIN (the kernel hands framebuffer handles to nobody else — see
 * native-host/src/capture/linux/KmsCapture.h), and the obvious way to grant it
 * is a file capability on the binary, as Sunshine's package does. That does not
 * work on OUR binary: it lives under /opt/moonlightweb with the bundled Qt
 * reached through a $ORIGIN rpath, and glibc runs any binary that gains a
 * capability at exec in "secure mode", where $ORIGIN is refused outside the
 * system library directories. Measured on Ubuntu 22.04 (05/09/2026):
 *
 *     ./bin/app: error while loading shared libraries: libanswer.so: cannot
 *     open shared object file
 *
 * with nothing changed but `setcap cap_sys_admin+p bin/app`. The package would
 * install an app that cannot start.
 *
 * So the file capability sits on this program instead — a few hundred bytes
 * that link only libc, which lives in a trusted directory and loads fine in
 * secure mode. It moves the capability into its AMBIENT set and execs the real
 * binary next to it. An exec that gains nothing beyond what the parent already
 * held is not a secure exec, so MoonlightWeb starts normally, rpath and all,
 * with CAP_SYS_ADMIN in its permitted set. What it does from there is its
 * business (common/LinuxCapabilities.cpp: keep it permitted, run without it
 * effective, hand it to the native stream worker and to nothing else).
 *
 * Without the file capability — a tree run by hand, the AppImage (FUSE mounts
 * are nosuid, which also disables file capabilities) — the hand-over fails and
 * this simply execs the app, which then reports that the capture is
 * unavailable and why. Nothing is printed here: an unprivileged start is a
 * supported state, not an error.
 *
 * The package sets `cap_sys_admin+p` (permitted only, like Sunshine): the
 * ambient raise needs the capability permitted and inheritable, not effective,
 * and a launcher that never holds it effective has nothing to misuse.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/capability.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Linux 4.3; older kernel headers may lack the names, the numbers are ABI. */
#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_RAISE 2
#endif

static const char kTarget[] = "MoonlightWeb";

/* Move @p cap from this process's permitted set into its ambient set, so the
 * exec below carries it. Raw syscalls: no libcap, nothing to bundle. Returns 0
 * on success, -1 (errno set) when the capability is not held. */
static int hand_over(int cap)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
    memset(&hdr, 0, sizeof hdr);
    memset(data, 0, sizeof data);
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    if (syscall(SYS_capget, &hdr, data) != 0) return -1;

    const unsigned idx = (unsigned)cap >> 5;
    const unsigned bit = 1u << ((unsigned)cap & 31);
    if (!(data[idx].permitted & bit)) {
        errno = EPERM;
        return -1;
    }
    /* PR_CAP_AMBIENT_RAISE wants the capability in the inheritable set too,
     * and a process may always add to that set what it holds permitted. */
    data[idx].inheritable |= bit;
    if (syscall(SYS_capset, &hdr, data) != 0) return -1;
    return prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0);
}

int main(int argc, char** argv)
{
    (void)argc;

    /* The real binary is the one next to us, never one found on PATH: a
     * capability must not follow a name into a directory someone else writes. */
    char path[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n < 0) {
        perror("moonlightweb-launch: readlink /proc/self/exe");
        return 127;
    }
    path[n] = '\0';
    char* slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) + 1 + sizeof kTarget > sizeof path) {
        fprintf(stderr, "moonlightweb-launch: cannot locate %s next to %s\n", kTarget, path);
        return 127;
    }
    memcpy(slash + 1, kTarget, sizeof kTarget);

    (void)hand_over(CAP_SYS_ADMIN);

    argv[0] = path;
    execv(path, argv);
    fprintf(stderr, "moonlightweb-launch: cannot exec %s: %s\n", path, strerror(errno));
    return 127;
}
