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

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

// Bookkeeping for the concurrent stream worker slots.
//
// A slot index is not an implementation detail: it picks the slot's signaling
// port and the WebSocket path the browser connects to (/ws, /ws1, /ws2…), so
// indices are part of the contract with the frontend and are reused rather
// than handed out freely.
//
// Port layout. Slot 0 keeps its historical pair — the signaling base and the
// port above it — with the control channel just after. Every other slot takes
// two consecutive ports from a second base, so the whole range stays compact:
// the old "base + 10 * index" spacing spent ten ports to use two, ran into the
// media range after a couple of slots, and hit the ports reserved above 48100
// at slot ten. Caller picks that second base; see planSlotPorts() in main.cpp,
// which places it clear of the media block and of the streaming servers' ports.
//
// The first `reservedSlots` indices keep their historical meaning — 0 is the
// owner's primary stream, 1 the standby used for seamless quality switching —
// and are addressed directly. Everything above is allocated through acquire(),
// which is what lets invited players today, and independent per-device sessions
// tomorrow, share the same pool.
//
// The worker is held as QPointer<QObject> on purpose: it keeps this unit free
// of the streaming/WebRTC dependency tree so it can be unit-tested. Callers
// use workerAs<StreamWorkerHost>().
class SessionPool
{
public:
    struct Slot
    {
        QPointer<QObject> worker;
        QString clientUniqueId;
        QString hostUuid;
        // The authenticated device behind this stream (AuthManager session).
        // Empty for localhost, which has no session row — and every scoping
        // rule treats empty as "unknown, keep legacy behaviour".
        QString sessionToken;
        int appId = 0; ///< app this slot streams — players resume into it

        // Multi-backend routing. Unused by the GameStream path; filled once
        // seats come from MultiSeat or Wolf.
        QString backendType;
        QString seatId;

        // What a co-op backend called the session behind this slot, reported by
        // the worker once the host answered. Kept here rather than in the
        // worker because closing that session is the supervisor's job: the
        // worker dying without a teardown is precisely the case that leaks it.
        QString coopSessionId;
    };

    /// @param signalingBasePort slot 0's signaling port (its relay takes the
    ///        next one, the control channel the one after).
    /// @param extraSlotBasePort signaling port of slot 1; every slot above it
    ///        follows two ports apart. Must be placed clear of the media range.
    SessionPool(quint16 signalingBasePort, quint16 extraSlotBasePort, int reservedSlots,
                int maxSlots);

    int size() const { return m_Slots.size(); }
    int reservedSlots() const { return m_Reserved; }
    int maxSlots() const { return m_Max; }

    bool isValid(int index) const { return index >= 0 && index < m_Slots.size(); }
    Slot& at(int index) { return m_Slots[index]; }
    const Slot& at(int index) const { return m_Slots[index]; }

    // Range-for over every slot, reserved ones included.
    QVector<Slot>::iterator begin() { return m_Slots.begin(); }
    QVector<Slot>::iterator end() { return m_Slots.end(); }
    QVector<Slot>::const_iterator begin() const { return m_Slots.begin(); }
    QVector<Slot>::const_iterator end() const { return m_Slots.end(); }

    // Typed view of the worker. The slot stores a QObject so this unit stays
    // testable; callers that need the real type go through here.
    template <class T> T* workerAs(int index) const
    {
        return isValid(index) ? qobject_cast<T*>(m_Slots[index].worker.data()) : nullptr;
    }

    // Slot 0: the signaling base. Slot N: extraSlotBase + 2 * (N - 1). The
    // relay always takes the next port up.
    quint16 signalingPort(int index) const;
    static QString wsPath(int index);

    bool live(int index) const;
    int liveCount() const;
    // True when ANY slot other than @p index still has a live worker. Sessions
    // on one host share a single running app, so a /cancel while this holds
    // would end the game for whoever is left.
    bool anyOtherLive(int index) const;
    // Same question, restricted to one host. "Sessions share a running app" is
    // only true per host: once independent sessions stream different hosts, a
    // live slot on host B must not suppress the /cancel that ends host A's app,
    // or that app keeps running with nobody attached.
    bool anyOtherLiveOnHost(int index, const QString& hostUuid) const;

    // The slots among the first @p firstSlots that stream @p hostUuid: what
    // ending that host's session takes down, and nothing streaming another
    // host. Both legs of a dual stream are on the same host, so they go
    // together. A live slot with no host recorded yet is still setting up and
    // counts as this host's, as in anyOtherLiveOnHost.
    QVector<int> slotsOnHost(const QString& hostUuid, int firstSlots) const;

    // Lowest free index at or above reservedSlots, reusing released ones.
    // Returns -1 when every slot up to maxSlots is taken.
    int acquire();

    // Called once for every slot the pool creates. Reserved slots never reach
    // it — they exist before anyone can install this — so wire it before the
    // first acquire(): it is where the caller registers the slot's proxy route.
    //
    // No slot is prepared ahead of need, and none has to be: acquire() runs
    // inside the launch request, so the route is registered before the response
    // that tells the browser which /wsN to open. Nothing here is asynchronous,
    // and a slot costs nothing until a worker is attached to it.
    void setOnSlotCreated(std::function<void(int slot)> fn);
    // Clears the slot so acquire() can hand the index out again. Does not touch
    // the worker object itself — ownership stays with the caller.
    void release(int index);

    int indexOfClientUniqueId(const QString& clientUniqueId) const;
    // Slot held by an authenticated device (its AuthManager session token).
    int indexOfSessionToken(const QString& sessionToken) const;

    // Whether the live slot @p index is the stream of @p sessionToken. Either
    // side empty is "unknown" (localhost has no session row) and answers yes,
    // like every other scoping rule here.
    bool streamOf(int index, const QString& sessionToken) const;

    // The owner stream a request from @p sessionToken is about, among the
    // first @p ownerSlots: its own live slot on @p wantedHost (on any host when
    // that is empty), else — so a page that names no host keeps its answer —
    // the first live slot on @p wantedHost, or on any host. -1 when none
    // streams there. Slots with no host never count.
    int ownerSlotFor(const QString& sessionToken, const QString& wantedHost, int ownerSlots) const;

    // Whether another device holds a live slot on @p hostUuid among the first
    // @p ownerSlots: one whose stream is not @p sessionToken's, or whose device
    // (@p clientUniqueId) is another. Either side empty counts as the same.
    bool hostHeldByAnother(const QString& hostUuid, const QString& sessionToken,
                           const QString& clientUniqueId, int ownerSlots) const;

    // Whether a Stop from one device ends the live slot @p index, one past
    // the owner slots: the stream of another app, or of a second device on a
    // multi-user host. It must be on @p hostUuid, be the slot the page named
    // (@p namedSlot; any when negative), and never another session's. Then it
    // is this device's when the uniqueid matches, or when the page named this
    // very slot from the session that started it: the page derives its quit
    // id from the slot, so that id alone can miss the one the launch carried.
    bool quitReachesAppSlot(int index, const QString& hostUuid, const QString& clientUniqueId,
                            const QString& sessionToken, int namedSlot) const;

    // The live slots that stream for one session, the one @p isThatSession
    // recognises from a slot's token. A slot with no token (localhost, an
    // invited player) is no session's, and never in the list.
    QVector<int>
    slotsOfSession(const std::function<bool(const QString& sessionToken)>& isThatSession) const;

private:
    bool inUse(int index) const;
    // Appends one slot and announces it. Returns its index, or -1 at maxSlots.
    int appendSlot();

    QVector<Slot> m_Slots;
    quint16 m_SignalingBase;
    quint16 m_ExtraSlotBase;
    int m_Reserved;
    int m_Max;
    std::function<void(int)> m_OnSlotCreated;
};
