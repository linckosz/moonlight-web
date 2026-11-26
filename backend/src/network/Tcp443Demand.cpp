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

#include "Tcp443Demand.h"

namespace mw::tcp443 {

bool Demand::ask(Asker who, bool allowed, uint64_t nowMs, QList<Line>* out)
{
    if (who == Asker::Other || !allowed) return false;
    // Wanted already: an ask neither opens a second time nor keeps 443 open
    // any longer, only a browser connected through it does
    if (m_State != State::Idle) return false;
    if (nowMs < m_RetryNotBeforeMs) return false;
    if (who == Asker::Rendezvous) {
        while (!m_RendezvousOpenings.empty() && nowMs - m_RendezvousOpenings.front() >= kHourMs)
            m_RendezvousOpenings.pop_front();
        if (static_cast<int>(m_RendezvousOpenings.size()) >= kRendezvousPerHour) {
            if (out && nowMs >= m_BoundSaidUntilMs) {
                m_BoundSaidUntilMs = nowMs + kHourMs;
                out->append({true, QStringLiteral("TCP 443: %1 browsers asked for it through the "
                                                  "introduction server within the hour; more are "
                                                  "not taken for now")
                                       .arg(kRendezvousPerHour)});
            }
            return false;
        }
        m_RendezvousOpenings.push_back(nowMs);
    }
    m_State = State::Opening;
    m_GraceUntilMs = 0;
    m_OpeningSinceMs = nowMs;
    return true;
}

void Demand::arrived(uint64_t /*nowMs*/)
{
    ++m_Peers;
    m_GraceUntilMs = 0;
}

void Demand::left(uint64_t nowMs)
{
    if (m_Peers > 0) --m_Peers;
    if (m_Peers == 0 && m_State == State::Held) m_GraceUntilMs = nowMs + kGraceMs;
}

bool Demand::settled(bool held, uint64_t nowMs)
{
    switch (m_State) {
    case State::Idle: return false;
    case State::Opening:
        if (held) {
            m_State = State::Held;
            // Held with nobody on it yet: the grace runs from now, so that a
            // browser that never comes does not keep it
            m_GraceUntilMs = m_Peers == 0 ? nowMs + kGraceMs : 0;
            return false;
        }
        break;
    case State::Held:
        if (held) return false;
        break;
    }
    // Not held: not wanted any more, and not asked again for a while
    m_State = State::Idle;
    m_GraceUntilMs = 0;
    m_RetryNotBeforeMs = nowMs + kRetryMs;
    return true;
}

bool Demand::tick(uint64_t nowMs)
{
    // An ask the guard never answered does not hold every later one back
    if (m_State == State::Opening)
        return nowMs >= m_OpeningSinceMs + kOpeningMaxMs && settled(false, nowMs);
    if (m_State != State::Held || m_Peers > 0 || m_GraceUntilMs == 0 || nowMs < m_GraceUntilMs)
        return false;
    m_State = State::Idle;
    m_GraceUntilMs = 0;
    return true;
}

uint64_t Demand::dueAtMs() const
{
    if (m_State == State::Opening) return m_OpeningSinceMs + kOpeningMaxMs;
    return m_State == State::Held && m_Peers == 0 ? m_GraceUntilMs : 0;
}

bool Demand::off()
{
    if (m_State == State::Idle) return false;
    m_State = State::Idle;
    m_GraceUntilMs = 0;
    return true;
}

} // namespace mw::tcp443
