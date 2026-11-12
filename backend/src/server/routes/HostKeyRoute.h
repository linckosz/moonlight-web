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

#include <QJsonObject>
#include <QString>

#include <functional>

class AuthManager;
class RestRouter;

/// What POST /api/auth/host-key takes from the server around it, as functions:
/// the tests stand in for the settings file, the network and the entry points.
struct HostKeyDeps
{
    /// Spends the key a link of this machine carried (HostKeyVault::spend): true
    /// once, for a key made here and still waiting. Asking is spending.
    std::function<bool(const QString& key)> spend;
    /// A key that opened nothing was presented from @p address: counted against
    /// it where an address can be counted against (HttpServer::reportAuthFailure,
    /// which never locks this machine or its network out).
    std::function<void(const QString& address)> refused;
    /// Whether @p address is one of this machine's own (HttpServer::isLocalRequest).
    std::function<bool(const QString& address)> thisMachine;
    /// This machine's public address, empty when it is not known.
    std::function<QString()> publicIp;
    /// The host's MW-BIND-v1 identity as a login answers it: `host_public_key`
    /// and `host_id`.
    std::function<QJsonObject()> hostIdentity;
};

/// Whose address a peer's is, as far as the host machine is concerned.
enum class HostAddress
{
    NotOurs,       ///< another machine's, of the same private network or not
    ThisMachine,   ///< one of this machine's own, loopback included
    PublicAddress, ///< the public address this machine is reached at
};

/// Which of them @p address (AuthManager::cleanClientAddress) is. @p thisMachine
/// says whether an address is one of this machine's own
/// (HttpServer::isLocalRequest); @p publicIp is empty when it is not known.
HostAddress hostAddressOf(const QString& address,
                          const std::function<bool(const QString& address)>& thisMachine,
                          const QString& publicIp);

/// Whether @p address (AuthManager::cleanClientAddress) can be the host machine
/// itself: one of its own, or its public address. Another machine of the same
/// private network is not the host machine.
/// The address check of the redemption, and of AuthManager::adoptLineKey.
bool plausiblyHostAddress(const QString& address,
                          const std::function<bool(const QString& address)>& thisMachine,
                          const QString& publicIp);

/// Register POST /api/auth/host-key: the browser on the host machine spends the
/// key of the link its owner was handed, and leaves with a host session.
/// Registered by registerSystemRoutes.
void registerHostKeyRoute(RestRouter& router, AuthManager& authManager, HostKeyDeps deps);
