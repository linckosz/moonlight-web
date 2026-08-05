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
    case ShareManager::State::Streaming: return QStringLiteral("streaming");
    }
    return QStringLiteral("off");
}

QJsonObject slotJson(const ShareManager::SlotStatus& st)
{
    QJsonObject obj;
    obj[QStringLiteral("slot")] = st.slot;
    obj[QStringLiteral("state")] = stateName(st.state);
    obj[QStringLiteral("permissions")] = st.permissions.toJson();
    obj[QStringLiteral("access_level")] = st.permissions.accessLevel();
    obj[QStringLiteral("locked")] = st.locked;
    obj[QStringLiteral("expires_at")] = st.expiresAt;
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

    // ── Owner side ──────────────────────────────────────────────────────────
    // Reachable by any authenticated MoonlightWeb user (the generic session gate
    // in HttpServer::processRequest covers these paths); deliberately NOT
    // admin-only. Cross-site drive-by is already refused by RequestGuard.

    // GET /api/share/status — the three player rows behind the Share button.
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

    // POST /api/share/slots/:n/activate — mint the link + PIN shown in the popin.
    router->post(QStringLiteral("/api/share/slots/:n/activate"),
                 [&share, &deps, &server](const HttpRequest& req) {
                     const int slot = slotParam(req);
                     if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

                     QString token;
                     QString pin;
                     ShareManager::SlotStatus st;
                     if (!share.activate(slot, token, pin, st))
                         return HttpResponse::error(500, "Could not share this slot");

                     QString origin = deps.publicOrigin ? deps.publicOrigin() : QString();
                     if (origin.isEmpty()) {
                         const quint16 port = server.activeHttpsPort();
                         origin = port == 443
                                      ? QStringLiteral("https://%1").arg(lanIPv4())
                                      : QStringLiteral("https://%1:%2").arg(lanIPv4()).arg(port);
                     }

                     QJsonObject obj = slotJson(st);
                     // The raw token and PIN exist in this response and nowhere
                     // else — only their digests are kept.
                     obj[QStringLiteral("url")] = origin + QStringLiteral("/p/") + token;
                     obj[QStringLiteral("pin")] = pin;
                     return HttpResponse::json(obj);
                 });

    // POST /api/share/slots/:n/permissions — while the popin is open.
    router->post(QStringLiteral("/api/share/slots/:n/permissions"),
                 [&share](const HttpRequest& req) {
                     const int slot = slotParam(req);
                     if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

                     const QJsonObject body = QJsonDocument::fromJson(req.body).object();
                     ShareManager::Permissions perms = ShareManager::Permissions::fromJson(body);
                     if (!share.setPermissions(slot, perms)) {
                         // Either nothing is shared, or the popin already closed
                         // and a worker may be carrying this policy.
                         QJsonObject obj;
                         obj[QStringLiteral("error")] = QStringLiteral("permissions_locked");
                         return HttpResponse::json(obj, 409);
                     }
                     return HttpResponse::json(slotJson(share.status().at(slot -
                                                                         ShareManager::kFirstSlot)));
                 });

    // POST /api/share/slots/:n/lock — the popin closed; the choice is final.
    router->post(QStringLiteral("/api/share/slots/:n/lock"), [&share](const HttpRequest& req) {
        const int slot = slotParam(req);
        if (slot < 0) return HttpResponse::error(404, "Unknown player slot");
        if (!share.lockPermissions(slot)) return HttpResponse::error(409, "Nothing shared");
        return HttpResponse::json(slotJson(share.status().at(slot - ShareManager::kFirstSlot)));
    });

    // POST /api/share/slots/:n/deactivate — owner clicked a live player row.
    router->post(QStringLiteral("/api/share/slots/:n/deactivate"),
                 [&share, &deps](const HttpRequest& req) {
                     const int slot = slotParam(req);
                     if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

                     // Kill the worker first, then the credentials: the player
                     // must not be able to rejoin between the two.
                     if (deps.stopPlayerStream) deps.stopPlayerStream(slot, true);
                     share.deactivate(slot, ShareManager::EndReason::OwnerToggle);
                     return HttpResponse::json(
                         slotJson(share.status().at(slot - ShareManager::kFirstSlot)));
                 });

    // ── Player side ─────────────────────────────────────────────────────────
    // Exempt from the session gate (see HttpServer::processRequest) — a player
    // has no MoonlightWeb account. Each route proves the player's identity from
    // the mw_player cookie, and feeds ConnectionGuard when that fails.

    // POST /api/share/player/pin — {token, pin} → the player cookie.
    router->post(QStringLiteral("/api/share/player/pin"), [&share,
                                                          &server](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString token = body.value(QStringLiteral("token")).toString();
        const QString pin = body.value(QStringLiteral("pin")).toString();

        const ShareManager::PinOutcome outcome = share.redeemPin(
            token, pin, AuthManager::rateLimitKey(AuthManager::cleanClientAddress(req.clientAddress)));

        QJsonObject obj;
        switch (outcome.result) {
        case ShareManager::PinResult::Ok: {
            obj[QStringLiteral("status")] = QStringLiteral("ok");
            HttpResponse resp = HttpResponse::json(obj);
            // Scoped to the join surface, so a stolen player cookie cannot be
            // replayed against the app's own API. Max-Age matches the 8h lease.
            resp.headers[QStringLiteral("Set-Cookie")] =
                QStringLiteral("%1=%2; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=%3")
                    .arg(QLatin1String(kPlayerCookie), outcome.cookie)
                    .arg(ShareManager::kTtlSecs);
            return resp;
        }
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
    router->get(QStringLiteral("/api/share/player/info"), [&share,
                                                          &deps](const HttpRequest& req) {
        const QString token = req.queryParams.value(QStringLiteral("token"));

        QJsonObject obj;
        // A dead link is told so plainly. The token is 256 bits: whoever holds
        // one already knows it existed, so this reveals nothing an attacker
        // could use — while hiding it would make every expired link look like a
        // wrong PIN. The PIN is the secret; liveness is not.
        if (!share.tokenIsLive(token)) {
            obj[QStringLiteral("error")] = QStringLiteral("dead_link");
            return HttpResponse::json(obj, 404);
        }

        const int slot = share.slotForCookie(HttpServer::cookieFromRequest(req, kPlayerCookie));
        if (slot < 0) {
            // Live link, but this device has not proved anything yet.
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
        obj[QStringLiteral("owner_streaming")] = deps.ownerStreamAlive ? deps.ownerStreamAlive()
                                                                      : false;
        return HttpResponse::json(obj);
    });

    // POST /api/share/player/join — {token, height} → start this player's stream.
    router->postAsync(
        QStringLiteral("/api/share/player/join"),
        [&share, &deps, &server](const HttpRequest& req, ResponseCallback respond) {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            const QString token = body.value(QStringLiteral("token")).toString();

            const int slot = share.slotForCookie(HttpServer::cookieFromRequest(req, kPlayerCookie));
            // Both factors, every time: the cookie proves the PIN was entered,
            // the token proves the owner's share is still the current one.
            if (slot < 0 || !share.tokenIsLive(token) || share.state(slot) ==
                                                             ShareManager::State::Off) {
                server.reportAuthFailure(req.clientAddress);
                QJsonObject obj;
                obj[QStringLiteral("error")] = QStringLiteral("dead_link");
                respond(HttpResponse::json(obj, 403));
                return;
            }

            if (share.state(slot) == ShareManager::State::Streaming) {
                // Never steal a running stream: the other device has to leave.
                QJsonObject obj;
                obj[QStringLiteral("error")] = QStringLiteral("stream_in_progress");
                respond(HttpResponse::json(obj, 409));
                return;
            }

            if (deps.ownerStreamAlive && !deps.ownerStreamAlive()) {
                // Sunshine only resumes into a running app, and a player never
                // launches one. Answer before touching the host.
                QJsonObject obj;
                obj[QStringLiteral("error")] = QStringLiteral("session_ended");
                respond(HttpResponse::json(obj, 409));
                return;
            }

            // Only the resolution comes from the player, and only from a fixed
            // set. Everything else — fps, codec, bitrate — is decided here.
            const int requested = body.value(QStringLiteral("height")).toInt(1080);
            const int height = (requested == 720 || requested == 1440) ? requested : 1080;

            if (!deps.startPlayerStream) {
                respond(HttpResponse::error(503, "Streaming unavailable"));
                return;
            }
            deps.startPlayerStream(slot, height, share.permissions(slot), respond);
        });

    // POST /api/share/player/leave — the player pressed Leave.
    router->post(QStringLiteral("/api/share/player/leave"), [&share,
                                                            &deps](const HttpRequest& req) {
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
