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

#include "CoopSessionResolver.h"

#include "../../backend/streambackend/IStreamBackend.h"

#include <QPointer>
#include <QDebug>

CoopSessionResolver::CoopSessionResolver(std::shared_ptr<IStreamBackend> backend,
                                         SessionIdReporter report, QObject* parent)
    : QObject(parent), m_Backend(std::move(backend)), m_Report(std::move(report))
{
}

void CoopSessionResolver::begin(const QByteArray& launchKey)
{
    if (m_Begun) return;
    m_Begun = true;

    if (!m_Backend || !m_Backend->capabilities().lobbies) return;

    QPointer<CoopSessionResolver> self(this);
    m_Backend->resolveCoopSessionId(
        launchKey, [self](bool ok, const BackendError& err, const QString& sessionId) {
            if (!self) return;
            if (!ok) {
                qWarning() << "[Coop] Could not ask the host for our session id:" << err.message;
                return;
            }
            if (sessionId.isEmpty()) {
                qWarning() << "[Coop] The host reports no session for this launch key — it may "
                              "have ended already";
                return;
            }
            if (self->m_Report) self->m_Report(sessionId);
        });
}
