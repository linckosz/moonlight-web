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

#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QString>

#include <functional>

/**
 * The house: where a device asks to be let in from, and where a paired session
 * that is not the administrator answers from. Shared by the phone's QR code
 * (MW-PAIR-v1, routes/PairRequestRoutes.cpp), where it was first written, and
 * the TV on plain HTTP let in for a short while (routes/LanHttpRoutes.cpp).
 */
namespace HouseLink {

/// What the server knows of its own links, as functions: the tests stand in.
struct Links
{
    /// This host's own links: each interface address with its prefix length,
    /// loopback left out (QNetworkInterface). Empty: nowhere.
    std::function<QList<QPair<QHostAddress, int>>()> localSubnets;
    /// Whether an address is one of this machine's own
    /// (HttpServer::isLocalRequest). Empty: the loopback only.
    std::function<bool(const QString& address)> thisMachine;
};

/// Whether @p address sits on one of this host's own links: inside the prefix of
/// one of its interfaces, an IPv6 address inside that interface's /64. An IPv4
/// address counts only when private or link-local; a public IPv6 address counts
/// on a line only, where ICE found it.
bool onThisHostsLink(const Links& l, const QString& address, bool line);

/// Whether a socket request names this host by a local name
/// (RequestGuard::isLocalHostName): a Host header is required.
bool underALocalName(const HttpRequest& req);

/// Whether a proxy passed this socket request along, on this machine or another
/// (RequestGuard::forwardedByProxy, the list every caller shares). Its sender
/// may stand anywhere.
bool passedAlong(const HttpRequest& req);

/// Whether @p address is this machine's own: the loopback, or what
/// Links::thisMachine says.
bool isThisMachine(const Links& l, const QString& address);

/// The house, where a device asks from and a paired session answers from: this
/// LAN as AuthManager::fromThisLan judges it, on one of this host's links. On a
/// line, where ICE found the caller. On a socket, under a local name too
/// (underALocalName): the public domain is how this host is reached from
/// anywhere, and only a line says where such a caller stands. A socket request
/// a proxy passed along (passedAlong) is not the house, and neither is a peer
/// that is this machine (isThisMachine), on a socket or on a line. Anywhere
/// else, a private address included, is the internet.
bool fromThisHousesLink(const Links& l, const HttpRequest& req);

} // namespace HouseLink
