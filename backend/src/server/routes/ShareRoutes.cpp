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
#include "network/LinkAddresses.h"
#include "network/UPNPClient.h"
#include "server/AuthManager.h"
#include "server/HttpServer.h"
#include "server/NetClassify.h"
#include "server/OwnerProof.h"
#include "server/RestRouter.h"
#include "server/routes/LogRoutes.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#include <optional>

namespace {

/// The cookie a player's browser keeps once it has entered the PIN. Separate
/// from mw_session on purpose: it never authenticates a MoonlightWeb *user*, it
/// only opens this player's own slot.
const char* const kPlayerCookie = ShareManager::kPlayerCookie;

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
    if (!st.opening.isEmpty()) obj[QStringLiteral("opening")] = st.opening;

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

/// This machine's LAN IPv4 when LinkAddresses found no LAN at all: the ranked
/// list's head, the default-route address, else an adapter that has a gateway.
/// Loopback would produce a link that only works on the host itself.
QString lanIPv4()
{
    return UPNPClient::getLocalIPs().value(0, QStringLiteral("127.0.0.1"));
}

/// The board's address menu, Automatic first.
QJsonArray linkAddressesJson(const QList<LinkAddresses::Choice>& list)
{
    QJsonArray out;
    QJsonObject automatic;
    automatic[QStringLiteral("id")] = QStringLiteral("auto");
    out.append(automatic);
    for (const LinkAddresses::Choice& c : list) {
        QJsonObject o;
        o[QStringLiteral("id")] = c.id;
        o[QStringLiteral("kind")] = QString::fromLatin1(LinkAddresses::kindName(c.kind));
        o[QStringLiteral("address")] = c.address;
        o[QStringLiteral("adapter")] = c.adapter;
        out.append(o);
    }
    return out;
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

    // The board's proofs that it is the browser its session was paired with
    // (OwnerProof). One store for every route, copied into each closure.
    auto proofs = std::make_shared<OwnerProof>();

    // Asked of every request that hands out a guest's way in: nullopt when it
    // may go on, else the refusal to answer with. A proof that does not hold
    // counts against the caller like a wrong PIN; one that is missing does not,
    // since a page loaded before this host was updated simply sends none.
    auto ownerRefusal = [&server, &deps, proofs](const HttpRequest& req, const QJsonObject& body,
                                                 const QString& action,
                                                 int slot) -> std::optional<HttpResponse> {
        const QByteArray key = deps.ownerProofKey ? deps.ownerProofKey(req) : QByteArray();
        if (key.isEmpty()) return std::nullopt;
        const OwnerProof::Result r =
            proofs->check(HttpServer::sessionTokenFromRequest(req), key,
                          deps.hostKeyId ? deps.hostKeyId() : QString(), action, slot,
                          body.value(QStringLiteral("owner_proof")).toObject(),
                          QDateTime::currentMSecsSinceEpoch());
        if (r == OwnerProof::Result::Ok) return std::nullopt;
        if (r == OwnerProof::Result::Refused) {
            server.reportAuthFailure(req.clientAddress);
            Logger::warning(QStringLiteral("[Share] Board request '%1' refused: its key proof "
                                           "does not hold")
                                .arg(action));
        }
        QJsonObject obj;
        obj[QStringLiteral("error")] = QStringLiteral("This browser is not the one this session "
                                                      "was paired with");
        obj[QStringLiteral("code")] = r == OwnerProof::Result::Missing
                                          ? QStringLiteral("owner_proof_required")
                                          : QStringLiteral("owner_proof_refused");
        return HttpResponse::json(obj, 403);
    };

    // POST /api/share/owner-proof — a challenge for the next request of the
    // board that hands out a guest's way in, when this session has a key to
    // prove: {required:false} otherwise, and the board signs nothing.
    router->post(QStringLiteral("/api/share/owner-proof"), [&deps, proofs](const HttpRequest& req) {
        QJsonObject obj;
        const QByteArray key = deps.ownerProofKey ? deps.ownerProofKey(req) : QByteArray();
        obj[QStringLiteral("required")] = !key.isEmpty();
        if (key.isEmpty()) return HttpResponse::json(obj);
        const QByteArray nonce = proofs->issue(HttpServer::sessionTokenFromRequest(req),
                                               QDateTime::currentMSecsSinceEpoch());
        if (nonce.isEmpty()) return HttpResponse::error(503, "Too many requests in flight");
        obj[QStringLiteral("nonce")] = QString::fromLatin1(nonce.toBase64());
        obj[QStringLiteral("host_id")] = deps.hostKeyId ? deps.hostKeyId() : QString();
        return HttpResponse::json(obj);
    });

    // The networks the board offers, the owner's own domain first when
    // settings.json names one.
    auto menu = [&server]() {
        return LinkAddresses::withDomain(LinkAddresses::choices(LinkAddresses::readAdapters()),
                                         server.domain());
    };

    // The address a guest opens, whole.
    //
    // The owner may have pinned the host's links to one address on its board
    // (LinkAddresses): their domain, the LAN, Tailscale, another card. That
    // address is used as long as this PC still has it; once it is gone
    // (Tailscale disconnected) the link falls back to Automatic and says so,
    // and the pick is kept for when it comes back.
    //
    // Automatic: the rendezvous answers it whenever this machine has a line to the outside:
    // the path names the machine and the invitation rides in the fragment, where
    // the introduction server never sees it. Without that line, in this order:
    //   - the owner's own domain (settings.json "domain"), which they serve on
    //     443 with their own certificate (issue #41);
    //   - the address the owner opened this board on: a name or a forward that
    //     reached this PC from their browser leads a guest to it too. Never
    //     loopback, never the rendezvous tunnel, and only ever an owner's
    //     request: both routes that build a link are on the owner's side;
    //   - this machine's own LAN address, which reaches a guest on the same
    //     network and nobody else — so the caller is told, and the board says
    //     so rather than letting an owner send a link that cannot open.
    //
    // Never loopback either way: the guest is on another PC.
    //
    // Both answers come out of one look, deliberately. Asking twice would let a
    // line that dropped in between hand out a rendezvous address flagged as
    // LAN-only, or the reverse — a rare disagreement, and the kind nobody would
    // ever reproduce.
    //
    // `reach` names where the link opens from (public, domain, direct, lan,
    // tailscale, other); `local_only` is kept for a board that predates it.
    // "domain" and "direct" go straight to this PC under a name or a forward
    // nothing here can test, so the board claims nothing about them.
    //
    // Copied into the route closures, and holds only references to objects that
    // outlive them (all three live in main).
    auto fillLink = [&deps, &server, &share, menu](QJsonObject& obj, const QString& token,
                                                   const QString& hostUuid,
                                                   const HttpRequest& req) {
        const QString wanted = share.linkAddress(hostUuid);
        const QList<LinkAddresses::Choice> list = menu();
        const QString path = QStringLiteral("/p/") + token;
        auto direct = [&obj, &path](const QString& origin, const QString& reach) {
            obj[QStringLiteral("url")] = origin + path;
            obj[QStringLiteral("reach")] = reach;
            obj[QStringLiteral("local_only")] = reach == QLatin1String("lan") ||
                                                reach == QLatin1String("tailscale") ||
                                                reach == QLatin1String("other");
        };
        std::optional<LinkAddresses::Choice> pinned;
        if (wanted != QLatin1String("auto")) {
            pinned = LinkAddresses::find(list, wanted);
            if (!pinned) obj[QStringLiteral("link_address_missing")] = true;
        }
        if (!pinned) {
            const QString remote = deps.playerLink ? deps.playerLink(token) : QString();
            if (!remote.isEmpty()) {
                obj[QStringLiteral("url")] = remote;
                obj[QStringLiteral("reach")] = QStringLiteral("public");
                obj[QStringLiteral("local_only")] = false;
                return;
            }
            pinned = LinkAddresses::find(list, QStringLiteral("domain"));
            if (!pinned && !req.viaTunnel) {
                // A reverse proxy in front of this PC names the address the
                // browser used; anyone else's word for it is the Host itself.
                const NetClassify::Kind peer = NetClassify::classify(req.clientAddress);
                QString authority;
                if (peer == NetClassify::Kind::Loopback || peer == NetClassify::Kind::Private)
                    authority = req.headers.value(QStringLiteral("x-forwarded-host"))
                                    .section(QLatin1Char(','), 0, 0)
                                    .trimmed();
                if (authority.isEmpty()) authority = req.headers.value(QStringLiteral("host"));
                if (const auto board = LinkAddresses::boardOrigin(authority, list)) {
                    direct(board->origin, board->reach);
                    return;
                }
            }
            if (!pinned) pinned = LinkAddresses::find(list, QStringLiteral("lan"));
        }
        if (pinned && pinned->kind == LinkAddresses::Kind::Domain) {
            direct(QStringLiteral("https://") + pinned->address, QStringLiteral("domain"));
            return;
        }
        const QString address = pinned ? pinned->address : lanIPv4();
        const quint16 port = server.activeHttpsPort();
        const QString origin = port == 443 ? QStringLiteral("https://%1").arg(address)
                                           : QStringLiteral("https://%1:%2").arg(address).arg(port);
        direct(origin, pinned ? QString::fromLatin1(LinkAddresses::kindName(pinned->kind))
                              : QStringLiteral("lan"));
    };

    // ── Owner side ──────────────────────────────────────────────────────────
    // Reachable by any authenticated MoonlightWeb user (the generic session gate
    // in HttpServer::processRequest covers these paths); deliberately NOT
    // admin-only. Cross-site drive-by is already refused by RequestGuard.

    // GET /api/share/status[?host=<uuid>] — the rows of the sharing board, and
    // the guests' picture height for the host it was opened on (the one being
    // streamed when none is named).
    router->get(QStringLiteral("/api/share/status"), [&share, &deps, menu](const HttpRequest& req) {
        // Not named `slots`: Qt's moc keyword macro would eat it.
        QJsonArray slotArray;
        for (const ShareManager::SlotStatus& st : share.status())
            slotArray.append(slotJson(st));

        QJsonObject obj;
        obj[QStringLiteral("slots")] = slotArray;
        obj[QStringLiteral("streaming")] = share.streamingCount();
        // Whether this machine answers from outside at this moment. It changes
        // nothing about the links — an invitation outlives a line that comes and
        // goes — it only lets the board say so instead of letting an owner hand
        // out an address believing it is being answered.
        obj[QStringLiteral("remote_reachable")] = deps.remoteReachable && deps.remoteReachable();
        // One height for every guest: on a native host they share one feed.
        QString hostUuid = req.queryParams.value(QStringLiteral("host"));
        if (hostUuid.isEmpty() && deps.currentOwnerContext)
            hostUuid =
                deps.currentOwnerContext(HttpServer::sessionTokenFromRequest(req), QString()).first;
        const ShareRoutesDeps::GuestPicture picture = deps.guestPicture && !hostUuid.isEmpty()
                                                          ? deps.guestPicture(hostUuid)
                                                          : ShareRoutesDeps::GuestPicture{};
        obj[QStringLiteral("feed_height")] = share.feedHeight();
        obj[QStringLiteral("owner_height")] = picture.ownerHeight;
        obj[QStringLiteral("shared_feed")] = picture.shared;
        // The address menu, and this host's pick. A pick whose network this PC
        // no longer has stays the pick; the board says it is missing.
        const QList<LinkAddresses::Choice> list = menu();
        const QString pick = share.linkAddress(hostUuid);
        obj[QStringLiteral("link_addresses")] = linkAddressesJson(list);
        obj[QStringLiteral("link_address")] = pick;
        obj[QStringLiteral("link_address_missing")] =
            pick != QLatin1String("auto") && !LinkAddresses::find(list, pick);
        return HttpResponse::json(obj);
    });

    // POST /api/share/link-address — {host_uuid, link_address}: the address the
    // host's guest links carry from now on. A live share keeps its token and
    // PIN; the board asks for its credentials again and shows the new link.
    router->post(QStringLiteral("/api/share/link-address"), [&share, menu](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString hostUuid = body.value(QStringLiteral("host_uuid")).toString();
        const QString id = body.value(QStringLiteral("link_address")).toString();
        if (hostUuid.isEmpty()) return HttpResponse::error(400, "Pick a host");
        // Only what the menu offers right now: an id that names no network of
        // this PC would just fall back to Automatic behind the owner's back.
        if (id != QLatin1String("auto") && !LinkAddresses::find(menu(), id))
            return HttpResponse::error(400, "Unknown address");
        if (!share.setLinkAddress(hostUuid, id)) return HttpResponse::error(400, "Unknown address");
        QJsonObject obj;
        obj[QStringLiteral("link_address")] = share.linkAddress(hostUuid);
        return HttpResponse::json(obj);
    });

    // POST /api/share/feed — {height}: the guests' picture height, for every
    // guest at once; a feed already running is rebuilt at the new size.
    router->post(QStringLiteral("/api/share/feed"), [&share](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        if (!share.setFeedHeight(body.value(QStringLiteral("height")).toInt()))
            return HttpResponse::error(400, "Unsupported height");
        QJsonObject obj;
        obj[QStringLiteral("feed_height")] = share.feedHeight();
        return HttpResponse::json(obj);
    });

    // POST /api/share/slots/:n/activate — open a row: mint its link + PIN.
    router->post(QStringLiteral("/api/share/slots/:n/activate"),
                 [&share, &deps, fillLink, ownerRefusal](const HttpRequest& req) {
                     const int slot = slotParam(req);
                     if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

                     const QJsonObject body = QJsonDocument::fromJson(req.body).object();
                     if (auto refused = ownerRefusal(req, body, QStringLiteral("activate"), slot))
                         return *refused;

                     // Where this invitation leads. A stream in progress decides it — the
                     // guest joins what the owner is already playing: this device's
                     // stream of the host its board was opened on. With nothing
                     // running there the board says so instead, which is what lets a
                     // row be opened cold from the host's kebab menu.
                     const QString wantedHost = body.value(QStringLiteral("host_uuid")).toString();
                     const std::pair<QString, int> owner =
                         deps.currentOwnerContext
                             ? deps.currentOwnerContext(HttpServer::sessionTokenFromRequest(req),
                                                        wantedHost)
                             : std::pair<QString, int>{};
                     QString hostUuid = owner.first;
                     int appId = owner.second;
                     if (hostUuid.isEmpty()) {
                         hostUuid = wantedHost;
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
                     fillLink(obj, token, hostUuid, req);
                     obj[QStringLiteral("pin")] = pin;
                     return HttpResponse::json(obj);
                 });

    // POST /api/share/slots/:n/credentials — the same link and PIN again, for an
    // owner reopening the popin on a share that is already live. POST, not GET:
    // it hands out a credential, so it goes through the full CSRF/origin gate
    // rather than being reachable by a top-level navigation.
    router->post(QStringLiteral("/api/share/slots/:n/credentials"),
                 [&share, fillLink, ownerRefusal](const HttpRequest& req) {
                     const int slot = slotParam(req);
                     if (slot < 0) return HttpResponse::error(404, "Unknown player slot");
                     if (auto refused =
                             ownerRefusal(req, QJsonDocument::fromJson(req.body).object(),
                                          QStringLiteral("credentials"), slot))
                         return *refused;

                     QString token;
                     QString pin;
                     QJsonObject obj;
                     // Which opening of the row this answer is about, so the
                     // board can tell when it no longer is (SlotStatus::opening).
                     const QString opening = share.opening(slot);
                     if (!opening.isEmpty()) obj[QStringLiteral("opening")] = opening;
                     if (!share.secrets(slot, token, pin)) {
                         // Either nothing is shared, or the process restarted
                         // since: the digests came back from share.json, the
                         // clear pair did not. The popin says so instead of
                         // pretending the link is gone.
                         obj[QStringLiteral("available")] = false;
                         return HttpResponse::json(obj);
                     }
                     obj[QStringLiteral("available")] = true;
                     fillLink(obj, token, share.hostForSlot(slot), req);
                     obj[QStringLiteral("pin")] = pin;
                     return HttpResponse::json(obj);
                 });

    // POST /api/share/slots/:n/permissions — at any time, including mid-stream.
    // ShareManager pushes the new policy down to the live worker; nothing here
    // freezes, so a guest is promoted or demoted without losing their picture.
    router->post(QStringLiteral("/api/share/slots/:n/permissions"), [&share, ownerRefusal](
                                                                        const HttpRequest& req) {
        const int slot = slotParam(req);
        if (slot < 0) return HttpResponse::error(404, "Unknown player slot");

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        if (auto refused = ownerRefusal(req, body, QStringLiteral("permissions"), slot))
            return *refused;
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
    router->post(QStringLiteral("/api/share/player/pin"), [&share,
                                                           &server](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString token = body.value(QStringLiteral("token")).toString();
        const QString pin = body.value(QStringLiteral("pin")).toString();

        const ShareManager::PinOutcome outcome = share.redeemPin(
            token, pin,
            AuthManager::rateLimitKey(AuthManager::attemptAddress(req.clientAddress, req.headers)),
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
            // Once the invitation has its device, a PIN form here is a form no
            // answer can pass — every correct PIN is already AlreadyBound. No
            // form then: it costs the real guest nothing (they hold the cookie),
            // and it gives whoever else has the link nothing to sit and guess
            // against. The page says the link is taken rather than dead, because
            // the usual reader is that same guest in a private tab or another
            // browser, who lost the cookie and needs to hear where it went.
            if (share.isBound(slot)) {
                obj[QStringLiteral("error")] = QStringLiteral("bound_elsewhere");
                return HttpResponse::json(obj, 404);
            }
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
        // Nothing running is no longer a wall: the guest's arrival is what
        // starts the app the owner chose. Say which one, and say that pressing
        // Join will wake the machine, so nobody boots a game by surprise.
        const std::pair<QString, bool> target =
            deps.joinTarget ? deps.joinTarget(slot) : std::pair<QString, bool>{};
        obj[QStringLiteral("app_name")] = target.first;
        obj[QStringLiteral("cold_start")] = target.second;
        // On a native host the picture's height is the owner's, one for every
        // guest: the page shows it instead of offering a choice.
        const ShareRoutesDeps::GuestPicture picture =
            deps.guestPicture ? deps.guestPicture(share.hostForSlot(slot))
                              : ShareRoutesDeps::GuestPicture{};
        if (picture.ownerHeight) obj[QStringLiteral("feed_height")] = share.feedHeight();
        obj[QStringLiteral("shared_feed")] = picture.shared;
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

            // A rung past the first is this same browser walking its transport
            // chain: the rung it just gave up on may still count as streaming
            // until its worker notices, and the relaunch replaces it (the slot is
            // this invitation's alone, and the cookie above proves the device).
            const int transportIndex =
                qMax(0, body.value(QStringLiteral("transport_index")).toInt(0));
            if (share.isStreaming(slot) && transportIndex == 0) {
                // Never steal a running stream: the other device has to leave.
                QJsonObject obj;
                obj[QStringLiteral("error")] = QStringLiteral("stream_in_progress");
                respond(HttpResponse::json(obj, 409));
                return;
            }

            // Back after this guest's worker crashed: only for that stream of
            // this slot, and never over a stream that started on it since.
            const QString reconnectOf = body.value(QStringLiteral("reconnect_of")).toString();
            if (!reconnectOf.isEmpty() &&
                (share.isStreaming(slot) || !deps.streamState ||
                 deps.streamState(slot, reconnectOf) != QLatin1String("crashed"))) {
                Logger::info(QStringLiteral("[Share] Slot %1: no rejoin after a crash").arg(slot));
                QJsonObject obj;
                obj[QStringLiteral("error")] = QStringLiteral("reconnect_refused");
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
            // On a native host not even that: its guests watch one picture, at
            // the height the owner picked.
            const int requested = body.value(QStringLiteral("height")).toInt(1080);
            int height = (requested == 720 || requested == 1440) ? requested : 1080;
            if (deps.guestPicture && deps.guestPicture(share.hostForSlot(slot)).ownerHeight)
                height = share.feedHeight();
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
                                   transportIndex, req, respond);
        });

    // GET /api/share/player/stream-state?stream=<id> — how the guest's stream
    // ended, asked once its link went (see ShareRoutesDeps::streamState).
    router->get(
        QStringLiteral("/api/share/player/stream-state"), [&share, &deps](const HttpRequest& req) {
            const int slot = share.slotForCookie(HttpServer::cookieFromRequest(req, kPlayerCookie));
            if (slot < 0) return HttpResponse::error(403, "Forbidden");
            const QString stream = req.queryParams.value(QStringLiteral("stream"));
            const QString state =
                deps.streamState ? deps.streamState(slot, stream) : QStringLiteral("unknown");
            return HttpResponse::json(QJsonObject{{QStringLiteral("state"), state}});
        });

    // GET /api/share/player/presence — who is watching the host this guest's
    // invitation leads to: whether the owner is, and every guest streaming it,
    // this one included (issue #47). A guest sees no more than the names the
    // owner gave its fellow guests' rows, never a link, a PIN or a device.
    router->get(
        QStringLiteral("/api/share/player/presence"), [&share, &deps](const HttpRequest& req) {
            const int self = share.slotForCookie(HttpServer::cookieFromRequest(req, kPlayerCookie));
            if (self < 0) return HttpResponse::error(403, "Forbidden");
            const QString hostUuid = share.hostForSlot(self);

            QJsonArray players;
            for (const ShareManager::SlotStatus& st : share.status()) {
                if (!st.streaming || share.hostForSlot(st.slot) != hostUuid) continue;
                QJsonObject p;
                p[QStringLiteral("slot")] = st.slot;
                p[QStringLiteral("name")] = st.name;
                players.append(p);
            }
            QJsonObject obj;
            obj[QStringLiteral("self")] = self;
            obj[QStringLiteral("owner")] =
                !hostUuid.isEmpty() && deps.ownerWatching && deps.ownerWatching(hostUuid);
            obj[QStringLiteral("players")] = players;
            return HttpResponse::json(obj);
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

    // POST /api/share/player/log — the guest page's console, in debug mode.
    // Here rather than /api/logs/client: a guest has no session, only the
    // cookie of their slot.
    router->post(QStringLiteral("/api/share/player/log"), [&share](const HttpRequest& req) {
        const int slot = share.slotForCookie(HttpServer::cookieFromRequest(req, kPlayerCookie));
        if (slot < 0) return HttpResponse::error(403, "Forbidden");
        return writeClientLog(req, QStringLiteral("guest %1").arg(slot));
    });

    Logger::info(QStringLiteral("[Share] Session sharing enabled (slots %1-%2)")
                     .arg(ShareManager::kFirstSlot)
                     .arg(ShareManager::kLastSlot));
}
