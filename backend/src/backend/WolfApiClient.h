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
using WolfPendingPairsCallback =
    std::function<void(bool ok, const WolfApiError& error, const QVector<WolfPendingPair>& pending)>;
using WolfPairedClientsCallback =
    std::function<void(bool ok, const WolfApiError& error, const QVector<WolfPairedClient>& clients)>;
using WolfJsonArrayCallback =
    std::function<void(bool ok, const WolfApiError& error, const QJsonArray& items)>;

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

private:
    void get(const QString& path, std::function<void(bool, const WolfApiError&,
                                                     const QJsonDocument&)> cb);
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
