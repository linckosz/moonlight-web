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
 * moonlightweb-kms — hands the stream worker the buffers of the screen its
 * user is looking at.
 *
 * The kernel gives a framebuffer's buffers (GETFB2) only to a process holding
 * CAP_SYS_ADMIN. This program is the one in the package that holds it — a file
 * capability, cap_sys_admin+p, set by the package — so that nothing else has
 * to: the worker that captures, converts and encodes asks it instead
 * (native-host/src/capture/linux/KmsHelperProtocol.h for the protocol,
 * KmsHelperClient.cpp for the asking).
 *
 * What it does, all of it:
 *  - at start, keeps CAP_SYS_ADMIN permitted and nothing else, never effective
 *    but around the two calls that need it; no new privileges; dies with the
 *    worker; no descriptor but its socket; reads no environment variable and
 *    no configuration;
 *  - for each request, answers only the user of the session that is active on
 *    the card's seat (logind's state), and only for a framebuffer that is on
 *    the given plane of the given CRTC right now — before and after reading it;
 *  - never hands a buffer the plane kept from before the seat was that user's
 *    (another session's desktop, the console), until the plane shows another;
 *  - answers with the buffer's layout and its DMA-BUF descriptors.
 * It never sets a mode, never takes the card's master, never writes anything.
 *
 * It links libc alone and talks to the kernel by its own ioctls: a program
 * that holds a capability loads nothing it does not have to.
 */

#define _GNU_SOURCE
#include "../../native-host/src/capture/linux/KmsHelperProtocol.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#endif
#ifndef PR_CAP_AMBIENT_CLEAR_ALL
#define PR_CAP_AMBIENT_CLEAR_ALL 4
#endif

#ifndef DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT
#define DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT 6
#endif

#define DRM_MAJOR 226

static int g_cards[MW_KMS_MAX_CARD + 1];

/* Whose the seat was at the last request, per card (MW_KMS_SEAT_*), and which
 * write of its state file that request read. */
static unsigned char g_seat[MW_KMS_MAX_CARD + 1];
static struct mw_kms_seat_mark g_mark[MW_KMS_MAX_CARD + 1];

/* The buffers the planes kept from before the seat was this user's
 * (KmsHelperProtocol.h). */
static struct mw_kms_foreign g_foreign[MW_KMS_FOREIGN_MAX];

/* CAP_SYS_ADMIN permitted and nothing else; effective only when @p raised.
 * Fails when it is not held (a copy without the file capability). */
static int keep_sys_admin(int raised)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
    memset(&hdr, 0, sizeof hdr);
    memset(data, 0, sizeof data);
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    data[0].permitted = 1u << CAP_SYS_ADMIN;
    data[0].effective = raised ? (1u << CAP_SYS_ADMIN) : 0;
    return (int)syscall(SYS_capset, &hdr, data);
}

/* No capability at all: what is left when CAP_SYS_ADMIN was not handed. */
static void keep_nothing(void)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
    memset(&hdr, 0, sizeof hdr);
    memset(data, 0, sizeof data);
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    (void)syscall(SYS_capset, &hdr, data);
}

/* Every descriptor but 0-2 and the socket. */
static void close_others(void)
{
    int fd;
#ifdef SYS_close_range
    if (syscall(SYS_close_range, MW_KMS_FD + 1, ~0u, 0) == 0) return;
#endif
    for (fd = MW_KMS_FD + 1; fd < 4096; ++fd)
        (void)close(fd);
}

/* A file only root may have written: owned by root, not writable by group or
 * others, a regular file reached without a symbolic link. Its status lands in
 * @p out when that is not NULL. */
static int read_root_file(const char* path, char* text, size_t size, size_t* len, struct stat* out)
{
    struct stat st;
    ssize_t n;
    size_t got = 0;
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) {
        (void)close(fd);
        errno = EPERM;
        return -1;
    }
    while (got < size - 1 && (n = read(fd, text + got, size - 1 - got)) > 0)
        got += (size_t)n;
    (void)close(fd);
    text[got] = '\0';
    *len = got;
    if (out) *out = st;
    return 0;
}

/* Whether this process's user is the one whose session is active on the
 * card's seat. root always is: the systemd unit runs the app as root. @p mark
 * receives which write of the seat's state file was read (logind rewrites it
 * when the active session changes), all zero when none was. */
static int seat_user(uint32_t card, struct mw_kms_seat_mark* mark)
{
    char path[96];
    char text[4096];
    char seat[64];
    size_t len = 0;
    uint32_t active = 0;
    struct stat st;
    const uid_t me = getuid();
    memset(mark, 0, sizeof *mark);
    if (me == 0) return 1;

    snprintf(path, sizeof path, "/run/udev/data/c%d:%u", DRM_MAJOR, card);
    if (read_root_file(path, text, sizeof text, &len, NULL) == 0) {
        if (!mw_kms_parse_udev_seat(text, len, seat, sizeof seat)) return 0;
    } else if (errno == ENOENT) {
        memcpy(seat, "seat0", 6);
    } else {
        return 0;
    }
    snprintf(path, sizeof path, "/run/systemd/seats/%s", seat);
    if (read_root_file(path, text, sizeof text, &len, &st) != 0) return 0;
    mark->dev = (uint64_t)st.st_dev;
    mark->ino = (uint64_t)st.st_ino;
    mark->sec = (int64_t)st.st_mtim.tv_sec;
    mark->nsec = (int64_t)st.st_mtim.tv_nsec;
    if (!mw_kms_parse_active_uid(text, len, &active)) return 0;
    return active == (uint32_t)me;
}

/* The card's descriptor, opened here, by index, once. */
static int card_fd(uint32_t card)
{
    char path[32];
    struct stat st;
    struct drm_set_client_cap cap;
    int fd;
    if (g_cards[card] >= 0) return g_cards[card];
    snprintf(path, sizeof path, "/dev/dri/card%u", card);
    fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) || major(st.st_rdev) != DRM_MAJOR ||
        minor(st.st_rdev) != card) {
        (void)close(fd);
        errno = ENODEV;
        return -1;
    }
    /* Opening a card nobody drives would make this its master: never. */
    (void)ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
    /* The primary and cursor planes, and their state, are listed only to a
     * client that asked for them. Reading only: nothing is ever set. */
    memset(&cap, 0, sizeof cap);
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    cap.value = 1;
    (void)ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    (void)ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    /* A virtual machine's driver lists its cursor plane only to a client that
     * declares this; the planes are read here, never set. */
    cap.capability = DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT;
    (void)ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    g_cards[card] = fd;
    return fd;
}

/* Whether @p fb_id is on @p plane_id of @p crtc_id right now. A plane that
 * moved on from a buffer noted as kept from before forgets it here. */
static int on_screen(uint32_t index, int card, const struct mw_kms_request* rq)
{
    struct drm_mode_get_plane plane;
    memset(&plane, 0, sizeof plane);
    plane.plane_id = rq->plane_id;
    if (ioctl(card, DRM_IOCTL_MODE_GETPLANE, &plane) != 0) return 0;
    mw_kms_foreign_settle(g_foreign, MW_KMS_FOREIGN_MAX, index, plane.plane_id, plane.fb_id);
    return plane.crtc_id == rq->crtc_id && plane.fb_id == rq->fb_id;
}

/* Notes what every plane of the card shows now as kept from before the seat
 * was this user's. Returns 0, or -1 when that could not be done completely. */
static int note_planes(uint32_t index, int card)
{
    struct drm_mode_get_plane_res res;
    uint32_t ids[MW_KMS_FOREIGN_MAX];
    uint32_t i;
    memset(&res, 0, sizeof res);
    if (ioctl(card, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) != 0 ||
        res.count_planes > sizeof ids / sizeof ids[0])
        return -1;
    res.plane_id_ptr = (uint64_t)(uintptr_t)ids;
    if (ioctl(card, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) != 0) return -1;
    for (i = 0; i < res.count_planes && i < sizeof ids / sizeof ids[0]; ++i) {
        struct drm_mode_get_plane plane;
        memset(&plane, 0, sizeof plane);
        plane.plane_id = ids[i];
        /* A plane left unnoted could hand its buffer: fail closed. */
        if (ioctl(card, DRM_IOCTL_MODE_GETPLANE, &plane) != 0) return -1;
        if (mw_kms_foreign_note(g_foreign, MW_KMS_FOREIGN_MAX, index, plane.plane_id,
                                plane.fb_id) != 0)
            return -1;
    }
    return 0;
}

/* The seat is someone else's: what the planes show is theirs. */
static void seat_lost(uint32_t index, int card, const struct mw_kms_seat_mark* mark)
{
    (void)note_planes(index, card);
    g_seat[index] = MW_KMS_SEAT_OTHER;
    g_mark[index] = *mark;
}

static void close_handles(int card, const uint32_t* handles)
{
    uint32_t i, j;
    for (i = 0; i < MW_KMS_MAX_PLANES && handles[i]; ++i) {
        int seen = 0;
        struct drm_gem_close gem;
        for (j = 0; j < i; ++j)
            if (handles[j] == handles[i]) seen = 1;
        if (seen) continue;
        memset(&gem, 0, sizeof gem);
        gem.handle = handles[i];
        (void)ioctl(card, DRM_IOCTL_GEM_CLOSE, &gem);
    }
}

/* One request: the reply, and the descriptors to send with it. */
static void serve(const struct mw_kms_request* rq, struct mw_kms_reply* rp, int* fds,
                  uint32_t* nfds)
{
    struct drm_mode_fb_cmd2 fb;
    uint32_t i, j;
    int card, failed, saved;
    struct mw_kms_seat_mark mark;

    card = card_fd(rq->card);
    if (card < 0) {
        rp->status = errno ? errno : ENODEV;
        return;
    }
    if (!seat_user(rq->card, &mark)) {
        seat_lost(rq->card, card, &mark);
        rp->status = EACCES;
        return;
    }
    /* The seat is this user's now. If it was someone else's at the last
     * request, or its state was written since (it may have gone and come back
     * in between), or this helper is new and the seat changed hands moments
     * ago: what the planes show was put there before, held back until
     * replaced. Once that change is old enough, the notes go
     * (mw_kms_notes_expire). */
    {
        const int moved = !mw_kms_seat_mark_same(&g_mark[rq->card], &mark);
        const int fresh = mw_kms_seat_fresh((long long)time(NULL), (long long)mark.sec);
        if (mw_kms_must_note(g_seat[rq->card], moved, fresh)) {
            if (note_planes(rq->card, card) != 0) {
                g_seat[rq->card] = MW_KMS_SEAT_OTHER;
                g_mark[rq->card] = mark;
                rp->status = EAGAIN;
                return;
            }
        } else if (mw_kms_notes_expire(g_seat[rq->card], moved, fresh)) {
            mw_kms_foreign_forget(g_foreign, MW_KMS_FOREIGN_MAX, rq->card);
        }
    }
    g_seat[rq->card] = MW_KMS_SEAT_MINE;
    g_mark[rq->card] = mark;
    if (!on_screen(rq->card, card, rq)) {
        rp->status = ENOENT;
        return;
    }
    if (mw_kms_foreign_is(g_foreign, MW_KMS_FOREIGN_MAX, rq->card, rq->plane_id, rq->fb_id)) {
        rp->status = EAGAIN;
        return;
    }

    memset(&fb, 0, sizeof fb);
    fb.fb_id = rq->fb_id;
    if (keep_sys_admin(1) != 0) {
        rp->status = EPERM;
        return;
    }
    failed = ioctl(card, DRM_IOCTL_MODE_GETFB2, &fb);
    saved = errno;
    /* Never go on with the capability effective. */
    if (keep_sys_admin(0) != 0) {
        if (!failed) close_handles(card, fb.handles);
        _exit(1);
    }
    if (failed) {
        rp->status = saved ? saved : EIO;
        return;
    }
    if (!fb.handles[0]) {
        rp->status = EPERM;
        return;
    }
    /* Still there, and still this user's: a buffer taken off the screen in
     * between is not handed, and neither is one a session switch put there —
     * the plane can show the next user's picture before the seat says so. */
    if (!on_screen(rq->card, card, rq)) {
        close_handles(card, fb.handles);
        rp->status = ENOENT;
        return;
    }
    if (!seat_user(rq->card, &mark)) {
        close_handles(card, fb.handles);
        seat_lost(rq->card, card, &mark);
        rp->status = EACCES;
        return;
    }
    /* The seat's state written in between: it may have gone and come back. */
    if (!mw_kms_seat_mark_same(&g_mark[rq->card], &mark)) {
        close_handles(card, fb.handles);
        if (note_planes(rq->card, card) != 0) g_seat[rq->card] = MW_KMS_SEAT_OTHER;
        g_mark[rq->card] = mark;
        rp->status = EAGAIN;
        return;
    }
    if (mw_kms_foreign_is(g_foreign, MW_KMS_FOREIGN_MAX, rq->card, rq->plane_id, rq->fb_id)) {
        close_handles(card, fb.handles);
        rp->status = EAGAIN;
        return;
    }

    for (i = 0; i < MW_KMS_MAX_PLANES && fb.handles[i]; ++i) {
        int index = -1;
        for (j = 0; j < i; ++j)
            if (fb.handles[j] == fb.handles[i]) index = rp->fd_index[j];
        if (index < 0) {
            struct drm_prime_handle prime;
            memset(&prime, 0, sizeof prime);
            prime.handle = fb.handles[i];
            prime.flags = DRM_CLOEXEC | DRM_RDWR;
            if (ioctl(card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
                saved = errno;
                for (j = 0; j < *nfds; ++j)
                    (void)close(fds[j]);
                *nfds = 0;
                close_handles(card, fb.handles);
                memset(rp->fd_index, 0xff, sizeof rp->fd_index);
                rp->status = saved ? saved : EIO;
                return;
            }
            index = (int)*nfds;
            fds[(*nfds)++] = prime.fd;
        }
        rp->fd_index[i] = index;
        rp->pitches[i] = fb.pitches[i];
        rp->offsets[i] = fb.offsets[i];
        rp->plane_count = i + 1;
    }
    close_handles(card, fb.handles);

    rp->width = fb.width;
    rp->height = fb.height;
    rp->pixel_format = fb.pixel_format;
    rp->flags = fb.flags;
    rp->modifier = (fb.flags & DRM_MODE_FB_MODIFIERS) ? fb.modifier[0] : 0;
    rp->fd_count = *nfds;
    rp->status = 0;
}

static int send_reply(const struct mw_kms_reply* rp, const int* fds, uint32_t nfds)
{
    struct iovec iov;
    struct msghdr msg;
    union
    {
        char buffer[CMSG_SPACE(sizeof(int) * MW_KMS_MAX_PLANES)];
        struct cmsghdr align;
    } control;
    iov.iov_base = (void*)rp;
    iov.iov_len = sizeof *rp;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (nfds) {
        struct cmsghdr* cmsg;
        memset(&control, 0, sizeof control);
        msg.msg_control = control.buffer;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);
    }
    return sendmsg(MW_KMS_FD, &msg, MSG_NOSIGNAL) == (ssize_t)sizeof *rp ? 0 : -1;
}

/* fd MW_KMS_FD must be a SOCK_SEQPACKET Unix socket whose peer is this user. */
static int socket_ok(void)
{
    int type = 0, domain = 0;
    socklen_t len = sizeof type;
    struct ucred peer;
    socklen_t peer_len = sizeof peer;
    if (getsockopt(MW_KMS_FD, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_SEQPACKET)
        return 0;
    len = sizeof domain;
    if (getsockopt(MW_KMS_FD, SOL_SOCKET, SO_DOMAIN, &domain, &len) != 0 || domain != AF_UNIX)
        return 0;
    if (getsockopt(MW_KMS_FD, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0) return 0;
    return peer.uid == getuid();
}

int main(int argc, char** argv)
{
    const pid_t parent = getppid();
    uint32_t i;
    (void)argc;
    (void)argv;

    /* Dies with the worker; and if the worker died before this was set, now. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0 || getppid() != parent) return 1;
    close_others();
    if (!socket_ok()) {
        fprintf(stderr, "moonlightweb-kms: descriptor %d is not the worker's socket\n", MW_KMS_FD);
        return 2;
    }
    if (keep_sys_admin(0) != 0) keep_nothing(); /* every request is then refused */
    (void)prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 1;
    (void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    for (i = 0; i <= MW_KMS_MAX_CARD; ++i)
        g_cards[i] = -1;

    for (;;) {
        struct mw_kms_request rq;
        struct mw_kms_reply rp;
        int fds[MW_KMS_MAX_PLANES];
        uint32_t nfds = 0, k;
        struct iovec iov;
        struct msghdr msg;
        ssize_t n;

        memset(&rq, 0, sizeof rq);
        iov.iov_base = &rq;
        iov.iov_len = sizeof rq;
        memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        n = recvmsg(MW_KMS_FD, &msg, MSG_CMSG_CLOEXEC);
        if (n == 0) return 0; /* the worker closed its end */
        if (n < 0) {
            if (errno == EINTR) continue;
            return 1;
        }

        memset(&rp, 0, sizeof rp);
        rp.magic = MW_KMS_REPLY_MAGIC;
        rp.seq = rq.seq;
        rp.fb_id = rq.fb_id;
        memset(rp.fd_index, 0xff, sizeof rp.fd_index);
        /* A message cut short, or carrying descriptors of its own: refused. */
        if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || !mw_kms_request_valid(&rq, (size_t)n)) {
            rp.status = EINVAL;
        } else {
            serve(&rq, &rp, fds, &nfds);
        }
        if (send_reply(&rp, fds, nfds) != 0) {
            for (k = 0; k < nfds; ++k)
                (void)close(fds[k]);
            return 1;
        }
        for (k = 0; k < nfds; ++k)
            (void)close(fds[k]);
    }
}
