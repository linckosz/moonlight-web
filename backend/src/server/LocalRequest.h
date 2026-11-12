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

#include <QByteArray>
#include <QString>

#include <optional>

/**
 * `moonlightweb --local-request "<METHOD> <path>"`: one request to the running
 * server over its local channel (server/LocalChannel.h), for a script on this
 * machine: a bench, a health check. The answer's body goes to standard output,
 * alone; its status to standard error.
 *
 * It asks nothing the channel does not already answer to a program of the
 * server's account, and less: the routes that hand out a credential, or that a
 * verb of its own already covers with its own rules, are not asked this way.
 * No session comes of it and no cookie.
 *
 * The rules are here, free of the channel and of the command line, so they are
 * tested as they are (tests/test_local_request.cpp).
 */
namespace LocalRequest {

struct Asked
{
    QString method; ///< "GET" or "POST"
    QString path;   ///< "/api/…", exact: no query
};

/// What the option's value names, or nothing: one method in capitals, one
/// space, one path from the root, with no query, no fragment and no second word.
std::optional<Asked> parse(const QString& value);

/// Whether this request may be asked this way: a route of the local channel
/// (RequestGuard::localChannelRoute), by its exact method and path, other than
///   GET  /api/server/remote-link   the owner's link: --print-link, --open-link
///   POST /api/admin/password       --set-admin-password, --clear-admin-password
///   POST /api/internet/enable      --enable-internet, which asks for consent
///   GET  /api/internet/upnp-probe  it talks to the router
/// Decided before the channel is opened.
bool allowed(const Asked& asked);

/// The largest body read from standard input: what one frame of the channel
/// carries.
qsizetype maxBodyBytes();

/// The answer's body as it may be printed. A JSON answer loses, at any depth,
/// every field named "pin", "unique_id" or "public_ip": the access PIN, which
/// GET /api/auth/status names to the host, this machine's permanent identifier
/// and its public address. A script that reads a status has no use for them,
/// and what a script prints is often kept. The one exception is the request
/// whose answer IS a PIN, POST /api/admin/pin/generate. A body that is not a
/// JSON object or array is left as it is.
///
/// What the answers of the routes asked this way carry, read route by route
/// when this was written; a route added to the channel is to be read the same
/// way before it is asked here:
///   /api/auth/status              the access PIN ("pin"): removed
///   /api/internet/status,         "unique_id" and "public_ip": removed. Its
///   /api/setup/status             LAN addresses and its rendezvous address
///                                 ("domain", "rendezvous") stay: a bench needs
///                                 the address a browser reaches it by, and is
///                                 to keep it out of what it prints. The paths
///                                 of an owner's certificate, never its contents
///   /api/native/status            displays, GPU, encoder, who is at the
///                                 console: a diagnosis
///   /api/server/stream-activity   device names, and an id that is a salted
///                                 hash made for this one use
///   /api/health, /api/server/status, /api/settings/streaming, and the answers
///   to the four POSTs             settings and states
/// None carries the admin key, a session's token, a host key or a link's key.
QByteArray printable(const Asked& asked, const QByteArray& body);

/// What a status is worth to the caller: 0 for a 2xx, 1 for anything else, an
/// answer that never came included.
int exitCodeFor(int status);

} // namespace LocalRequest
