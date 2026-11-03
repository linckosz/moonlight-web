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

#include "backend/NvComputer.h"
#include "common/Logger.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <utility>

/// What a machine's answer does to the host list once someone has typed its
/// address. Any session can add a new host, or name an address its host already
/// has; taking a known host to another address is for mayMoveHost alone, and a
/// paired host is not switched to another of its addresses by that answer either:
/// the poll does it, on proof. The manual address of a known host is mayMoveHost's
/// too. A template so the tests can drive it with a list of their own.
template <typename Hosts>
std::pair<int, QJsonObject> mergeManualHost(Hosts& hosts, const QString& serverInfo,
                                            const NvAddress& address, bool mayMoveHost)
{
    const NvComputer answer(serverInfo, address);

    const NvComputer* known = hosts.findHostByUuid(answer.uuid);
    const bool mayName = !known || mayMoveHost;
    if (known && !mayMoveHost && !known->uniqueAddresses().contains(address)) {
        Logger::warning(QString("Manual host refused: %1 (%2) is known at another address")
                            .arg(known->name, known->uuid));
        return {409,
                {{"status", "error"},
                 {"message", "This host is known at another address; only its administrator "
                             "can change it"}}};
    }

    if (known && !mayMoveHost && known->movesOnlyWithProof(address)) {
        QJsonArray unchanged;
        unchanged.append(known->toJson());
        return {200, QJsonObject{{"hosts", unchanged}}};
    }

    hosts.addOrUpdateHost(serverInfo, address);

    // Look up the resulting host to return its JSON
    NvComputer* host = hosts.findHostByUuid(answer.uuid);
    if (!host) {
        return {500,
                {{"status", "error"}, {"message", "Host was added but could not be retrieved"}}};
    }

    // Remember it as the manual address. This is the operator's answer to a host
    // discovery cannot reach, so it has to outlive the process — until now it
    // only landed in activeAddress and was gone at the next restart.
    if (mayName && host->manualAddress != address) {
        host->manualAddress = address;
        hosts.saveHosts();
    }

    if (mayName) {
        Logger::info(QString("Manual host added: %1 (%2) at %3")
                         .arg(host->name, host->uuid, address.toString()));
    }

    QJsonArray added;
    added.append(host->toJson());
    return {200, QJsonObject{{"hosts", added}}};
}
