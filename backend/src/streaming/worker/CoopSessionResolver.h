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

#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

class IStreamBackend;

/**
 * @brief Reads back a co-op backend's own id for this worker's session (Wolf).
 *
 * On a backend with native co-op the session is keyed host-side by the client
 * certificate, with an id we cannot compute. The supervisor needs that id to
 * close the session when the stream ends: the GameStream /cancel cannot, because
 * it resolves the session from a certificate that is not the per-seat one this
 * launch used, so the session and the container behind it would otherwise
 * survive and pile up until the host is restarted.
 *
 * This recovers the id by asking the backend which of its live sessions carries
 * our launch key — the one field that cannot collide between two of our own
 * concurrent sessions — and reports it exactly once. It does nothing on a
 * backend without native co-op.
 *
 * Scope note: this used to also move the stream onto a shared lobby so an
 * invited player saw the owner's screen. That join is now done natively, inside
 * the host's own streamed UI (Wolf UI lists the multi-user lobbies and joins
 * them), so it was removed from here — only the id resolution the reaping
 * depends on remains. See docs/integration-multiseat-wolf.md.
 *
 * Lives in the worker rather than in StreamSession, which deletes itself as soon
 * as it has answered /start; this is parented to the application and outlives it.
 */
class CoopSessionResolver : public QObject
{
    Q_OBJECT

public:
    /// Reports the resolved backend session id exactly once. The supervisor
    /// records it so it can close the session host-side even if this process
    /// dies without a teardown — the only way a crashed worker stops leaking
    /// sessions and containers.
    using SessionIdReporter = std::function<void(const QString& sessionId)>;

    CoopSessionResolver(std::shared_ptr<IStreamBackend> backend, SessionIdReporter report,
                        QObject* parent = nullptr);

    /// Call once the stream is up. @p launchKey is the session's `rikey`, the
    /// key the backend stored so we can tell OUR session from every other one.
    void begin(const QByteArray& launchKey);

private:
    std::shared_ptr<IStreamBackend> m_Backend;
    SessionIdReporter m_Report;
    bool m_Begun = false;
};
