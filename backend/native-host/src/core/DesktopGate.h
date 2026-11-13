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

#include <atomic>
#include <cstdint>

namespace mw::native {

/// What a read of the session's own state said (WTSSessionInfoEx on Windows,
/// platform/windows/SessionLockWatch).
enum class LockRead
{
    /// Unlocked, and the active session of its console.
    Unlocked,
    /// Locked, or not the active session: another user switched to, a
    /// console disconnected, a remote session.
    Locked,
    /// The read failed, or said anything else. Counted as locked.
    Unknown,
};

/// A notification of the session's state, as received.
enum class LockNotice
{
    Lock,
    Unlock,
    /// Any other change (a console or remote connection, a logon): a reason
    /// to read again, nothing more.
    Other,
};

/// The gate of a stream that acts on the user's own desktop only: a TV on
/// plain HTTP (SessionConfig::defaultDesktopOnly, issue #42). No picture goes
/// out and no input goes in unless it is open.
///
/// ── Why the session's state as well ─────────────────────────────────────────
///
/// Windows 11 draws its lock screen (the clock, the date, the picture) on the
/// user's own desktop; only the password prompt has the secure desktop. The
/// input desktop alone does not tell a locked session from an open one, so
/// the session's lock state is read as well, and the gate opens on both.
///
/// ── Who moves it ────────────────────────────────────────────────────────────
///
///  - Closed at the start, and until a first read says unlocked.
///  - The picture and input paths read the input desktop at each picture and
///    each event, and close it at once when it is not the user's (admit()).
///    They never open it.
///  - The watcher (one thread) closes it on a lock notice of this session,
///    and on a read that says locked or cannot say. A notice about another
///    session is never believed: it only has the session read again.
///  - Only the watcher opens it, on a read that says unlocked while the input
///    desktop is the user's. After a lock it waits kSettleAfterLockUs more:
///    the lock screen slides away after Windows Hello lets the user in.
///
/// ── The generation ──────────────────────────────────────────────────────────
///
/// One atomic counter carries the whole state, so that a single read tells
/// it: even while closed, odd while open, one more at every change. A picture
/// is taken under the generation read before its capture, and goes out only
/// if the gate is still at that very generation: whatever was taken while it
/// was closed, or before a close, never goes. Read in one load, it cannot be
/// seen half-changed.
///
/// Pure and clocked by the caller. Tested in tests/test_desktop_gate.cpp.
class DesktopGate
{
public:
    /// How long after the first read that says unlocked a gate a lock closed
    /// stays closed (security review 13/11/2026, Q2).
    static constexpr int64_t kSettleAfterLockUs = 1000000;

    /// What moved it last, for the log.
    enum class Why : int
    {
        Start,
        Desktop,  ///< the input desktop is not the user's own
        Locked,   ///< the session is locked, or not the active one
        Unknown,  ///< its state could not be read
        Unlocked, ///< open: unlocked, and the user's desktop
    };

    /// What a call of the watcher did, for it to act on.
    struct Change
    {
        /// Closed by this call: the viewer's held input is let go of.
        bool closed = false;
        /// Opened by this call: a fresh picture, sent as a key frame.
        bool opened = false;
        /// The session is to be read again at once.
        bool readNow = false;
    };

    explicit DesktopGate(int64_t settleAfterLockUs = kSettleAfterLockUs)
        : m_SettleUs(settleAfterLockUs)
    {}

    DesktopGate(const DesktopGate&) = delete;
    DesktopGate& operator=(const DesktopGate&) = delete;

    // ── Any thread ──────────────────────────────────────────────────────────

    uint64_t generation() const { return m_Generation.load(std::memory_order_acquire); }
    static bool isOpen(uint64_t generation) { return (generation & 1) != 0; }
    bool open() const { return isOpen(generation()); }
    /// How many times the gate had closed by @p generation.
    static uint64_t closes(uint64_t generation) { return generation / 2; }
    Why why() const { return static_cast<Why>(m_Why.load(std::memory_order_acquire)); }

    /// An event about to be injected: whether it may, the input desktop
    /// having just been read. One that is not the user's closes the gate.
    /// @p closedNow: set when this very call closed it.
    bool admit(bool onUserDesktop, bool* closedNow = nullptr)
    {
        if (closedNow) *closedNow = false;
        if (onUserDesktop) return open();
        const bool closed = closeIfOpen(Why::Desktop);
        if (closedNow) *closedNow = closed;
        return false;
    }

    /// A picture about to be encoded, or handed out: whether it may, when it
    /// was taken at @p pictureGeneration. Only while the gate is open and has
    /// not moved since.
    bool admitPicture(uint64_t pictureGeneration, bool onUserDesktop, bool* closedNow = nullptr)
    {
        if (!admit(onUserDesktop, closedNow)) return false;
        return generation() == pictureGeneration;
    }

    /// A frame of sound about to be encoded: whether the host's sound may go
    /// rather than silence. It goes with the picture (security review
    /// 13/11/2026): while the gate is open, once a picture of this very
    /// generation went out (@p pictureSentGeneration, the last one that did).
    bool soundMayGo(uint64_t pictureSentGeneration) const
    {
        const uint64_t g = generation();
        return isOpen(g) && pictureSentGeneration == g;
    }

    // ── The watcher's thread only ───────────────────────────────────────────

    /// A notification. @p ownSession false: about another session, whose word
    /// is never taken (security review 13/11/2026, Q1): it changes nothing,
    /// and has this session read again.
    Change notice(LockNotice notice, bool ownSession)
    {
        Change c;
        c.readNow = true;
        if (!ownSession || notice != LockNotice::Lock) return c;
        heldByLock();
        c.closed = closeIfOpen(Why::Locked);
        return c;
    }

    /// A read of the session's own state at @p nowUs, with the input desktop
    /// read at the same moment. The one call that opens the gate.
    Change read(LockRead lock, bool onUserDesktop, int64_t nowUs)
    {
        Change c;
        if (lock != LockRead::Unlocked) {
            heldByLock();
            c.closed = closeIfOpen(lock == LockRead::Locked ? Why::Locked : Why::Unknown);
            return c;
        }
        if (!onUserDesktop) {
            c.closed = closeIfOpen(Why::Desktop);
            return c;
        }
        if (open()) return c;
        if (m_HeldByLock && m_SettleUs > 0) {
            if (m_UnlockedSinceUs < 0) m_UnlockedSinceUs = nowUs;
            if (nowUs - m_UnlockedSinceUs < m_SettleUs) return c;
        }
        m_HeldByLock = false;
        m_UnlockedSinceUs = -1;
        c.opened = openIfClosed();
        return c;
    }

private:
    void heldByLock()
    {
        m_HeldByLock = true;
        m_UnlockedSinceUs = -1;
    }

    /// From any thread: open to closed, once. The reason goes first, so that
    /// whoever sees the new generation reads a reason to close; two closing
    /// together may leave either's.
    bool closeIfOpen(Why why)
    {
        uint64_t g = m_Generation.load(std::memory_order_acquire);
        while (isOpen(g)) {
            m_Why.store(static_cast<int>(why), std::memory_order_release);
            if (m_Generation.compare_exchange_weak(g, g + 1, std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
                return true;
        }
        return false;
    }

    /// From the watcher only, which is the only one that opens it: closed to
    /// open. The others only ever close it, which an open gate is not.
    bool openIfClosed()
    {
        uint64_t g = m_Generation.load(std::memory_order_acquire);
        if (isOpen(g)) return false;
        m_Why.store(static_cast<int>(Why::Unlocked), std::memory_order_release);
        return m_Generation.compare_exchange_strong(g, g + 1, std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
    }

    std::atomic<uint64_t> m_Generation{0};
    std::atomic<int> m_Why{static_cast<int>(Why::Start)};

    // The watcher's own.
    const int64_t m_SettleUs;
    bool m_HeldByLock = false;
    int64_t m_UnlockedSinceUs = -1;
};

/// The viewer's held input is let go of once per close of the gate, whoever
/// sees the close first: the input path at its next event, or the watcher at
/// once. Under the caller's lock (the session's input mutex).
class GateCloses
{
public:
    /// True once for each close made by @p generation since the last true.
    bool releaseDue(uint64_t generation)
    {
        const uint64_t closes = DesktopGate::closes(generation);
        if (closes == m_Released) return false;
        m_Released = closes;
        return true;
    }

private:
    uint64_t m_Released = 0;
};

} // namespace mw::native
