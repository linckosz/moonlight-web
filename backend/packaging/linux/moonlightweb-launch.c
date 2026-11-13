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
 * moonlightweb-launch — hands CAP_SYS_NICE to MoonlightWeb's native stream
 * worker, then becomes it.
 *
 * Why a launcher exists at all. The stream's GPU work — the colour conversion
 * today — runs above normal priority only in a process that holds
 * CAP_SYS_NICE: amdgpu, i915 and xe create a context above normal priority for
 * nobody else, and under a game that saturates the GPU that halves the
 * conversion's wait (docs/bench-native-host.md §8o.1). The obvious way to grant
 * it is a file capability on the binary, as Sunshine's package does. That does
 * not work on OUR binary: it lives under /opt/moonlightweb with the bundled Qt
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
 * with CAP_SYS_NICE in its permitted set. What it does from there is its
 * business (common/LinuxCapabilities.cpp: keep it permitted, run without it
 * effective, raise it on one thread for the one call that creates the context).
 *
 * Only the native stream worker is handed it, which the app starts through
 * this launcher with --stream-worker as the first argument. It starts from a
 * known environment, in /, rather than the caller's (moonlightweb-launch-env.h).
 * The app itself, however it is started, is handed nothing: it execs here with
 * its environment as it came — less what glibc strips from any program that
 * carries a file capability, as it always has — and holds no capability at all.
 *
 * Without the file capability — a tree run by hand, the AppImage (FUSE mounts
 * are nosuid, which also disables file capabilities) — the hand-over fails and
 * this simply execs what it was asked to, as it was asked; the conversion then
 * runs at normal priority and says so. Nothing is printed here: an
 * unprivileged start is a supported state, not an error.
 *
 * Capturing the screen through KMS needs CAP_SYS_ADMIN, and that one is handed
 * to no MoonlightWeb process: moonlightweb-kms, next to this program, carries
 * it and hands the capture each framebuffer it asks for
 * (native-host/src/capture/linux/KmsHelperClient.h).
 *
 * Descriptors. This program opens none and closes none, and both execs below
 * keep whatever its caller left open across its own. The app counts on it: it
 * starts its native worker with the UDP sockets its streams share
 * (streaming/IceMuxHandles.h), and they reach the worker through here. Whatever
 * is added to this program keeps that: nothing of the caller's is closed, and
 * nothing opened here is left open.
 *
 * The package sets `cap_sys_nice+p` (permitted only, like Sunshine): the
 * ambient raise needs a capability permitted and inheritable, not effective,
 * and a launcher that never holds it effective has nothing to misuse.
 */

#define _GNU_SOURCE
#include "moonlightweb-launch-env.h"

#include <errno.h>
#include <limits.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

extern char** environ;

/* Linux 4.3; older kernel headers may lack the names, the numbers are ABI. */
#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_RAISE 2
#endif

/* The binary this launcher execs, next to it: MoonlightWeb, or MoonlightWebDev
 * for the DEV edition (set by CMake from MW_EDITION). */
#ifndef MW_LAUNCH_TARGET
#define MW_LAUNCH_TARGET "MoonlightWeb"
#endif
static const char kTarget[] = MW_LAUNCH_TARGET;

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

    const int handed =
        mw_launch_mode(argc, argv) == MW_LAUNCH_WORKER && hand_over(CAP_SYS_NICE) == 0;

    argv[0] = path;
    if (!handed) {
        execv(path, argv);
        fprintf(stderr, "moonlightweb-launch: cannot exec %s: %s\n", path, strerror(errno));
        return 127;
    }

    int count = 0;
    while (environ && environ[count])
        ++count;
    const int cap = count + MW_LAUNCH_ENV_EXTRA;
    const char** env = calloc((size_t)cap, sizeof *env);
    char home[PATH_MAX + 32];
    char xauth[PATH_MAX + 32];
    if (!env || mw_launch_env(environ, env, cap, home, xauth, sizeof xauth) < 0) {
        fprintf(stderr, "moonlightweb-launch: cannot build the environment of %s\n", argv[1]);
        return 127;
    }
    if (chdir("/") != 0) {
        perror("moonlightweb-launch: chdir /");
        return 127;
    }
    execve(path, argv, (char* const*)env);
    fprintf(stderr, "moonlightweb-launch: cannot exec %s: %s\n", path, strerror(errno));
    return 127;
}
