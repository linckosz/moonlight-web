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
class MoonlightShim;

/**
 * @brief Puts one worker's stream onto a shared co-op lobby (Wolf).
 *
 * On a backend with native co-op, a second GameStream launch does NOT give a
 * second view of the first screen: it opens a second, private desktop with its
 * own compositor, so an invited player would look at their own machine. What
 * makes two people see the same thing is a *lobby*, which owns the compositor
 * and runs the app once; a session joins it and its stream producer is re-routed
 * onto the lobby's output.
 *
 * Two rules shape this class, both learned from the host's source rather than
 * guessed, and both cheap to get wrong:
 *
 *  1. **The join must come after video is flowing.** The re-route is an event
 *     handled by the session's video pipeline, and the host only builds that
 *     pipeline once the client's first RTP ping arrives. A join sent any earlier
 *     is accepted, recorded, and has no effect on the picture — the player keeps
 *     watching their own screen and nothing reports an error. So the trigger is
 *     the first decoded frame, which is proof the pipeline exists.
 *
 *  2. **The session id has to be read back.** It is derived host-side from the
 *     client certificate, so it cannot be computed here. It is recovered by
 *     asking the backend which of its live sessions carries our launch key —
 *     the one field that cannot collide between two of our own sessions.
 *
 * This lives in the worker rather than in StreamSession because StreamSession
 * deletes itself as soon as it has answered /start, long before any of this is
 * done. The joiner is parented to the application and survives the whole stream.
 */
class CoopLobbyJoiner : public QObject
{
    Q_OBJECT

public:
    /// Reports the resolved backend session id exactly once. The supervisor
    /// records it so it can close the session host-side even if this process
    /// dies without a teardown — which is the only way a crashed worker stops
    /// leaking sessions and containers.
    using SessionIdReporter = std::function<void(const QString& sessionId)>;

    /// @param peerSessionId The host-side session of the person whose lobby we
    ///                      join — the owner. Empty for the owner themselves,
    ///                      who is already on it and has nothing to join. The
    ///                      lobby is looked up from it at the moment of joining
    ///                      rather than passed in, so a lobby that appears (or
    ///                      disappears) between the invitation and the first
    ///                      frame is seen as it really is.
    CoopLobbyJoiner(std::shared_ptr<IStreamBackend> backend, QString peerSessionId,
                    SessionIdReporter report, QObject* parent = nullptr);

    /// Call once the stream is up. @p launchKey is the session's `rikey`,
    /// @p shim the live bridge whose first frame arms the join.
    void begin(const QByteArray& launchKey, MoonlightShim* shim);

private:
    void armJoin(MoonlightShim* shim);
    void join();

    std::shared_ptr<IStreamBackend> m_Backend;
    QString m_PeerSessionId;
    SessionIdReporter m_Report;
    QString m_SessionId;
    bool m_Begun = false;
    /// The two preconditions for the join, which arrive in either order: the
    /// host has named our session, and it is provably producing video.
    bool m_SawFirstFrame = false;
    bool m_Joined = false;
};
