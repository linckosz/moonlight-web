/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSslCertificate>
#include <QStandardPaths>
#include <QTimer>
#include <QDateTime>
#include <QHostInfo>
#include <QHostAddress>
#include <QRandomGenerator>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
#include <functional>
#include "server/AppSettings.h"
#include "server/HttpServer.h"
#include "server/RestRouter.h"
#include "server/AuthManager.h"
#include "common/Logger.h"
#include "backend/ComputerManager.h"
#include "backend/IdentityManager.h"
#include "streaming/Session.h"
#include "streaming/MoonlightShim.h"
#include "streaming/DataChannelRelay.h"
#include "streaming/MediaTrackRelay.h"
#include "streaming/StreamRelay.h"
#include "streaming/TransportPriorities.h"
#include "network/InternetAccessManager.h"
#include "network/GeoIpService.h"
#include "TrayManager.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

// Load KEY=VALUE pairs from a .env file into the process environment.
// Supports PEM blocks: if a value starts with "-----BEGIN", lines are
// accumulated until "-----END" is found (multi-line key support).
// Called early in main() so that env vars (e.g. MW_CERT_KEY) are available
// before any certificate loading happens.
static void loadEnvFile()
{
    // Look for .env next to the executable first (production / installed build),
    // then at the project root (Qt Creator dev build).
    QString path = QCoreApplication::applicationDirPath() + "/.env";
    if (!QFile::exists(path)) {
        path = QStringLiteral(PROJECT_ROOT) + ".env";
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    int count = 0;
    while (!f.atEnd()) {
        QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        int eq = line.indexOf('=');
        if (eq <= 0)
            continue;

        QByteArray key = line.left(eq).trimmed();
        QByteArray value = line.mid(eq + 1).trimmed();

        // Strip surrounding quotes
        if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
            value = value.mid(1, value.size() - 2);

        // PEM block: accumulate BEGIN..END lines.
        // After each END, check if another PEM block (e.g. intermediate
        // certificate) follows on the next non-blank line.  Keep consuming
        // until we hit a different variable or EOF.
        if (value.startsWith("-----BEGIN")) {
            QByteArray pem = value + '\n';
            bool more = true;
            while (more && !f.atEnd()) {
                QByteArray next = f.readLine().trimmed();
                pem += next + '\n';
                if (next.startsWith("-----END")) {
                    qint64 saved = f.pos();
                    QByteArray peek;
                    while (!f.atEnd()) {
                        peek = f.readLine().trimmed();
                        if (!peek.isEmpty() && !peek.startsWith('#'))
                            break;
                    }
                    if (peek.startsWith("-----BEGIN")) {
                        pem += peek + '\n';
                    } else {
                        f.seek(saved);
                        more = false;
                    }
                }
            }
            qputenv(key, pem);
        } else {
            qputenv(key, value);
        }
        count++;
    }

    Logger::info(QString("[.env] Loaded %1 variables from %2").arg(count).arg(path));
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Moonlight-Web");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Moonlight-Web");

    // Load .env file before anything reads environment variables
    loadEnvFile();

    // Parse command line
    QCommandLineParser parser;
    parser.setApplicationDescription("Moonlight-Web streaming server");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOption("port", "HTTP server port", "port", "80");
    parser.addOption(portOption);

    QCommandLineOption logOption("log", "Log file path", "path");
    parser.addOption(logOption);

    QCommandLineOption wsPortOption("ws-port", "WebRTC signaling WebSocket port", "port", "48001");
    parser.addOption(wsPortOption);

    parser.process(app);

    // Configure logging
    if (parser.isSet(logOption))
        Logger::instance()->setLogFile(parser.value(logOption));

    Logger::info("Moonlight-Web server starting...");
    Logger::info("Version: " + QCoreApplication::applicationVersion());

    // Read HTTP/HTTPS port preferences from persisted settings.
    // CLI --port overrides the persisted HTTP port when explicitly provided.
    AppSettings appSettings;
    quint16 httpPort = appSettings.httpPort(80);
    if (parser.isSet("port"))
        httpPort = parser.value("port").toUShort();

    HttpServer server(httpPort);

    // Pass domain and cert settings to HttpServer so loadCert() can find
    // the correct certificate by CN matching (or load from env var / file).
    server.setDomain(appSettings.domain());
    server.setCertPem(appSettings.certPem());
    server.setCertKey(appSettings.certKey());

    // Initialize ComputerManager (Phase 2: host discovery)
    ComputerManager computerManager(&app);
    computerManager.init();

    // Phase 3: Initialize pairing identity (generates RSA keypair if needed)
    IdentityManager::get();
    Logger::info("Pairing identity initialized");

    // Force OpenSSL init before any PeerConnection is created.
    // libdatachannel inits OpenSSL lazily; if the first DTLS handshake
    // triggers a race on the static init mutex, SSL_ERROR_SYSCALL can occur.
    // Doing a synchronous init here avoids the race entirely.
    {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                         | OPENSSL_INIT_LOAD_CRYPTO_STRINGS
                         | OPENSSL_INIT_NO_ATEXIT, nullptr);
        OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
        Logger::info("OpenSSL initialized");
    }

    // Read remaining persistent settings
    quint16 httpsPort = appSettings.httpsPort(443);
    VideoCodec preferredCodec = appSettings.videoCodec();
    bool upnpEnabled = appSettings.upnpEnabled();
    QString stunServer = appSettings.stunServer();
    Logger::info("[main] Settings: http_port=" + QString::number(httpPort)
                 + ", https_port=" + QString::number(httpsPort)
                 + ", video_codec=" + AppSettings::videoCodecToString(preferredCodec)
                 + ", upnp_enabled=" + (upnpEnabled ? "true" : "false")
                 + ", stun_server=" + stunServer);

    // ── AuthManager (PIN-based + certificate authentication) ────────────────
    AuthManager authManager(&appSettings);

    // Generate certificate token on first startup (persisted forever)
    if (authManager.certificateToken().isEmpty()) {
        QString token = authManager.generateCertificateToken();
        Logger::info(QString("[Auth] Initial certificate token generated (%1 chars)")
            .arg(token.size()));
    } else {
        Logger::info("[Auth] Certificate token already exists");
    }

    Logger::info("[Auth] No PIN set by default — admin must generate one");
    Logger::info("[Auth] Access the admin page at https://localhost/");
    Logger::info("[Auth] Remote access requires a generated PIN");
    server.setAuthManager(&authManager);

    QObject::connect(&authManager, &AuthManager::pinChanged,
        [](const QString& pin) {
            Logger::info(QString("[Auth] PIN changed: %1").arg(pin));
        });

    // Phase 5b: WebRTC DataChannel relay + signaling tracking
    quint16 signalingPort = parser.value("ws-port").toUShort();
    QPointer<DataChannelRelay> g_ActiveRelay;
    QPointer<MediaTrackRelay> g_ActiveMediaTrackRelay;
    QPointer<StreamRelay> g_ActiveStreamRelay;
    // The StreamSession that owns the single active relay+signaling graph. Needed
    // so a take-over can call its quit() (stops the SignalingServer FIRST, freeing
    // the fixed signaling port before the slow relay/moonlight teardown).
    QPointer<StreamSession> g_ActiveSession;
    // The active relay QObject. A QPointer auto-nulls only when the relay is fully
    // DESTROYED — i.e. after the slow moonlight LiStopConnection and after the
    // child SignalingServer released the fixed port. A new session must wait for
    // this to be null before starting (one signaling port + one moonlight singleton
    // process-wide). Covers both take-over and self-disconnect-then-relaunch.
    QPointer<QObject> g_ActiveRelayRoot;
    // Per-browser uniqueid of the relay that currently owns the single active
    // session. Used to reject a stale /quit from a client that was taken over
    // (its quit would otherwise tear down the NEW owner via the global pointer).
    QString g_ActiveClientUniqueId;

    // Suspend host polling whenever a relay is active, so we stop hammering
    // Sunshine's HTTP server while a stream is running (avoids wedging it and
    // making the host appear offline to native clients).
    // g_ActiveRelayRoot stays valid through a take-over teardown (until the old
    // relay is fully destroyed), and g_ActiveSession covers the gap between a
    // /start request and the new relay being created (deferred start). Both keep
    // polling suspended across the whole transition, so a poll never opens an
    // HTTPS 47984 socket that would linger ~120s and hide the host from native
    // clients.
    computerManager.setStreamActivePredicate(
        [&g_ActiveRelay, &g_ActiveMediaTrackRelay, &g_ActiveStreamRelay,
         &g_ActiveRelayRoot, &g_ActiveSession]() {
            return !g_ActiveRelay.isNull()
                || !g_ActiveMediaTrackRelay.isNull()
                || !g_ActiveStreamRelay.isNull()
                || !g_ActiveRelayRoot.isNull()
                || !g_ActiveSession.isNull();
        });

    InternetAccessManager internetAccess(&appSettings);
    GeoIpService geoIpService;

    // Re-sync domain on HttpServer — ensureIdentifiers() (called in
    // the InternetAccessManager constructor) may have just generated a
    // new unique_id, which changes the computed domain.
    server.setDomain(appSettings.domain());

    // ── Startup cert domain sync ──────────────────────────────────────────────
    // After ensureIdentifiers() has computed the correct domain from unique_id,
    // check if the embedded cert (MW_CERT_PEM/MW_CERT_KEY)
    // has a CN that matches this domain.
    //
    // If the CN matches, restore the env-var references in settings.json,
    // overriding any file paths that were left over from a previous ACME run
    // (e.g. after a unique_id change-and-revert cycle).
    //
    // If the CN does NOT match, leave settings as-is; loadCert() will fall
    // through to file scan or trigger ACME issuance via InternetAccessManager.
    {
        QString domain = server.domain();
        if (!domain.isEmpty()) {
            // Resolve MW_CERT_PEM from process env (loadEnvFile already ran)
            QByteArray certData = qgetenv("MW_CERT_PEM");
    #ifdef MW_CERT_PEM
            if (certData.isEmpty())
                certData = QByteArray(MW_CERT_PEM);
    #endif
            QByteArray keyData = qgetenv("MW_CERT_KEY");
    #ifdef MW_CERT_KEY
            if (keyData.isEmpty())
                keyData = QByteArray(MW_CERT_KEY);
    #endif

            if (!certData.isEmpty() && !keyData.isEmpty()) {
                QList<QSslCertificate> certs = QSslCertificate::fromData(certData, QSsl::Pem);
                if (!certs.isEmpty()) {
                    QString cn = certs.first().subjectInfo(QSslCertificate::CommonName).value(0);
                    if (!cn.isEmpty() && cn.compare(domain, Qt::CaseInsensitive) == 0) {
                        // Embedded cert matches the computed domain — restore env var
                        // references, overwriting any stale LE file paths from settings.
                        Logger::info(QString("[main] Embedded cert CN=%1 matches domain=%2 "
                                             "-- restoring cert_pem/cert_key to env var refs")
                            .arg(cn, domain));
                        appSettings.setCertPem(QStringLiteral("MW_CERT_PEM"));
                        appSettings.setCertKey(QStringLiteral("MW_CERT_KEY"));
                        server.setCertPem(QStringLiteral("MW_CERT_PEM"));
                        server.setCertKey(QStringLiteral("MW_CERT_KEY"));
                    } else {
                        Logger::info(QString("[main] Embedded cert CN=%1 does not match "
                                             "domain=%2 -- leaving settings as-is")
                            .arg(cn, domain));
                    }
                }
            }
        }
    }

    // Hot-reload TLS when certificate is renewed (no server restart needed)
    QObject::connect(&internetAccess, &InternetAccessManager::certificateChanged,
        [&server, &appSettings]() {
            qInfo() << "[main] Certificate renewed, reloading TLS";
            // Sync the domain on HttpServer too — it may have been updated since
            // the initial setDomain() call (e.g. unique_id changed via API).
            // Without this, reloadTls() uses the stale m_Domain and can reject
            // the newly issued certificate due to CN mismatch.
            server.setDomain(appSettings.domain());
            server.setCertPem(appSettings.certPem());
            server.setCertKey(appSettings.certKey());
            if (!server.reloadTls()) {
                qWarning() << "[main] TLS reload failed -- restart may be required";
            }
        });

    // Register API routes
    server.router()->get("/api/health", [](const HttpRequest&) {
        QJsonObject obj;
        obj["status"] = "ok";
        obj["version"] = QCoreApplication::applicationVersion();
        return HttpResponse::json(obj);
    });

    // GET /api/server/hostname — returns the server's hostname and OS info
    server.router()->get("/api/server/hostname", [](const HttpRequest&) {
        QJsonObject obj;
#ifdef Q_OS_WIN
        // Use Windows API GetComputerNameW() for the real NetBIOS name
        wchar_t buf[256];
        DWORD sz = static_cast<DWORD>(sizeof(buf) / sizeof(wchar_t));
        if (GetComputerNameW(buf, &sz)) {
            obj["hostname"] = QString::fromWCharArray(buf, static_cast<int>(sz));
        } else {
            obj["hostname"] = qEnvironmentVariable("COMPUTERNAME", "PC");
        }
        obj["os"] = "Windows";
#else
        obj["hostname"] = QHostInfo::localHostName();
    #ifdef Q_OS_MACOS
        obj["os"] = "macOS";
    #elif defined(Q_OS_LINUX)
        obj["os"] = "Linux";
    #else
        obj["os"] = "Unknown";
    #endif
#endif
        return HttpResponse::json(obj);
    });

    // ── Auth routes ─────────────────────────────────────────────────────────
    // POST /api/auth/validate — validate PIN or certificate, create session, set cookie
    server.router()->post("/api/auth/validate",
        [&authManager, &geoIpService](const HttpRequest& req) {
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
                    geoIpService.lookupIp(req.clientAddress,
                        [&authManager, token](const QString& city, const QString& country) {
                            authManager.setSessionGeo(token, city, country);
                        });

                    QJsonObject obj;
                    obj["status"] = "ok";
                    obj["auth_method"] = "certificate";
                    HttpResponse resp = HttpResponse::json(obj);
                    resp.headers["Set-Cookie"] = QString(
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
            if (pin.isEmpty())
                return HttpResponse::error(400, "Missing 'pin' field");

            // Rate limiting uses the real socket IP
            auto result = authManager.validatePin(req.clientAddress, pin);

            QJsonObject obj;
            switch (result.result) {
            case AuthManager::Valid: {
                QString token = authManager.createSession(req.clientAddress, machineName);

                // Look up the IP geolocation asynchronously and store in the session
                geoIpService.lookupIp(req.clientAddress,
                    [&authManager, token](const QString& city, const QString& country) {
                        authManager.setSessionGeo(token, city, country);
                    });

                // Auto-regenerate PIN — immediately invalidate the just-used PIN
                authManager.autoRegeneratePin();

                obj["status"] = "ok";
                obj["pin_regenerated"] = true;
                HttpResponse resp = HttpResponse::json(obj);
                // Set HttpOnly session cookie, 10-year expiry, Strict SameSite
                resp.headers["Set-Cookie"] = QString(
                    "mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000")
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
    server.router()->post("/api/admin/pin/generate",
        [&authManager](const HttpRequest& req) {
            if (!HttpServer::isLocalRequest(req.clientAddress))
                return HttpResponse::error(403, "Only available from localhost");

            QString pin = authManager.generatePin();
            QJsonObject obj;
            obj["status"] = "ok";
            obj["pin"] = pin;
            return HttpResponse::json(obj);
        });

    // POST /api/auth/regenerate — regenerate PIN (localhost only)
    server.router()->post("/api/auth/regenerate",
        [&authManager](const HttpRequest& req) {
            if (!HttpServer::isLocalRequest(req.clientAddress))
                return HttpResponse::error(403, "Only available from localhost");

            authManager.regeneratePin();
            QJsonObject obj;
            obj["status"] = "ok";
            obj["pin"] = authManager.currentPin();
            return HttpResponse::json(obj);
        });

    // POST /api/admin/pin/clear — reset PIN to "--------" (localhost only)
    server.router()->post("/api/admin/pin/clear",
        [&authManager](const HttpRequest& req) {
            if (!HttpServer::isLocalRequest(req.clientAddress))
                return HttpResponse::error(403, "Only available from localhost");

            authManager.clearPin();
            QJsonObject obj;
            obj["status"] = "ok";
            obj["pin"] = authManager.currentPin();
            return HttpResponse::json(obj);
        });

    // GET /api/auth/status — check current auth status
    server.router()->get("/api/auth/status",
        [&authManager, &geoIpService](const HttpRequest& req) {
            QJsonObject obj;
            QString authedToken;  // set when a valid session cookie is found below

            bool isLocal = HttpServer::isLocalRequest(req.clientAddress);
            obj["is_localhost"] = isLocal;

            if (isLocal) {
                obj["authenticated"] = true;  // localhost is always authenticated
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
                                    geoIpService.lookupIp(req.clientAddress,
                                        [&authManager, token](const QString& city, const QString& country) {
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
                    if (lockoutSecs > 0)
                        obj["lockout_seconds"] = lockoutSecs;
                }
            }

            obj["requires_pin"] = !isLocal;
            obj["active_sessions"] = authManager.activeSessionCount();
            obj["cert_auth_enabled"] = authManager.certAuthEnabled();

            HttpResponse resp = HttpResponse::json(obj);
            // Slide the cookie browser-side too, so an active client keeps a
            // fresh 90-day window without ever re-entering the PIN.
            if (!authedToken.isEmpty()) {
                resp.headers["Set-Cookie"] = QString(
                    "mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000")
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
    server.router()->post("/api/auth/sessions/revoke",
        [&authManager](const HttpRequest& req) {
            if (!HttpServer::isLocalRequest(req.clientAddress))
                return HttpResponse::error(403, "Only available from localhost");

            QJsonDocument doc = QJsonDocument::fromJson(req.body);
            QString token = doc.object().value("token").toString();
            Logger::info(QString("[Auth] Revoke request — token='%1', size=%2")
                .arg(token, QString::number(token.size())));
            if (token.isEmpty())
                return HttpResponse::error(400, "Missing 'token' in request body");

            authManager.destroySession(token);
            QJsonObject obj;
            obj["status"] = "revoked";
            return HttpResponse::json(obj);
        });

    // GET /api/admin/certificate/download — download certificate token (localhost only)
    server.router()->get("/api/admin/certificate/download",
        [&authManager](const HttpRequest& req) {
            if (!HttpServer::isLocalRequest(req.clientAddress))
                return HttpResponse::error(403, "Only available from localhost");

            QString token = authManager.certificateToken();
            if (token.isEmpty())
                return HttpResponse::error(500, "Certificate token not initialized");

            HttpResponse resp;
            resp.statusCode = 200;
            resp.contentType = "text/plain; charset=utf-8";
            resp.headers["Content-Disposition"] = "attachment; filename=\"moonlight-web-certificate.txt\"";
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

    server.router()->get("/api/server/status", [&server](const HttpRequest&) {
        QJsonObject obj;
        obj["status"] = "running";
        obj["version"] = QCoreApplication::applicationVersion();
        obj["http_port"] = static_cast<int>(server.httpPort());
        obj["https_port"] = static_cast<int>(server.activeHttpsPort());
        return HttpResponse::json(obj);
    });

    server.router()->get("/api/hosts", [&computerManager](const HttpRequest&) {
        QJsonObject obj;
        obj["hosts"] = computerManager.getHostsJson();
        return HttpResponse::json(obj);
    });

    server.router()->post("/api/hosts/scan", [&computerManager](const HttpRequest&) {
        computerManager.handleScanRequest();
        QJsonObject obj;
        obj["status"] = "scanning";
        return HttpResponse::json(obj);
    });

    server.router()->post("/api/hosts/manual", [&computerManager](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        QString address = body["address"].toString();
        if (address.isEmpty())
            return HttpResponse::error(400, "Missing 'address' field");

        auto [status, result] = computerManager.handleAddManualHost(address);
        return HttpResponse::json(result, status);
    });

    server.router()->del("/api/hosts/:id", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty())
            return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleDeleteHost(uuid);
        return HttpResponse::json(result, status);
    });

    server.router()->post("/api/hosts/:id/wol", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty())
            return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleWakeHost(uuid);
        return HttpResponse::json(result, status);
    });

    // Phase 3: Pairing routes
    server.router()->get("/api/hosts/:id/pair", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty())
            return HttpResponse::error(400, "Missing host ID");
        auto [status, result] = computerManager.handleStartPairing(uuid);
        return HttpResponse::json(result, status);
    });

    server.router()->post("/api/hosts/:id/pair", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty())
            return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleSubmitPin(uuid);
        return HttpResponse::json(result, status);
    });

    // Phase 4: App list (async — fetches from Sunshine via HTTPS)
    server.router()->getAsync("/api/hosts/:id/apps",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }
        computerManager.handleGetAppList(uuid, std::move(respond));
    });

    // Phase 4: App asset proxy — PNG (async, fetches on demand if not cached)
    server.router()->getAsync("/api/hosts/:id/appasset",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }

        bool ok;
        int appId = req.queryParams.value("appid").toInt(&ok);
        if (!ok || appId <= 0) {
            respond(HttpResponse::error(400, "Missing or invalid appid parameter"));
            return;
        }

        computerManager.handleGetBoxArt(uuid, appId, std::move(respond));
    });

    // Phase 5: Start streaming — launch app + RTSP handshake
    auto effectiveUpnpEnabled = upnpEnabled;  // Capture by value for the lambda

    server.router()->postAsync("/api/hosts/:id/start",
        [&computerManager, signalingPort, &g_ActiveRelay, &g_ActiveStreamRelay, &g_ActiveMediaTrackRelay, &g_ActiveSession, &g_ActiveRelayRoot, &g_ActiveClientUniqueId, &server, &appSettings, &authManager, effectiveUpnpEnabled, stunServer](const HttpRequest& req, ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }

        NvComputer* host = computerManager.getHost(uuid);
        if (!host) {
            respond(HttpResponse::error(404, "Host not found"));
            return;
        }

        // ── Take-over: this backend streams ONE session at a time ──────────────
        // moonlight-common-c is a process-global singleton and the signaling port
        // is fixed, so a second Web stream cannot coexist with an active one.
        // Rather than fail (and freeze the first), the newcomer takes over: notify
        // the live client so it shows a graceful exit, then tear its relay down
        // locally WITHOUT quitting Sunshine — the /launch→/resume below reclaims
        // the same session (Sunshine reassigns the RTSP stream on /resume). No
        // /cancel is sent. The relay's sessionEnded is disconnected first so the
        // auto-quit lambda never fires during the take-over.
        //
        // A CONNECTED relay means another device is live-streaming → real take-over
        // (send the notice). A non-connected relay is a dying same-browser fallback
        // attempt → tear it down silently, no notice (avoids a false exit animation
        // mid transport-fallback).
        //
        // Keyed on the typed relay pointers (reliably valid for the whole stream —
        // g_ActiveSession self-clears once streaming starts). Teardown mirrors the
        // proven /quit path: stop moonlight + relay, deleteLater the relay. Its
        // destroyed() later frees the signaling port + moonlight singleton, which
        // is exactly what the deferred start() below waits on. sessionEnded is
        // severed first so the auto-/cancel lambda never cancels the Sunshine
        // session the newcomer is about to /resume.
        if (g_ActiveRelay) {
            DataChannelRelay* old = g_ActiveRelay;
            g_ActiveRelay = nullptr;
            qInfo() << "[Session] Take-over: tearing down active DataChannelRelay" << old;
            if (old->isConnected()) old->notifyClientTakenOver();
            QObject::disconnect(old, &DataChannelRelay::sessionEnded, nullptr, nullptr);
            if (old->moonlightShim()) old->moonlightShim()->stopConnection();
            old->stop();
            old->deleteLater();
        }
        if (g_ActiveMediaTrackRelay) {
            MediaTrackRelay* old = g_ActiveMediaTrackRelay;
            g_ActiveMediaTrackRelay = nullptr;
            qInfo() << "[Session] Take-over: tearing down active MediaTrackRelay" << old;
            if (old->isConnected()) old->notifyClientTakenOver();
            QObject::disconnect(old, &MediaTrackRelay::sessionEnded, nullptr, nullptr);
            if (old->moonlightShim()) old->moonlightShim()->stopConnection();
            old->stop();
            old->deleteLater();
        }
        if (g_ActiveStreamRelay) {
            StreamRelay* old = g_ActiveStreamRelay;
            g_ActiveStreamRelay = nullptr;
            qInfo() << "[Session] Take-over: tearing down active StreamRelay" << old;
            if (old->isClientConnected()) old->notifyClientTakenOver();
            QObject::disconnect(old, &StreamRelay::sessionEnded, nullptr, nullptr);
            if (old->moonlightShim()) old->moonlightShim()->stopConnection();
            old->stop();
            old->deleteLater();
        }
        g_ActiveSession = nullptr;
        g_ActiveClientUniqueId.clear();

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        int appId = body["appId"].toInt(0);
        if (appId <= 0) {
            respond(HttpResponse::error(400, "Missing or invalid appId"));
            return;
        }

        // Identify the authenticated session behind this stream. Remote clients
        // send the mw_session cookie; empty for localhost (no session row to flag).
        QString sessionToken;
        {
            const QString cookie = req.headers.value("cookie");
            const auto cookies = cookie.split(';');
            for (const QString& c : cookies) {
                const QString t = c.trimmed();
                if (t.startsWith("mw_session=", Qt::CaseInsensitive)) {
                    sessionToken = t.mid(QStringLiteral("mw_session=").length());
                    break;
                }
            }
        }

        // ── Per-request streaming settings ─────────────────────────────────────
        // The browser sends its per-browser preferences (from localStorage)
        // alongside the launch request. These override AppSettings defaults
        // for this session only.
        VideoCodec reqCodec = appSettings.videoCodec();
        if (body.contains("video_codec"))
            reqCodec = AppSettings::videoCodecFromString(body["video_codec"].toString());

        bool reqGamingMode = body.contains("gaming_mode")
            ? body["gaming_mode"].toBool()
            : appSettings.gamingMode();

        int reqBitrate = body.contains("stream_bitrate") && body["stream_bitrate"].toInt() > 0
            ? body["stream_bitrate"].toInt()
            : appSettings.streamBitrate();

        int reqHeight = body.contains("stream_height") && body["stream_height"].toInt() > 0
            ? body["stream_height"].toInt()
            : appSettings.streamHeight();

        // Aspect ratio → explicit width. Fix the height, derive the width from
        // the host's actual screen format so ultrawide hosts (21:9 / 32:9) stream
        // un-stretched. "auto" (default) reads the host's largest reported
        // DisplayMode — the host format is detected here, not assumed in advance.
        // An explicit "W:H" overrides it (manual 16:9 / 21:9 / 32:9).
        QString reqAspect = body.contains("stream_aspect") && !body["stream_aspect"].toString().isEmpty()
            ? body["stream_aspect"].toString()
            : appSettings.streamAspect();
        double aspect = 16.0 / 9.0;
        if (reqAspect.contains(':')) {
            const QStringList parts = reqAspect.split(':');
            int w = parts.value(0).toInt(), h = parts.value(1).toInt();
            if (w > 0 && h > 0) aspect = static_cast<double>(w) / h;
        } else if (!host->displayModes.isEmpty()) {
            // "auto": displayModes are sorted largest-first → native host format.
            const NvDisplayMode& top = host->displayModes.first();
            if (top.width > 0 && top.height > 0)
                aspect = static_cast<double>(top.width) / top.height;
        }
        // Even width (encoders require it), 0 height stays native (width 0).
        int reqWidth = (reqHeight > 0) ? (static_cast<int>(reqHeight * aspect + 0.5) & ~1) : 0;
        qInfo() << "[Session] Aspect" << reqAspect << "→" << reqWidth << "x" << reqHeight;

        int reqFps = body.contains("stream_fps") && body["stream_fps"].toInt() > 0
            ? body["stream_fps"].toInt()
            : appSettings.streamFps();

        bool reqHdr = body.contains("hdr_enabled")
            ? body["hdr_enabled"].toBool()
            : appSettings.hdrEnabled();

        // Video enhancement (WebGPU): the browser renders via canvas, so when it
        // is on the transport negotiation must avoid webrtc-media (<video>).
        bool reqVideoEnhancement = body.contains("video_enhancement")
            ? (body["video_enhancement"].toString() == "on")
            : (appSettings.videoEnhancement() == "on");

        // Per-browser Sunshine unique ID (from the browser's localStorage).
        // Sanitized to hex (max 32 chars) before it reaches the launch URL.
        // Empty → StreamSession falls back to the shared Moonlight unique ID.
        QString reqClientUniqueId;
        for (const QChar& c : body["client_uniqueid"].toString()) {
            QChar u = c.toUpper();
            if (u.isDigit() || (u >= 'A' && u <= 'F'))
                reqClientUniqueId += u;
            if (reqClientUniqueId.size() >= 32)
                break;
        }

        qInfo() << "[Session] Per-request streaming settings:"
                << "codec=" << AppSettings::videoCodecToString(reqCodec)
                << "gaming=" << reqGamingMode
                << "bitrate=" << reqBitrate
                << "height=" << reqHeight
                << "fps=" << reqFps
                << "hdr=" << reqHdr
                << "videoEnhancement=" << reqVideoEnhancement;

        // Determine signaling host from the browser's Host header.
        // Works for both LAN (localhost:443) and remote access via moonlightweb.top.
        QString serverHost = req.headers.value("host");
        int colon = serverHost.indexOf(':');
        if (colon >= 0)
            serverHost = serverHost.left(colon);

        // ================================================================
        // Resolve transport mode (from AppSettings).
        //
        // transport_mode values:
        //   "auto"                 → automatic fallback chain (see below)
        //   "webrtc-media-udp"     → MediaTrack + UDP only
        //   "webrtc-dc-udp"       → DataChannel + UDP only
        //   "webrtc-media-tcp"    → MediaTrack + UDP+TCP
        //   "webrtc-dc-tcp"       → DataChannel + UDP+TCP
        //   "wss"                 → StreamRelay (WebSocket, always works)
        // ================================================================
        QString transportMode = appSettings.transportMode();
        if (transportMode.isEmpty())
            transportMode = "auto";

        // A per-request transport_mode (sent by the browser) overrides the admin
        // setting. The browser does NOT normally send it; it only sends
        // transport_index to walk the fallback chain on relaunch.
        if (body.contains("transport_mode") && !body["transport_mode"].toString().isEmpty())
            transportMode = body["transport_mode"].toString();

        // Index into the fallback chain that this attempt targets (the browser
        // increments it and relaunches when a transport fails to connect).
        int reqTransportIndex = body.contains("transport_index")
            ? body["transport_index"].toInt(0) : 0;

        // ── Auto-mode: priority-ordered transport list from header ─────

        // Helper: does the host support a given codec?
        auto hostSupportsCodec = [](NvComputer* h, VideoCodec c) -> bool {
            int support = h->serverCodecModeSupport;
            switch (c) {
            case VideoCodec::H264: return (support & 0x01) != 0; // SERVER_CODEC_MODE_H264
            case VideoCodec::HEVC: return (support & 0x02) != 0; // SERVER_CODEC_MODE_H265
            case VideoCodec::AV1:  return (support & 0x20) != 0; // SERVER_CODEC_MODE_AV1
            default:               return true;
            }
        };

        // ── HDR codec compatibility ───────────────────────────────────────────
        // HDR requires 10-bit HEVC (or AV1). H.264 cannot carry HDR, and the
        // MediaTrack transport is H.264-only. So when HDR is requested we force
        // HEVC — provided the host supports it. If the host has no HEVC, HDR is
        // disabled (we keep the requested codec and stream SDR).
        if (reqHdr) {
            if (hostSupportsCodec(host, VideoCodec::HEVC)) {
                if (reqCodec != VideoCodec::HEVC) {
                    qInfo() << "[Session] HDR requested — forcing codec HEVC (was"
                            << AppSettings::videoCodecToString(reqCodec) << ")";
                    reqCodec = VideoCodec::HEVC;
                }
            } else {
                qWarning() << "[Session] HDR requested but host has no HEVC support"
                           << "— disabling HDR, streaming SDR";
                reqHdr = false;
            }
        }

        // Helper: filter transport list by codec compatibility.
        //
        // Rules:
        //   - H.264 works on all transports.
        //   - HEVC and AV1 are skipped for MediaTrack modes (<video> element
        //     codec support varies across browsers; DataChannel WebCodecs
        //     is more reliable).
        //   - If the host doesn't support the selected codec, try H.264
        //     instead (if host supports it).
        auto filterTransportsByCodec = [&](const QStringList& transports,
                                            VideoCodec codec,
                                            NvComputer* h) -> QStringList {
            // Determine effective codec (resolve Auto → HEVC if host supports)
            VideoCodec effective = codec;
            if (effective == VideoCodec::Auto) {
                effective = hostSupportsCodec(h, VideoCodec::HEVC)
                    ? VideoCodec::HEVC : VideoCodec::H264;
            }

            // If host doesn't support the effective codec, fall back to H.264
            if (!hostSupportsCodec(h, effective)) {
                if (hostSupportsCodec(h, VideoCodec::H264)) {
                    qInfo() << "[Auto] Codec" << static_cast<int>(effective)
                            << "not supported by host, falling back to H.264";
                    effective = VideoCodec::H264;
                } else {
                    qWarning() << "[Auto] Host supports NO video codec!";
                    return transports; // Return all, let it fail naturally
                }
            }

            qInfo() << "[Auto] Effective codec:" << static_cast<int>(effective);

            QStringList result;
            for (const auto& t : transports) {
                if ((effective == VideoCodec::AV1 || effective == VideoCodec::HEVC)
                    && t.startsWith("webrtc-media")) {
                    qInfo() << "[Auto] Skipping" << t
                            << "(MediaTrack only supports H.264, codec is"
                            << static_cast<int>(effective) << ")";
                    continue; // Skip non-H.264 codecs on MediaTrack
                }
                // Note: with video enhancement ON, MediaTrack is NOT skipped — it
                // is kept as a last resort (ordered last by TransportPriorities)
                // and streams WITHOUT enhancement (<video> cannot be processed
                // by WebGPU). The ordering is handled in orderedTransports().
                result.append(t);
            }
            return result;
        };

        // ── Build the ordered transport fallback chain ─────────────────────────
        // The chain is the single source of truth, computed once and echoed back
        // to the browser. The browser drives the fallback by relaunching with the
        // next index when a transport fails to establish a connection.
        //
        //   - "auto"      → the priority-ordered list (enhancement-aware, codec-
        //                   filtered).
        //   - explicit X  → X first; if X is a "-udp" mode, its "-tcp" sibling is
        //                   promoted to second (prefer the same transport family
        //                   before switching), then the rest of the auto order.
        QStringList autoOrder = filterTransportsByCodec(
            TransportPriorities::orderedTransports(reqVideoEnhancement), reqCodec, host);

        QStringList transportChain;
        if (transportMode == "auto") {
            transportChain = autoOrder;
        } else {
            transportChain.append(transportMode); // forced mode first
            // Keep the same transport family on first fallback: a forced "-udp"
            // mode prefers its "-tcp" sibling second (e.g. webrtc-media-udp →
            // webrtc-media-tcp) before falling through to other transports.
            QString sibling;
            if (transportMode.endsWith(QStringLiteral("-udp")))
                sibling = transportMode.left(transportMode.length() - 4)
                          + QStringLiteral("-tcp");
            if (!sibling.isEmpty() && autoOrder.contains(sibling))
                transportChain.append(sibling);
            for (const QString& m : autoOrder)
                if (m != transportMode && m != sibling)
                    transportChain.append(m);
        }
        qInfo() << "[Session] Transport chain:" << transportChain
                << "requested index=" << reqTransportIndex;

        if (transportChain.isEmpty()
            || reqTransportIndex < 0
            || reqTransportIndex >= transportChain.size()) {
            qWarning() << "[Session] Transport index out of range — chain exhausted";
            respond(HttpResponse::error(502, "All transport modes failed"));
            return;
        }

        // Resolve the mode for this attempt → internal transport + ICE config.
        QString chainMode = transportChain[reqTransportIndex];
        bool enableIceTcp = chainMode.endsWith(QStringLiteral("-tcp"));
        QString internalTransport;
        if (chainMode.startsWith(QStringLiteral("webrtc-media")))
            internalTransport = QStringLiteral("webrtc-media");
        else if (chainMode.startsWith(QStringLiteral("webrtc-dc")))
            internalTransport = QStringLiteral("webrtc");
        else
            internalTransport = QStringLiteral("wss");
        qInfo() << "[Session] Attempt" << (reqTransportIndex + 1) << "/"
                << transportChain.size() << ":" << chainMode
                << "internal=" << internalTransport << "iceTcp=" << enableIceTcp;

        // ── Helper: attach lifecycle relay tracking for a new session ───────────
        // Adds the standard relay-created and session-ended connections that
        // maintain the global relay pointers (g_ActiveRelay, etc.) and send a
        // best-effort HTTPS quit to Sunshine when a session ends unexpectedly.
        auto attachRelayTracking = [&](StreamSession* s) {
            // Per-browser uniqueid so an unexpected-end auto-quit cancels only THIS
            // browser's Sunshine session (keyed like /launch), never a co-located
            // iOS/Qt client's session. Empty → shared id (legacy fallback).
            QString quitUid = reqClientUniqueId;

            // WSS mode: StreamRelay tracking
            QObject::connect(s, &StreamSession::streamRelayCreated,
                [&g_ActiveStreamRelay, &g_ActiveRelayRoot, &g_ActiveClientUniqueId, &computerManager, &authManager, sessionToken, host, quitUid](StreamRelay* r) {
                    qInfo() << "[main] streamRelayCreated, relay=" << r;
                    g_ActiveStreamRelay = r;
                    g_ActiveRelayRoot = r;
                    g_ActiveClientUniqueId = quitUid;
                    authManager.setSessionStreaming(sessionToken, true);

                    // Context = qApp so the lambda runs on the main thread: the relay
                    // emits sessionEnded from its dedicated thread, but quitAppAsync()
                    // touches the shared QNAM that lives on the main thread.
                    QObject::connect(r, &StreamRelay::sessionEnded, qApp,
                        [r, &g_ActiveStreamRelay, &computerManager, &authManager, sessionToken, host, quitUid]() {
                            qInfo() << "[main] StreamRelay sessionEnded";
                            authManager.setSessionStreaming(sessionToken, false);
                            auto* identity = IdentityManager::get();
                            auto* quitReply = computerManager.http()->quitAppAsync(
                                host->activeAddress, host->activeHttpsPort,
                                identity->getCertificate(), identity->getPrivateKey(), quitUid);
                            QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);
                            r->stop();
                            r->deleteLater();
                            if (g_ActiveStreamRelay == r) g_ActiveStreamRelay = nullptr;
                        });
                });

            // WebRTC DataChannel mode: DataChannelRelay tracking
            QObject::connect(s, &StreamSession::relayCreated,
                [&g_ActiveRelay, &g_ActiveRelayRoot, &g_ActiveClientUniqueId, &computerManager, &authManager, sessionToken, host, quitUid](DataChannelRelay* r) {
                    qInfo() << "[main] relayCreated, relay=" << r;
                    g_ActiveRelay = r;
                    g_ActiveRelayRoot = r;
                    g_ActiveClientUniqueId = quitUid;
                    authManager.setSessionStreaming(sessionToken, true);

                    // Context = qApp: see StreamRelay note above (run on main thread).
                    QObject::connect(r, &DataChannelRelay::sessionEnded, qApp,
                        [r, &g_ActiveRelay, &computerManager, &authManager, sessionToken, host, quitUid]() {
                            qInfo() << "[main] sessionEnded fired, relay=" << r;
                            authManager.setSessionStreaming(sessionToken, false);
                            auto* identity = IdentityManager::get();
                            auto* quitReply = computerManager.http()->quitAppAsync(
                                host->activeAddress, host->activeHttpsPort,
                                identity->getCertificate(), identity->getPrivateKey(), quitUid);
                            QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);
                            r->stop();
                            r->deleteLater();
                            if (g_ActiveRelay == r) {
                                g_ActiveRelay = nullptr;
                            }
                        });
                });

            // WebRTC Media Track mode: MediaTrackRelay tracking
            QObject::connect(s, &StreamSession::mediaTrackRelayCreated,
                [&g_ActiveMediaTrackRelay, &g_ActiveRelayRoot, &g_ActiveClientUniqueId, &computerManager, &authManager, sessionToken, host, quitUid](MediaTrackRelay* r) {
                    qInfo() << "[main] mediaTrackRelayCreated, relay=" << r;
                    g_ActiveMediaTrackRelay = r;
                    g_ActiveRelayRoot = r;
                    g_ActiveClientUniqueId = quitUid;
                    authManager.setSessionStreaming(sessionToken, true);

                    // Context = qApp: see StreamRelay note above (run on main thread).
                    QObject::connect(r, &MediaTrackRelay::sessionEnded, qApp,
                        [r, &g_ActiveMediaTrackRelay, &computerManager, &authManager, sessionToken, host, quitUid]() {
                            qInfo() << "[main] MediaTrackRelay sessionEnded, relay=" << r;
                            authManager.setSessionStreaming(sessionToken, false);
                            auto* identity = IdentityManager::get();
                            auto* quitReply = computerManager.http()->quitAppAsync(
                                host->activeAddress, host->activeHttpsPort,
                                identity->getCertificate(), identity->getPrivateKey(), quitUid);
                            QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);
                            r->stop();
                            r->deleteLater();
                            if (g_ActiveMediaTrackRelay == r) g_ActiveMediaTrackRelay = nullptr;
                        });
                });
        };

        // ── Helper: create a session with the given transport mode and attach tracking ─
        // transportMode: full mode string ("webrtc-media-udp", "wss", etc.)
        // iceTcp: whether to enable ICE-TCP candidates
        auto createSession = [&](const QString& transportMode, bool iceTcp,
                                  ResponseCallback rsp,
                                  VideoCodec codecOverride = VideoCodec::Auto) -> StreamSession* {
            // Map transport mode to internal transport string
            QString internal;
            if (transportMode == "webrtc-media-udp" || transportMode == "webrtc-media-tcp")
                internal = "webrtc-media";
            else if (transportMode == "webrtc-dc-udp" || transportMode == "webrtc-dc-tcp")
                internal = "webrtc";
            else
                internal = transportMode; // "wss"

            auto* s = new StreamSession(
                host, appId,
                computerManager.http(),
                std::move(rsp),
                signalingPort,
                serverHost,
                (codecOverride != VideoCodec::Auto) ? codecOverride : reqCodec,
                reqGamingMode,
                effectiveUpnpEnabled,
                internal,
                stunServer,
                reqHeight,
                reqWidth,
                reqFps,
                reqBitrate,
                reqHdr
            );
            s->setHttpsPort(server.activeHttpsPort());
            s->setStreamRelayPort(signalingPort + 1);
            s->setTransportMode(transportMode); // Full mode for response
            s->setEnableIceTcp(iceTcp);
            s->setClientUniqueId(reqClientUniqueId);
            attachRelayTracking(s);
            // Track the active session so a later take-over can quit() it (stops
            // the SignalingServer first → frees the fixed port). QPointer auto-
            // nulls when the session self-destructs on normal end.
            g_ActiveSession = s;
            return s;
        };

        // ═════════════════════════════════════════════════════════════════════
        // Single attempt for chainMode (transportChain[reqTransportIndex]).
        //
        // The browser owns the fallback loop: when a transport fails to connect,
        // it relaunches with transport_index+1. The full chain is echoed back in
        // the response so the browser knows how far it can go. This is required
        // because the signaling response is sent BEFORE the ICE connection is
        // established — connection failures are only observable on the client.
        // ═════════════════════════════════════════════════════════════════════
        {
            // MediaTrack only carries H.264 → force it if the user selected
            // HEVC/AV1 (MediaTrackRelay cannot encode those).
            VideoCodec effectiveCodec = reqCodec;
            bool codecOverridden = false;
            VideoCodec originalCodec = VideoCodec::Auto;

            if (internalTransport == "webrtc-media"
                && (effectiveCodec == VideoCodec::HEVC
                    || effectiveCodec == VideoCodec::AV1)) {
                qInfo() << "[Session] MediaTrack attempt but codec is"
                        << AppSettings::videoCodecToString(effectiveCodec)
                        << "- forcing H.264 (MediaTrack only supports H.264)";
                originalCodec = effectiveCodec;
                effectiveCodec = VideoCodec::H264;
                codecOverridden = true;
            }

            auto* session = createSession(chainMode, enableIceTcp,
                std::move(respond), effectiveCodec);
            if (codecOverridden) {
                session->setCodecOverridden(true, originalCodec);
            }
            session->setTransportChain(transportChain, reqTransportIndex);
            // Disable the in-session WS fallback: the browser drives the chain
            // (… → wss is a distinct relaunch, not an in-session reroute).
            session->setAutoMode(true);

            // Serialize with any previous relay still tearing down. The signaling
            // port and the moonlight singleton (one LiStartConnection at a time)
            // are released only when the old relay graph is fully DESTROYED (after
            // the slow moonlight LiStopConnection). Defer start() until then —
            // this is the real fix for "second session won't start": it covers
            // both take-over and a self-disconnect immediately followed by a
            // relaunch. start() then runs on the main thread (qApp context).
            if (g_ActiveRelayRoot) {
                qInfo() << "[Session] Previous relay" << g_ActiveRelayRoot.data()
                        << "still tearing down — deferring start() until destroyed";
                QObject::connect(g_ActiveRelayRoot, &QObject::destroyed, qApp,
                    [session]() {
                        qInfo() << "[Session] Previous relay gone — starting deferred session" << session;
                        session->start();
                    });
            } else {
                session->start();
            }
        }
    });

    // Phase 5: Quit running app
    server.router()->postAsync("/api/hosts/:id/quit",
        [&computerManager, &g_ActiveRelay, &g_ActiveStreamRelay, &g_ActiveMediaTrackRelay, &g_ActiveSession, &g_ActiveClientUniqueId](const HttpRequest& req, ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        qInfo() << "[quit] ENTER — uuid=" << uuid << "relay=" << g_ActiveRelay.data()
                << "relay valid=" << (!g_ActiveRelay.isNull());

        if (uuid.isEmpty()) {
            qWarning() << "[quit] Empty uuid, returning 400";
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }

        NvComputer* host = computerManager.getHost(uuid);
        if (!host) {
            qWarning() << "[quit] Host not found for uuid=" << uuid;
            respond(HttpResponse::error(404, "Host not found"));
            return;
        }
        qInfo() << "[quit] Host found:" << host->name << host->activeAddress.address()
                << ":" << host->activeHttpsPort;

        // Per-browser unique ID so the cancel targets this browser's own
        // session (must match the one used at /launch). Sanitized to hex.
        QString quitUniqueId;
        {
            QJsonObject qbody = QJsonDocument::fromJson(req.body).object();
            for (const QChar& c : qbody["client_uniqueid"].toString()) {
                QChar u = c.toUpper();
                if (u.isDigit() || (u >= 'A' && u <= 'F'))
                    quitUniqueId += u;
                if (quitUniqueId.size() >= 32)
                    break;
            }
        }

        // Ownership guard: a client that was taken over by another device may
        // still send a late /quit. The relay teardown below acts on the GLOBAL
        // active relay — which is now the NEW owner's session — so tearing it
        // down would kill the wrong stream. Only stop the relay when this request
        // owns the active session (matching uniqueid). The quitAppAsync below is
        // keyed by uniqueid and harmlessly cancels only the requester's own
        // (already-gone) Sunshine session. Empty ids (localhost) keep legacy
        // behaviour.
        bool ownsSession = g_ActiveClientUniqueId.isEmpty()
                        || quitUniqueId.isEmpty()
                        || quitUniqueId == g_ActiveClientUniqueId;
        if (!ownsSession) {
            qInfo() << "[quit] Stale quit from non-owner uniqueid=" << quitUniqueId
                    << "(active owner=" << g_ActiveClientUniqueId
                    << ") — skipping relay teardown";
        }

        // Stop the transport relay first (closes PeerConnection or WS)
        bool relayStopped = false;

        if (ownsSession && g_ActiveRelay) {
            qInfo() << "[quit] WebRTC relay exists, stopping relay=" << g_ActiveRelay.data();
            DataChannelRelay* relay = g_ActiveRelay;
            g_ActiveRelay = nullptr;
            // Explicitly stop MoonlightShim before relay cleanup so LiStopConnection
            // runs on the main thread, not deferred to the relay destructor.
            if (relay->moonlightShim())
                relay->moonlightShim()->stopConnection();
            relay->stop();
            relay->deleteLater();
            relayStopped = true;
        }

        if (ownsSession && g_ActiveMediaTrackRelay) {
            qInfo() << "[quit] MediaTrackRelay exists, stopping relay=" << g_ActiveMediaTrackRelay.data();
            MediaTrackRelay* relay = g_ActiveMediaTrackRelay;
            g_ActiveMediaTrackRelay = nullptr;
            // Explicitly stop MoonlightShim before relay cleanup.
            if (relay->moonlightShim())
                relay->moonlightShim()->stopConnection();
            relay->stop();
            relay->deleteLater();
            relayStopped = true;
        }

        if (ownsSession && g_ActiveStreamRelay) {
            qInfo() << "[quit] StreamRelay exists, stopping relay=" << g_ActiveStreamRelay.data();
            StreamRelay* relay = g_ActiveStreamRelay;
            g_ActiveStreamRelay = nullptr;
            relay->stop();
            relay->deleteLater();
            relayStopped = true;
        }

        // Drop the active-session handle too (its relay was just torn down above)
        // so a later take-over never calls quit() on a session whose relay is
        // already gone. The session shell self-destructs here.
        if (ownsSession && g_ActiveSession) {
            g_ActiveSession->deleteLater();
            g_ActiveSession = nullptr;
            g_ActiveClientUniqueId.clear();
        }

        if (!relayStopped) {
            qInfo() << "[quit] No active relay (already stopped or never started)";
        }

        qInfo() << "[quit] Sending quitAppAsync to Sunshine ...";
        auto* identity = IdentityManager::get();
        QNetworkReply* reply = computerManager.http()->quitAppAsync(
            host->activeAddress, host->activeHttpsPort,
            identity->getCertificate(), identity->getPrivateKey(), quitUniqueId);
        qInfo() << "[quit] quitAppAsync reply=" << reply;

        // Wait for quit to complete, then respond
        QObject::connect(reply, &QNetworkReply::finished, [reply, respond]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                qWarning() << "[quit] Sunshine quit failed: error=" << reply->error()
                           << "errorString=" << reply->errorString();
                qInfo() << "[quit] EXIT — returning 502";
                respond(HttpResponse::error(502, "Quit failed: " + reply->errorString()));
            } else {
                QByteArray body = reply->readAll();
                qInfo() << "[quit] Sunshine quit OK, body size=" << body.size()
                        << "body=" << body.left(200);
                QJsonObject result;
                result["status"] = "quit";
                qInfo() << "[quit] EXIT — returning 200 OK";
                respond(HttpResponse::json(result));
            }
        });
    });

    if (!server.start(httpsPort))
        return 1;

    // Persist active ports (may differ from preferred due to fallback)
    {
        quint16 activeHttps = server.activeHttpsPort();
        if (activeHttps > 0 && appSettings.httpsPort(0) != activeHttps)
            appSettings.setHttpsPort(activeHttps);

        quint16 activeHttp = server.httpPort();
        if (appSettings.httpPort(0) != activeHttp)
            appSettings.setHttpPort(activeHttp);
    }

    // Sync UPnP port mapping port with the actual server port
    internetAccess.setPorts(server.httpPort(), server.activeHttpsPort());

    // Auto-start Internet Access if it was enabled before last shutdown.
    // This handles DNS registration + public IP detection at boot without
    // waiting for the user to toggle the checkbox in the UI.
    if (appSettings.internetAccessEnabled()) {
        qInfo() << "[main] internet_access_enabled is true — auto-starting...";
        internetAccess.start();
        QJsonObject st = internetAccess.statusJson();
        qInfo() << "[main] auto-start completed — active:" << internetAccess.isActive()
                << "domain:" << st.value("domain").toString()
                << "lastError:" << st.value("last_error").toString();
    }

    // Configure HttpServer to proxy WebSocket upgrades to the signaling server.
    // Both HTTPS and WebSocket signaling share the same port (443 by default).
    server.setSignalingPort(signalingPort);
    // Legacy WSS StreamRelay uses the next port for its local WS server.
    server.setStreamRelayPort(signalingPort + 1);

    // — Internet Access via PowerDNS —

    // API route: get Internet Access status
    server.router()->get("/api/internet/status", [&](const HttpRequest& req) {
        QJsonObject obj = internetAccess.statusJson();
        // The admin UI runs on localhost and needs the full payload. Remote
        // sessions must not learn the internal network topology / file layout.
        if (!HttpServer::isLocalRequest(req.clientAddress)) {
            for (const char* key : { "local_ip", "public_ip", "unique_id",
                                     "cert_pem", "cert_key", "last_error" })
                obj.remove(QLatin1String(key));
        }
        return HttpResponse::json(obj);
    });

    // API route: enable/configure Internet Access
    server.router()->post("/api/internet/enable", [&](const HttpRequest& req) {
        // Only localhost can modify internet access settings
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Internet access settings can only be modified from localhost");

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();

        // unique_id is immutable once assigned: it keys this instance's subdomain,
        // its DNS ownership token, and its certificate. Only accept it when unset.
        if (body.contains("unique_id") && appSettings.uniqueId().isEmpty())
            appSettings.setUniqueId(body["unique_id"].toString());
        // pdns_token is no longer stored in settings; set MW_PDNS_TOKEN env var instead.
        if (body.contains("auto_ip_detection"))
            appSettings.setAutoIpDetection(body["auto_ip_detection"].toBool());
        if (body.contains("transport_mode"))
            appSettings.setTransportMode(body["transport_mode"].toString());
        if (body.contains("public_ip"))
            appSettings.setPublicIp(body["public_ip"].toString());

        if (body.contains("internet_access_enabled"))
            appSettings.setInternetAccessEnabled(body["internet_access_enabled"].toBool());
        if (body.contains("upnp_enabled"))
            appSettings.setUpnpEnabled(body["upnp_enabled"].toBool());

        bool enabled = body.value("internet_access_enabled").toBool(
            appSettings.internetAccessEnabled());

        if (enabled) {
            qInfo() << "[main] POST /api/internet/enable — calling internetAccess.start()...";
            internetAccess.start();
            QJsonObject obj = internetAccess.statusJson();
            qInfo() << "[main] internetAccess.start() completed — active:" << internetAccess.isActive()
                    << "lastError:" << obj.value("last_error").toString();
            obj["status"] = "enabled";
            return HttpResponse::json(obj);
        } else {
            internetAccess.stop();
            QJsonObject obj;
            obj["status"] = "disabled";
            obj["internet_access_enabled"] = false;
            return HttpResponse::json(obj);
        }
    });

    // API route: disable Internet Access
    server.router()->post("/api/internet/disable", [&](const HttpRequest& req) {
        // Only localhost can modify internet access settings
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Internet access settings can only be modified from localhost");

        internetAccess.stop();
        appSettings.setInternetAccessEnabled(false);

        QJsonObject obj;
        obj["status"] = "disabled";
        return HttpResponse::json(obj);
    });

    // API route: force refresh (re-check IP, DNS, certificate)
    server.router()->post("/api/internet/refresh", [&](const HttpRequest&) {
        internetAccess.forceRefresh();
        return HttpResponse::json(internetAccess.statusJson());
    });

    // API route: renew TLS certificate
    server.router()->post("/api/internet/renew-cert", [&](const HttpRequest&) {
        internetAccess.renewCertificate();
        QJsonObject obj;
        obj["status"] = "renewing";
        return HttpResponse::json(obj);
    });

    // — Admin settings (localhost only, server config) —

    server.router()->get("/api/admin/settings", [&server, &appSettings, &authManager](const HttpRequest&) {
        QJsonObject obj;
        obj["http_port"] = static_cast<int>(server.httpPort());
        obj["https_port"] = static_cast<int>(server.activeHttpsPort());
        obj["cert_auth_enabled"] = authManager.certAuthEnabled();
        return HttpResponse::json(obj);
    });

    server.router()->post("/api/admin/settings", [&server, &appSettings, &authManager, &internetAccess](const HttpRequest& req) {
        // Only localhost can modify server admin settings
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Admin settings can only be modified from localhost");

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        bool hadChange = false;
        QJsonObject obj;

        // ── Certificate authentication toggle ────────────────────────────
        if (body.contains("cert_auth_enabled")) {
            bool enabled = body["cert_auth_enabled"].toBool();
            authManager.setCertAuthEnabled(enabled);
            obj["cert_auth_enabled"] = enabled;
            hadChange = true;
        }

        // ── HTTPS port change ────────────────────────────────────────────
        if (body.contains("https_port")) {
            quint16 newPort = static_cast<quint16>(body["https_port"].toInt(443));
            quint16 oldPort = server.activeHttpsPort();

            appSettings.setHttpsPort(newPort);

            obj["https_port"] = static_cast<int>(newPort);

            if (newPort == oldPort || oldPort == 0) {
                obj["status"] = "saved";
            } else {
                obj["status"] = "saved";
                obj["port_changed"] = true;

                qInfo() << "[admin] Scheduled deferred HTTPS port change from"
                        << oldPort << "to" << newPort;

                QTimer::singleShot(0, [&server, &internetAccess, newPort, oldPort]() {
                    qInfo() << "[admin] Deferred: changing HTTPS port to" << newPort;
                    if (!server.changeHttpsPort(newPort)) {
                        qWarning() << "[admin] Port change failed, restoring" << oldPort;
                        server.changeHttpsPort(oldPort);
                    }
                    // Sync the active port to InternetAccessManager so statusJson()
                    // returns the correct https_port (used by the admin UI).
                    internetAccess.setPorts(server.httpPort(), server.activeHttpsPort());
                });
            }

            hadChange = true;
        }

        if (!hadChange)
            return HttpResponse::error(400, "No supported settings provided");

        return HttpResponse::json(obj);
    });

    // — Streaming settings —

    server.router()->get("/api/settings/streaming", [&appSettings](const HttpRequest&) {
        QJsonObject obj;

        // Normalise "auto" → "hevc" (the old "auto" default is replaced by explicit HEVC).
        // Existing settings.json entries with "video_codec":"auto" are migrated on read.
        VideoCodec codec = appSettings.videoCodec();
        QString codecStr = AppSettings::videoCodecToString(codec);
        if (codecStr == "auto")
            codecStr = "hevc";
        obj["video_codec"] = codecStr;

        obj["gaming_mode"] = appSettings.gamingMode();
        obj["show_performance_stats"] = appSettings.showPerformanceStats();
        obj["upnp_enabled"] = appSettings.upnpEnabled();
        obj["transport"] = appSettings.transport();
        obj["stun_server"] = appSettings.stunServer();
        obj["internet_access_enabled"] = appSettings.internetAccessEnabled();
        QString transportMode = appSettings.transportMode();
        obj["transport_mode"] = transportMode;
        obj["media_track_only_h264"] = (transportMode == "webrtc-media-udp" || transportMode == "webrtc-media-tcp");
        obj["auto_ip_detection"] = appSettings.autoIpDetection();
        obj["stream_bitrate"] = appSettings.streamBitrate();
        obj["stream_height"] = appSettings.streamHeight();
        obj["stream_aspect"] = appSettings.streamAspect();
        obj["stream_fps"] = appSettings.streamFps();
        obj["hdr_enabled"] = appSettings.hdrEnabled();
        obj["video_enhancement"] = appSettings.videoEnhancement();
        obj["video_enhancement_algo"] = appSettings.videoEnhancementAlgo();
        // Debug build flag: the UI exposes the enhancement algo selector only in
        // debug builds (Qt Creator); production forces 'auto'.
#ifdef QT_DEBUG
        obj["debug_build"] = true;
#else
        obj["debug_build"] = false;
#endif
        return HttpResponse::json(obj);
    });

    server.router()->post("/api/settings/streaming", [&appSettings](const HttpRequest& req) {
        // Only localhost can modify server-side streaming settings
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Streaming settings can only be modified from localhost");

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();

        QJsonObject obj;
        bool hadChange = false;

        if (body.contains("video_codec")) {
            VideoCodec codec = AppSettings::videoCodecFromString(body["video_codec"].toString());
            appSettings.setVideoCodec(codec);
            obj["video_codec"] = AppSettings::videoCodecToString(codec);
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("gaming_mode")) {
            bool enabled = body["gaming_mode"].toBool();
            appSettings.setGamingMode(enabled);
            obj["gaming_mode"] = enabled;
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("show_performance_stats")) {
            bool enabled = body["show_performance_stats"].toBool();
            appSettings.setShowPerformanceStats(enabled);
            obj["show_performance_stats"] = enabled;
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("stream_bitrate")) {
            int kbps = body["stream_bitrate"].toInt(20000);
            appSettings.setStreamBitrate(kbps);
            obj["stream_bitrate"] = appSettings.streamBitrate();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("stream_height")) {
            int height = body["stream_height"].toInt(1080);
            appSettings.setStreamHeight(height);
            obj["stream_height"] = appSettings.streamHeight();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("stream_aspect")) {
            appSettings.setStreamAspect(body["stream_aspect"].toString());
            obj["stream_aspect"] = appSettings.streamAspect();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("stream_fps")) {
            int fps = body["stream_fps"].toInt(60);
            appSettings.setStreamFps(fps);
            obj["stream_fps"] = appSettings.streamFps();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("hdr_enabled")) {
            bool enabled = body["hdr_enabled"].toBool();
            appSettings.setHdrEnabled(enabled);
            obj["hdr_enabled"] = enabled;
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("upnp_enabled")) {
            bool enabled = body["upnp_enabled"].toBool();
            appSettings.setUpnpEnabled(enabled);
            obj["upnp_enabled"] = enabled;
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("video_enhancement")) {
            appSettings.setVideoEnhancement(body["video_enhancement"].toString());
            obj["video_enhancement"] = appSettings.videoEnhancement();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("video_enhancement_algo")) {
            appSettings.setVideoEnhancementAlgo(body["video_enhancement_algo"].toString());
            obj["video_enhancement_algo"] = appSettings.videoEnhancementAlgo();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (!hadChange)
            return HttpResponse::error(400, "No supported settings provided");

        return HttpResponse::json(obj);
    });

    // Phase N: System tray icon
    TrayManager trayManager(&server);
    trayManager.init();

    Logger::info("Server ready. Open https://localhost" + (httpsPort != 443 ? ":" + QString::number(server.activeHttpsPort()) : QString()) + " in your browser.");

    return app.exec();
}