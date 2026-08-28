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

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

/// One entry of GET /api/v1/pair/pending (wolf::api::PendingPairClient).
struct WolfPendingPair
{
    QString pairSecret;
    QString clientIp;
};

/// One entry of GET /api/v1/clients (wolf::api::PairedClient). `appStateFolder`
/// is Wolf's per-client isolation root — hash(client_cert), see the integration
/// doc §3: it is the *client* identity, not the user-facing profile.
struct WolfPairedClient
{
    QString clientId;
    QString appStateFolder;
};

/// One entry of GET /api/v1/sessions (reflected wolf::core::events::StreamSession).
/// `sessionId` is the value Wolf exposes as `client_id` — reflectors.hpp sets it to
/// `std::to_string(session.session_id)`, and `session_id == std::hash(client_cert)`
/// (config.hpp:get_client_id). It is exactly the `moonlight_session_id` that
/// /lobbies/join expects. `aesKey` is the `rikey` the launcher put in the GameStream
/// launch URL (endpoints.hpp:create_run_session), so a caller that generated its own
/// rikey can single out its own session unambiguously — every other field
/// (client_ip, resolution) can collide between concurrent MoonlightWeb sessions.
struct WolfStreamSession
{
    QString sessionId; ///< == the reflected `client_id`; pass this to joinLobby
    QString clientIp;
    QString aesKey; ///< == the launch rikey; the only collision-free match key
    QString appId;
    int width = 0;
    int height = 0;
    int fps = 0;
};

/// One entry of GET /api/v1/lobbies (reflected wolf::core::events::Lobby). A lobby
/// owns the shared Wayland compositor + runner; `connectedSessions` holds the
/// `moonlight_session_id`s currently switched onto it (sessions/lobbies.cpp).
struct WolfLobby
{
    QString id;
    QString name;
    bool multiUser = false;
    bool pinRequired = false;
    QString startedByProfileId;
    QStringList connectedSessions;
};

/// Why a Wolf API call failed. Deliberately transport-level: WolfBackend maps
/// these onto BackendError so the interface layer owns the user-facing wording.
struct WolfApiError
{
    enum Kind
    {
        None,
        Unreachable, ///< never got an answer
        Timeout,
        HttpError, ///< answered, but not 2xx
        Protocol,  ///< 2xx whose body did not parse, or success == false
    };

    Kind kind = None;
    int httpStatus = 0;
    QString message;

    static WolfApiError make(Kind k, const QString& msg, int status = 0)
    {
        return WolfApiError{k, status, msg};
    }
};

using WolfVoidCallback = std::function<void(bool ok, const WolfApiError& error)>;
using WolfPendingPairsCallback = std::function<void(bool ok, const WolfApiError& error,
                                                    const QVector<WolfPendingPair>& pending)>;
using WolfPairedClientsCallback = std::function<void(bool ok, const WolfApiError& error,
                                                     const QVector<WolfPairedClient>& clients)>;
using WolfJsonArrayCallback =
    std::function<void(bool ok, const WolfApiError& error, const QJsonArray& items)>;
using WolfSessionsCallback = std::function<void(bool ok, const WolfApiError& error,
                                                const QVector<WolfStreamSession>& sessions)>;
using WolfLobbiesCallback =
    std::function<void(bool ok, const WolfApiError& error, const QVector<WolfLobby>& lobbies)>;

/**
 * @brief Client for Wolf's control API (`/api/v1`).
 *
 * Wolf serves this API on a **Unix socket** (`WOLF_SOCKET_PATH`) and has no
 * authentication of its own: anything that can open the socket is fully
 * privileged. It is therefore never exposed as-is. MoonlightWeb reaches it
 * through a local, authenticated reverse proxy that fronts the socket, so this
 * client speaks plain HTTP to a configurable base URL and sends an optional
 * bearer token. Pointing it at a raw, unauthenticated TCP exposure of the Wolf
 * socket would hand the network root-equivalent control of the host — see the
 * risks section of docs/integration-multiseat-wolf.md.
 *
 * The one call that carries real weight is submitPin(). Wolf's GameStream
 * pairing phase 1 fires a PairSignal holding an *unresolved* `user_pin` promise
 * and defers the HTTP response until someone resolves it (rest/endpoints.hpp).
 * Normally a human types the PIN into Wolf's web UI. But MoonlightWeb *is* the
 * Moonlight client, so it already knows the PIN it would have displayed: it can
 * post the PIN itself and pair with no human in the loop. That is the same
 * trick SunshineRestClient::sendPin() plays on Sunshine's `POST /api/pin`.
 *
 * Every call is asynchronous and reports (ok, error, payload); nothing here
 * spins a nested event loop.
 */
class WolfApiClient : public QObject
{
    Q_OBJECT

public:
    /// @param baseUrl   Root of the proxied API, e.g. "http://192.168.1.9:8080".
    ///                  A trailing "/api/v1" is appended by this client.
    /// @param authToken Sent as `Authorization: Bearer <token>` when non-empty.
    WolfApiClient(QString baseUrl, QString authToken, QNetworkAccessManager* nam,
                  QObject* parent = nullptr);

    QString baseUrl() const { return m_BaseUrl; }

    /// GET /api/v1/pair/pending — pairing handshakes currently parked on an
    /// unresolved PIN promise.
    void pendingPairRequests(WolfPendingPairsCallback cb);

    /// POST /api/v1/pair/client {pair_secret, pin} — resolves the promise and
    /// unblocks the client's phase-1 response.
    void submitPin(const QString& pairSecret, const QString& pin, WolfVoidCallback cb);

    /// GET /api/v1/clients
    void pairedClients(WolfPairedClientsCallback cb);

    /// POST /api/v1/unpair/client {client_id}
    void unpairClient(const QString& clientId, WolfVoidCallback cb);

    /// GET /api/v1/apps — Wolf's *catalogue*, for the admin surface only.
    /// A player's app list must keep coming from Wolf UI over GameStream, or
    /// the profile/PIN/catalogue/lobby experience is bypassed (integration doc
    /// §4bis). Returned as raw JSON: the shape is reflection-generated upstream
    /// and would drift under a typed mirror.
    void listApps(WolfJsonArrayCallback cb);

    /// GET /api/v1/profiles — a profile is the user-facing isolation unit.
    void listProfiles(WolfJsonArrayCallback cb);

    /// GET /api/v1/lobbies — native co-op; only meaningful on Wolf.
    void listLobbies(WolfJsonArrayCallback cb);

    /// GET /api/v1/lobbies, parsed. Each lobby's `connectedSessions` names the
    /// moonlight sessions switched onto its shared compositor — how a joiner finds
    /// the owner's lobby.
    void lobbies(WolfLobbiesCallback cb);

    /// GET /api/v1/sessions, parsed. Used to resolve a caller's own
    /// `moonlight_session_id` by matching `aesKey` against its launch rikey.
    void listSessions(WolfSessionsCallback cb);

    /// POST /api/v1/lobbies/join {lobby_id, moonlight_session_id, pin?} — switches
    /// an already-running session's stream producer onto the lobby's shared
    /// compositor (sessions/lobbies.cpp). The session must exist first.
    ///
    /// **Timing.** The switch is an event the *video pipeline* handles, and Wolf
    /// only builds that pipeline after the client's first RTP video ping
    /// (sessions/moonlight.cpp waits on it, streaming.cpp registers the handler
    /// while constructing it). Joining before the pipeline exists is silently
    /// lost — the lobby records the session and moves the input devices over, but
    /// the interpipesrc keeps listening to the joiner's own compositor, so the
    /// player watches their own screen and nothing reports a failure. Call this
    /// only once frames are actually flowing.
    void joinLobby(const QString& lobbyId, const QString& moonlightSessionId,
                   const QVector<int>& pin, WolfVoidCallback cb);

    /// POST /api/v1/lobbies/leave {lobby_id, moonlight_session_id} — switches the
    /// session back to its own compositor; empties (and, if
    /// stop_when_everyone_leaves, stops) the lobby when it was the last one.
    ///
    /// Not needed to end a stream: stopSession() takes the session out of its
    /// lobby too (lobbies.cpp routes StopStreamEvent to the same handler). This
    /// is for going back to one's own Wolf UI *while staying connected*.
    void leaveLobby(const QString& lobbyId, const QString& moonlightSessionId, WolfVoidCallback cb);

    /// POST /api/v1/lobbies/stop {lobby_id, pin?} — tears the lobby down, ejecting
    /// every connected session.
    void stopLobby(const QString& lobbyId, const QVector<int>& pin, WolfVoidCallback cb);

    /// POST /api/v1/sessions/stop {session_id} — ends one streaming session host
    /// side, whatever became of the client that owned it.
    ///
    /// This is the only reliable way for us to close a Wolf session. The
    /// GameStream `/cancel` Wolf also honours resolves the session from the
    /// *presented client certificate* (rest/endpoints.hpp), so a cancel sent
    /// under our default identity matches nothing when the session was launched
    /// under a per-seat certificate — the session and its `Wolf-UI_*` container
    /// then survive forever and pile up.
    void stopSession(const QString& sessionId, WolfVoidCallback cb);

    /// Digits of a user-typed PIN, in the `std::vector<short>` shape Wolf's API
    /// expects (events.hpp). Sending the PIN as a string makes reflect-cpp
    /// reject the whole request body with a 500 "Invalid event".
    /// Returns an empty vector for an empty or non-numeric PIN.
    static QVector<int> pinDigits(const QString& pin);

    // Wire format, split out from the calls so it can be asserted directly.
    // Every field below is shaped by how Wolf's reflection-based parser reads
    // it, not by what reads naturally — and getting one wrong is rejected as a
    // malformed body, with no hint which field was at fault.
    static QJsonObject joinLobbyBody(const QString& lobbyId, const QString& moonlightSessionId,
                                     const QVector<int>& pin);
    static QJsonObject leaveLobbyBody(const QString& lobbyId, const QString& moonlightSessionId);
    static QJsonObject stopLobbyBody(const QString& lobbyId, const QVector<int>& pin);
    static QJsonObject stopSessionBody(const QString& sessionId);

    // Response parsing, split out for the same reason: the field names are
    // generated from Wolf's own structs (events/reflectors.hpp), so they are
    // worth pinning against a captured answer.
    static QVector<WolfStreamSession> parseSessions(const QJsonDocument& doc);
    static QVector<WolfLobby> parseLobbies(const QJsonDocument& doc);

private:
    void get(const QString& path,
             std::function<void(bool, const WolfApiError&, const QJsonDocument&)> cb);
    void post(const QString& path, const QJsonObject& body,
              std::function<void(bool, const WolfApiError&, const QJsonDocument&)> cb);

    /// Shared reply handling: transport error, HTTP status, JSON parse, and
    /// Wolf's uniform `{"success": bool, "error": string}` envelope.
    void finish(QNetworkReply* reply,
                std::function<void(bool, const WolfApiError&, const QJsonDocument&)> cb);

    /// GET returning the array under `key` of a success envelope.
    void getArray(const QString& path, const QString& key, WolfJsonArrayCallback cb);

    QString m_BaseUrl;
    QString m_AuthToken;
    QNetworkAccessManager* m_Nam = nullptr;
};
