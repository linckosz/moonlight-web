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
#include <QTimer>
#include <QDebug>

CoopSessionResolver::CoopSessionResolver(std::shared_ptr<IStreamBackend> backend,
                                         SessionIdReporter report, QObject* parent)
    : QObject(parent)
    , m_Backend(std::move(backend))
    , m_Report(std::move(report))
{}

void CoopSessionResolver::begin(const QByteArray& launchKey)
{
    if (m_Begun) return;
    m_Begun = true;

    if (!m_Backend || !m_Backend->capabilities().lobbies) return;

    m_LaunchKey = launchKey;
    attempt();
}

void CoopSessionResolver::attempt()
{
    QPointer<CoopSessionResolver> self(this);
    m_Backend->resolveCoopSessionId(m_LaunchKey, [self](bool ok, const BackendError& err,
                                                        const QString& sessionId) {
        if (!self) return;

        // Landed: report once and stop. A stable answer never changes for the
        // life of the stream, so there is nothing to keep polling for.
        if (ok && !sessionId.isEmpty()) {
            if (self->m_Report) self->m_Report(sessionId);
            return;
        }

        // A failed call and an empty answer are both "not yet": the session
        // may still be registering, or the request was dropped in the proxy.
        // Keep trying on a cadence — a single miss is exactly what used to
        // leave the session unreaped — until it lands or the budget runs out.
        if (++self->m_Attempts >= kMaxAttempts) {
            if (!ok)
                qWarning() << "[Coop] Gave up asking the host for our session id after"
                           << self->m_Attempts << "tries; last error:" << err.message;
            else
                qWarning() << "[Coop] No session matched our launch key after" << self->m_Attempts
                           << "tries — it likely never started, or "
                              "ended before we could see it";
            return;
        }
        QTimer::singleShot(kRetryMs, self, [self]() {
            if (self) self->attempt();
        });
    });
}
