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

#include "WolfApiClient.h"

#include "../common/Logger.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

// Wolf answers control calls off a local socket, so anything slower than this
// means the proxy or Wolf itself is wedged. Deliberately short: ensurePaired()
// polls on top of it and must not stack up multi-second waits.
constexpr int kRequestTimeoutMs = 8000;

} // namespace

WolfApiClient::WolfApiClient(QString baseUrl, QString authToken, QNetworkAccessManager* nam,
                             QObject* parent)
    : QObject(parent)
    , m_BaseUrl(std::move(baseUrl))
    , m_AuthToken(std::move(authToken))
    , m_Nam(nam)
{
    while (m_BaseUrl.endsWith(QLatin1Char('/'))) {
        m_BaseUrl.chop(1);
    }
}

void WolfApiClient::finish(QNetworkReply* reply,
                           std::function<void(bool, const WolfApiError&, const QJsonDocument&)> cb)
{
    // Bound every call: QNetworkAccessManager has no per-request deadline, and a
    // proxy that accepts the connection then never answers would otherwise leave
    // the callback — and any pairing waiting on it — pending forever.
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(kRequestTimeoutMs);
    QObject::connect(timer, &QTimer::timeout, reply, [reply]() { reply->abort(); });
    timer->start();

    QObject::connect(
        reply, &QNetworkReply::finished, this, [this, reply, cb = std::move(cb)]() mutable {
            reply->deleteLater();

            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();

            if (reply->error() != QNetworkReply::NoError && status == 0) {
                const bool aborted = reply->error() == QNetworkReply::OperationCanceledError;
                cb(false,
                   WolfApiError::make(aborted ? WolfApiError::Timeout : WolfApiError::Unreachable,
                                      reply->errorString()),
                   QJsonDocument());
                return;
            }

            QJsonParseError parseError{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);

            if (status < 200 || status >= 300) {
                // Wolf reports failures as {"success": false, "error": "..."} — use
                // that wording when it is there, since it is far more specific than
                // the bare status line.
                QString message = QStringLiteral("Wolf API returned HTTP %1").arg(status);
                if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                    const QString upstream = doc.object().value(QStringLiteral("error")).toString();
                    if (!upstream.isEmpty()) message = upstream;
                }
                cb(false, WolfApiError::make(WolfApiError::HttpError, message, status), doc);
                return;
            }

            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                cb(false,
                   WolfApiError::make(WolfApiError::Protocol,
                                      QStringLiteral("Malformed JSON from Wolf API: %1")
                                          .arg(parseError.errorString()),
                                      status),
                   QJsonDocument());
                return;
            }

            // A 2xx carrying success == false is still a failure.
            const QJsonObject obj = doc.object();
            if (obj.contains(QStringLiteral("success")) &&
                !obj.value(QStringLiteral("success")).toBool()) {
                QString message = obj.value(QStringLiteral("error")).toString();
                if (message.isEmpty()) message = QStringLiteral("Wolf API reported failure");
                cb(false, WolfApiError::make(WolfApiError::Protocol, message, status), doc);
                return;
            }

            cb(true, WolfApiError{}, doc);
        });
}

void WolfApiClient::get(const QString& path,
                        std::function<void(bool, const WolfApiError&, const QJsonDocument&)> cb)
{
    QNetworkRequest request{QUrl(m_BaseUrl + QStringLiteral("/api/v1") + path)};
    if (!m_AuthToken.isEmpty()) {
        request.setRawHeader("Authorization",
                             QStringLiteral("Bearer %1").arg(m_AuthToken).toUtf8());
    }

    finish(m_Nam->get(request), std::move(cb));
}

void WolfApiClient::post(const QString& path, const QJsonObject& body,
                         std::function<void(bool, const WolfApiError&, const QJsonDocument&)> cb)
{
    QNetworkRequest request{QUrl(m_BaseUrl + QStringLiteral("/api/v1") + path)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_AuthToken.isEmpty()) {
        request.setRawHeader("Authorization",
                             QStringLiteral("Bearer %1").arg(m_AuthToken).toUtf8());
    }

    finish(m_Nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact)), std::move(cb));
}

void WolfApiClient::getArray(const QString& path, const QString& key, WolfJsonArrayCallback cb)
{
    get(path,
        [key, cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument& doc) {
            if (!ok) {
                cb(false, err, QJsonArray());
                return;
            }
            cb(true, WolfApiError{}, doc.object().value(key).toArray());
        });
}

void WolfApiClient::pendingPairRequests(WolfPendingPairsCallback cb)
{
    get(QStringLiteral("/pair/pending"),
        [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument& doc) {
            if (!ok) {
                cb(false, err, {});
                return;
            }

            QVector<WolfPendingPair> pending;
            const QJsonArray requests = doc.object().value(QStringLiteral("requests")).toArray();
            pending.reserve(requests.size());
            for (const QJsonValue& value : requests) {
                const QJsonObject obj = value.toObject();
                WolfPendingPair entry;
                entry.pairSecret = obj.value(QStringLiteral("pair_secret")).toString();
                entry.clientIp = obj.value(QStringLiteral("client_ip")).toString();
                if (!entry.pairSecret.isEmpty()) pending.append(entry);
            }

            cb(true, WolfApiError{}, pending);
        });
}

void WolfApiClient::submitPin(const QString& pairSecret, const QString& pin, WolfVoidCallback cb)
{
    QJsonObject body;
    body[QStringLiteral("pair_secret")] = pairSecret;
    body[QStringLiteral("pin")] = pin;

    post(QStringLiteral("/pair/client"), body,
         [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument&) {
             if (!ok) {
                 Logger::warning(
                     QStringLiteral("Wolf auto-pair: PIN rejected (%1)").arg(err.message));
             }
             cb(ok, err);
         });
}

void WolfApiClient::pairedClients(WolfPairedClientsCallback cb)
{
    get(QStringLiteral("/clients"),
        [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument& doc) {
            if (!ok) {
                cb(false, err, {});
                return;
            }

            QVector<WolfPairedClient> clients;
            const QJsonArray entries = doc.object().value(QStringLiteral("clients")).toArray();
            clients.reserve(entries.size());
            for (const QJsonValue& value : entries) {
                const QJsonObject obj = value.toObject();
                WolfPairedClient client;
                client.clientId = obj.value(QStringLiteral("client_id")).toString();
                client.appStateFolder = obj.value(QStringLiteral("app_state_folder")).toString();
                if (!client.clientId.isEmpty()) clients.append(client);
            }

            cb(true, WolfApiError{}, clients);
        });
}

void WolfApiClient::unpairClient(const QString& clientId, WolfVoidCallback cb)
{
    QJsonObject body;
    body[QStringLiteral("client_id")] = clientId;

    post(QStringLiteral("/unpair/client"), body,
         [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument&) {
             cb(ok, err);
         });
}

void WolfApiClient::listApps(WolfJsonArrayCallback cb)
{
    getArray(QStringLiteral("/apps"), QStringLiteral("apps"), std::move(cb));
}

void WolfApiClient::listProfiles(WolfJsonArrayCallback cb)
{
    getArray(QStringLiteral("/profiles"), QStringLiteral("profiles"), std::move(cb));
}

void WolfApiClient::listLobbies(WolfJsonArrayCallback cb)
{
    getArray(QStringLiteral("/lobbies"), QStringLiteral("lobbies"), std::move(cb));
}

QVector<WolfLobby> WolfApiClient::parseLobbies(const QJsonDocument& doc)
{
    QVector<WolfLobby> lobbies;
    const QJsonArray entries = doc.object().value(QStringLiteral("lobbies")).toArray();
    lobbies.reserve(entries.size());
    for (const QJsonValue& value : entries) {
        const QJsonObject obj = value.toObject();
        WolfLobby lobby;
        lobby.id = obj.value(QStringLiteral("id")).toString();
        lobby.name = obj.value(QStringLiteral("name")).toString();
        lobby.multiUser = obj.value(QStringLiteral("multi_user")).toBool();
        lobby.pinRequired = obj.value(QStringLiteral("pin_required")).toBool();
        lobby.startedByProfileId = obj.value(QStringLiteral("started_by_profile_id")).toString();
        for (const QJsonValue& s : obj.value(QStringLiteral("connected_sessions")).toArray()) {
            lobby.connectedSessions.append(s.toString());
        }
        if (!lobby.id.isEmpty()) lobbies.append(lobby);
    }
    return lobbies;
}

QVector<WolfStreamSession> WolfApiClient::parseSessions(const QJsonDocument& doc)
{
    QVector<WolfStreamSession> sessions;
    const QJsonArray entries = doc.object().value(QStringLiteral("sessions")).toArray();
    sessions.reserve(entries.size());
    for (const QJsonValue& value : entries) {
        const QJsonObject obj = value.toObject();
        WolfStreamSession session;
        // `client_id`, not `session_id`: the reflector renames it on the way out
        // (reflectors.hpp) even though it IS the session id every lobby call
        // wants. There is no `session_id` field to read here.
        session.sessionId = obj.value(QStringLiteral("client_id")).toString();
        session.clientIp = obj.value(QStringLiteral("client_ip")).toString();
        session.aesKey = obj.value(QStringLiteral("aes_key")).toString();
        session.appId = obj.value(QStringLiteral("app_id")).toString();
        session.width = obj.value(QStringLiteral("video_width")).toInt();
        session.height = obj.value(QStringLiteral("video_height")).toInt();
        session.fps = obj.value(QStringLiteral("video_refresh_rate")).toInt();
        if (!session.sessionId.isEmpty()) sessions.append(session);
    }
    return sessions;
}

void WolfApiClient::lobbies(WolfLobbiesCallback cb)
{
    get(QStringLiteral("/lobbies"),
        [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument& doc) {
            if (!ok) {
                cb(false, err, {});
                return;
            }
            cb(true, WolfApiError{}, parseLobbies(doc));
        });
}

void WolfApiClient::listSessions(WolfSessionsCallback cb)
{
    get(QStringLiteral("/sessions"),
        [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument& doc) {
            if (!ok) {
                cb(false, err, {});
                return;
            }
            cb(true, WolfApiError{}, parseSessions(doc));
        });
}

QVector<int> WolfApiClient::pinDigits(const QString& pin)
{
    QVector<int> digits;
    digits.reserve(pin.size());
    for (const QChar c : pin) {
        if (!c.isDigit()) return {};
        digits.append(c.digitValue());
    }
    return digits;
}

namespace {

/// Add the `pin` member in the shape Wolf parses — a JSON array of numbers.
/// Absent when there is no PIN: `check_lobby_pin` compares the *optional*
/// against the lobby's own, so an empty array is NOT the same as none and
/// would be rejected on a lobby that has no PIN.
void addPin(QJsonObject& body, const QVector<int>& pin)
{
    if (pin.isEmpty()) return;
    QJsonArray arr;
    for (int d : pin)
        arr.append(d);
    body[QStringLiteral("pin")] = arr;
}

} // namespace

QJsonObject WolfApiClient::joinLobbyBody(const QString& lobbyId, const QString& moonlightSessionId,
                                         const QVector<int>& pin)
{
    QJsonObject body;
    body[QStringLiteral("lobby_id")] = lobbyId;
    // A string on purpose: the field is a std::size_t upstream, but its parser
    // is specialised to read from a JSON string (reflectors.hpp) because JSON
    // has no unsigned 64-bit integer. Sending a number would be rejected.
    body[QStringLiteral("moonlight_session_id")] = moonlightSessionId;
    addPin(body, pin);
    return body;
}

QJsonObject WolfApiClient::leaveLobbyBody(const QString& lobbyId, const QString& moonlightSessionId)
{
    QJsonObject body;
    body[QStringLiteral("lobby_id")] = lobbyId;
    body[QStringLiteral("moonlight_session_id")] = moonlightSessionId;
    return body;
}

QJsonObject WolfApiClient::stopLobbyBody(const QString& lobbyId, const QVector<int>& pin)
{
    QJsonObject body;
    body[QStringLiteral("lobby_id")] = lobbyId;
    addPin(body, pin);
    return body;
}

QJsonObject WolfApiClient::stopSessionBody(const QString& sessionId)
{
    return QJsonObject{{QStringLiteral("session_id"), sessionId}};
}

void WolfApiClient::joinLobby(const QString& lobbyId, const QString& moonlightSessionId,
                              const QVector<int>& pin, WolfVoidCallback cb)
{
    post(QStringLiteral("/lobbies/join"), joinLobbyBody(lobbyId, moonlightSessionId, pin),
         [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument&) {
             cb(ok, err);
         });
}

void WolfApiClient::leaveLobby(const QString& lobbyId, const QString& moonlightSessionId,
                               WolfVoidCallback cb)
{
    post(QStringLiteral("/lobbies/leave"), leaveLobbyBody(lobbyId, moonlightSessionId),
         [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument&) {
             cb(ok, err);
         });
}

void WolfApiClient::stopLobby(const QString& lobbyId, const QVector<int>& pin, WolfVoidCallback cb)
{
    post(QStringLiteral("/lobbies/stop"), stopLobbyBody(lobbyId, pin),
         [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument&) {
             cb(ok, err);
         });
}

void WolfApiClient::stopSession(const QString& sessionId, WolfVoidCallback cb)
{
    post(QStringLiteral("/sessions/stop"), stopSessionBody(sessionId),
         [cb = std::move(cb)](bool ok, const WolfApiError& err, const QJsonDocument&) {
             cb(ok, err);
         });
}
