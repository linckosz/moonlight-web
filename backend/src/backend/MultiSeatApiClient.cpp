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

#include "MultiSeatApiClient.h"

#include "../common/Logger.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

// Provisioning a seat creates a Windows session, a virtual display and an
// Apollo process, so it is far slower than a read. One timeout covers both:
// picking the generous one is safer than timing out a half-created seat, which
// would leave the service mid-provision with nobody watching.
constexpr int kRequestTimeoutMs = 60000;

constexpr const char* kApiKeyHeader = "X-MultiSeat-Key";

} // namespace

MultiSeatApiClient::MultiSeatApiClient(QString baseUrl, QString apiKey, QNetworkAccessManager* nam,
                                       QObject* parent)
    : QObject(parent)
    , m_BaseUrl(std::move(baseUrl))
    , m_ApiKey(std::move(apiKey))
    , m_Nam(nam)
{
    while (m_BaseUrl.endsWith(QLatin1Char('/'))) {
        m_BaseUrl.chop(1);
    }
}

MultiSeatSeat MultiSeatApiClient::parseSeat(const QJsonObject& obj)
{
    // camelCase with string enums — both set explicitly in ApiServer.cs.
    MultiSeatSeat seat;
    seat.id = obj.value(QStringLiteral("id")).toString();
    seat.accountName = obj.value(QStringLiteral("accountName")).toString();
    seat.sessionId = obj.value(QStringLiteral("sessionId")).toInt(-1);
    seat.status = obj.value(QStringLiteral("status")).toString();
    seat.width = obj.value(QStringLiteral("width")).toInt();
    seat.height = obj.value(QStringLiteral("height")).toInt();
    seat.fps = obj.value(QStringLiteral("fps")).toInt();
    seat.portBase = obj.value(QStringLiteral("portBase")).toInt();
    seat.apolloProcessId = obj.value(QStringLiteral("apolloProcessId")).toInt();
    seat.errorMessage = obj.value(QStringLiteral("errorMessage")).toString();
    seat.provisioningStep = obj.value(QStringLiteral("provisioningStep")).toString();
    return seat;
}

void MultiSeatApiClient::finish(QNetworkReply* reply, RawCallback cb)
{
    // QNetworkAccessManager has no per-request deadline, and a service that
    // accepts the connection then stalls mid-provision would otherwise leave the
    // callback pending forever.
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->setInterval(kRequestTimeoutMs);
    QObject::connect(timer, &QTimer::timeout, reply, [reply]() { reply->abort(); });
    timer->start();

    QObject::connect(reply, &QNetworkReply::finished, this, [reply, cb = std::move(cb)]() mutable {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (reply->error() != QNetworkReply::NoError && status == 0) {
            const bool aborted = reply->error() == QNetworkReply::OperationCanceledError;
            cb(false,
               MultiSeatApiError::make(aborted ? MultiSeatApiError::Timeout
                                               : MultiSeatApiError::Unreachable,
                                       reply->errorString()),
               QJsonDocument());
            return;
        }

        // A wrong key is worth its own kind: it is the one failure an admin can
        // fix on the spot, and it must not read as "MultiSeat is down".
        if (status == 401 || status == 403) {
            cb(false,
               MultiSeatApiError::make(MultiSeatApiError::Unauthorized,
                                       QStringLiteral("MultiSeat rejected the API key"), status),
               QJsonDocument());
            return;
        }

        if (status < 200 || status >= 300) {
            QString message = QStringLiteral("MultiSeat returned HTTP %1").arg(status);
            // MultiSeat's own failures answer {"error": "..."} — verified
            // against the live service, whose text is long and genuinely
            // actionable (which prerequisite to re-run, which registry value to
            // check). "detail" is the fallback for framework-level failures,
            // which use RFC 7807 instead.
            const QJsonDocument problem = QJsonDocument::fromJson(body);
            if (problem.isObject()) {
                const QJsonObject obj = problem.object();
                QString upstream = obj.value(QStringLiteral("error")).toString();
                if (upstream.isEmpty()) {
                    upstream = obj.value(QStringLiteral("detail")).toString();
                }
                if (!upstream.isEmpty()) message = upstream;
            }
            cb(false, MultiSeatApiError::make(MultiSeatApiError::HttpError, message, status),
               problem);
            return;
        }

        // 204 and other empty successes are legitimate (DELETE answers one).
        if (body.trimmed().isEmpty()) {
            cb(true, MultiSeatApiError{}, QJsonDocument());
            return;
        }

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            cb(false,
               MultiSeatApiError::make(MultiSeatApiError::Protocol,
                                       QStringLiteral("Malformed JSON from MultiSeat: %1")
                                           .arg(parseError.errorString()),
                                       status),
               QJsonDocument());
            return;
        }

        cb(true, MultiSeatApiError{}, doc);
    });
}

void MultiSeatApiClient::get(const QString& path, RawCallback cb)
{
    QNetworkRequest req{QUrl(m_BaseUrl + path)};
    if (!m_ApiKey.isEmpty()) req.setRawHeader(kApiKeyHeader, m_ApiKey.toUtf8());
    finish(m_Nam->get(req), std::move(cb));
}

void MultiSeatApiClient::post(const QString& path, const QJsonObject& body, RawCallback cb)
{
    QNetworkRequest req{QUrl(m_BaseUrl + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_ApiKey.isEmpty()) req.setRawHeader(kApiKeyHeader, m_ApiKey.toUtf8());
    finish(m_Nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact)), std::move(cb));
}

void MultiSeatApiClient::del(const QString& path, RawCallback cb)
{
    QNetworkRequest req{QUrl(m_BaseUrl + path)};
    if (!m_ApiKey.isEmpty()) req.setRawHeader(kApiKeyHeader, m_ApiKey.toUtf8());
    finish(m_Nam->deleteResource(req), std::move(cb));
}

void MultiSeatApiClient::listSeats(MultiSeatSeatsCallback cb)
{
    get(QStringLiteral("/api/seats"),
        [cb = std::move(cb)](bool ok, const MultiSeatApiError& err, const QJsonDocument& doc) {
            if (!ok) {
                cb(false, err, {});
                return;
            }

            QVector<MultiSeatSeat> seats;
            const QJsonArray arr = doc.array();
            seats.reserve(arr.size());
            for (const QJsonValue& value : arr) {
                MultiSeatSeat seat = parseSeat(value.toObject());
                if (!seat.id.isEmpty()) seats.append(seat);
            }

            cb(true, MultiSeatApiError{}, seats);
        });
}

void MultiSeatApiClient::getSeat(const QString& seatId, MultiSeatSeatCallback cb)
{
    get(QStringLiteral("/api/seats/%1").arg(seatId),
        [cb = std::move(cb)](bool ok, const MultiSeatApiError& err, const QJsonDocument& doc) {
            if (!ok) {
                cb(false, err, MultiSeatSeat{});
                return;
            }
            cb(true, MultiSeatApiError{}, parseSeat(doc.object()));
        });
}

void MultiSeatApiClient::provisionSeat(const QJsonObject& params, MultiSeatSeatCallback cb)
{
    post(QStringLiteral("/api/seats"), params,
         [cb = std::move(cb)](bool ok, const MultiSeatApiError& err, const QJsonDocument& doc) {
             if (!ok) {
                 Logger::warning(
                     QStringLiteral("MultiSeat: provisioning failed (%1)").arg(err.message));
                 cb(false, err, MultiSeatSeat{});
                 return;
             }
             cb(true, MultiSeatApiError{}, parseSeat(doc.object()));
         });
}

void MultiSeatApiClient::teardownSeat(const QString& seatId, MultiSeatVoidCallback cb)
{
    del(QStringLiteral("/api/seats/%1").arg(seatId),
        [cb = std::move(cb)](bool ok, const MultiSeatApiError& err, const QJsonDocument&) {
            cb(ok, err);
        });
}

void MultiSeatApiClient::seatClients(const QString& seatId, MultiSeatJsonArrayCallback cb)
{
    get(QStringLiteral("/api/seats/%1/clients").arg(seatId),
        [cb = std::move(cb)](bool ok, const MultiSeatApiError& err, const QJsonDocument& doc) {
            cb(ok, err, ok ? doc.array() : QJsonArray());
        });
}

void MultiSeatApiClient::clearSeatClients(const QString& seatId, MultiSeatVoidCallback cb)
{
    del(QStringLiteral("/api/seats/%1/clients").arg(seatId),
        [cb = std::move(cb)](bool ok, const MultiSeatApiError& err, const QJsonDocument&) {
            cb(ok, err);
        });
}

void MultiSeatApiClient::listAccounts(MultiSeatJsonArrayCallback cb)
{
    get(QStringLiteral("/api/accounts"),
        [cb = std::move(cb)](bool ok, const MultiSeatApiError& err, const QJsonDocument& doc) {
            cb(ok, err, ok ? doc.array() : QJsonArray());
        });
}
