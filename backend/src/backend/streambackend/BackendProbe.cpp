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

#include "backend/streambackend/BackendProbe.h"

#include "backend/NvHTTP.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>

namespace {

/// Short on purpose. This runs alongside host polling and its answer is a
/// nicety, never something a user waits on: a machine that is not MultiSeat
/// must cost the poll almost nothing.
constexpr int kProbeTimeoutMs = 1500;

} // namespace

namespace BackendProbe {

QString toString(Detected d)
{
    switch (d) {
    case Detected::GameStream: return QStringLiteral("gamestream");
    case Detected::NvidiaGfe: return QStringLiteral("gfe");
    case Detected::Unknown: break;
    }
    return QStringLiteral("unknown");
}

Detected classifyServerInfo(const QString& serverInfo)
{
    if (serverInfo.isEmpty()) return Detected::Unknown;

    const QString state = NvHTTP::getXmlString(serverInfo, QStringLiteral("state"));

    // The original NVIDIA software is the one thing that names itself.
    if (state.contains(QStringLiteral("MJOLNIR"))) return Detected::NvidiaGfe;

    // Everything else in the family says SUNSHINE_SERVER_{FREE,BUSY} — Sunshine,
    // Apollo and Wolf alike. See the header: this is as far as serverinfo goes.
    if (state.startsWith(QStringLiteral("SUNSHINE_SERVER"))) return Detected::GameStream;

    // No state, but a document that carries the fields a GameStream host must
    // have. Some forks answer a reduced serverinfo before pairing; treating that
    // as "speaks GameStream" is right, and it is also the safe direction — the
    // caller only uses this to decide whether to look further.
    if (!NvHTTP::getXmlString(serverInfo, QStringLiteral("uniqueid")).isEmpty() &&
        !NvHTTP::getXmlString(serverInfo, QStringLiteral("appversion")).isEmpty()) {
        return Detected::GameStream;
    }

    return Detected::Unknown;
}

bool looksLikeMultiSeatAuth(const QByteArray& body)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;

    // The endpoint answers {"authEnabled": true|false}. The key's presence is
    // the signal; its value only says whether a credential will be needed next,
    // which is a separate question from "is MultiSeat there".
    const QJsonObject obj = doc.object();
    return obj.contains(QStringLiteral("authEnabled")) &&
           obj.value(QStringLiteral("authEnabled")).isBool();
}

void probeMultiSeat(QNetworkAccessManager* nam, const QString& address,
                    std::function<void(bool present)> cb)
{
    if (!nam || address.isEmpty()) {
        if (cb) cb(false);
        return;
    }

    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(address);
    url.setPort(kMultiSeatApiPort);
    url.setPath(QStringLiteral("/api/system/auth"));

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(kProbeTimeoutMs);

    QNetworkReply* reply = nam->get(req);

    // Belt and braces alongside setTransferTimeout: a peer that accepts the
    // connection and then says nothing is exactly the shape a stalled probe
    // takes, and this one must never outlive the poll that started it.
    QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(kProbeTimeoutMs, reply, [guard]() {
        if (guard && !guard->isFinished()) guard->abort();
    });

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, cb]() {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        looksLikeMultiSeatAuth(reply->readAll());
        if (cb) cb(ok);
    });
}

} // namespace BackendProbe
