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

#include "test_framework.h"
#include "../native-host/src/capture/linux/KmsHelperProtocol.h"

#include <cstring>
#include <string>

namespace {

mw_kms_request request()
{
    mw_kms_request r{};
    r.magic = MW_KMS_REQUEST_MAGIC;
    r.op = MW_KMS_OP_PLANE_FB;
    r.seq = 7;
    r.card = 1;
    r.crtc_id = 64;
    r.plane_id = 31;
    r.fb_id = 162;
    return r;
}

// A buffer of three planes over two descriptors (a DCC surface: the colour
// and its metadata share one object).
mw_kms_reply reply()
{
    mw_kms_reply r{};
    r.magic = MW_KMS_REPLY_MAGIC;
    r.seq = 7;
    r.status = 0;
    r.fb_id = 162;
    r.width = 2560;
    r.height = 1440;
    r.plane_count = 3;
    r.fd_count = 2;
    r.fd_index[0] = 0;
    r.fd_index[1] = 0;
    r.fd_index[2] = 1;
    r.fd_index[3] = -1;
    return r;
}

bool replyOk(const mw_kms_reply& r, uint32_t fds)
{
    return mw_kms_reply_valid(&r, sizeof r, 7, 162, fds) != 0;
}

bool seatOf(const std::string& text, std::string& seat, size_t size = 64)
{
    char buffer[64] = {};
    const int ok = mw_kms_parse_udev_seat(text.data(), text.size(), buffer, size);
    seat = buffer;
    return ok != 0;
}

bool uidOf(const std::string& text, uint32_t& uid)
{
    uid = 0xFFFFFFFFu;
    return mw_kms_parse_active_uid(text.data(), text.size(), &uid) != 0;
}

} // namespace

// moonlightweb-kms holds the capture capability and answers the stream
// worker's requests; both sides read these bytes. What either refuses.
void run_kms_helper_protocol_tests()
{
    SECTION("KMS helper: the request");

    mw_kms_request r = request();
    CHECK(mw_kms_request_valid(&r, sizeof r));
    CHECK(!mw_kms_request_valid(nullptr, sizeof r));
    CHECK(!mw_kms_request_valid(&r, sizeof r - 1));
    CHECK(!mw_kms_request_valid(&r, sizeof r + 1));
    r.magic ^= 1;
    CHECK(!mw_kms_request_valid(&r, sizeof r));
    r = request();
    r.op = 2;
    CHECK(!mw_kms_request_valid(&r, sizeof r));
    r = request();
    r.card = MW_KMS_MAX_CARD;
    CHECK(mw_kms_request_valid(&r, sizeof r));
    r.card = MW_KMS_MAX_CARD + 1;
    CHECK(!mw_kms_request_valid(&r, sizeof r));
    r = request();
    r.crtc_id = 0;
    CHECK(!mw_kms_request_valid(&r, sizeof r));
    r = request();
    r.plane_id = 0;
    CHECK(!mw_kms_request_valid(&r, sizeof r));
    r = request();
    r.fb_id = 0;
    CHECK(!mw_kms_request_valid(&r, sizeof r));
    r = request();
    r.reserved = 1;
    CHECK(!mw_kms_request_valid(&r, sizeof r));

    SECTION("KMS helper: the reply");

    mw_kms_reply a = reply();
    CHECK(replyOk(a, 2));
    // The descriptors that came are the ones it names, no more, no fewer.
    CHECK(!replyOk(a, 1));
    CHECK(!replyOk(a, 3));
    CHECK(!replyOk(a, 0));
    // Not this request's, or not this buffer's.
    CHECK(!mw_kms_reply_valid(&a, sizeof a, 8, 162, 2));
    CHECK(!mw_kms_reply_valid(&a, sizeof a, 7, 163, 2));
    CHECK(!mw_kms_reply_valid(&a, sizeof a - 1, 7, 162, 2));
    CHECK(!mw_kms_reply_valid(nullptr, sizeof a, 7, 162, 2));
    a.magic = MW_KMS_REQUEST_MAGIC;
    CHECK(!replyOk(a, 2));
    // A plane that names a descriptor that did not come, or none at all.
    a = reply();
    a.fd_index[2] = 2;
    CHECK(!replyOk(a, 2));
    a = reply();
    a.fd_index[1] = -1;
    CHECK(!replyOk(a, 2));
    // Past the planes, nothing.
    a = reply();
    a.fd_index[3] = 0;
    CHECK(!replyOk(a, 2));
    // Plane and descriptor counts in bounds.
    a = reply();
    a.plane_count = 0;
    CHECK(!replyOk(a, 2));
    a = reply();
    a.plane_count = MW_KMS_MAX_PLANES + 1;
    CHECK(!replyOk(a, 2));
    a = reply();
    a.fd_count = 4;
    CHECK(!replyOk(a, 4));
    // A size no screen has.
    a = reply();
    a.width = 0;
    CHECK(!replyOk(a, 2));
    a = reply();
    a.height = 16385;
    CHECK(!replyOk(a, 2));
    // A refusal: a positive errno and no descriptor.
    mw_kms_reply no{};
    no.magic = MW_KMS_REPLY_MAGIC;
    no.seq = 7;
    no.fb_id = 162;
    no.status = 13; // EACCES
    CHECK(replyOk(no, 0));
    CHECK(!replyOk(no, 1));
    no.status = -1;
    CHECK(!replyOk(no, 0));

    SECTION("KMS helper: the card's seat, from udev");

    std::string seat;
    CHECK(seatOf("I:12345\nE:ID_FOR_SEAT=drm-pci-0000_c5_00_0\nG:seat\nG:master-of-seat\n", seat));
    CHECK_EQ(seat, std::string("seat0"));
    CHECK(seatOf("I:1\nE:ID_SEAT=seat1\nG:seat\n", seat));
    CHECK_EQ(seat, std::string("seat1"));
    CHECK(seatOf("E:ID_SEAT=seat-2_b", seat));
    CHECK_EQ(seat, std::string("seat-2_b"));
    CHECK(seatOf("", seat));
    CHECK_EQ(seat, std::string("seat0"));
    // Malformed: refused, never read as another seat.
    CHECK(!seatOf("E:ID_SEAT=\n", seat));
    CHECK(!seatOf("E:ID_SEAT=../seat0\n", seat));
    CHECK(!seatOf("E:ID_SEAT=seat 1\n", seat));
    CHECK(!seatOf("E:ID_SEAT=seat0/x\n", seat));
    CHECK(!seatOf("E:ID_SEAT=" + std::string(64, 'a') + "\n", seat));
    CHECK(!seatOf("E:ID_SEAT=seat1\n", seat, 5));
    // A line that only contains the key is not it.
    CHECK(seatOf("XE:ID_SEAT=seat9\n", seat));
    CHECK_EQ(seat, std::string("seat0"));

    SECTION("KMS helper: the seat's active user, from logind");

    uint32_t uid = 0;
    CHECK(uidOf("# This is private data. Do not parse.\nACTIVE=2\nACTIVE_UID=1000\nSESSIONS=2 c1\n",
                uid));
    CHECK_EQ(uid, 1000u);
    CHECK(uidOf("ACTIVE_UID=0\n", uid));
    CHECK_EQ(uid, 0u);
    CHECK(uidOf("ACTIVE_UID=4294967294", uid));
    CHECK_EQ(uid, 4294967294u);
    // No active session, or anything that is not one plain number.
    CHECK(!uidOf("ACTIVE=\nSESSIONS=\n", uid));
    CHECK(!uidOf("", uid));
    CHECK(!uidOf("ACTIVE_UID=\n", uid));
    CHECK(!uidOf("ACTIVE_UID=12a\n", uid));
    CHECK(!uidOf("ACTIVE_UID=-1\n", uid));
    CHECK(!uidOf("ACTIVE_UID= 1000\n", uid));
    CHECK(!uidOf("ACTIVE_UID=4294967295\n", uid));
    CHECK(!uidOf("ACTIVE_UID=12345678901\n", uid));
    CHECK(!uidOf("ACTIVE_UID=1000\nACTIVE_UID=1001\n", uid));
    CHECK(!uidOf("XACTIVE_UID=1000\n", uid));

    SECTION("KMS helper: what the planes kept from before the seat was this user's");

    mw_kms_foreign kept[3]{};
    // Plane 32 of card 0 still shows buffer 40, the console's.
    CHECK_EQ(mw_kms_foreign_note(kept, 3, 0, 32, 40), 0);
    CHECK(mw_kms_foreign_is(kept, 3, 0, 32, 40));
    // Only that buffer, on that plane, of that card.
    CHECK(!mw_kms_foreign_is(kept, 3, 0, 32, 41));
    CHECK(!mw_kms_foreign_is(kept, 3, 0, 33, 40));
    CHECK(!mw_kms_foreign_is(kept, 3, 1, 32, 40));
    // A plane with no buffer notes nothing.
    CHECK_EQ(mw_kms_foreign_note(kept, 3, 0, 33, 0), 0);
    CHECK(!mw_kms_foreign_is(kept, 3, 0, 33, 0));
    // Noted again: the plane's note is replaced, not doubled.
    CHECK_EQ(mw_kms_foreign_note(kept, 3, 0, 32, 44), 0);
    CHECK(mw_kms_foreign_is(kept, 3, 0, 32, 44));
    CHECK(!mw_kms_foreign_is(kept, 3, 0, 32, 40));
    // Still on the plane: still held back.
    mw_kms_foreign_settle(kept, 3, 0, 32, 44);
    CHECK(mw_kms_foreign_is(kept, 3, 0, 32, 44));
    // Settling another plane does not touch this one.
    mw_kms_foreign_settle(kept, 3, 0, 33, 45);
    CHECK(mw_kms_foreign_is(kept, 3, 0, 32, 44));
    // The compositor's own buffer replaced it: the note goes, and the id the
    // kernel hands out again later is not held against the new buffer.
    mw_kms_foreign_settle(kept, 3, 0, 32, 41);
    CHECK(!mw_kms_foreign_is(kept, 3, 0, 32, 44));
    CHECK(!mw_kms_foreign_is(kept, 3, 0, 32, 41));
    // A full table says so: the helper then refuses rather than forget.
    CHECK_EQ(mw_kms_foreign_note(kept, 3, 0, 1, 10), 0);
    CHECK_EQ(mw_kms_foreign_note(kept, 3, 0, 2, 11), 0);
    CHECK_EQ(mw_kms_foreign_note(kept, 3, 0, 3, 12), 0);
    CHECK_EQ(mw_kms_foreign_note(kept, 3, 0, 4, 13), -1);
    // A freed slot is used again.
    mw_kms_foreign_settle(kept, 3, 0, 2, 0);
    CHECK_EQ(mw_kms_foreign_note(kept, 3, 0, 4, 13), 0);
    CHECK(mw_kms_foreign_is(kept, 3, 0, 4, 13));

    // A helper that did not see the seat change hands: recent means < 5 s,
    // and a date ahead of the clock counts as recent.
    CHECK(mw_kms_seat_fresh(1000, 1000));
    CHECK(mw_kms_seat_fresh(1000, 996));
    CHECK(!mw_kms_seat_fresh(1000, 995));
    CHECK(!mw_kms_seat_fresh(1000, 0));
    CHECK(mw_kms_seat_fresh(1000, 2000));

    // When the planes are noted, the seat being this user's now.
    CHECK(mw_kms_must_note(MW_KMS_SEAT_OTHER, 0, 0));   // it was someone else's
    CHECK(mw_kms_must_note(MW_KMS_SEAT_MINE, 1, 0));    // written since: gone and back
    CHECK(!mw_kms_must_note(MW_KMS_SEAT_MINE, 0, 1));   // untouched since the last request
    CHECK(mw_kms_must_note(MW_KMS_SEAT_UNSEEN, 0, 1));  // a new helper, a recent change
    CHECK(!mw_kms_must_note(MW_KMS_SEAT_UNSEEN, 1, 0)); // a new helper, an old one

    // When a helper forgets its notes: the seat this user's, untouched since the
    // last request, and its last change old enough.
    CHECK(mw_kms_notes_expire(MW_KMS_SEAT_MINE, 0, 0));
    CHECK(!mw_kms_notes_expire(MW_KMS_SEAT_MINE, 1, 0));
    CHECK(!mw_kms_notes_expire(MW_KMS_SEAT_MINE, 0, 1));
    CHECK(!mw_kms_notes_expire(MW_KMS_SEAT_OTHER, 0, 0));
    CHECK(!mw_kms_notes_expire(MW_KMS_SEAT_UNSEEN, 0, 0));
    CHECK(!mw_kms_notes_expire(MW_KMS_SEAT_UNSEEN, 0, 1));
    // Forgetting is per card.
    mw_kms_foreign two[2]{};
    CHECK_EQ(mw_kms_foreign_note(two, 2, 0, 32, 44), 0);
    CHECK_EQ(mw_kms_foreign_note(two, 2, 1, 32, 44), 0);
    mw_kms_foreign_forget(two, 2, 0);
    CHECK(!mw_kms_foreign_is(two, 2, 0, 32, 44));
    CHECK(mw_kms_foreign_is(two, 2, 1, 32, 44));

    // Which write of the seat's file: device, inode and time to the nanosecond.
    const mw_kms_seat_mark seen{2049, 812, 1000, 5};
    mw_kms_seat_mark now = seen;
    CHECK(mw_kms_seat_mark_same(&seen, &now));
    now.ino = 813; // renamed over: a new inode
    CHECK(!mw_kms_seat_mark_same(&seen, &now));
    now = seen;
    now.nsec = 6; // the same second
    CHECK(!mw_kms_seat_mark_same(&seen, &now));
    now = seen;
    now.dev = 2050;
    CHECK(!mw_kms_seat_mark_same(&seen, &now));
}
