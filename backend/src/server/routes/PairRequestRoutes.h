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

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

#include <functional>

class AppSettings;
class AuthManager;
class RestRouter;

/// What the pairing routes take from the server around them, as functions:
/// the tests stand in for the server, its location lookup and its clock.
struct PairRequestDeps
{
    /// This host's MW-BIND-v1 public key, SPKI DER (AppSettings::hostSigningPublicKey).
    std::function<QByteArray()> hostKey;
    /// The raw session cookie a request carries (HttpServer::sessionTokenFromRequest).
    std::function<QString(const HttpRequest&)> sessionToken;
    /// A failure fed to the connection guard (HttpServer::reportAuthFailure).
    std::function<void(const QString& address)> reportAuthFailure;
    /// Where a new session is, looked up and stored (GeoIpService); may be empty.
    std::function<void(const QString& token, const QString& address)> locate;
    /// This machine's public link while its line is up, empty otherwise: the
    /// page the QR code opens. May be empty.
    std::function<QString()> rendezvousUrl;
    /// Seconds since the epoch.
    std::function<qint64()> now;
    /// Whether the administrator lets a device ask from outside this LAN too
    /// (AppSettings::pairRequestInternet). Empty: no.
    std::function<bool()> internetRequests;
    /// Whether the administrator lets the server look up where an address is
    /// (AppSettings::sessionLocationEnabled), read on every request. Empty: no.
    std::function<bool()> locationAllowed;
    /// The city an address is in as far as the server knows it, empty when it
    /// does not know yet; a lookup is started then (GeoIpService). Asked only
    /// while locationAllowed says yes. May be empty.
    std::function<QString(const QString& address)> cityOf;
    /// This host's own links: each interface address with its prefix length,
    /// loopback left out (QNetworkInterface). Where a paired session must stand
    /// to answer a device without the administrator's right. Empty: nowhere.
    std::function<QList<QPair<QHostAddress, int>>()> localSubnets;
    /// Whether an address is one of this machine's own (HttpServer::isLocalRequest):
    /// such a peer is never the house, to ask or to answer; its administrator
    /// answers as such. Empty: the loopback only.
    std::function<bool(const QString& address)> thisMachine;
};

/// Register /api/auth/pair-request…: a device paired from the QR code it shows,
/// approved by this host's administrator (server/PhonePairing.h has the
/// protocol and what each side may do).
void registerPairRequestRoutes(RestRouter& router, AuthManager& authManager, PairRequestDeps deps);

/// POST /api/admin/settings's `pair_request_internet` (the admin page's box for
/// QR code requests from the internet), written for the administrator alone,
/// with the right every key of that route asks for (HttpRequest::isLocal).
/// Returns whether @p body carried the key and @p settings took it; @p reply
/// then echoes the value kept. Anyone else changes nothing.
bool writePairRequestInternet(const HttpRequest& req, const QJsonObject& body,
                              AppSettings& settings, QJsonObject& reply);
