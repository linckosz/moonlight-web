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

#include "server/routes/AuthRoutes.h"

#include "server/HttpServer.h"
#include "server/RestRouter.h"
#include "server/AuthManager.h"
#include "server/AppSettings.h"
#include "server/NetClassify.h"
#include "network/GeoIpService.h"
#include "common/Logger.h"
#include "common/PairingCrypto.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

/**
 * The session cookie handed back to the browser.
 *
 * Remembered sessions carry a 90-day Max-Age, matching the server-side sliding
 * TTL. A session the visitor asked not to remember deliberately carries none:
 * a cookie without Max-Age/Expires is session-scoped and dies with the browser
 * process, which is what someone on a public machine expects. Never add one
 * later either — the periodic refresh in /api/auth/status would otherwise
 * promote a temporary login to 90 days behind the user's back.
 */
QString sessionCookie(const QString& token, bool remember)
{
    QString cookie =
        QStringLiteral("mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict").arg(token);
    if (remember) cookie += QStringLiteral("; Max-Age=7776000");
    return cookie;
}

/// Expire the session cookie immediately (logout).
QString clearedSessionCookie()
{
    return QStringLiteral("mw_session=; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=0");
}

/// "remember me" is opt-out: a body without the field keeps the old 90-day
/// behaviour, so older clients and the share flow are unaffected.
bool wantsRemember(const QJsonObject& body)
{
    return body.value(QStringLiteral("remember")).toBool(true);
}

/// The MW-BIND-v1 identity a browser needs to verify this host's SDP offers.
/// `host_id` is the host key's own identifier: it goes inside every signature so
/// one captured from another installation cannot be replayed here.
QJsonObject hostPairingIdentity(AppSettings& settings)
{
    const QByteArray spki = settings.hostSigningPublicKey();
    QJsonObject obj;
    obj["host_public_key"] = QString::fromLatin1(spki.toBase64());
    obj["host_id"] = PairingCrypto::keyId(spki);
    return obj;
}

/// Bind the browser public key carried in a login body, when there is one.
/// Pairing with a PIN is the one moment a key may be registered from anywhere,
/// including the internet: the visitor has just proven they hold the secret.
void bindKeyFromLoginBody(AuthManager& authManager, const QString& token, const QJsonObject& body,
                          QJsonObject& reply, AppSettings& settings)
{
    const QString b64 = body.value(QStringLiteral("public_key")).toString();
    if (b64.isEmpty()) return; // older frontend: nothing to bind, nothing to fail

    if (!authManager.bindSessionKey(token, QByteArray::fromBase64(b64.toUtf8()))) {
        reply["pairing_key"] = "rejected";
        return;
    }
    reply["pairing_key"] = "bound";
    const QJsonObject identity = hostPairingIdentity(settings);
    for (auto it = identity.constBegin(); it != identity.constEnd(); ++it) reply[it.key()] = it.value();
}

} // namespace

void registerAuthRoutes(HttpServer& server, AuthManager& authManager, GeoIpService& geoIpService,
                        AppSettings& settings)
{
    // ── Auth routes ─────────────────────────────────────────────────────────
    // POST /api/auth/validate — validate PIN or certificate, create session, set cookie
    server.router()->post("/api/auth/validate", [&authManager, &geoIpService,
                                                 &settings](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        QString pin = body["pin"].toString();
        QString certificate = body["certificate"].toString();
        QString machineName = body["machine_name"].toString();
        const bool remember = wantsRemember(body);

        // ── Certificate authentication (alternative to PIN) ────────────
        if (!certificate.isEmpty() && authManager.certAuthEnabled()) {
            if (authManager.validateCertificate(certificate)) {
                // Certificate valid — create session (same flow as PIN success)
                QString token =
                    authManager.createSession(req.clientAddress, machineName, false, !remember);
                geoIpService.lookupIp(
                    req.clientAddress,
                    [&authManager, token](const QString& city, const QString& country) {
                        authManager.setSessionGeo(token, city, country);
                    });

                QJsonObject obj;
                obj["status"] = "ok";
                obj["auth_method"] = "certificate";
                obj["remembered"] = remember;
                bindKeyFromLoginBody(authManager, token, body, obj, settings);
                HttpResponse resp = HttpResponse::json(obj);
                resp.headers["Set-Cookie"] = sessionCookie(token, remember);
                return resp;
            } else {
                // Certificate invalid
                QJsonObject obj;
                obj["status"] = "error";
                obj["error"] = "invalid_certificate";
                return HttpResponse::json(obj, 401);
            }
        }

        // ── PIN validation (default path) ──────────────────────────────
        if (pin.isEmpty()) return HttpResponse::error(400, "Missing 'pin' field");

        // Rate limiting uses the real socket IP
        auto result = authManager.validatePin(req.clientAddress, pin);

        QJsonObject obj;
        switch (result.result) {
        case AuthManager::Valid: {
            QString token =
                authManager.createSession(req.clientAddress, machineName, false, !remember);

            // Look up the IP geolocation asynchronously and store in the session
            geoIpService.lookupIp(req.clientAddress, [&authManager, token](const QString& city,
                                                                           const QString& country) {
                authManager.setSessionGeo(token, city, country);
            });

            // Auto-regenerate PIN — immediately invalidate the just-used PIN
            authManager.autoRegeneratePin();

            obj["status"] = "ok";
            obj["pin_regenerated"] = true;
            obj["remembered"] = remember;
            bindKeyFromLoginBody(authManager, token, body, obj, settings);
            HttpResponse resp = HttpResponse::json(obj);
            resp.headers["Set-Cookie"] = sessionCookie(token, remember);
            return resp;
        }
        case AuthManager::InvalidPin:
            obj["status"] = "error";
            obj["error"] = "invalid_pin";
            obj["remaining"] = result.remainingAttempts;
            obj["lockout_seconds"] = result.lockoutSeconds;
            return HttpResponse::json(obj, 401);
        case AuthManager::RateLimited:
            obj["status"] = "error";
            obj["error"] = "rate_limited";
            obj["lockout_seconds"] = result.lockoutSeconds;
            return HttpResponse::json(obj, 429);
        }
        return HttpResponse::error(500, "Internal error");
    });

    // POST /api/auth/pairing-key — register this browser's MW-BIND-v1 public key
    // on a session that predates the mechanism, and collect the host's own.
    //
    // This is trust-on-first-use over a channel that is already authenticated by
    // the session cookie, so that browsers paired before MW-BIND-v1 existed do
    // not all have to re-enter a PIN. It is deliberately restricted to peers
    // that reached us without crossing the internet — loopback, the LAN, or a
    // mesh VPN. From anywhere else the browser must pair with a PIN, which is
    // the path that carries its key in the login body.
    //
    // A session that already has a key gets the same answer as long as it sends
    // the same key; a *different* key is refused by bindSessionKey().
    server.router()->post("/api/auth/pairing-key", [&authManager,
                                                    &settings](const HttpRequest& req) {
        const QString token = HttpServer::sessionTokenFromRequest(req);
        if (!authManager.validateSession(token)) return HttpResponse::error(401, "Not authenticated");

        const NetClassify::Kind kind = NetClassify::classify(req.clientAddress);
        if (!NetClassify::isTrustedPeer(kind)) {
            Logger::warning(QStringLiteral("[Auth] Refused silent pairing-key registration from %1 "
                                        "(%2) — PIN required from the internet")
                             .arg(req.clientAddress, NetClassify::toString(kind)));
            return HttpResponse::error(403, "Pair with a PIN from this network");
        }

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString b64 = body.value(QStringLiteral("public_key")).toString();
        if (b64.isEmpty()) return HttpResponse::error(400, "Missing 'public_key' field");

        if (!authManager.bindSessionKey(token, QByteArray::fromBase64(b64.toUtf8()))) {
            return HttpResponse::error(409, "This session is already bound to another key");
        }

        QJsonObject obj = hostPairingIdentity(settings);
        obj["status"] = "ok";
        return HttpResponse::json(obj);
    });

    // POST /api/admin/pin/generate — generate a new PIN without revoking sessions (localhost only)
    server.router()->post("/api/admin/pin/generate", [&authManager](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        QString pin = authManager.generatePin();
        QJsonObject obj;
        obj["status"] = "ok";
        obj["pin"] = pin;
        return HttpResponse::json(obj);
    });

    // POST /api/auth/regenerate — regenerate PIN (localhost only)
    server.router()->post("/api/auth/regenerate", [&authManager](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        authManager.regeneratePin();
        QJsonObject obj;
        obj["status"] = "ok";
        obj["pin"] = authManager.currentPin();
        return HttpResponse::json(obj);
    });

    // POST /api/admin/pin/clear — reset PIN to "--------" (localhost only)
    server.router()->post("/api/admin/pin/clear", [&authManager](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        authManager.clearPin();
        QJsonObject obj;
        obj["status"] = "ok";
        obj["pin"] = authManager.currentPin();
        return HttpResponse::json(obj);
    });

    // ── Remote admin unlock ─────────────────────────────────────────────────
    // Admin access is granted by address: the host machine has it, nobody else
    // does. These two routes add a second door for a LAN machine that is not
    // the host — the operator sets a password from the host, and any other
    // machine on the same network can spend it to get the same admin page.
    //
    // POST /api/auth/admin-unlock — spend the password to promote this session.
    server.router()->post("/api/auth/admin-unlock", [&authManager](const HttpRequest& req) {
        // Already an admin (host machine, or unlocked earlier): nothing to do.
        // Answering "ok" keeps the frontend's flow simple and, more to the
        // point, means the host can never burn attempts on its own bucket.
        // isHostMachine as well as isLocal, because isLocal additionally wants
        // the admin key on a POST and the host has no session to fall back on.
        if (req.isLocal || req.isHostMachine) {
            QJsonObject obj;
            obj["status"] = "ok";
            return HttpResponse::json(obj);
        }

        // LAN only, by explicit design: the password is a convenience for the
        // machines in the same house, not a second front door on the internet.
        // Two independent conditions, because either one alone is forgeable in
        // a deployment we support — a TLS-terminating tunnel makes every
        // visitor look like 127.0.0.1 (peer check alone would pass), and a
        // hairpin-NAT LAN client arrives under the public domain (Host check
        // alone would pass for internet visitors too).
        if (!AuthManager::isLanAddress(req.clientAddress) || !req.hostTrusted) {
            QJsonObject obj;
            obj["status"] = "error";
            obj["error"] = "lan_only";
            return HttpResponse::json(obj, 403);
        }

        // The unlock upgrades an existing session rather than creating one, so
        // a device still has to pass the PIN first: the password raises what an
        // already-admitted device may do, it does not admit anyone.
        const QString token = HttpServer::sessionTokenFromRequest(req);
        if (!authManager.validateSession(token)) {
            QJsonObject obj;
            obj["status"] = "error";
            obj["error"] = "not_authenticated";
            return HttpResponse::json(obj, 401);
        }

        // Either the operator turned remote administration off, or they never
        // set a password — there is no built-in default, so an unset password
        // means the door does not open for anyone. Say so plainly instead of
        // letting the caller burn attempts against something nothing can match.
        if (!authManager.remoteAdminEnabled() || !authManager.adminPasswordSet()) {
            QJsonObject obj;
            obj["status"] = "error";
            obj["error"] = "not_configured";
            return HttpResponse::json(obj, 403);
        }

        const QString password = QJsonDocument::fromJson(req.body).object()["password"].toString();
        const auto result = authManager.validateAdminPassword(req.clientAddress, password);

        QJsonObject obj;
        switch (result.result) {
        case AuthManager::Valid:
            authManager.promoteSessionToAdmin(token);
            obj["status"] = "ok";
            return HttpResponse::json(obj);
        case AuthManager::InvalidPin:
            obj["status"] = "error";
            obj["error"] = "invalid_password";
            obj["remaining"] = result.remainingAttempts;
            obj["lockout_seconds"] = result.lockoutSeconds;
            return HttpResponse::json(obj, 401);
        case AuthManager::RateLimited:
            obj["status"] = "error";
            obj["error"] = "rate_limited";
            obj["lockout_seconds"] = result.lockoutSeconds;
            return HttpResponse::json(obj, 429);
        }
        return HttpResponse::error(500, "Internal error");
    });

    // POST /api/admin/password — change the remote admin password and/or turn
    // remote administration on and off (admin only). Body may carry either or
    // both of {password, enabled}; the toggle is applied first so a single call
    // can re-enable the door and set a password behind it.
    server.router()->post("/api/admin/password", [&authManager](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString password = body["password"].toString();

        // Changing either setting revokes every unlock the old password bought,
        // which would include the caller's own if they are a remote admin doing
        // the reset. They just proved they are an admin, so hand it back.
        const QString token = HttpServer::sessionTokenFromRequest(req);
        const bool callerWasRemoteAdmin = authManager.isAdminSession(token);

        if (body.contains("enabled")) authManager.setRemoteAdminEnabled(body["enabled"].toBool());

        if (!password.isEmpty() && !authManager.setAdminPassword(password)) {
            QJsonObject obj;
            obj["status"] = "error";
            obj["error"] = "too_short";
            obj["min_length"] = AuthManager::MIN_ADMIN_PASSWORD_LEN;
            return HttpResponse::json(obj, 400);
        }
        if (callerWasRemoteAdmin && authManager.remoteAdminEnabled())
            authManager.promoteSessionToAdmin(token);

        QJsonObject obj;
        obj["status"] = "ok";
        obj["remote_admin_enabled"] = authManager.remoteAdminEnabled();
        obj["admin_password_set"] = authManager.adminPasswordSet();
        return HttpResponse::json(obj);
    });

    // GET /api/auth/status — check current auth status
    server.router()->get("/api/auth/status", [&authManager, &geoIpService](const HttpRequest& req) {
        QJsonObject obj;
        QString authedToken; // set when a valid session cookie is found below

        bool isLocal = req.isLocal;
        // Historical name: it means "has admin access", which since the remote
        // admin password is no longer the same thing as "is the host machine".
        obj["is_localhost"] = isLocal;
        obj["is_host_machine"] = req.isHostMachine;

        if (isLocal) {
            obj["authenticated"] = true; // localhost is always authenticated
            obj["pin"] = authManager.currentPin();
            obj["pin_consumed"] = authManager.isPinConsumed();
            obj["remote_admin_enabled"] = authManager.remoteAdminEnabled();
            // Drives the admin page's banner: remote administration is on but no
            // password has been set, so the LAN door is advertised and shut. The
            // owner is the only one who can open it, and only from here.
            obj["admin_password_set"] = authManager.adminPasswordSet();
        } else {
            // Check session cookie
            bool auth = false;
            QString cookie = req.headers.value("cookie");
            if (!cookie.isEmpty()) {
                QStringList cookies = cookie.split(";");
                for (const QString& c : cookies) {
                    QString trimmed = c.trimmed();
                    if (trimmed.startsWith("mw_session=", Qt::CaseInsensitive)) {
                        QString token = trimmed.mid(QStringLiteral("mw_session=").length());
                        if (authManager.validateSession(token)) {
                            auth = true;
                            authedToken = token;
                            // Activity → slide the session's expiration window.
                            authManager.touchSession(token);
                            // Reconnection: refresh source IP and re-run
                            // geolocation if it changed since last seen.
                            if (authManager.updateSessionAddress(token, req.clientAddress)) {
                                geoIpService.lookupIp(
                                    req.clientAddress,
                                    [&authManager, token](const QString& city,
                                                          const QString& country) {
                                        authManager.setSessionGeo(token, city, country);
                                    });
                            }
                            break;
                        }
                    }
                }
            }
            obj["authenticated"] = auth;
            if (!auth) {
                obj["remaining"] = authManager.remainingAttempts(req.clientAddress);
                int lockoutSecs = authManager.lockoutSeconds(req.clientAddress);
                if (lockoutSecs > 0) obj["lockout_seconds"] = lockoutSecs;
            }
        }

        obj["requires_pin"] = !isLocal;
        obj["active_sessions"] = authManager.activeSessionCount();
        obj["cert_auth_enabled"] = authManager.certAuthEnabled();
        // Tells the frontend to offer the admin page behind a password prompt.
        // Mirrors exactly what /api/auth/admin-unlock will accept, so the
        // button never appears where the unlock would be refused.
        obj["admin_unlock_available"] =
            !isLocal && obj["authenticated"].toBool() && authManager.remoteAdminEnabled() &&
            authManager.adminPasswordSet() && AuthManager::isLanAddress(req.clientAddress) &&
            req.hostTrusted;

        // Whether this browser holds a session cookie at all — true even on
        // localhost, where authenticated is hardcoded. It is what tells the UI
        // that there is something to log out of: the loopback page has no
        // session, a host-key browser on the public domain has one.
        obj["has_session"] = authManager.validateSession(HttpServer::sessionTokenFromRequest(req));

        HttpResponse resp = HttpResponse::json(obj);
        // Slide the cookie browser-side too, so an active client keeps a fresh
        // 90-day window without ever re-entering the PIN. A session the visitor
        // asked not to remember stays session-scoped — see sessionCookie().
        if (!authedToken.isEmpty()) {
            resp.headers["Set-Cookie"] =
                sessionCookie(authedToken, !authManager.isEphemeralSession(authedToken));
        }
        return resp;
    });

    // POST /api/auth/logout — end *this* browser's session.
    //
    // Deliberately not localhost-gated, unlike /api/auth/sessions/revoke: a
    // visitor holds nothing but their own cookie, so the worst they can do is
    // lock themselves out. That is the whole point — leaving a public machine
    // must not require reaching the host's admin page.
    server.router()->post("/api/auth/logout", [&authManager](const HttpRequest& req) {
        const QString token = HttpServer::sessionTokenFromRequest(req);
        const bool destroyed = authManager.logoutSession(token);

        QJsonObject obj;
        obj["status"] = "ok";
        obj["had_session"] = destroyed;
        HttpResponse resp = HttpResponse::json(obj);
        // Clear the cookie either way: a token the server does not know is
        // exactly what a stale cookie looks like, and it should go too.
        resp.headers["Set-Cookie"] = clearedSessionCookie();
        return resp;
    });

    // GET /api/auth/sessions — list active sessions with metadata (localhost only)
    server.router()->get(
        "/api/auth/sessions", [&authManager, &geoIpService](const HttpRequest& req) {
            if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

            // Which row is the caller itself. Revoking it logs this browser out
            // — and for the host machine's own session, costs it its admin
            // access until it redeems a fresh host key — so the UI must be able
            // to say so before the click rather than after.
            const QString currentId =
                authManager.sessionIdForToken(HttpServer::sessionTokenFromRequest(req));

            QJsonArray arr;
            const auto sessions = authManager.sessions();
            for (const auto& s : sessions) {
                QJsonObject entry = s.toJson();
                // "Local" for private IPs, else city/country from stored geo data
                entry["location"] = AuthManager::isPrivateIP(s.ip);
                entry["current"] = !currentId.isEmpty() && s.token == currentId;
                arr.append(entry);
            }
            QJsonObject obj;
            obj["sessions"] = arr;
            return HttpResponse::json(obj);
        });

    // POST /api/auth/sessions/revoke — revoke a session (token in JSON body, localhost only)
    server.router()->post("/api/auth/sessions/revoke", [&authManager](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QString token = doc.object().value("token").toString();
        Logger::info(QString("[Auth] Revoke request — token='%1', size=%2")
                         .arg(token, QString::number(token.size())));
        if (token.isEmpty()) return HttpResponse::error(400, "Missing 'token' in request body");

        // Never the session making the request. Revoking yourself is not an
        // operation anyone wants — on the host machine's own session it costs
        // the admin access that was being used to click, and takes the stream
        // that session is flagged with down with it. The admin page does not
        // even list that row; this is the half of the guard a stray or replayed
        // request cannot get around.
        const QString currentId =
            authManager.sessionIdForToken(HttpServer::sessionTokenFromRequest(req));
        if (!currentId.isEmpty() && token == currentId) {
            Logger::warning("[Auth] Refused to revoke the caller's own session");
            QJsonObject obj;
            obj["status"] = "error";
            obj["error"] = "cannot_revoke_self";
            return HttpResponse::json(obj, 409);
        }

        authManager.destroySession(token);
        QJsonObject obj;
        obj["status"] = "revoked";
        return HttpResponse::json(obj);
    });

    // GET /api/admin/certificate/download — download certificate token (localhost only)
    server.router()->get("/api/admin/certificate/download", [&authManager](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        QString token = authManager.certificateToken();
        if (token.isEmpty()) return HttpResponse::error(500, "Certificate token not initialized");

        HttpResponse resp;
        resp.statusCode = 200;
        resp.contentType = "text/plain; charset=utf-8";
        resp.headers["Content-Disposition"] =
            "attachment; filename=\"moonlightweb-certificate.txt\"";
        resp.body = token.toUtf8();
        return resp;
    });

    // POST /api/admin/certificate/regenerate — generate a new certificate token (localhost only)
    server.router()->post("/api/admin/certificate/regenerate",
                          [&authManager](const HttpRequest& req) {
                              if (!req.isLocal)
                                  return HttpResponse::error(403, "Only available from localhost");

                              QString newToken = authManager.generateCertificateToken();
                              QJsonObject obj;
                              obj["status"] = "ok";
                              obj["certificate_regenerated"] = true;
                              Logger::info("[Auth] Certificate token regenerated by admin");
                              return HttpResponse::json(obj);
                          });
}
