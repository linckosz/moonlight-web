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

#include "server/routes/ShareRoutes.h"

#include "common/Logger.h"
#include "server/AuthManager.h"
#include "server/HttpServer.h"
#include "server/RestRouter.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>

namespace {

/// The cookie a player's browser keeps once it has entered the PIN. Separate
/// from mw_session on purpose: it never authenticates a MoonlightWeb *user*, it
/// only opens this player's own slot.
const char* const kPlayerCookie = "mw_player";

QString stateName(ShareManager::State s)
{
    switch (s) {
    case ShareManager::State::Off: return QStringLiteral("off");
    case ShareManager::State::Shared: return QStringLiteral("shared");
    case ShareManager::State::Binded: return QStringLiteral("binded");
    }
    return QStringLiteral("off");
}

QJsonObject slotJson(const ShareManager::SlotStatus& st)
{
    QJsonObject obj;
    obj[QStringLiteral("slot")] = st.slot;
    obj[QStringLiteral("state")] = stateName(st.state);
    obj[QStringLiteral("name")] = st.name;
    obj[QStringLiteral("permissions")] = st.permissions.toJson();
    obj[QStringLiteral("access_level")] = st.permissions.accessLevel();
    obj[QStringLiteral("streaming")] = st.streaming;
    obj[QStringLiteral("ttl_secs")] = st.ttlSecs;
    // null, not 0: an unlimited invitation has no deadline to count down to, and
    // a 0 would render as "expired in 1970".
    if (st.state != ShareManager::State::Off && st.ttlSecs == 0)
        obj[QStringLiteral("expires_at")] = QJsonValue::Null;
    else
        obj[QStringLiteral("expires_at")] = st.expiresAt;

    QJsonArray devices;
    for (const ShareManager::BoundDevice& d : st.devices) {
        QJsonObject dev;
        dev[QStringLiteral("bound_at")] = d.boundAt;
        dev[QStringLiteral("user_agent")] = d.userAgent;
        devices.append(dev);
    }
    obj[QStringLiteral("devices")] = devices;

    // The event an owner had no way of seeing before: someone else opened this
    // link with the right PIN and was turned away.
    if (st.lastRefusedAt > 0) {
        QJsonObject refused;
        refused[QStringLiteral("at")] = st.lastRefusedAt;
        refused[QStringLiteral("user_agent")] = st.lastRefusedAgent;
        obj[QStringLiteral("last_refused")] = refused;
    }
    return obj;
}

/// This machine's first routable IPv4 — the address a player on the LAN can
/// reach. Loopback would produce a link that only works on the host itself.
QString lanIPv4()
{
    for (const QHostAddress& addr : QNetworkInterface::allAddresses()) {
        if (addr.isLoopback() || addr.isNull()) continue;
        if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
        if (addr.isLinkLocal()) continue;
        return addr.toString();
    }
    return QStringLiteral("127.0.0.1"); // nothing better to offer
}

/// Slot number from a :n path parameter, or -1 when it is not a player slot.
int slotParam(const HttpRequest& req)
{
    bool ok = false;
    const int slot = req.pathParams.value(QStringLiteral("n")).toInt(&ok);
    return (ok && ShareManager::isPlayerSlot(slot)) ? slot : -1;
}

} // namespace

void registerShareRoutes(HttpServer& server, ShareManager& share, const ShareRoutesDeps& deps)
{
    // The flag is the whole switch: with no routes registered, every share URL
    // is an unknown path and answers 404 — indistinguishable from a build that
    // never had the feature.
    if constexpr (!ShareManager::kSessionSharingEnabled) {
        Logger::info(QStringLiteral("[Share] Session sharing disabled at build time"));
        return;
    }

    RestRouter* router = server.router();

    // The base a player's link is built on: the public domain once Internet
    // Access is live, otherwise this machine's LAN address and HTTPS port.
    // Copied into the route closures, and holds only references to objects that
    // outlive them (both live in main).
    auto shareOrigin = [&deps, &server]() -> QString {
        QString origin = deps.publicOrigin ? deps.publicOrigin() : QString();
        if (!origin.isEmpty()) return origin;
        const quint16 port = server.activeHttpsPort();
        return port == 443 ? QStringLiteral("https://%1").arg(lanIPv4())
                           : QStringLiteral("https://%1:%2").arg(lanIPv4()).arg(port);
    };

    // ── Owner side ──────────────────────────────────────────────────────────
    // Reachable by any authenticated MoonlightWeb user (the generic session gate
    // in HttpServer::processRequest covers these paths); deliberately NOT
    // admin-only. Cross-site drive-by is already refused by RequestGuard.

    // GET /api/share/status — the four rows of the sharing board.
    router->get(QStringLiteral("/api/share/status"), [&share](const HttpRequest&) {
        // Not named `slots`: Qt's moc keyword macro would eat it.
        QJsonArray slotArray;
        for (const ShareManager::SlotStatus& st : share.status())
            slotArray.append(slotJson(st));

        QJsonObject obj;
        obj[QStringLiteral("slots")] = slotArray;
        obj[QStringLiteral("streaming")] = share.streamingCount();
        return HttpResponse::json(obj);
    });

    // POST /api/share/slots/:n/activate — open a row: mint its link + PIN.
    router->post(
        QStringLiteral("/api/share/slots/:n/activate"),
        [&share, &deps, shareOrigin](const HttpRequest& req) {
            const int slot = slotParam(req);
            if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

            const QJsonObject body = QJsonDocument::fromJson(req.body).object();

            // Where this invitation leads. A stream in progress decides it — the
            // guest joins what the owner is already playing. With nothing
            // running the board says so instead, which is what lets a row be
            // opened cold from the host's kebab menu.
            const std::pair<QString, int> owner =
                deps.currentOwnerContext ? deps.currentOwnerContext() : std::pair<QString, int>{};
            QString hostUuid = owner.first;
            int appId = owner.second;
            if (hostUuid.isEmpty()) {
                hostUuid = body.value(QStringLiteral("host_uuid")).toString();
                appId = body.value(QStringLiteral("app_id")).toInt(-1);
                if (hostUuid.isEmpty())
                    return HttpResponse::error(400, "Pick a host to share");
                // Cold, so the pair is input rather than a fact about a running
                // stream. An app that does not exist would only fail much later,
                // in front of the guest.
                if (deps.hostAppExists && !deps.hostAppExists(hostUuid, appId))
                    return HttpResponse::error(404, "Unknown host or app");
            }

            const qint64 ttl = static_cast<qint64>(
                body.value(QStringLiteral("ttl_secs")).toDouble(ShareManager::kTtlSecs));
            if (!ShareManager::isValidTtl(ttl))
                return HttpResponse::error(400, "Unsupported lifetime");

            QString token;
            QString pin;
            ShareManager::SlotStatus st;
            if (!share.activate(slot, hostUuid, appId, ttl, token, pin, st))
                return HttpResponse::error(500, "Could not share this slot");

            QJsonObject obj = slotJson(st);
            obj[QStringLiteral("url")] = shareOrigin() + QStringLiteral("/p/") + token;
            obj[QStringLiteral("pin")] = pin;
            return HttpResponse::json(obj);
        });

    // POST /api/share/slots/:n/credentials — the same link and PIN again, for an
    // owner reopening the popin on a share that is already live. POST, not GET:
    // it hands out a credential, so it goes through the full CSRF/origin gate
    // rather than being reachable by a top-level navigation.
    router->post(QStringLiteral("/api/share/slots/:n/credentials"),
                 [&share, shareOrigin](const HttpRequest& req) {
                     const int slot = slotParam(req);
                     if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

                     QString token;
                     QString pin;
                     QJsonObject obj;
                     if (!share.secrets(slot, token, pin)) {
                         // Either nothing is shared, or the process restarted
                         // since: the digests came back from share.json, the
                         // clear pair did not. The popin says so instead of
                         // pretending the link is gone.
                         obj[QStringLiteral("available")] = false;
                         return HttpResponse::json(obj);
                     }
                     obj[QStringLiteral("available")] = true;
                     obj[QStringLiteral("url")] = shareOrigin() + QStringLiteral("/p/") + token;
                     obj[QStringLiteral("pin")] = pin;
                     return HttpResponse::json(obj);
                 });

    // POST /api/share/slots/:n/permissions — at any time, including mid-stream.
    // ShareManager pushes the new policy down to the live worker; nothing here
    // freezes, so a guest is promoted or demoted without losing their picture.
    router->post(
        QStringLiteral("/api/share/slots/:n/permissions"), [&share](const HttpRequest& req) {
            const int slot = slotParam(req);
            if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            ShareManager::Permissions perms = ShareManager::Permissions::fromJson(body);
            // Accepted whatever the row's state: shared, streaming, or not yet
            // started (where it records what START will be minted with).
            if (!share.setPermissions(slot, perms))
                return HttpResponse::error(404, "Unknown player slot");
            return HttpResponse::json(slotJson(share.status().at(slot - ShareManager::kFirstSlot)));
        });

    // POST /api/share/slots/:n/ttl — how long this invitation lives.
    router->post(QStringLiteral("/api/share/slots/:n/ttl"), [&share](const HttpRequest& req) {
        const int slot = slotParam(req);
        if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const qint64 ttl = static_cast<qint64>(body.value(QStringLiteral("ttl_secs")).toDouble(-1));
        if (!share.setTtl(slot, ttl)) return HttpResponse::error(400, "Unsupported lifetime");
        return HttpResponse::json(slotJson(share.status().at(slot - ShareManager::kFirstSlot)));
    });

    // POST /api/share/slots/:n/name — label a player row.
    router->post(QStringLiteral("/api/share/slots/:n/name"), [&share](const HttpRequest& req) {
        const int slot = slotParam(req);
        if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        if (!share.setName(slot, body.value(QStringLiteral("name")).toString()))
            return HttpResponse::error(400, "Could not rename this slot");
        return HttpResponse::json(slotJson(share.status().at(slot - ShareManager::kFirstSlot)));
    });

    // POST /api/share/slots/:n/deactivate — owner clicked a live player row.
    router->post(
        QStringLiteral("/api/share/slots/:n/deactivate"), [&share, &deps](const HttpRequest& req) {
            const int slot = slotParam(req);
            if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

            // Kill the worker first, then the credentials: the player
            // must not be able to rejoin between the two.
            if (deps.stopPlayerStream) deps.stopPlayerStream(slot, true);
            share.deactivate(slot, ShareManager::EndReason::OwnerToggle);
            return HttpResponse::json(slotJson(share.status().at(slot - ShareManager::kFirstSlot)));
        });

    // ── Player side ─────────────────────────────────────────────────────────
    // Exempt from the session gate (see HttpServer::processRequest) — a player
    // has no MoonlightWeb account. Each route proves the player's identity from
    // the mw_player cookie, and feeds ConnectionGuard when that fails.

    // POST /api/share/player/pin — {token, pin} → the player cookie.
    router->post(
        QStringLiteral("/api/share/player/pin"), [&share, &server](const HttpRequest& req) {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            const QString token = body.value(QStringLiteral("token")).toString();
            const QString pin = body.value(QStringLiteral("pin")).toString();

            const ShareManager::PinOutcome outcome = share.redeemPin(
                token, pin,
                AuthManager::rateLimitKey(AuthManager::cleanClientAddress(req.clientAddress)),
                req.headers.value(QStringLiteral("user-agent")));

            QJsonObject obj;
            switch (outcome.result) {
            case ShareManager::PinResult::Ok: {
                obj[QStringLiteral("status")] = QStringLiteral("ok");
                HttpResponse resp = HttpResponse::json(obj);
                // Scoped to the join surface, so a stolen player cookie cannot be
                // replayed against the app's own API. Max-Age follows this
                // invitation's own lease; an unlimited one gets a long finite
                // value, because "forever" is not something a cookie can say and
                // browsers cap it anyway.
                const qint64 ttl = share.ttlSecs(outcome.slot);
                resp.headers[QStringLiteral("Set-Cookie")] =
                    QStringLiteral("%1=%2; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=%3")
                        .arg(QLatin1String(kPlayerCookie), outcome.cookie)
                        .arg(ttl > 0 ? ttl : qint64(400) * 24 * 3600);
                return resp;
            }
            case ShareManager::PinResult::AlreadyBound:
                // The PIN was right, so this is not an attacker guessing — it is
                // the link in a second pair of hands. Say so plainly rather than
                // through a generic failure, and do not feed ConnectionGuard: the
                // person on the other end may well be the guest on a new laptop.
                obj[QStringLiteral("error")] = QStringLiteral("already_bound");
                return HttpResponse::json(obj, 409);
            case ShareManager::PinResult::RateLimited:
                obj[QStringLiteral("error")] = QStringLiteral("rate_limited");
                obj[QStringLiteral("lockout_seconds")] = outcome.lockoutSeconds;
                server.reportAuthFailure(req.clientAddress);
                return HttpResponse::json(obj, 429);
            case ShareManager::PinResult::Invalid:
                // One answer for a wrong PIN, an unknown token and an expired one.
                obj[QStringLiteral("error")] = QStringLiteral("invalid_pin");
                obj[QStringLiteral("remaining")] = outcome.remainingAttempts;
                obj[QStringLiteral("lockout_seconds")] = outcome.lockoutSeconds;
                server.reportAuthFailure(req.clientAddress);
                return HttpResponse::json(obj, 401);
            }
            return HttpResponse::error(500, "Internal error");
        });

    // GET /api/share/player/info?token=… — what the join page should display.
    router->get(QStringLiteral("/api/share/player/info"), [&share, &deps](const HttpRequest& req) {
        const QString token = req.queryParams.value(QStringLiteral("token"));

        QJsonObject obj;
        // A dead link is told so plainly. The token is 256 bits: whoever holds
        // one already knows it existed, so this reveals nothing an attacker
        // could use — while hiding it would make every expired link look like a
        // wrong PIN. The PIN is the secret; liveness is not.
        const int slot = share.slotForToken(token);
        if (slot < 0) {
            obj[QStringLiteral("error")] = QStringLiteral("dead_link");
            return HttpResponse::json(obj, 404);
        }

        // The slot comes from the *link*; the cookie only says whether this
        // device already passed that link's PIN. A browser still carrying
        // player 1's cookie opening player 2's link is a new guest, and must
        // enter player 2's PIN — otherwise it would be shown player 1's state.
        if (share.slotForCookie(HttpServer::cookieFromRequest(req, kPlayerCookie)) != slot) {
            obj[QStringLiteral("needs_pin")] = true;
            return HttpResponse::json(obj);
        }

        const ShareManager::Permissions perms = share.permissions(slot);
        obj[QStringLiteral("needs_pin")] = false;
        obj[QStringLiteral("machine_name")] = deps.machineName ? deps.machineName() : QString();
        obj[QStringLiteral("state")] = stateName(share.state(slot));
        obj[QStringLiteral("access_level")] = perms.accessLevel();
        obj[QStringLiteral("permissions")] = perms.toJson();
        obj[QStringLiteral("expires_at")] = share.expiresAt(slot);
        obj[QStringLiteral("owner_streaming")] =
            deps.ownerStreamAlive ? deps.ownerStreamAlive() : false;
        // Transparency, not a choice: the guest's session is counted (or not)
        // by the machine they are joining, and the privacy panel says which.
        obj[QStringLiteral("stats_reporting")] =
            deps.statsReporting ? deps.statsReporting() : false;
        return HttpResponse::json(obj);
    });

    // POST /api/share/player/join — {token, height} → start this player's stream.
    router->postAsync(
        QStringLiteral("/api/share/player/join"),
        [&share, &deps, &server](const HttpRequest& req, ResponseCallback respond) {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            const QString token = body.value(QStringLiteral("token")).toString();

            // Both factors, every time, and they must name the *same* slot: the
            // link says which player this is, the cookie proves that link's PIN
            // was entered on this device.
            const int slot = share.slotForToken(token);
            if (slot < 0 ||
                share.slotForCookie(HttpServer::cookieFromRequest(req, kPlayerCookie)) != slot ||
                share.state(slot) == ShareManager::State::Off) {
                server.reportAuthFailure(req.clientAddress);
                QJsonObject obj;
                obj[QStringLiteral("error")] = QStringLiteral("dead_link");
                respond(HttpResponse::json(obj, 403));
                return;
            }

            if (share.isStreaming(slot)) {
                // Never steal a running stream: the other device has to leave.
                QJsonObject obj;
                obj[QStringLiteral("error")] = QStringLiteral("stream_in_progress");
                respond(HttpResponse::json(obj, 409));
                return;
            }

            // Nothing running is no longer a dead end: an invitation opened cold
            // carries the app it was opened on, and the guest's arrival is what
            // starts it. startPlayerStream decides between resuming into a live
            // session and launching that app — including telling the guest when
            // the app has since been removed from the host.

            // Only the resolution comes from the player: the height from a fixed
            // set, plus their screen aspect ("W:H") so the stream fills their
            // display. Everything else — fps, codec, bitrate — is decided here.
            const int requested = body.value(QStringLiteral("height")).toInt(1080);
            const int height = (requested == 720 || requested == 1440) ? requested : 1080;
            const QString aspect = body.value(QStringLiteral("aspect")).toString();

            if (!deps.startPlayerStream) {
                respond(HttpResponse::error(503, "Streaming unavailable"));
                return;
            }

            // The signaling URL the worker hands back has to name the host this
            // player actually reached — the public domain, the LAN IP, whatever
            // is in their address bar. Leaving it empty produced an unparseable
            // URL, and the browser fell back to /ws: the owner's slot.
            QString serverHost = req.headers.value(QStringLiteral("host"));
            const int colon = serverHost.indexOf(QLatin1Char(':'));
            if (colon >= 0) serverHost = serverHost.left(colon);

            deps.startPlayerStream(slot, height, aspect, share.permissions(slot), serverHost,
                                   respond);
        });

    // POST /api/share/player/leave — the player pressed Leave.
    router->post(
        QStringLiteral("/api/share/player/leave"), [&share, &deps](const HttpRequest& req) {
            const int slot = share.slotForCookie(HttpServer::cookieFromRequest(req, kPlayerCookie));
            if (slot < 0) return HttpResponse::error(403, "Forbidden");

            // Leaving frees the slot for a later join — it does NOT end the share.
            if (deps.stopPlayerStream) deps.stopPlayerStream(slot, false);
            QJsonObject obj;
            obj[QStringLiteral("status")] = QStringLiteral("ok");
            return HttpResponse::json(obj);
        });

    Logger::info(QStringLiteral("[Share] Session sharing enabled (slots %1-%2)")
                     .arg(ShareManager::kFirstSlot)
                     .arg(ShareManager::kLastSlot));
}
