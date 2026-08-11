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

// Bookkeeping for the concurrent stream worker slots.
//
// A slot index is not an implementation detail: it picks the signaling port
// (base + 10 * index) and the WebSocket path the browser connects to (/ws,
// /ws1, /ws2…), so indices are part of the contract with the frontend and are
// reused rather than handed out freely.
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
    };

    SessionPool(quint16 signalingBasePort, int reservedSlots, int maxSlots);

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

    // signaling = base + 10 * index; the relay takes the next port up.
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

    // Lowest free index at or above reservedSlots, reusing released ones.
    // Returns -1 when every slot up to maxSlots is taken.
    int acquire();
    // Clears the slot so acquire() can hand the index out again. Does not touch
    // the worker object itself — ownership stays with the caller.
    void release(int index);

    int indexOfClientUniqueId(const QString& clientUniqueId) const;
    // Slot held by an authenticated device (its AuthManager session token).
    int indexOfSessionToken(const QString& sessionToken) const;

private:
    bool inUse(int index) const;

    QVector<Slot> m_Slots;
    quint16 m_SignalingBase;
    int m_Reserved;
    int m_Max;
};
