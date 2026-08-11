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

SessionPool::SessionPool(quint16 signalingBasePort, int reservedSlots, int maxSlots)
    : m_SignalingBase(signalingBasePort)
    , m_Reserved(reservedSlots < 0 ? 0 : reservedSlots)
    , m_Max(maxSlots < m_Reserved ? m_Reserved : maxSlots)
{
    // The reserved slots always exist: they are addressed by index, never
    // acquired, so they must be there before the first stream starts.
    m_Slots.resize(m_Reserved);
}

quint16 SessionPool::signalingPort(int index) const
{
    return static_cast<quint16>(m_SignalingBase + 10 * index);
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

// A slot counts as taken while it has a live worker OR still carries the
// identity of a session being set up — the worker is attached slightly after
// the slot is claimed, and handing the same index out twice in that window
// would collide on ports and on the WebSocket path.
bool SessionPool::inUse(int index) const
{
    const Slot& s = m_Slots[index];
    return !s.worker.isNull() || !s.clientUniqueId.isEmpty() || !s.sessionToken.isEmpty();
}

int SessionPool::acquire()
{
    for (int i = m_Reserved; i < m_Slots.size(); ++i) {
        if (!inUse(i)) return i;
    }

    if (m_Slots.size() >= m_Max) return -1;

    m_Slots.append(Slot{});
    return m_Slots.size() - 1;
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
