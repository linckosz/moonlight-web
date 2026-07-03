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
#include "network/GeoIpService.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

void registerAuthRoutes(HttpServer& server, AuthManager& authManager, GeoIpService& geoIpService)
{
    // ── Auth routes ─────────────────────────────────────────────────────────
    // POST /api/auth/validate — validate PIN or certificate, create session, set cookie
    server.router()->post("/api/auth/validate", [&authManager,
                                                 &geoIpService](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        QString pin = body["pin"].toString();
        QString certificate = body["certificate"].toString();
        QString machineName = body["machine_name"].toString();

        // ── Certificate authentication (alternative to PIN) ────────────
        if (!certificate.isEmpty() && authManager.certAuthEnabled()) {
            if (authManager.validateCertificate(certificate)) {
                // Certificate valid — create session (same flow as PIN success)
                QString token = authManager.createSession(req.clientAddress, machineName);
                geoIpService.lookupIp(
                    req.clientAddress,
                    [&authManager, token](const QString& city, const QString& country) {
                        authManager.setSessionGeo(token, city, country);
                    });

                QJsonObject obj;
                obj["status"] = "ok";
                obj["auth_method"] = "certificate";
                HttpResponse resp = HttpResponse::json(obj);
                resp.headers["Set-Cookie"] =
                    QString(
                        "mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000")
                        .arg(token);
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
            QString token = authManager.createSession(req.clientAddress, machineName);

            // Look up the IP geolocation asynchronously and store in the session
            geoIpService.lookupIp(req.clientAddress, [&authManager, token](const QString& city,
                                                                           const QString& country) {
                authManager.setSessionGeo(token, city, country);
            });

            // Auto-regenerate PIN — immediately invalidate the just-used PIN
            authManager.autoRegeneratePin();

            obj["status"] = "ok";
            obj["pin_regenerated"] = true;
            HttpResponse resp = HttpResponse::json(obj);
            // Set HttpOnly session cookie, 90-day expiry, Strict SameSite
            // (Max-Age=7776000s; matches the server-side sliding TTL).
            resp.headers["Set-Cookie"] =
                QString("mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000")
                    .arg(token);
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

    // POST /api/admin/pin/generate — generate a new PIN without revoking sessions (localhost only)
    server.router()->post("/api/admin/pin/generate", [&authManager](const HttpRequest& req) {
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

        QString pin = authManager.generatePin();
        QJsonObject obj;
        obj["status"] = "ok";
        obj["pin"] = pin;
        return HttpResponse::json(obj);
    });

    // POST /api/auth/regenerate — regenerate PIN (localhost only)
    server.router()->post("/api/auth/regenerate", [&authManager](const HttpRequest& req) {
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

        authManager.regeneratePin();
        QJsonObject obj;
        obj["status"] = "ok";
        obj["pin"] = authManager.currentPin();
        return HttpResponse::json(obj);
    });

    // POST /api/admin/pin/clear — reset PIN to "--------" (localhost only)
    server.router()->post("/api/admin/pin/clear", [&authManager](const HttpRequest& req) {
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

        authManager.clearPin();
        QJsonObject obj;
        obj["status"] = "ok";
        obj["pin"] = authManager.currentPin();
        return HttpResponse::json(obj);
    });

    // GET /api/auth/status — check current auth status
    server.router()->get("/api/auth/status", [&authManager, &geoIpService](const HttpRequest& req) {
        QJsonObject obj;
        QString authedToken; // set when a valid session cookie is found below

        bool isLocal = HttpServer::isLocalRequest(req.clientAddress);
        obj["is_localhost"] = isLocal;

        if (isLocal) {
            obj["authenticated"] = true; // localhost is always authenticated
            obj["pin"] = authManager.currentPin();
            obj["pin_consumed"] = authManager.isPinConsumed();
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

        HttpResponse resp = HttpResponse::json(obj);
        // Slide the cookie browser-side too, so an active client keeps a
        // fresh 90-day window without ever re-entering the PIN.
        if (!authedToken.isEmpty()) {
            resp.headers["Set-Cookie"] =
                QString("mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000")
                    .arg(authedToken);
        }
        return resp;
    });

    // GET /api/auth/sessions — list active sessions with metadata (localhost only)
    server.router()->get("/api/auth/sessions",
                         [&authManager, &geoIpService](const HttpRequest& req) {
                             if (!HttpServer::isLocalRequest(req.clientAddress))
                                 return HttpResponse::error(403, "Only available from localhost");

                             QJsonArray arr;
                             const auto sessions = authManager.sessions();
                             for (const auto& s : sessions) {
                                 QJsonObject entry = s.toJson();
                                 // "Local" for private IPs, else city/country from stored geo data
                                 entry["location"] = AuthManager::isPrivateIP(s.ip);
                                 arr.append(entry);
                             }
                             QJsonObject obj;
                             obj["sessions"] = arr;
                             return HttpResponse::json(obj);
                         });

    // POST /api/auth/sessions/revoke — revoke a session (token in JSON body, localhost only)
    server.router()->post("/api/auth/sessions/revoke", [&authManager](const HttpRequest& req) {
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QString token = doc.object().value("token").toString();
        Logger::info(QString("[Auth] Revoke request — token='%1', size=%2")
                         .arg(token, QString::number(token.size())));
        if (token.isEmpty()) return HttpResponse::error(400, "Missing 'token' in request body");

        authManager.destroySession(token);
        QJsonObject obj;
        obj["status"] = "revoked";
        return HttpResponse::json(obj);
    });

    // GET /api/admin/certificate/download — download certificate token (localhost only)
    server.router()->get("/api/admin/certificate/download", [&authManager](const HttpRequest& req) {
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

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
                              if (!HttpServer::isLocalRequest(req.clientAddress))
                                  return HttpResponse::error(403, "Only available from localhost");

                              QString newToken = authManager.generateCertificateToken();
                              QJsonObject obj;
                              obj["status"] = "ok";
                              obj["certificate_regenerated"] = true;
                              Logger::info("[Auth] Certificate token regenerated by admin");
                              return HttpResponse::json(obj);
                          });
}
