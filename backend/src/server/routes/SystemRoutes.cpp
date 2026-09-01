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

#include "server/routes/SystemRoutes.h"

#include "server/HttpServer.h"
#include "server/RestRouter.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/Provisioning.h"
#include "network/InternetAccessManager.h"
#include "network/UPNPClient.h"
#include "backend/ComputerManager.h"
#include "backend/SunshineInstaller.h"
#include "backend/SunshineRestClient.h"
#include "Autostart.h"
#include "DisplaySleep.h"
#include "common/DesktopSession.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

// Exit code the restart endpoint leaves with so the service supervisor treats it
// as "come back": NSSM's `AppExit Default Restart` respawns on any code it hasn't
// been told to Exit on, and only 0 is mapped to a real stop (install-service.bat).
static constexpr int kServiceRestartExitCode = 79;

void registerSystemRoutes(HttpServer& server, AppSettings& appSettings, AuthManager& authManager,
                          InternetAccessManager& internetAccess, ComputerManager& computerManager,
                          std::function<void()> onHostKeyRotated,
                          std::function<void(bool)> onInternetAccessToggled,
                          std::function<QJsonObject()> rendezvousStatus)
{
    // API route: get Internet Access status
    // rendezvousStatus captured BY VALUE, like the callbacks below: the
    // parameter dies when this function returns, the route lambda does not.
    server.router()->get("/api/internet/status", [&, rendezvousStatus](const HttpRequest& req) {
        QJsonObject obj = internetAccess.statusJson();
        if (rendezvousStatus) obj[QStringLiteral("rendezvous")] = rendezvousStatus();
        // The admin UI runs on localhost and needs the full payload. Remote
        // sessions must not learn the internal network topology / file layout.
        //
        // The rendezvous block goes with them, for the reason `unique_id`
        // already did: it carries this instance's permanent identifier, and an
        // identifier that never changes is a tracking handle. It names the
        // machine rather than granting anything — losing it costs nobody access,
        // and a remote admin who needs the address has it in their own address
        // bar. Redaction stays keyed on isLocal so there is one rule here, not
        // two.
        if (!req.isLocal) {
            for (const char* key : {"local_ip", "local_ips", "public_ip", "unique_id", "cert_pem",
                                    "cert_key", "last_error", "rendezvous"})
                obj.remove(QLatin1String(key));
        }
        return HttpResponse::json(obj);
    });

    // API route: is there a UPnP-IGD router willing to map our ports?
    //
    // `upnp_available` in /api/internet/status only becomes meaningful once
    // Internet Access has run a discovery, so a LAN-only install always reports
    // false there. This endpoint answers the question on demand — it is what
    // tells a headless operator whether opening the server to the internet will
    // configure itself or needs a manual port forward on the router.
    //
    // Blocks for up to the M-SEARCH timeout (~2 s) on the main thread. That is
    // why it is localhost-only and never polled: it is an explicit operator
    // action (`moonlightweb --status`, the admin page), not a background check.
    server.router()->get("/api/internet/upnp-probe", [&](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        UPNPClient* upnp = internetAccess.upnpClient();
        // Re-using a live IGD avoids re-running discovery under an active
        // Internet Access session (and avoids disturbing its mappings).
        const bool available = upnp->isAvailable() || upnp->discover(2000);

        QJsonObject obj;
        obj["available"] = available;
        if (available) {
            obj["gateway"] = upnp->gatewayAddress().toString();
            obj["lan_ip"] = QString::fromStdString(upnp->lanAddress());
            const std::string ext = upnp->getExternalIPAddress();
            if (!ext.empty()) obj["external_ip"] = QString::fromStdString(ext);
        }
        return HttpResponse::json(obj);
    });

    // API route: enable/configure Internet Access
    // onInternetAccessToggled captured BY VALUE: the parameter dies when this
    // registration function returns, but the route lambdas live on — the same
    // reason onHostKeyRotated is captured that way below.
    server.router()->post("/api/internet/enable", [&, onInternetAccessToggled](
                                                      const HttpRequest& req) {
        // Only localhost can modify internet access settings
        if (!req.isLocal)
            return HttpResponse::error(
                403, "Internet access settings can only be modified from localhost");

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();

        // unique_id is immutable once assigned: it keys this instance's subdomain,
        // its DNS ownership token, and its certificate. Only accept it when unset.
        if (body.contains("unique_id") && appSettings.uniqueId().isEmpty()) {
            const QString requestedUid = body["unique_id"].toString().trimmed().toLower();
            // Reject labels the PowerDNS stack owns (www, api, stats, ns1/ns2, ...):
            // using one would hijack the DNS server's own records for this domain.
            if (InternetAccessManager::isReservedSubdomain(requestedUid))
                return HttpResponse::error(
                    400, "This subdomain is reserved by the DNS server — choose another unique_id");
            appSettings.setUniqueId(requestedUid);
        }
        // pdns_token is no longer stored in settings; set MW_PDNS_TOKEN env var instead.
        if (body.contains("auto_ip_detection"))
            appSettings.setAutoIpDetection(body["auto_ip_detection"].toBool());
        if (body.contains("public_ip")) appSettings.setPublicIp(body["public_ip"].toString());

        if (body.contains("internet_access_enabled"))
            appSettings.setInternetAccessEnabled(body["internet_access_enabled"].toBool());
        if (body.contains("upnp_enabled"))
            appSettings.setUpnpEnabled(body["upnp_enabled"].toBool());

        // Legal traceability: record the exact agreement text the user read when
        // they ticked the checkbox. The mechanism is decided here, not by the
        // client: a legacy instance still consents to the DNS registration it
        // keeps running, everyone else to the rendezvous-era behaviour (UPnP
        // during sessions, peer-visible IP, no DNS record, no certificate).
        if (body.value("internet_access_enabled").toBool(false) &&
            body.contains("consent_message")) {
            const bool legacy = !appSettings.registeredUid().isEmpty();
            appSettings.setInternetConsent(
                body["consent_message"].toString(), QStringLiteral("admin"),
                legacy ? QStringLiteral("dns") : QStringLiteral("rendezvous"));
        }

        bool enabled =
            body.value("internet_access_enabled").toBool(appSettings.internetAccessEnabled());

        if (enabled) {
            qInfo() << "[main] POST /api/internet/enable — calling internetAccess.start()...";
            internetAccess.start();
            if (onInternetAccessToggled) onInternetAccessToggled(true);
            QJsonObject obj = internetAccess.statusJson();
            qInfo() << "[main] internetAccess.start() completed — active:"
                    << internetAccess.isActive()
                    << "lastError:" << obj.value("last_error").toString();
            obj["status"] = "enabled";
            return HttpResponse::json(obj);
        } else {
            internetAccess.stop();
            if (onInternetAccessToggled) onInternetAccessToggled(false);
            QJsonObject obj;
            obj["status"] = "disabled";
            obj["internet_access_enabled"] = false;
            return HttpResponse::json(obj);
        }
    });

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
    // onHostKeyRotated captured by value: the parameter dies when this
    // registration function returns, but the route lambda lives on.
    server.router()->post("/api/auth/host-key", [&, onHostKeyRotated](const HttpRequest& req) {
        const QString key = QJsonDocument::fromJson(req.body).object().value("key").toString();
        const QString addr = AuthManager::cleanClientAddress(req.clientAddress);
        const bool plausiblyHost =
            HttpServer::isLocalRequest(addr) ||
            AuthManager::isPrivateIP(addr) == QLatin1String("Local") ||
            (!internetAccess.publicIp().isEmpty() && addr == internetAccess.publicIp());
        // Compare hashes: constant-time, no length leak.
        const QByteArray given = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256);
        const QByteArray expected =
            QCryptographicHash::hash(appSettings.localKey().toUtf8(), QCryptographicHash::Sha256);
        if (!plausiblyHost || key.isEmpty() || given != expected) {
            Logger::warning(QStringLiteral("[Auth] Host key redemption refused for %1").arg(addr));
            return HttpResponse::error(403, "Invalid host key");
        }

        QString token =
            authManager.createSession(req.clientAddress,
                                      req.viaTunnel ? QStringLiteral("Host machine (remote link)")
                                                    : QStringLiteral("Host machine"),
                                      true, false, req.viaTunnel);
        Logger::info(QStringLiteral("[Auth] Host session created for %1 (via host key)").arg(addr));

        // Single-use: burn the redeemed key and rewrite the entry points that
        // embed it, so a leaked/history URL cannot be replayed.
        appSettings.rotateLocalKey();
        if (onHostKeyRotated) onHostKeyRotated();

        QJsonObject obj;
        obj["status"] = "ok";
        obj["is_localhost"] = true;
        HttpResponse resp = HttpResponse::json(obj);
        resp.headers["Set-Cookie"] =
            QString("mw_session=%1; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=7776000")
                .arg(token);
        return resp;
    });

    // ── First-run setup wizard (localhost only) ──────────────────────────────
    // macOS/Linux ship without a native installer, so the app hosts the wizard
    // the Windows Inno Setup installer provides: authorize Internet Access,
    // install + pair the local Sunshine. Endpoints are localhost-only.

    // GET /api/setup/status — what the wizard needs to render its steps.
    // rendezvousStatus captured BY VALUE, for the reason given above.
    server.router()->get("/api/setup/status", [&, rendezvousStatus](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        QJsonObject obj;
#if defined(Q_OS_WIN)
        // Windows provisioning is owned by the Inno Setup installer — never show
        // the in-app wizard there.
        obj["setup_completed"] = true;
        obj["os"] = "Windows";
#elif defined(Q_OS_MACOS)
        obj["setup_completed"] = appSettings.setupCompleted();
        obj["os"] = "macOS";
#else
        obj["setup_completed"] = appSettings.setupCompleted();
        obj["os"] = "Linux";
#endif
        // No display server (server, container, systemd unit): the wizard hides
        // its Sunshine step entirely — nothing on this machine can capture a
        // screen or encode on a GPU. The install script reads the same flag.
        obj["headless"] = !mw::hasDesktopSession();

        SunshineInstaller::DetectResult sun = SunshineInstaller::detect();
        QJsonObject sunObj;
        sunObj["installed"] = sun.installed;
        sunObj["can_auto_install"] = SunshineInstaller::canAutoInstall();
        sunObj["running"] = sun.installed && SunshineInstaller::isRunning();
        // Paired = some known host is paired AND lives on this machine. The
        // address matching this used to do by hand is exactly what
        // NvComputer::isLocalMachine() does (loopback, or one of our own
        // interface addresses — mDNS may register the local Sunshine under its
        // LAN IP rather than 127.0.0.1), and it is surfaced as isLocalHost.
        bool paired = false;
        const QJsonArray hosts = computerManager.getHostsJson();
        for (const QJsonValue& v : hosts) {
            const QJsonObject h = v.toObject();
            if (h.value("pairState").toString() != QLatin1String("paired")) continue;
            if (h.value("isLocalHost").toBool()) {
                paired = true;
                break;
            }
        }
        sunObj["paired"] = paired;
        obj["sunshine"] = sunObj;

        // Keep-the-display-awake offer. Only shown when this desktop actually
        // exposes the setting, and rendered as "already done" once it is set —
        // the wizard must never re-apply what the user already has (or claim it
        // can fix a desktop it has no knobs for). See DisplaySleep.h.
        QJsonObject disp;
        disp["supported"] = DisplaySleep::isSupported();
        disp["kept_awake"] = DisplaySleep::isDisplayKeptAwake();
        obj["display_sleep"] = disp;

        obj["autostart_installed"] = Autostart::isLoginItemInstalled();
        // The wizard is served over http://localhost (no cert warning); its
        // "Open MoonlightWeb" button switches to the HTTPS origin for streaming.
        obj["https_port"] = static_cast<int>(server.activeHttpsPort());

        QJsonObject inet;
        inet["enabled"] = appSettings.internetAccessEnabled();
        inet["active"] = internetAccess.isActive();
        inet["domain"] = internetAccess.domain();
        inet["phase"] = internetAccess.statusJson().value("phase");
        // The address this machine is actually reached at. `domain` is empty on
        // every fresh install — the wizard had nothing else to show, so it
        // congratulated the user on an Internet link and named no address at
        // all. No redaction concern: this route is localhost-only.
        if (rendezvousStatus) inet["rendezvous"] = rendezvousStatus();
        obj["internet"] = inet;

        // Live checklist written by /api/setup/apply (reuses the installer's file).
        QFile f(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                "/provisioning.status.json");
        if (f.open(QIODevice::ReadOnly)) {
            obj["steps"] = QJsonDocument::fromJson(f.readAll()).object().value("steps").toObject();
            f.close();
        }
        return HttpResponse::json(obj);
    });

    // POST /api/setup/sunshine-check — do these credentials open the Sunshine
    // already installed on this machine? The wizard asks before moving on, so a
    // user who does not know them is stopped on the page (where they can still
    // skip the Sunshine step) instead of discovering a failed pairing at the end.
    server.router()->post("/api/setup/sunshine-check", [&](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString user = body.value("username").toString();
        const QString pass = body.value("password").toString();
        QJsonObject result;
        if (user.isEmpty() || pass.isEmpty()) {
            result["ok"] = false;
            result["reason"] = "missing";
            return HttpResponse::json(result);
        }

        // Sunshine's web-UI/REST port is its GameStream base port + 1 (47990 by
        // default) — the same derivation Provisioning::pairSunshine makes.
        const int port = body.value("port").toInt(47990);
        SunshineRestClient rest;
        const SunshineRestClient::CredentialCheck check =
            rest.checkCredentials(user, pass, static_cast<quint16>(port));

        result["ok"] = check.outcome == SunshineRestClient::CredentialCheck::Accepted;
        if (check.outcome == SunshineRestClient::CredentialCheck::Rejected)
            result["reason"] = "unauthorized";
        else if (check.outcome == SunshineRestClient::CredentialCheck::Unreachable) {
            result["reason"] = "unreachable";
            result["error"] = check.error;
        }
        return HttpResponse::json(result);
    });

    // POST /api/setup/apply — run the wizard actions, then answer; the frontend
    // polls /api/setup/status meanwhile to animate the checklist.
    //
    // Async, and deferred to a later event-loop turn, for one reason: the work
    // below blocks for as long as the local Sunshine takes to pair (up to 65 s)
    // and drives a *nested* event loop while it waits. Run straight from the
    // request path, that nested loop let the server's own 30 s async timeout
    // fire underneath the handler — the 504 went out, the client hung up, and
    // the socket was destroyed while the frames owning it were still on the
    // stack. That is a segfault, and it is not hypothetical: it takes down the
    // backend on every host whose pairing does not complete (a Sunshine that
    // never accepts the PIN is enough). Answering from a fresh turn leaves
    // nothing of the request on the stack, and a response that arrives after
    // the timeout is discarded by HttpServer's pending-socket check, which is
    // already hardened for exactly that.
    server.router()->postAsync("/api/setup/apply", [&, onInternetAccessToggled,
                                                    rendezvousStatus](const HttpRequest& req,
                                                                      ResponseCallback respond) {
        if (!req.isLocal) {
            respond(HttpResponse::error(403, "Only available from localhost"));
            return;
        }
        // Copied out of the request: nothing of `req` may be read from the
        // deferred body — it is gone by the time that runs.
        const QJsonObject requestBody = QJsonDocument::fromJson(req.body).object();
        QTimer::singleShot(
            0, &server,
            [&, requestBody, onInternetAccessToggled, rendezvousStatus,
             respond = std::move(respond)]() {
                const QJsonObject& body = requestBody;
                const bool internetAuth = body.value("internet_access_authorized").toBool(false);
                const bool autostart = body.value("autostart").toBool(false);
                const bool keepAwake = body.value("keep_display_awake").toBool(false);
                const QJsonObject sun = body.value("sunshine").toObject();
                const bool wantInstall = sun.value("install").toBool(false);
                const QString user = sun.value("username").toString();
                const QString pass = sun.value("password").toString();
                const bool haveCreds = !user.isEmpty() && !pass.isEmpty();

                SunshineInstaller::DetectResult det = SunshineInstaller::detect();

                // Seed the checklist so the frontend renders the full task list up front.
                Provisioning::setStepStatus("install", (wantInstall && !det.installed) ? "running"
                                                                                       : "skipped");
                Provisioning::setStepStatus("pairing", haveCreds ? "pending" : "skipped");
                Provisioning::setStepStatus("arecord", internetAuth ? "pending" : "skipped");

                QJsonObject result;

                // 1) Install Sunshine (macOS DMG / Linux .deb) when requested and not
                //    already present.
                if (wantInstall && !det.installed) {
                    const QString err = SunshineInstaller::install(user, pass);
                    if (err.isEmpty()) {
                        Provisioning::setStepStatus("install", "done");
                        det = SunshineInstaller::detect();
                        // Start Sunshine: on macOS this surfaces its Screen-Recording /
                        // Accessibility permission prompts (cannot be granted for it);
                        // everywhere it must be serving before the pairing below. Give
                        // the fresh process a moment to open its GameStream port.
                        if (SunshineInstaller::launch()) QThread::sleep(3);
                    } else {
                        Provisioning::setStepStatus("install", "failed");
                        result["sunshine_error"] = err;
                    }
                } else if (det.installed && haveCreds) {
                    // Already installed: (re)apply the provided credentials so the REST
                    // PIN push during pairing authenticates.
                    SunshineInstaller::setCredentials(user, pass);
                }

                // 2) Pair the local Sunshine over GameStream + its REST /api/pin.
                if (haveCreds && det.installed) {
                    Provisioning::setStepStatus("pairing", "running");
                    const bool ok = Provisioning::pairSunshine(computerManager, user, pass);
                    Provisioning::setStepStatus("pairing", ok ? "done" : "failed");
                    result["paired"] = ok;
                }

                // 3) Internet Access — flip the flag and bring the tunnel up.
                if (internetAuth) {
                    Provisioning::setStepStatus("arecord", "running");
                    // Legal traceability: the wizard sends the exact agreement text shown.
                    // The wizard only runs on a fresh install, which never registers a
                    // subdomain — the consent is for the rendezvous-era behaviour.
                    appSettings.setInternetConsent(body.value("consent_message").toString(),
                                                   QStringLiteral("setup"),
                                                   QStringLiteral("rendezvous"));
                    appSettings.setInternetAccessEnabled(true);
                    internetAccess.start();
                    // The rendezvous line follows the same switch, and nothing else
                    // throws it: /api/internet/enable calls this, the wizard did not, so
                    // a machine set up here stayed unreachable from the internet until
                    // the next restart brought the line up at boot.
                    if (onInternetAccessToggled) onInternetAccessToggled(true);
                    const bool active = internetAccess.isActive();
                    // start() returns once the A record resolves, but the ACME order for
                    // that domain is still in flight — and it cannot progress while this
                    // handler holds the event loop. So leave the step "running" and let
                    // the certificateChanged/error handlers in main.cpp close it once we
                    // return; the wizard keeps polling until then. Closing it here would
                    // send the user to a domain their browser still rejects, because it
                    // is served with the self-signed fallback until the order lands.
                    const bool certPending = active && internetAccess.certificateIssuing();
                    if (!certPending)
                        Provisioning::setStepStatus("arecord", active ? "done" : "failed");
                    result["internet_active"] = active;
                    result["certificate_pending"] = certPending;
                    result["domain"] = internetAccess.domain();
                    // Usually empty right here: the claim is a round-trip that cannot
                    // run while this handler holds the event loop. The wizard picks the
                    // address up from /api/setup/status, which it is already polling.
                    if (rendezvousStatus) result["rendezvous"] = rendezvousStatus();
                }

                // Mark setup done even if an optional step failed: the user retries from
                // the admin page, and the wizard must not reappear on every launch.
                appSettings.setSetupCompleted(true);

                // Start at login (macOS LaunchAgent / Linux XDG autostart — GUI session,
                // keeps the tray icon). Mirrors the Windows installer's logon-task
                // checkbox: only when the wizard's checkbox was ticked. Best-effort.
                result["autostart"] = autostart ? Autostart::installLoginItem() : false;

                // Stop this desktop from blanking its screen, so Sunshine always has
                // something to capture (a blanked output makes every /launch answer 503).
                // Opt-in: it changes the user's own power settings, so only on an explicit
                // tick. The reason travels back for the "done" screen to show.
                if (keepAwake) {
                    const QString err = DisplaySleep::keepDisplayAwake();
                    result["display_kept_awake"] = err.isEmpty();
                    if (!err.isEmpty()) result["display_sleep_error"] = err;
                }

                result["status"] = "completed";
                respond(HttpResponse::json(result));
            });
    });

    // POST /api/system/open-screen-recording — open macOS' Screen Recording
    // privacy pane so the user can grant Sunshine capture permission. Sunshine
    // can't stream without it (its log: "No screen capture permission!"), and
    // macOS forbids granting it programmatically (TCC). Localhost-only + macOS-
    // only: it acts on the host, and only someone physically at that Mac can use
    // the pane it opens.
    server.router()->post("/api/system/open-screen-recording", [&](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");
#if defined(Q_OS_MACOS)
        QProcess::startDetached(
            QStringLiteral("/usr/bin/open"),
            {QStringLiteral(
                "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")});
        QJsonObject obj;
        obj["status"] = "opened";
        return HttpResponse::json(obj);
#else
        return HttpResponse::error(400, "Only available on macOS");
#endif
    });

    // POST /api/system/stop-sunshine — stop the local Sunshine server. Localhost-
    // only: it's a host-machine service action (and would kill any in-progress
    // stream), so only someone at/administering the host triggers it.
    server.router()->post("/api/system/stop-sunshine", [&](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");
        const bool ok = SunshineInstaller::stop();
        QJsonObject obj;
        obj["status"] = ok ? "stopped" : "not_running";
        return HttpResponse::json(obj);
    });

    // POST /api/system/start-sunshine — start the local Sunshine server. Localhost-
    // only (host-machine service action). Used by the admin page when Sunshine is
    // installed but not currently running.
    server.router()->post("/api/system/start-sunshine", [&](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");
        const bool ok = SunshineInstaller::launch();
        QJsonObject obj;
        obj["status"] = ok ? "started" : "failed";
        return HttpResponse::json(obj);
    });

    // POST /api/system/restart — restart this MoonlightWeb process. Localhost-only.
    //
    // This is what lets the desktop tray restart a server that runs where the tray
    // cannot: the Windows service in session 0. The tray client (another session,
    // unelevated) drives it over loopback — no UAC, no SCM rights needed — because
    // the restart is the server exiting on its own and its supervisor bringing it
    // back. install-service.bat maps `AppExit Default Restart` / `AppExit 0 Exit`,
    // so any NON-zero exit means "come back". Off a supervisor (a plain desktop
    // run) nothing would respawn us, so relaunch a fresh copy ourselves first.
    server.router()->post("/api/system/restart", [](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        const bool supervised = !qEnvironmentVariableIsEmpty("MW_SERVICE");
        if (!supervised) {
            QString appPath = QCoreApplication::applicationFilePath();
            QStringList args = QCoreApplication::arguments();
            if (!args.isEmpty()) args.removeFirst(); // argv[0]; startDetached re-adds it
            QProcess::startDetached(appPath, args);
        }
        Logger::info(QStringLiteral("[System] Restart requested over loopback (%1)")
                         .arg(supervised ? QStringLiteral("supervisor respawn")
                                         : QStringLiteral("self relaunch")));

        // Delay the exit so the HTTP response flushes over TLS before the socket
        // dies under it — otherwise the caller sees a transport error, not a 200.
        QTimer::singleShot(300, qApp, [supervised]() {
            QCoreApplication::exit(supervised ? kServiceRestartExitCode : 0);
        });

        QJsonObject obj;
        obj["status"] = "restarting";
        return HttpResponse::json(obj);
    });

    // POST /api/system/quit — stop this MoonlightWeb process. Localhost-only.
    //
    // A clean exit(0): under the service supervisor `AppExit 0 Exit` is a real
    // stop, so the server stays down (starting it again is `net start` / the
    // Services panel). Off a supervisor it simply ends. Drives the tray client's
    // "Quit Server" entry — the counterpart to Restart above.
    server.router()->post("/api/system/quit", [](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");
        Logger::info(QStringLiteral("[System] Quit requested over loopback"));
        QTimer::singleShot(300, qApp, []() { QCoreApplication::exit(0); });
        QJsonObject obj;
        obj["status"] = "quitting";
        return HttpResponse::json(obj);
    });

    // API route: disable Internet Access
    server.router()->post(
        "/api/internet/disable", [&, onInternetAccessToggled](const HttpRequest& req) {
            // Only localhost can modify internet access settings
            if (!req.isLocal)
                return HttpResponse::error(
                    403, "Internet access settings can only be modified from localhost");

            internetAccess.stop();
            appSettings.setInternetAccessEnabled(false);
            if (onInternetAccessToggled) onInternetAccessToggled(false);

            QJsonObject obj;
            obj["status"] = "disabled";
            return HttpResponse::json(obj);
        });

    // API route: force refresh (re-check IP, DNS, certificate)
    server.router()->post("/api/internet/refresh", [&](const HttpRequest& req) {
        // Localhost only: refresh re-runs DNS/ACME and must not be triggerable by
        // a remote session (avoids abusing the ACME provider's rate limits).
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        internetAccess.forceRefresh();
        return HttpResponse::json(internetAccess.statusJson());
    });

    // API route: renew TLS certificate
    server.router()->post("/api/internet/renew-cert", [&](const HttpRequest& req) {
        // Localhost only: certificate issuance is subject to strict ACME rate
        // limits — never let a remote session drive it.
        if (!req.isLocal) return HttpResponse::error(403, "Only available from localhost");

        internetAccess.renewCertificate();
        QJsonObject obj;
        obj["status"] = "renewing";
        return HttpResponse::json(obj);
    });

    // — Admin settings (localhost only, server config) —

    server.router()->get(
        "/api/admin/settings", [&server, &appSettings, &authManager](const HttpRequest& req) {
            QJsonObject obj;
            obj["http_port"] = static_cast<int>(server.httpPort());
            // Report the persisted (canonical) HTTPS port, not the
            // primary listener's live port: after a port-parity
            // rebind the host adopts the router-side port (e.g.
            // 44729) as its single https_port — served locally by
            // the secondary listener on every interface — so the
            // admin UI's Local Access URL and port input must show
            // that port, matching the public-domain URL. In the
            // normal case main.cpp keeps this in sync with the
            // active listener at startup, so they are identical.
            obj["https_port"] = static_cast<int>(appSettings.httpsPort(server.activeHttpsPort()));
            obj["cert_auth_enabled"] = authManager.certAuthEnabled();
            // Host machine only: the current host key, so the
            // admin page can carry its session over to the
            // public-domain URL after Internet activation. Not
            // isLocal — a LAN admin that unlocked with the password
            // has the same rights but is NOT the host, and handing
            // it the key would rotate the one the host's own
            // shortcut still embeds.
            if (req.isHostMachine) obj["local_key"] = appSettings.localKey();
            return HttpResponse::json(obj);
        });

    server.router()->post("/api/admin/settings", [&server, &appSettings, &authManager,
                                                  &internetAccess](const HttpRequest& req) {
        // Only localhost can modify server admin settings
        if (!req.isLocal)
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

                qInfo() << "[admin] Scheduled deferred HTTPS port change from" << oldPort << "to"
                        << newPort;

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

        if (!hadChange) return HttpResponse::error(400, "No supported settings provided");

        return HttpResponse::json(obj);
    });

    // — Streaming settings —

    server.router()->get("/api/settings/streaming", [&appSettings](const HttpRequest&) {
        QJsonObject obj;

        // Normalise "auto" → "hevc" (the old "auto" default is replaced by explicit HEVC).
        // Existing settings.json entries with "video_codec":"auto" are migrated on read.
        VideoCodec codec = appSettings.videoCodec();
        QString codecStr = AppSettings::videoCodecToString(codec);
        if (codecStr == "auto") codecStr = "hevc";
        obj["video_codec"] = codecStr;

        obj["gaming_mode"] = appSettings.gamingMode();
        obj["show_performance_stats"] = appSettings.showPerformanceStats();
        obj["upnp_enabled"] = appSettings.upnpEnabled();
        obj["stun_server"] = appSettings.stunServer();
        obj["internet_access_enabled"] = appSettings.internetAccessEnabled();
        QString transportMode = appSettings.transportMode();
        obj["transport_mode"] = transportMode;
        obj["media_track_only_h264"] =
            (transportMode == "webrtc-media-udp" || transportMode == "webrtc-media-tcp");
        obj["auto_ip_detection"] = appSettings.autoIpDetection();
        obj["stream_bitrate"] = appSettings.streamBitrate();
        obj["stream_height"] = appSettings.streamHeight();
        obj["stream_aspect"] = appSettings.streamAspect();
        obj["stream_fps"] = appSettings.streamFps();
        obj["hdr_enabled"] = appSettings.hdrEnabled();
        obj["mute_host_audio"] = appSettings.muteHostAudio();
        obj["chroma_444_enabled"] = appSettings.chroma444Enabled();
        obj["video_enhancement"] = appSettings.videoEnhancement();
        obj["video_enhancement_algo"] = appSettings.videoEnhancementAlgo();
        // Audio time-stretch (WSOLA) — file-only setting, default false.
        obj["audio_time_stretch"] = appSettings.audioTimeStretch();
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
        if (!req.isLocal)
            return HttpResponse::error(403,
                                       "Streaming settings can only be modified from localhost");

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

        if (body.contains("chroma_444_enabled")) {
            bool enabled = body["chroma_444_enabled"].toBool();
            appSettings.setChroma444Enabled(enabled);
            obj["chroma_444_enabled"] = enabled;
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("mute_host_audio")) {
            bool enabled = body["mute_host_audio"].toBool();
            appSettings.setMuteHostAudio(enabled);
            obj["mute_host_audio"] = enabled;
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

        // Transport chain preference. Lived on POST /api/internet/enable for
        // historical reasons; it is a streaming setting with no relation to
        // internet reachability, so it is written here with its siblings.
        if (body.contains("transport_mode")) {
            appSettings.setTransportMode(body["transport_mode"].toString());
            obj["transport_mode"] = appSettings.transportMode();
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

        if (!hadChange) return HttpResponse::error(400, "No supported settings provided");

        return HttpResponse::json(obj);
    });
}
