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
#include "server/RequestGuard.h"
#include "server/RestRouter.h"

#include <QJsonDocument>

#include <utility>

HostAddress hostAddressOf(const QString& address,
                          const std::function<bool(const QString& address)>& thisMachine,
                          const QString& publicIp)
{
    if (thisMachine && thisMachine(address)) return HostAddress::ThisMachine;
    if (!publicIp.isEmpty() && address == publicIp) return HostAddress::PublicAddress;
    return HostAddress::NotOurs;
}

bool plausiblyHostAddress(const QString& address,
                          const std::function<bool(const QString& address)>& thisMachine,
                          const QString& publicIp)
{
    return hostAddressOf(address, thisMachine, publicIp) != HostAddress::NotOurs;
}

void registerHostKeyRoute(RestRouter& router, AuthManager& authManager, HostKeyDeps deps)
{
    // POST /api/auth/host-key — prove the browser runs on the host machine.
    // The host's own entry points open the browser on a link that carries a
    // key made for that one visit (HostKeyVault); the frontend spends it here
    // for a session that says "this browser is the host machine's". Redemption
    // is refused for peers that cannot plausibly be the host machine itself.
    //
    // Two ways in, and the difference is recorded on the session because it
    // decides what that session is worth afterwards. Either way the key rides
    // in the FRAGMENT of the link (#k=...), the one part of a URL a browser
    // never sends to the server it fetched from:
    //
    //   socket  a link to this machine's own address.
    //   tunnel  a rendezvous link. The introduction server never sees the key,
    //           and it reaches this handler inside the tunnel's own
    //           encryption. This is what gives the owner an admin page under a
    //           certificate the browser already trusts.
    //
    // The plausibility gate below is what makes the tunnel case safe, and it is
    // stronger there than it looks: for a tunnel peer clientAddress is the
    // address ICE actually settled on — where our own stack is sending packets,
    // not anything the browser said. A forwarded link redeemed from elsewhere
    // arrives from an address that is not ours.
    //
    // The session is bound to the browser's MW-BIND-v1 key, as a PIN login binds
    // its own: the offer of a stream is then signed for that key, and the
    // browser checks it against the host key it holds. Over a rendezvous line
    // the key is the line's own — the one its hello presented and proved, and
    // the one the page pinned this host under — so a page has nothing to send
    // and a line without one opens nothing. On a socket the page names its key
    // in `public_key`; one that names none opens nothing either, and its link
    // stays good for a browser that can.
    router.post("/api/auth/host-key", [&authManager,
                                       deps = std::move(deps)](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString key = body.value("key").toString();
        const QString addr = AuthManager::cleanClientAddress(req.clientAddress);
        const QString publicIp = deps.publicIp ? deps.publicIp() : QString();
        // On a socket, a request a proxy forwarded comes from wherever the proxy
        // was reached: its peer address says nothing about the browser.
        const bool forwarded = !req.viaTunnel && RequestGuard::forwardedByProxy(req.headers);
        const HostAddress whose = hostAddressOf(addr, deps.thisMachine, publicIp);
        const bool plausiblyHost = !forwarded && whose != HostAddress::NotOurs;
        // Say which gate closed: the address is the one ICE settled on for a
        // tunnel peer, and whether it passed is the whole question when the
        // owner's own link lands on the PIN page. Never the key, nor anything
        // made from it.
        const auto refuse = [&](const QString& why) {
            static int refusals = 0;
            Logger::warning(
                QStringLiteral("[Auth] Host key redemption refused for %1 (%2; %3; %4 refused "
                               "since this server started)")
                    .arg(addr,
                         req.viaTunnel ? QStringLiteral("via tunnel") : QStringLiteral("direct"),
                         why)
                    .arg(++refusals));
            if (deps.refused) deps.refused(req.clientAddress);
            return HttpResponse::error(403, "Invalid host key");
        };
        if (!plausiblyHost) return refuse(QStringLiteral("address not plausibly this machine"));
        if (key.isEmpty()) return refuse(QStringLiteral("no key given"));
        // The browser key is checked before the link's key is spent: a refusal
        // here leaves the owner's link good for a browser that can hold the
        // session.
        if (req.viaTunnel && !PairingCrypto::isValidP256Spki(req.tunnelKey)) {
            Logger::warning(QStringLiteral("[Auth] Host key redemption refused for %1 (via tunnel; "
                                           "the line carries no browser key)")
                                .arg(addr));
            return HttpResponse::error(403, "Invalid host key");
        }
        // On a socket the page names its key in `public_key`, and has to: a
        // host session no offer can be signed for is one whose streams the
        // browser itself refuses.
        const QString named = req.viaTunnel ? QString() : body.value("public_key").toString();
        const QByteArray browserKey =
            req.viaTunnel ? req.tunnelKey : QByteArray::fromBase64(named.toUtf8());
        if (!req.viaTunnel && !PairingCrypto::isValidP256Spki(browserKey)) {
            Logger::warning(QStringLiteral("[Auth] Host key redemption refused for %1 (direct; %2)")
                                .arg(addr, named.isEmpty()
                                               ? QStringLiteral("the page names no browser key")
                                               : QStringLiteral("the browser key it names does "
                                                                "not parse")));
            return HttpResponse::error(400, "Invalid 'public_key' field");
        }

        // The key, last of the refusals and in one step: it is spent by being
        // recognised, so one that was good is used up whatever happens next,
        // and a link read from a history or a log opens nothing a second time.
        if (!deps.spend || !deps.spend(key)) return refuse(QStringLiteral("key does not match"));

        QString token =
            authManager.createSession(req.clientAddress,
                                      req.viaTunnel ? QStringLiteral("Host machine (remote link)")
                                                    : QStringLiteral("Host machine"),
                                      true, false, req.viaTunnel);
        // Which of the two the address was: when the owner's own link lands on
        // the PIN page on another network, this line of a visit that worked is
        // what the refusal is read against.
        Logger::info(
            QStringLiteral("[Auth] Host session created for %1 (via host key; %2; %3)")
                .arg(addr, req.viaTunnel ? QStringLiteral("via tunnel") : QStringLiteral("direct"),
                     whose == HostAddress::ThisMachine
                         ? QStringLiteral("one of this machine's addresses")
                         : QStringLiteral("this machine's public address")));

        QJsonObject obj;
        obj["status"] = "ok";
        obj["is_localhost"] = true;

        // A host session that holds no browser key is what this route no longer
        // makes. A session just made, given a key that parses, takes it; should
        // it ever not, the session ends here and the answer is an error. The
        // link's key stays spent.
        if (!authManager.bindSessionKey(token, browserKey)) {
            authManager.logoutSession(token);
            Logger::warning(QStringLiteral("[Auth] Host session for %1 ended at once: it could "
                                           "not take the browser's key")
                                .arg(addr));
            return HttpResponse::error(500, "The session could not take the browser's key");
        }
        obj["pairing_key"] = "bound";
        const QJsonObject identity = deps.hostIdentity ? deps.hostIdentity() : QJsonObject();
        for (auto it = identity.constBegin(); it != identity.constEnd(); ++it)
            obj[it.key()] = it.value();

        HttpResponse resp = HttpResponse::json(obj);
        resp.headers["Set-Cookie"] =
            QString("mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000")
                .arg(token);
        return resp;
    });
}
