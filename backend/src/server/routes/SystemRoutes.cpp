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
#include "backend/ComputerManager.h"
#include "backend/SunshineInstaller.h"
#include "Autostart.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

void registerSystemRoutes(HttpServer& server, AppSettings& appSettings, AuthManager& authManager,
                          InternetAccessManager& internetAccess, ComputerManager& computerManager)
{
    // API route: get Internet Access status
    server.router()->get("/api/internet/status", [&](const HttpRequest& req) {
        QJsonObject obj = internetAccess.statusJson();
        // The admin UI runs on localhost and needs the full payload. Remote
        // sessions must not learn the internal network topology / file layout.
        if (!HttpServer::isLocalRequest(req.clientAddress)) {
            for (const char* key :
                 {"local_ip", "public_ip", "unique_id", "cert_pem", "cert_key", "last_error"})
                obj.remove(QLatin1String(key));
        }
        return HttpResponse::json(obj);
    });

    // API route: enable/configure Internet Access
    server.router()->post("/api/internet/enable", [&](const HttpRequest& req) {
        // Only localhost can modify internet access settings
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(
                403, "Internet access settings can only be modified from localhost");

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
        if (body.contains("public_ip")) appSettings.setPublicIp(body["public_ip"].toString());

        if (body.contains("internet_access_enabled"))
            appSettings.setInternetAccessEnabled(body["internet_access_enabled"].toBool());
        if (body.contains("upnp_enabled"))
            appSettings.setUpnpEnabled(body["upnp_enabled"].toBool());

        bool enabled =
            body.value("internet_access_enabled").toBool(appSettings.internetAccessEnabled());

        if (enabled) {
            qInfo() << "[main] POST /api/internet/enable — calling internetAccess.start()...";
            internetAccess.start();
            QJsonObject obj = internetAccess.statusJson();
            qInfo() << "[main] internetAccess.start() completed — active:"
                    << internetAccess.isActive()
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

    // ── First-run setup wizard (localhost only) ──────────────────────────────
    // macOS/Linux ship without a native installer, so the app hosts the wizard
    // the Windows Inno Setup installer provides: authorize Internet Access,
    // install + pair the local Sunshine. Endpoints are localhost-only.

    // GET /api/setup/status — what the wizard needs to render its steps.
    server.router()->get("/api/setup/status", [&](const HttpRequest& req) {
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

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
        SunshineInstaller::DetectResult sun = SunshineInstaller::detect();
        QJsonObject sunObj;
        sunObj["installed"] = sun.installed;
        sunObj["can_auto_install"] = SunshineInstaller::canAutoInstall();
        // Paired = some known host is paired AND lives on this machine (loopback
        // or one of our own interface addresses — mDNS may have registered the
        // local Sunshine under its LAN IP rather than 127.0.0.1).
        bool paired = false;
        const QList<QHostAddress> selfAddrs = QNetworkInterface::allAddresses();
        const QJsonArray hosts = computerManager.getHostsJson();
        for (const QJsonValue& v : hosts) {
            const QJsonObject h = v.toObject();
            if (h.value("pairState").toString() != QLatin1String("paired")) continue;
            for (const char* key : {"activeAddress", "localAddress", "manualAddress"}) {
                QString addr = h.value(QLatin1String(key)).toString();
                // Strip a ":port" suffix (single colon only — leave IPv6 alone).
                const int colon = addr.lastIndexOf(':');
                if (colon > 0 && addr.indexOf(':') == colon) addr.truncate(colon);
                if (addr.isEmpty()) continue;
                const QHostAddress ip(addr);
                if (addr == QLatin1String("localhost") || ip.isLoopback() ||
                    selfAddrs.contains(ip)) {
                    paired = true;
                    break;
                }
            }
            if (paired) break;
        }
        sunObj["paired"] = paired;
        obj["sunshine"] = sunObj;

        obj["autostart_installed"] = Autostart::isLoginItemInstalled();

        QJsonObject inet;
        inet["enabled"] = appSettings.internetAccessEnabled();
        inet["active"] = internetAccess.isActive();
        inet["domain"] = internetAccess.domain();
        inet["phase"] = internetAccess.statusJson().value("phase");
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

    // POST /api/setup/apply — run the wizard actions synchronously; the frontend
    // polls /api/setup/status meanwhile to animate the checklist.
    server.router()->post("/api/setup/apply", [&](const HttpRequest& req) {
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

        QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const bool internetAuth = body.value("internet_access_authorized").toBool(false);
        const bool autostart = body.value("autostart").toBool(false);
        const QJsonObject sun = body.value("sunshine").toObject();
        const bool wantInstall = sun.value("install").toBool(false);
        const QString user = sun.value("username").toString();
        const QString pass = sun.value("password").toString();
        const bool haveCreds = !user.isEmpty() && !pass.isEmpty();

        SunshineInstaller::DetectResult det = SunshineInstaller::detect();

        // Seed the checklist so the frontend renders the full task list up front.
        Provisioning::setStepStatus("install",
                                    (wantInstall && !det.installed) ? "running" : "skipped");
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
            appSettings.setInternetAccessEnabled(true);
            internetAccess.start();
            const bool active = internetAccess.isActive();
            Provisioning::setStepStatus("arecord", active ? "done" : "failed");
            result["internet_active"] = active;
            result["domain"] = internetAccess.domain();
        }

        // Mark setup done even if an optional step failed: the user retries from
        // the admin page, and the wizard must not reappear on every launch.
        appSettings.setSetupCompleted(true);

        // Start at login (macOS LaunchAgent / Linux XDG autostart — GUI session,
        // keeps the tray icon). Mirrors the Windows installer's logon-task
        // checkbox: only when the wizard's checkbox was ticked. Best-effort.
        result["autostart"] = autostart ? Autostart::installLoginItem() : false;

        result["status"] = "completed";
        return HttpResponse::json(result);
    });

    // API route: disable Internet Access
    server.router()->post("/api/internet/disable", [&](const HttpRequest& req) {
        // Only localhost can modify internet access settings
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(
                403, "Internet access settings can only be modified from localhost");

        internetAccess.stop();
        appSettings.setInternetAccessEnabled(false);

        QJsonObject obj;
        obj["status"] = "disabled";
        return HttpResponse::json(obj);
    });

    // API route: force refresh (re-check IP, DNS, certificate)
    server.router()->post("/api/internet/refresh", [&](const HttpRequest& req) {
        // Localhost only: refresh re-runs DNS/ACME and must not be triggerable by
        // a remote session (avoids abusing the ACME provider's rate limits).
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

        internetAccess.forceRefresh();
        return HttpResponse::json(internetAccess.statusJson());
    });

    // API route: renew TLS certificate
    server.router()->post("/api/internet/renew-cert", [&](const HttpRequest& req) {
        // Localhost only: certificate issuance is subject to strict ACME rate
        // limits — never let a remote session drive it.
        if (!HttpServer::isLocalRequest(req.clientAddress))
            return HttpResponse::error(403, "Only available from localhost");

        internetAccess.renewCertificate();
        QJsonObject obj;
        obj["status"] = "renewing";
        return HttpResponse::json(obj);
    });

    // — Admin settings (localhost only, server config) —

    server.router()->get("/api/admin/settings",
                         [&server, &appSettings, &authManager](const HttpRequest&) {
                             QJsonObject obj;
                             obj["http_port"] = static_cast<int>(server.httpPort());
                             obj["https_port"] = static_cast<int>(server.activeHttpsPort());
                             obj["cert_auth_enabled"] = authManager.certAuthEnabled();
                             return HttpResponse::json(obj);
                         });

    server.router()->post("/api/admin/settings", [&server, &appSettings, &authManager,
                                                  &internetAccess](const HttpRequest& req) {
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
        obj["transport"] = appSettings.transport();
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
        if (!HttpServer::isLocalRequest(req.clientAddress))
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
