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

#include "CoopLobbyJoiner.h"

#include "../MoonlightShim.h"
#include "../../backend/streambackend/IStreamBackend.h"

#include <QPointer>
#include <QDebug>

CoopLobbyJoiner::CoopLobbyJoiner(std::shared_ptr<IStreamBackend> backend, QString peerSessionId,
                                 SessionIdReporter report, QObject* parent)
    : QObject(parent), m_Backend(std::move(backend)), m_PeerSessionId(std::move(peerSessionId)),
      m_Report(std::move(report))
{
}

void CoopLobbyJoiner::begin(const QByteArray& launchKey, MoonlightShim* shim)
{
    if (m_Begun) return;
    m_Begun = true;

    if (!m_Backend || !m_Backend->capabilities().lobbies) return;

    // Arm the trigger before asking who we are. The lookup is a round trip to
    // the host and the first frame can beat it; arming first means the frame is
    // recorded rather than missed, and join() simply runs when both are in.
    armJoin(shim);

    QPointer<CoopLobbyJoiner> self(this);
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
            self->m_SessionId = sessionId;
            if (self->m_Report) self->m_Report(sessionId);
            self->join();
        });
}

void CoopLobbyJoiner::armJoin(MoonlightShim* shim)
{
    if (m_PeerSessionId.isEmpty() || !shim) return;

    // A single-shot connection: the cost is one extra copy of one frame for the
    // whole stream, which is what buying certainty about the host's pipeline is
    // worth. Anything cheaper — a timer, the connection-established callback —
    // is a guess about how long the host takes to build it.
    connect(
        shim, &MoonlightShim::videoFrameReady, this,
        [this](const QByteArray&, int, int, qint64) {
            qInfo() << "[Coop] First frame decoded — the host's pipeline is live";
            m_SawFirstFrame = true;
            join();
        },
        Qt::SingleShotConnection);
}

void CoopLobbyJoiner::join()
{
    if (m_PeerSessionId.isEmpty() || m_SessionId.isEmpty() || !m_SawFirstFrame) return;
    if (m_Joined) return;
    m_Joined = true;

    QPointer<CoopLobbyJoiner> self(this);
    m_Backend->findCoopLobby(
        m_PeerSessionId, [self](bool ok, const BackendError& err, const QString& lobbyId) {
            if (!self) return;
            if (!ok) {
                qWarning() << "[Coop] Could not look up the host's lobby:" << err.message;
                self->m_Joined = false;
                return;
            }
            if (lobbyId.isEmpty()) {
                // The single most likely outcome in practice, and not a bug:
                // the shared lobby only exists once the person at the host has
                // started co-op from the host's own UI. We deliberately do not
                // create one for them — that would duplicate the host's own
                // co-op flow and take the decision out of their hands.
                qWarning() << "[Coop] The host is not on a shareable lobby — nobody has started "
                              "co-op there. This player stays on their own screen.";
                self->m_Joined = false;
                return;
            }
            self->m_Backend->joinCoopLobby(
                lobbyId, self->m_SessionId, [self, lobbyId](bool ok2, const BackendError& err2) {
                    if (!self) return;
                    if (!ok2) {
                        // The player keeps streaming — they are simply looking
                        // at their own screen instead of the shared one. Say
                        // which, so the log does not read like a dead session.
                        qWarning() << "[Coop] Join of lobby" << lobbyId
                                   << "was refused:" << err2.message
                                   << "— this player stays on their own screen";
                        self->m_Joined = false;
                        return;
                    }
                    qInfo() << "[Coop] Joined lobby" << lobbyId << "as session"
                            << self->m_SessionId;
                });
        });
}
