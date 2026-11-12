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

/*
 * What the stream worker and moonlightweb-kms say to each other — C, so the
 * helper (packaging/linux/moonlightweb-kms.c) and the capture
 * (KmsHelperClient.cpp) read the same bytes, and pure, so the backend's tests
 * check its bounds on every platform.
 *
 * moonlightweb-kms is the one program of the package that holds
 * CAP_SYS_ADMIN, which the kernel requires to hand out a framebuffer's
 * buffers. The worker starts it with one end of a SOCK_SEQPACKET socketpair on
 * descriptor MW_KMS_FD, and asks, one request at a time: « the buffer FB is on
 * PLANE of CRTC of card N — give it to me ». It answers with the buffer's
 * layout and its DMA-BUF descriptors (SCM_RIGHTS), or a refusal, and exits
 * when the worker closes its end or dies.
 *
 * It answers only the user of the session that is ACTIVE on the card's seat
 * (logind's own state, read from the files sd-login reads), and only for a
 * buffer that is on a plane of that card right now — what that user is looking
 * at, and nothing else: not a buffer the plane kept from before the seat was
 * that user's (EAGAIN, until the plane shows another one).
 */

#ifndef MW_KMS_HELPER_PROTOCOL_H
#define MW_KMS_HELPER_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The helper's end of the socketpair. */
#define MW_KMS_FD 3

#define MW_KMS_REQUEST_MAGIC 0x514b574du /* "MWKQ" */
#define MW_KMS_REPLY_MAGIC 0x524b574du   /* "MWKR" */

#define MW_KMS_OP_PLANE_FB 1u

#define MW_KMS_MAX_CARD 63u
#define MW_KMS_MAX_PLANES 4u

/* What the worker sends: always exactly this many bytes. */
struct mw_kms_request
{
    uint32_t magic;    /* MW_KMS_REQUEST_MAGIC */
    uint32_t op;       /* MW_KMS_OP_PLANE_FB */
    uint32_t seq;      /* echoed in the reply */
    uint32_t card;     /* /dev/dri/card<card>, 0..MW_KMS_MAX_CARD */
    uint32_t crtc_id;  /* the CRTC the plane is on */
    uint32_t plane_id; /* the plane */
    uint32_t fb_id;    /* the framebuffer the worker read on that plane */
    uint32_t reserved; /* 0 */
};

/* What the helper answers: always exactly this many bytes, plus fd_count
 * descriptors in one SCM_RIGHTS message when status is 0. */
struct mw_kms_reply
{
    uint32_t magic; /* MW_KMS_REPLY_MAGIC */
    uint32_t seq;   /* the request's */
    int32_t status; /* 0, or a positive errno: EACCES, ENOENT, EINVAL, … */
    uint32_t fb_id; /* the request's */
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format; /* DRM fourcc */
    uint32_t flags;        /* DRM_MODE_FB_* (modifiers) */
    uint64_t modifier;
    uint32_t pitches[MW_KMS_MAX_PLANES];
    uint32_t offsets[MW_KMS_MAX_PLANES];
    int32_t fd_index[MW_KMS_MAX_PLANES]; /* the descriptor each plane uses, -1 past plane_count */
    uint32_t plane_count;                /* 1..MW_KMS_MAX_PLANES */
    uint32_t fd_count;                   /* 1..plane_count: planes share a buffer */
};

/* Whether @p size bytes at @p r are a request the helper may act on. */
static inline int mw_kms_request_valid(const struct mw_kms_request* r, size_t size)
{
    return r && size == sizeof(struct mw_kms_request) && r->magic == MW_KMS_REQUEST_MAGIC &&
           r->op == MW_KMS_OP_PLANE_FB && r->card <= MW_KMS_MAX_CARD && r->crtc_id != 0 &&
           r->plane_id != 0 && r->fb_id != 0 && r->reserved == 0;
}

/* Whether a reply of @p size bytes, for the request numbered @p seq, that came
 * with @p fds_received descriptors, can be used. A refusal carries none; an
 * answer carries exactly fd_count, and every plane names one of them. */
static inline int mw_kms_reply_valid(const struct mw_kms_reply* r, size_t size, uint32_t seq,
                                     uint32_t fb_id, uint32_t fds_received)
{
    uint32_t i;
    if (!r || size != sizeof(struct mw_kms_reply) || r->magic != MW_KMS_REPLY_MAGIC ||
        r->seq != seq)
        return 0;
    if (r->status != 0) return r->status > 0 && fds_received == 0;
    if (r->fb_id != fb_id || r->width == 0 || r->height == 0 || r->width > 16384 ||
        r->height > 16384)
        return 0;
    if (r->plane_count < 1 || r->plane_count > MW_KMS_MAX_PLANES) return 0;
    if (r->fd_count < 1 || r->fd_count > r->plane_count || r->fd_count != fds_received) return 0;
    for (i = 0; i < MW_KMS_MAX_PLANES; ++i) {
        if (i < r->plane_count) {
            if (r->fd_index[i] < 0 || (uint32_t)r->fd_index[i] >= r->fd_count) return 0;
        } else if (r->fd_index[i] != -1) {
            return 0;
        }
    }
    return 1;
}

/* ── Who may ask: the seat's active user ──────────────────────────────────── */

/* The seat a card belongs to, from udev's database entry for it
 * (/run/udev/data/c226:<minor>): its "E:ID_SEAT=" line, "seat0" when it has
 * none. A seat name is [A-Za-z0-9_-], at most @p size - 1 bytes. Returns 0 on
 * a malformed entry. */
static inline int mw_kms_parse_udev_seat(const char* text, size_t len, char* seat, size_t size)
{
    static const char key[] = "E:ID_SEAT=";
    size_t at = 0;
    if (!seat || size < 6) return 0;
    memcpy(seat, "seat0", 6);
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n')
            ++end;
        if (end - at >= sizeof key - 1 && memcmp(text + at, key, sizeof key - 1) == 0) {
            const size_t start = at + sizeof key - 1;
            size_t n = end - start, i;
            if (n == 0 || n >= size) return 0;
            for (i = 0; i < n; ++i) {
                const char c = text[start + i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-'))
                    return 0;
            }
            memcpy(seat, text + start, n);
            seat[n] = '\0';
            return 1;
        }
        at = end + 1;
    }
    return 1;
}

/* The uid of the user whose session is active on a seat, from logind's state
 * file for it (/run/systemd/seats/<seat>): its one "ACTIVE_UID=" line, in
 * decimal. Returns 0 when there is none (no active session), when there are
 * two, or when the value is not a plain number. */
static inline int mw_kms_parse_active_uid(const char* text, size_t len, uint32_t* uid)
{
    static const char key[] = "ACTIVE_UID=";
    size_t at = 0;
    int found = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n')
            ++end;
        if (end - at >= sizeof key - 1 && memcmp(text + at, key, sizeof key - 1) == 0) {
            const size_t start = at + sizeof key - 1;
            uint64_t value = 0;
            size_t i;
            if (found || end == start || end - start > 10) return 0;
            for (i = start; i < end; ++i) {
                if (text[i] < '0' || text[i] > '9') return 0;
                value = value * 10 + (uint64_t)(text[i] - '0');
            }
            if (value > 0xFFFFFFFEu) return 0;
            *uid = (uint32_t)value;
            found = 1;
        }
        at = end + 1;
    }
    return found;
}

/* ── What the planes kept from before the seat was this user's ─────────────── */

/* A plane follows the seat late. When logind makes a session active, the
 * planes still show what was on them before — another user's desktop, or the
 * console — until that session's compositor puts a buffer of its own there.
 * The helper notes those buffers, per plane, and never hands one: a plane's
 * buffer seen while the seat was someone else's, or there when it became this
 * user's, is held back until the plane shows another buffer. */
#define MW_KMS_FOREIGN_MAX 256u

/* How recently the seat must have changed hands for a helper that has not seen
 * it happen to hold back what the planes show when it starts. */
#define MW_KMS_FRESH_SEAT_SECONDS 5

struct mw_kms_foreign
{
    uint32_t card;
    uint32_t plane;
    uint32_t fb; /* 0: a free slot */
};

/* Notes @p fb as what @p plane of @p card shows from before (it replaces what
 * was noted for that plane). Returns 0, or -1 when the table is full. */
static inline int mw_kms_foreign_note(struct mw_kms_foreign* t, size_t n, uint32_t card,
                                      uint32_t plane, uint32_t fb)
{
    size_t i, free_slot = n;
    if (!fb) return 0;
    for (i = 0; i < n; ++i) {
        if (t[i].fb && t[i].card == card && t[i].plane == plane) {
            t[i].fb = fb;
            return 0;
        }
        if (!t[i].fb && free_slot == n) free_slot = i;
    }
    if (free_slot == n) return -1;
    t[free_slot].card = card;
    t[free_slot].plane = plane;
    t[free_slot].fb = fb;
    return 0;
}

/* Whether @p fb is what was noted on @p plane of @p card. */
static inline int mw_kms_foreign_is(const struct mw_kms_foreign* t, size_t n, uint32_t card,
                                    uint32_t plane, uint32_t fb)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (t[i].fb && t[i].fb == fb && t[i].card == card && t[i].plane == plane) return 1;
    return 0;
}

/* The plane shows @p fb_now: once it shows another buffer than the one noted,
 * the note goes — that buffer left the screen, and an id the kernel hands out
 * again later is a new buffer. */
static inline void mw_kms_foreign_settle(struct mw_kms_foreign* t, size_t n, uint32_t card,
                                         uint32_t plane, uint32_t fb_now)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (t[i].fb && t[i].card == card && t[i].plane == plane && t[i].fb != fb_now) t[i].fb = 0;
}

/* Whether the seat changed hands recently enough (its state file was written
 * at @p changed, in seconds, @p now being the clock's) that a helper which did
 * not see it happen must hold back what the planes show. A date in the
 * future counts as recent. */
static inline int mw_kms_seat_fresh(long long now, long long changed)
{
    return now - changed < MW_KMS_FRESH_SEAT_SECONDS;
}

/* Which write of the seat's state file a helper read: logind writes it to a
 * temporary file and renames it over, so every write is a new inode with its
 * own modification time. A helper that asked nothing while the seat went to
 * someone else and came back sees the mark move. */
struct mw_kms_seat_mark
{
    uint64_t dev;
    uint64_t ino;
    int64_t sec;
    int64_t nsec;
};

static inline int mw_kms_seat_mark_same(const struct mw_kms_seat_mark* a,
                                        const struct mw_kms_seat_mark* b)
{
    return a->dev == b->dev && a->ino == b->ino && a->sec == b->sec && a->nsec == b->nsec;
}

/* Whose the seat was at a helper's last request on a card. */
enum
{
    MW_KMS_SEAT_UNSEEN = 0, /* no request yet: the helper did not see it change */
    MW_KMS_SEAT_MINE = 1,
    MW_KMS_SEAT_OTHER = 2
};

/* Whether, the seat being this user's now, what the planes show must be noted
 * as kept from before: the seat was someone else's at the last request; or it
 * was this user's but its state was written since (@p moved: it may have gone
 * and come back between two requests); or the helper never saw it and it
 * changed hands moments ago (@p fresh, mw_kms_seat_fresh). */
static inline int mw_kms_must_note(int state, int moved, int fresh)
{
    return state == MW_KMS_SEAT_OTHER || (state == MW_KMS_SEAT_MINE && moved) ||
           (state == MW_KMS_SEAT_UNSEEN && fresh);
}

/* Whether a helper forgets every note of a card: the seat is this user's, was
 * at the last request with the same write of its state file, and that write
 * is MW_KMS_FRESH_SEAT_SECONDS old or more. A compositor that draws into the
 * buffer it scans out never replaces it, and a buffer noted by mistake would
 * be held back forever. A new helper serves the planes from that age on
 * anyway, so forgetting then exposes nothing more — including a seat that
 * came back to this user more than that long after it changed hands, served
 * without holding back, exactly as a new helper would. */
static inline int mw_kms_notes_expire(int state, int moved, int fresh)
{
    return state == MW_KMS_SEAT_MINE && !moved && !fresh;
}

/* Forgets every note of @p card. */
static inline void mw_kms_foreign_forget(struct mw_kms_foreign* t, size_t n, uint32_t card)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (t[i].card == card) t[i].fb = 0;
}

#endif /* MW_KMS_HELPER_PROTOCOL_H */
