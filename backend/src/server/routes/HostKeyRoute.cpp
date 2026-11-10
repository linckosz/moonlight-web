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

#include "server/routes/HostKeyRoute.h"

#include "common/Logger.h"
#include "common/PairingCrypto.h"
#include "server/AuthManager.h"
#include "server/RestRouter.h"

#include <QCryptographicHash>
#include <QJsonDocument>

#include <utility>

void registerHostKeyRoute(RestRouter& router, AuthManager& authManager, HostKeyDeps deps)
{
    // POST /api/auth/host-key — prove the browser runs on the host machine.
    // The host's own entry points embed a persistent random key; the frontend
    // redeems it here to obtain a localhost-equivalent session (admin access),
    // since anywhere but loopback the peer address is not the machine's own.
    // Redemption is refused for peers that cannot plausibly be the host machine
    // itself.
    //
    // Two ways in, and the difference is recorded on the session because it
    // decides what that session is worth afterwards:
    //
    //   socket  the key rides in ?mwk= on a local URL. The query reaches only
    //           this machine, so it is safe there and nowhere else.
    //   tunnel  the key rides in the FRAGMENT of a rendezvous link (#k=...),
    //           which is the one part of a URL a browser never sends to the
    //           server it fetched from — so the introduction server never sees
    //           it, and it reaches this handler inside the tunnel's own
    //           encryption. This is what gives the owner an admin page under a
    //           certificate the browser already trusts.
    //
    // The plausibility gate below is what makes the tunnel case safe, and it is
    // stronger there than it looks: for a tunnel peer clientAddress is the
    // address ICE actually settled on — where our own stack is sending packets,
    // not anything the browser said. A forwarded link redeemed from elsewhere on
    // the internet arrives from an address that is neither private nor ours.
    //
    // The session is bound to the browser's MW-BIND-v1 key, as a PIN login binds
    // its own: the offer of a stream is then signed for that key, and the
    // browser checks it against the host key it holds. Over a rendezvous line
    // the key is the line's own — the one its hello presented and proved, and
    // the one the page pinned this host under — so a page has nothing to send
    // and a line without one opens nothing. On a socket the page names its key
    // in `public_key`; one that names none keeps the session it always had.
    router.post("/api/auth/host-key", [&authManager,
                                       deps = std::move(deps)](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString key = body.value("key").toString();
        const QString addr = AuthManager::cleanClientAddress(req.clientAddress);
        const QString publicIp = deps.publicIp ? deps.publicIp() : QString();
        const bool plausiblyHost = (deps.thisMachine && deps.thisMachine(addr)) ||
                                   AuthManager::isPrivateIP(addr) == QLatin1String("Local") ||
                                   (!publicIp.isEmpty() && addr == publicIp);
        // Compare hashes: constant-time, no length leak.
        const QByteArray given = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256);
        const QByteArray expected = QCryptographicHash::hash(
            (deps.key ? deps.key() : QString()).toUtf8(), QCryptographicHash::Sha256);
        if (!plausiblyHost || key.isEmpty() || given != expected) {
            // Say which gate closed: the address is the one ICE settled on for
            // a tunnel peer, and whether it passed is the whole question when
            // the owner's own link lands on the PIN page.
            Logger::warning(
                QStringLiteral("[Auth] Host key redemption refused for %1 (%2; %3)")
                    .arg(addr,
                         req.viaTunnel ? QStringLiteral("via tunnel") : QStringLiteral("direct"),
                         !plausiblyHost  ? QStringLiteral("address not plausibly this machine")
                         : key.isEmpty() ? QStringLiteral("no key given")
                                         : QStringLiteral("key does not match")));
            return HttpResponse::error(403, "Invalid host key");
        }
        // The browser key is checked before the link's key is spent: a refusal
        // here leaves the owner's link good for a browser that can hold the
        // session.
        if (req.viaTunnel && !PairingCrypto::isValidP256Spki(req.tunnelKey)) {
            Logger::warning(QStringLiteral("[Auth] Host key redemption refused for %1 (via tunnel; "
                                           "the line carries no browser key)")
                                .arg(addr));
            return HttpResponse::error(403, "Invalid host key");
        }
        const QString named = req.viaTunnel ? QString() : body.value("public_key").toString();
        const QByteArray browserKey =
            req.viaTunnel ? req.tunnelKey : QByteArray::fromBase64(named.toUtf8());
        if (!named.isEmpty() && !PairingCrypto::isValidP256Spki(browserKey)) {
            Logger::warning(QStringLiteral("[Auth] Host key redemption refused for %1 (direct; the "
                                           "browser key it names does not parse)")
                                .arg(addr));
            return HttpResponse::error(400, "Invalid 'public_key' field");
        }

        QString token =
            authManager.createSession(req.clientAddress,
                                      req.viaTunnel ? QStringLiteral("Host machine (remote link)")
                                                    : QStringLiteral("Host machine"),
                                      true, false, req.viaTunnel);
        Logger::info(QStringLiteral("[Auth] Host session created for %1 (via host key)").arg(addr));

        // Single-use: burn the redeemed key and rewrite the entry points that
        // embed it, so a leaked/history URL cannot be replayed.
        if (deps.spent) deps.spent();

        QJsonObject obj;
        obj["status"] = "ok";
        obj["is_localhost"] = true;

        if (!browserKey.isEmpty()) {
            if (authManager.bindSessionKey(token, browserKey)) {
                obj["pairing_key"] = "bound";
                const QJsonObject identity =
                    deps.hostIdentity ? deps.hostIdentity() : QJsonObject();
                for (auto it = identity.constBegin(); it != identity.constEnd(); ++it)
                    obj[it.key()] = it.value();
            } else {
                obj["pairing_key"] = "rejected";
            }
        } else {
            Logger::info(QStringLiteral("[Auth] Host session for %1 holds no browser key: the "
                                        "page named none")
                             .arg(addr));
        }

        HttpResponse resp = HttpResponse::json(obj);
        resp.headers["Set-Cookie"] =
            QString("mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000")
                .arg(token);
        return resp;
    });
}
