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

#include "SessionPool.h"

SessionPool::SessionPool(quint16 signalingBasePort, quint16 extraSlotBasePort, int reservedSlots,
                         int maxSlots)
    : m_SignalingBase(signalingBasePort)
    , m_ExtraSlotBase(extraSlotBasePort)
    , m_Reserved(reservedSlots < 0 ? 0 : reservedSlots)
    , m_Max(maxSlots < m_Reserved ? m_Reserved : maxSlots)
{
    // The reserved slots always exist: they are addressed by index, never
    // acquired, so they must be there before the first stream starts.
    m_Slots.resize(m_Reserved);
}

quint16 SessionPool::signalingPort(int index) const
{
    // Slot 0 keeps the historical base (its relay is the port above, the
    // control channel the one after that). The rest live two ports apart in
    // their own block, which the caller placed clear of the media range.
    if (index <= 0) return m_SignalingBase;
    return static_cast<quint16>(m_ExtraSlotBase + 2 * (index - 1));
}

QString SessionPool::wsPath(int index)
{
    return index == 0 ? QStringLiteral("/ws") : QStringLiteral("/ws%1").arg(index);
}

bool SessionPool::live(int index) const
{
    return isValid(index) && !m_Slots[index].worker.isNull();
}

int SessionPool::liveCount() const
{
    int n = 0;
    for (const Slot& s : m_Slots)
        if (!s.worker.isNull()) n++;
    return n;
}

bool SessionPool::anyOtherLive(int index) const
{
    for (int i = 0; i < m_Slots.size(); ++i)
        if (i != index && !m_Slots[i].worker.isNull()) return true;
    return false;
}

bool SessionPool::anyOtherLiveOnHost(int index, const QString& hostUuid) const
{
    for (int i = 0; i < m_Slots.size(); ++i) {
        if (i == index || m_Slots[i].worker.isNull()) continue;
        // A slot with no host recorded yet is still setting up; treat it as
        // "same host" so the conservative answer wins and no /cancel slips
        // through underneath it.
        if (m_Slots[i].hostUuid.isEmpty() || m_Slots[i].hostUuid == hostUuid) return true;
    }
    return false;
}

QVector<int> SessionPool::slotsOnHost(const QString& hostUuid, int firstSlots) const
{
    QVector<int> found;
    const int n = qMin(firstSlots, int(m_Slots.size()));
    for (int i = 0; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (s.hostUuid == hostUuid || (s.hostUuid.isEmpty() && !s.worker.isNull())) found.append(i);
    }
    return found;
}

// A slot counts as taken while it has a live worker OR still carries the
// identity of a session being set up — the worker is attached slightly after
// the slot is claimed, and handing the same index out twice in that window
// would collide on ports and on the WebSocket path.
bool SessionPool::inUse(int index) const
{
    const Slot& s = m_Slots[index];
    return !s.worker.isNull() || !s.clientUniqueId.isEmpty() || !s.sessionToken.isEmpty();
}

void SessionPool::setOnSlotCreated(std::function<void(int)> fn)
{
    m_OnSlotCreated = std::move(fn);
}

int SessionPool::appendSlot()
{
    if (m_Slots.size() >= m_Max) return -1;

    m_Slots.append(Slot{});
    const int index = m_Slots.size() - 1;
    // Announce before returning: the caller wires the slot's proxy route here,
    // and acquire() may hand this very index out on the next line.
    if (m_OnSlotCreated) m_OnSlotCreated(index);
    return index;
}

int SessionPool::acquire()
{
    // A released slot comes back before a new one is created: indices are part
    // of the contract with the frontend (/wsN) and stay as few as possible.
    for (int i = m_Reserved; i < m_Slots.size(); ++i) {
        if (!inUse(i)) return i;
    }
    return appendSlot();
}

void SessionPool::release(int index)
{
    if (!isValid(index)) return;
    m_Slots[index] = Slot{};
}

int SessionPool::indexOfClientUniqueId(const QString& clientUniqueId) const
{
    if (clientUniqueId.isEmpty()) return -1;
    for (int i = 0; i < m_Slots.size(); ++i)
        if (m_Slots[i].clientUniqueId == clientUniqueId) return i;
    return -1;
}

int SessionPool::indexOfSessionToken(const QString& sessionToken) const
{
    if (sessionToken.isEmpty()) return -1;
    for (int i = 0; i < m_Slots.size(); ++i)
        if (m_Slots[i].sessionToken == sessionToken) return i;
    return -1;
}

bool SessionPool::streamOf(int index, const QString& sessionToken) const
{
    if (!isValid(index)) return false;
    const QString& owner = m_Slots[index].sessionToken;
    return sessionToken.isEmpty() || owner.isEmpty() || owner == sessionToken;
}

int SessionPool::ownerSlotFor(const QString& sessionToken, const QString& wantedHost,
                              int ownerSlots) const
{
    const int n = qMin(ownerSlots, m_Slots.size());
    auto streams = [&](int i) {
        return !m_Slots[i].worker.isNull() && !m_Slots[i].hostUuid.isEmpty() &&
               (wantedHost.isEmpty() || m_Slots[i].hostUuid == wantedHost);
    };
    for (int i = 0; i < n; ++i)
        if (streams(i) && streamOf(i, sessionToken)) return i;
    for (int i = 0; i < n; ++i)
        if (streams(i)) return i;
    return -1;
}

bool SessionPool::hostHeldByAnother(const QString& hostUuid, const QString& sessionToken,
                                    const QString& clientUniqueId, int ownerSlots) const
{
    const int n = qMin(ownerSlots, m_Slots.size());
    for (int i = 0; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (s.worker.isNull() || s.hostUuid != hostUuid) continue;
        const bool otherSession = !streamOf(i, sessionToken);
        const bool otherDevice = !clientUniqueId.isEmpty() && !s.clientUniqueId.isEmpty() &&
                                 s.clientUniqueId != clientUniqueId;
        if (otherSession || otherDevice) return true;
    }
    return false;
}

bool SessionPool::quitReachesAppSlot(int index, const QString& hostUuid,
                                     const QString& clientUniqueId, const QString& sessionToken,
                                     int namedSlot) const
{
    if (index < 0 || index >= m_Slots.size()) return false;
    const Slot& s = m_Slots[index];
    if (s.worker.isNull() || s.hostUuid != hostUuid) return false;
    if (namedSlot >= 0 && namedSlot != index) return false;
    if (!sessionToken.isEmpty() && !s.sessionToken.isEmpty() && sessionToken != s.sessionToken)
        return false;
    if (!clientUniqueId.isEmpty() && s.clientUniqueId == clientUniqueId) return true;
    // The authenticated session is the stronger proof: a browser picks its
    // own uniqueid. It needs the slot named, and a token on both sides.
    return namedSlot == index && !sessionToken.isEmpty() && s.sessionToken == sessionToken;
}

QVector<int> SessionPool::slotsOfSession(
    const std::function<bool(const QString& sessionToken)>& isThatSession) const
{
    QVector<int> found;
    if (!isThatSession) return found;
    for (int i = 0; i < m_Slots.size(); ++i) {
        const Slot& s = m_Slots[i];
        if (!s.worker.isNull() && !s.sessionToken.isEmpty() && isThatSession(s.sessionToken))
            found.append(i);
    }
    return found;
}
