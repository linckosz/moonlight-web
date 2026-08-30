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

#include "StreamBackendSetup.h"

#include "../../common/Logger.h"
#include "GameStreamBackend.h"
#include "MultiSeatBackend.h"
#include "NativeHostBackend.h"
#include "StreamBackendRegistry.h"
#include "WolfBackend.h"

#include <QJsonObject>

namespace StreamBackendSetup {

namespace {

/// Every factory needs the host uuid; none can do anything without it.
QString requireUuid(const QJsonObject& config, const char* type)
{
    const QString uuid = config.value(QStringLiteral("hostUuid")).toString();
    if (uuid.isEmpty()) {
        Logger::warning(
            QStringLiteral("%1 backend: config is missing 'hostUuid'").arg(QLatin1String(type)));
    }
    return uuid;
}

} // namespace

void registerAll(NvHTTP* http, QNetworkAccessManager* nam, HostLookup lookup, PairingCommit commit)
{
    StreamBackendRegistry& registry = StreamBackendRegistry::instance();

    // This machine's own screen, captured in-process. Unlike every other
    // factory it needs no host uuid, no address and no credential: there is
    // nothing to dial and nobody to authenticate to.
    registry.registerFactory(NativeHostBackend::typeName(),
                             [](const QJsonObject&) -> std::unique_ptr<IStreamBackend> {
                                 return std::make_unique<NativeHostBackend>();
                             });

    registry.registerFactory(
        QStringLiteral("gamestream"),
        [http, nam, lookup](const QJsonObject& config) -> std::unique_ptr<IStreamBackend> {
            const QString uuid = requireUuid(config, "gamestream");
            if (uuid.isEmpty()) return nullptr;

            // No QObject parent: the unique_ptr is the sole owner. Parenting as
            // well would double-delete.
            return std::make_unique<GameStreamBackend>(
                uuid, [lookup, uuid]() { return lookup(uuid); }, http, nam, nullptr);
        });

    registry.registerFactory(
        QStringLiteral("wolf"),
        [http, nam, lookup, commit](const QJsonObject& config) -> std::unique_ptr<IStreamBackend> {
            const QString uuid = requireUuid(config, "wolf");
            if (uuid.isEmpty()) return nullptr;

            return std::make_unique<WolfBackend>(
                uuid, [lookup, uuid]() { return lookup(uuid); }, http, nam,
                config.value(QStringLiteral("apiUrl")).toString(),
                config.value(QStringLiteral("apiToken")).toString(),
                // The backend performs the handshake; what a pairing *means*
                // for a host belongs to whoever owns the host list.
                [commit, uuid](const QByteArray& serverCertPem) {
                    if (commit) commit(uuid, serverCertPem);
                },
                nullptr);
        });

    registry.registerFactory(
        QStringLiteral("multiseat"),
        [http, nam, lookup](const QJsonObject& config) -> std::unique_ptr<IStreamBackend> {
            const QString uuid = requireUuid(config, "multiseat");
            if (uuid.isEmpty()) return nullptr;

            // No pairing commit: MultiSeat's control API is key-authenticated,
            // so there is no certificate to write back to the host.
            return std::make_unique<MultiSeatBackend>(
                uuid, [lookup, uuid]() { return lookup(uuid); }, http, nam,
                config.value(QStringLiteral("apiUrl")).toString(),
                config.value(QStringLiteral("apiToken")).toString(),
                config.value(QStringLiteral("pairUser")).toString(),
                config.value(QStringLiteral("pairPassword")).toString(), nullptr);
        });

    Logger::info(QStringLiteral("Stream backends registered: [%1]")
                     .arg(registry.knownTypes().join(QStringLiteral(", "))));
}

} // namespace StreamBackendSetup
