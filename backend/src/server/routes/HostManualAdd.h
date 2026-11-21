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

#include "common/Types.h"
#include "server/HostWritePacing.h"
#include "server/RestRouter.h"
#include "server/routes/HostWrites.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

/// A host as a reader gets it: where its backend is and whom it knows are the
/// administrator's, like changing them. backendConfigured stays.
inline QJsonObject hostForReader(QJsonObject host, bool admin)
{
    if (!admin) {
        host.remove(QStringLiteral("backendApiUrl"));
        host.remove(QStringLiteral("backendPairUser"));
    }
    return host;
}

/// Adding a host by address, any session's: the manager decides what the answer
/// may change. A template so the tests can drive this very route.
///
/// The address is the caller's, and the server goes and asks it. So for anyone
/// but the administrator: paced (HostWritePacing::addBegin, counted before the
/// body is read, whatever comes of it), one at a time, and every failure gets
/// the same answer — which port is shut, filtered or open is not handed out.
template <typename Manager>
void registerHostManualAdd(RestRouter& router, Manager& manager, HostWritePacing& pacing,
                           HostWriteCaller caller)
{
    router.post("/api/hosts/manual", [&manager, &pacing, caller](const HttpRequest& req) {
        const bool admin = req.isLocal;
        if (!admin) {
            const qint64 wait = pacing.addBegin(caller(req));
            if (wait > 0) return HostWriteAnswers::tooManyAttempts(wait);
        }
        struct EndAdd
        {
            HostWritePacing* pacing;
            ~EndAdd()
            {
                if (pacing) pacing->addEnd();
            }
        } endAdd{admin ? nullptr : &pacing};

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        QString address = body["address"].toString();
        if (address.isEmpty()) return HttpResponse::error(400, "Missing 'address' field");

        auto [status, result] = manager.handleAddManualHost(address, admin);
        if (!admin && status != 200) return HostWriteAnswers::hostAddFailed();
        if (result.contains("hosts")) {
            QJsonArray hosts = result["hosts"].toArray();
            for (qsizetype i = 0; i < hosts.size(); ++i)
                hosts[i] = hostForReader(hosts[i].toObject(), req.isLocal);
            result["hosts"] = hosts;
        }
        return HttpResponse::json(result, status);
    });
}
