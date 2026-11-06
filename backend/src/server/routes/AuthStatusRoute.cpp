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

#include "server/routes/AuthStatusRoute.h"

#include "server/AuthManager.h"
#include "server/RestRouter.h"
#include "server/SessionCookie.h"

#include <QJsonObject>
#include <QStringList>

#include <utility>

void registerAuthStatusRoute(RestRouter& router, AuthManager& authManager, AuthStatusDeps deps)
{
    // GET /api/auth/status — check current auth status
    router.get("/api/auth/status", [&authManager, deps = std::move(deps)](const HttpRequest& req) {
        QJsonObject obj;
        QString authedToken; // set when a valid session cookie is found below

        bool isLocal = req.isLocal;
        // Historical name: it means "has admin access", which since the remote
        // admin password is no longer the same thing as "is the host machine".
        obj["is_localhost"] = isLocal;
        obj["is_host_machine"] = req.isHostMachine;
        // --noadmin: nobody administers, the browser on the host still streams.
        obj["admin_locked"] = req.adminLocked;
        // share_in_booth_mode: the booth still offers Share during a stream.
        if (req.adminLocked) obj["booth_share"] = req.boothShare;
        // Signed in by where it is, never asked a PIN: an admin, or the booth's
        // own machine. Not to be confused with req.hostTrusted, whether the Host
        // header names this machine.
        const bool trustedWithoutPin = isLocal || (req.adminLocked && req.isHostMachine);

        if (isLocal) {
            obj["authenticated"] = true; // localhost is always authenticated
            obj["pin"] = authManager.currentPin();
            obj["pin_consumed"] = authManager.isPinConsumed();
            obj["remote_admin_enabled"] = authManager.remoteAdminEnabled();
            obj["remote_admin_internet"] = authManager.remoteAdminInternet();
            // Drives the admin page's banner: remote administration is on but no
            // password has been set, so the LAN door is advertised and shut. The
            // owner is the only one who can open it, and only from here.
            obj["admin_password_set"] = authManager.adminPasswordSet();
        } else if (trustedWithoutPin) {
            obj["authenticated"] = true; // the booth itself: streams, never asked a PIN
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
                                if (deps.locate) deps.locate(token, req.clientAddress);
                            }
                            break;
                        }
                    }
                }
            }
            obj["authenticated"] = auth;
            if (!auth) {
                obj["remaining"] = authManager.remainingAttempts(
                    AuthManager::attemptAddress(req.clientAddress, req.headers));
                int lockoutSecs = authManager.lockoutSeconds(
                    AuthManager::attemptAddress(req.clientAddress, req.headers));
                if (lockoutSecs > 0) obj["lockout_seconds"] = lockoutSecs;
            }
        }

        obj["requires_pin"] = !trustedWithoutPin;
        obj["active_sessions"] = authManager.activeSessionCount();
        obj["cert_auth_enabled"] = authManager.certAuthEnabled();
        // Tells the frontend to offer the admin page behind a password prompt.
        // Mirrors exactly what /api/auth/admin-unlock will accept, so the
        // button never appears where the unlock would be refused.
        obj["admin_unlock_available"] =
            !isLocal && !req.adminLocked && obj["authenticated"].toBool() &&
            authManager.remoteAdminEnabled() && authManager.adminPasswordSet() &&
            AuthManager::canUnlockAdmin(req.clientAddress, req.hostTrusted, req.viaTunnel,
                                        authManager.remoteAdminInternet());

        // Whether this browser holds a session cookie at all — true even on
        // localhost, where authenticated is hardcoded. It is what tells the UI
        // that there is something to log out of: the loopback page has no
        // session, a host-key browser on the public domain has one.
        obj["has_session"] = authManager.validateSession(deps.sessionToken(req));

        // What this install calls itself, for the PIN screen to say which
        // machine it unlocks: to a caller on this LAN alone, by the rule a
        // device's QR code request is held to. Everyone else gets the answer
        // without it.
        if (AuthManager::fromThisLan(req)) obj["display_name"] = deps.displayName();

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
}
