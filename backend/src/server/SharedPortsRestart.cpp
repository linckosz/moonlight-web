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

#include "server/SharedPortsRestart.h"

#include "common/Logger.h"

#include <utility>

SharedPortsRestart* SharedPortsRestart::s_Shared = nullptr;

SharedPortsRestart::Step SharedPortsRestart::decide(bool wished, bool fileWantsShared,
                                                    bool sharedRun, bool busy)
{
    if (!wished || fileWantsShared == sharedRun) return Step::Nothing;
    return busy ? Step::Wait : Step::Restart;
}

SharedPortsRestart::SharedPortsRestart(Probes probes, int lookEveryMs, QObject* parent)
    : QObject(parent)
    , m_Probes(std::move(probes))
{
    m_Timer.setInterval(lookEveryMs);
    connect(&m_Timer, &QTimer::timeout, this, &SharedPortsRestart::look);
}

void SharedPortsRestart::asked()
{
    m_Wished = true;
    look();
}

void SharedPortsRestart::look()
{
    // Dropped first, the file read again: what is done is what it says now.
    const bool wished = std::exchange(m_Wished, false);
    switch (decide(wished, m_Probes.fileWantsShared(), m_Probes.sharedRun(), m_Probes.busy())) {
    case Step::Nothing:
        m_Timer.stop();
        m_SaidWaiting = false;
        return;
    case Step::Wait:
        m_Wished = true;
        if (!m_SaidWaiting) {
            Logger::info(QStringLiteral("[System] Restart to apply \"One shared port for "
                                        "streams\" waits for the streams and pairings to end"));
            m_SaidWaiting = true;
        }
        if (!m_Timer.isActive()) m_Timer.start();
        return;
    case Step::Restart:
        m_Timer.stop();
        m_SaidWaiting = false;
        // Not done (no new copy could be started): not tried again by itself;
        // the next write of the box asks again.
        m_Probes.restart();
        return;
    }
}
