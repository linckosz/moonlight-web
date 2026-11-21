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

#include "server/HouseLink.h"

#include "server/AuthManager.h"
#include "server/RequestGuard.h"

namespace HouseLink {

bool onThisHostsLink(const Links& l, const QString& address, bool line)
{
    QHostAddress a(AuthManager::cleanClientAddress(address));
    bool mapped = false;
    const quint32 v4 = a.toIPv4Address(&mapped);
    if (mapped) a = QHostAddress(v4);
    if (a.isNull() || a.isLoopback() || !l.localSubnets) return false;
    const bool v6 = a.protocol() == QAbstractSocket::IPv6Protocol;
    if (!v6 && !AuthManager::isLanAddress(a.toString())) return false;
    if (v6 && !line && !a.isLinkLocal() && !a.isUniqueLocalUnicast()) return false;
    const QList<QPair<QHostAddress, int>> subnets = l.localSubnets();
    for (const auto& net : subnets) {
        if (net.first.protocol() != a.protocol()) continue;
        if (a.isInSubnet(net.first, v6 ? 64 : net.second)) return true;
    }
    return false;
}

bool underALocalName(const HttpRequest& req)
{
    const QString host = req.headers.value(QStringLiteral("host"));
    return !host.isEmpty() && RequestGuard::isLocalHostName(host);
}

bool passedAlong(const HttpRequest& req)
{
    return RequestGuard::forwardedByProxy(req.headers);
}

bool isThisMachine(const Links& l, const QString& address)
{
    const QString clean = AuthManager::cleanClientAddress(address);
    if (QHostAddress(clean).isLoopback()) return true;
    return l.thisMachine && l.thisMachine(clean);
}

bool fromThisHousesLink(const Links& l, const HttpRequest& req)
{
    if (isThisMachine(l, req.clientAddress)) return false;
    if (req.viaTunnel) return onThisHostsLink(l, req.clientAddress, true);
    if (!underALocalName(req) || passedAlong(req)) return false;
    return AuthManager::fromThisLan(req) &&
           onThisHostsLink(l, AuthManager::attemptAddress(req.clientAddress, req.headers), false);
}

} // namespace HouseLink
