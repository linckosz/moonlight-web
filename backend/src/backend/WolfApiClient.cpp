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

    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, cb = std::move(cb)]() mutable {
        reply->deleteLater();

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
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
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_AuthToken).toUtf8());
    }

    finish(m_Nam->get(request), std::move(cb));
}

void WolfApiClient::post(const QString& path, const QJsonObject& body,
                         std::function<void(bool, const WolfApiError&, const QJsonDocument&)> cb)
{
    QNetworkRequest request{QUrl(m_BaseUrl + QStringLiteral("/api/v1") + path)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_AuthToken.isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_AuthToken).toUtf8());
    }

    finish(m_Nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact)), std::move(cb));
}

void WolfApiClient::getArray(const QString& path, const QString& key, WolfJsonArrayCallback cb)
{
    get(path, [key, cb = std::move(cb)](bool ok, const WolfApiError& err,
                                        const QJsonDocument& doc) {
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
            const QJsonArray requests =
                doc.object().value(QStringLiteral("requests")).toArray();
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
                 Logger::warning(QStringLiteral("Wolf auto-pair: PIN rejected (%1)").arg(err.message));
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
