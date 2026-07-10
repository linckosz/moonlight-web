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

#include <QApplication>
#include <QCommandLineParser>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
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
#include "server/Provisioning.h"
#include "server/HttpServer.h"
#include "server/ControlChannel.h"
#include "server/RestRouter.h"
#include "server/AuthManager.h"
#include "server/routes/AuthRoutes.h"
#include "server/routes/HostRoutes.h"
#include "server/routes/SystemRoutes.h"
#include "common/Logger.h"
#include "common/CrashHandler.h"
#include "backend/ComputerManager.h"
#include "backend/IdentityManager.h"
#include "backend/SunshineInstaller.h"
#include "Autostart.h"
#include "streaming/Session.h"
#include "streaming/MoonlightShim.h"
#include "Limelight.h" // SCM_* codec-support masks
#include "streaming/DataChannelRelay.h"
#include "streaming/MediaTrackRelay.h"
#include "streaming/StreamRelay.h"
#include "streaming/TransportPriorities.h"
#include "network/InternetAccessManager.h"
#include "network/GeoIpService.h"
#include "network/UpdateChecker.h"
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
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    int count = 0;
    while (!f.atEnd()) {
        QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        int eq = line.indexOf('=');
        if (eq <= 0) continue;

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
                        if (!peek.isEmpty() && !peek.startsWith('#')) break;
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

// Apply compile-time embedded defaults (baked in by CI via CMake from repo
// secrets) for any Internet-Access env var the runtime environment / .env did
// not already provide. Lets the distributed build carry its DNS/ACME config
// without shipping a .env next to the executable. Runtime env and .env win;
// embedded values are only a fallback.
static void applyEmbeddedEnvDefaults()
{
    auto setIfEmpty = [](const char* key, const char* value) {
        if (qEnvironmentVariableIsEmpty(key)) qputenv(key, value);
    };
    (void)setIfEmpty; // may be unused when nothing was embedded at build time
#ifdef MW_DOMAIN
    setIfEmpty("MW_DOMAIN", MW_DOMAIN);
#endif
#ifdef MW_PDNS_URL
    setIfEmpty("MW_PDNS_URL", MW_PDNS_URL);
#endif
#ifdef MW_PDNS_TOKEN
    setIfEmpty("MW_PDNS_TOKEN", MW_PDNS_TOKEN);
#endif
#ifdef MW_ZEROSSL_EAB_KID
    setIfEmpty("MW_ZEROSSL_EAB_KID", MW_ZEROSSL_EAB_KID);
#endif
#ifdef MW_ZEROSSL_EAB_HMAC
    setIfEmpty("MW_ZEROSSL_EAB_HMAC", MW_ZEROSSL_EAB_HMAC);
#endif
}

// Version string baked in by CMake (MW_VERSION cache var, overridden by the
// release tag in CI); fallback for builds that bypass CMake.
#ifndef MW_VERSION
#define MW_VERSION "0.1.2"
#endif

// Forward Qt's qDebug/qInfo/qWarning/qCritical (emitted across modules) into the
// Logger so the windowless release build still records them in the log file —
// there is no console to print to.
static void mwMessageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    switch (type) {
    case QtDebugMsg: Logger::debug(msg); break;
    case QtInfoMsg: Logger::info(msg); break;
    case QtWarningMsg: Logger::warning(msg); break;
    default: Logger::error(msg); break; // Critical / Fatal
    }
}

// Write/refresh the Desktop shortcut that opens the admin page. The installer
// cannot know the runtime HTTPS port or the assigned domain, so the server owns
// the shortcut: it self-heals on every startup (and when Internet Access becomes
// ready). Skipped under a service supervisor (session 0 has the wrong desktop).
static void writeAdminShortcut(const QString& url)
{
    // Expose the resolved URL to the installer (post-install "open admin page"
    // action) regardless of the shortcut below.
    Provisioning::setInfo(QStringLiteral("admin_url"), url);
    if (!qEnvironmentVariableIsEmpty("MW_SERVICE")) return;
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty()) return;
#if defined(Q_OS_LINUX)
    // No .url handler on Linux (it opens in a text editor). A Type=Link entry
    // renders a generic icon and, on GNOME, silently refuses to launch; a
    // Type=Application entry both shows the app icon (Icon=moonlightweb,
    // installed in the hicolor theme by the package) and launches reliably.
    // GNOME's desktop icons require the entry to be executable AND carry the gio
    // "trusted" metadata to launch without an "untrusted" prompt.
    //
    // Exec launches the binary itself, NOT the URL: the windowless app then
    // starts (when not running) or, when already running, its single-instance
    // logic surfaces the admin page — either way the user lands on Admin. This
    // is why the URL argument is unused here (it is still published for the
    // installer via Provisioning::setInfo above).
    Q_UNUSED(url);
    const QString exe = QCoreApplication::applicationFilePath();
    // Legacy name used before 2026-07 — remove it so only the "MoonlightWeb"
    // entry below remains on the desktop.
    QFile::remove(desktop + "/MoonlightWeb Admin.desktop");
    const QString path = desktop + "/MoonlightWeb.desktop";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(("[Desktop Entry]\nVersion=1.0\nType=Application\nName=MoonlightWeb\n"
             "Exec=\"" +
             exe + "\"\nIcon=moonlightweb\nTerminal=false\n")
                .toUtf8());
    f.close();
    // Owner rwx + group/other r-x: launchable and (re)writable on the next port
    // change, but not world-writable.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                    QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                    QFileDevice::ExeOther);
    QProcess::startDetached(
        QStringLiteral("gio"),
        {QStringLiteral("set"), path, QStringLiteral("metadata::trusted"), QStringLiteral("true")});
#elif defined(Q_OS_WIN)
    // The Windows Desktop/Start-Menu shortcuts are .lnk files pointing at the
    // exe, created by the Inno Setup installer ([Icons]). Launching the exe both
    // starts the app (when down) and surfaces the admin page (when up), so there
    // is no runtime-port URL to self-heal here — only the published admin_url
    // above (used by the installer's post-install "open admin page" action).
    Q_UNUSED(url);
#else
    // macOS: the app is launched from the Dock/Applications; keep a plain .url
    // pointer on the Desktop as a convenience. Remove the legacy "MoonlightWeb
    // Admin.url" name used before 2026-07.
    QFile::remove(desktop + "/MoonlightWeb Admin.url");
    QFile f(desktop + "/MoonlightWeb.url");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(("[InternetShortcut]\r\nURL=" + url + "\r\n").toUtf8());
    f.close();
#endif
}

// True when a desktop session can show a browser/tray: never under a service
// supervisor, and on Linux only when a display server is reachable. Do NOT use
// QSystemTrayIcon::isSystemTrayAvailable() for this: GNOME has no system tray
// by default, which would wrongly report "headless" and skip the first-run
// setup wizard.
static bool hasGuiSession()
{
    if (!qEnvironmentVariableIsEmpty("MW_SERVICE")) return false;
#if defined(Q_OS_LINUX)
    return !qEnvironmentVariableIsEmpty("DISPLAY") ||
           !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
#else
    return true;
#endif
}

// Open a URL in the user's default browser. On Linux, QDesktopServices::openUrl
// routes through the XDG desktop portal, which under Ubuntu's snap-packaged
// Firefox (the distro default) can pop a blank window that never receives the
// URL — the bar stays empty. Invoking xdg-open directly (the same path a working
// .desktop launcher uses) delivers the URL reliably. Other platforms use Qt's
// opener. Note: a self-signed localhost cert still shows a one-time browser
// warning the user must accept — that is inherent to self-signed TLS.
static void openInBrowser(const QString& url)
{
#if defined(Q_OS_LINUX)
    if (QProcess::startDetached(QStringLiteral("xdg-open"), {url})) return;
    // xdg-open missing (minimal desktop): fall through to Qt's opener.
#endif
    QDesktopServices::openUrl(QUrl(url));
}

// Ask an already-running instance (reached over loopback HTTPS at @p base, e.g.
// "https://localhost:443") to surface the admin page: it redirects an existing
// browser tab to /admin via the control channel, or opens a fresh tab when no
// tab is connected. Returns true when the running instance handled the request
// (HTTP 200); false on any network/timeout error so the caller can fall back to
// opening the browser itself. The localhost cert is self-signed, so peer
// verification is disabled for this one loopback call.
static bool requestFocusAdmin(const QString& base)
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(base + QStringLiteral("/api/local/focus"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(ssl);

    QNetworkReply* reply = nam.post(req, QByteArrayLiteral("{}"));
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(3000);
    loop.exec();

    const bool ok = timer.isActive() && reply->error() == QNetworkReply::NoError &&
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    reply->deleteLater();
    return ok;
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("MoonlightWeb");
    QCoreApplication::setApplicationVersion(QStringLiteral(MW_VERSION));
    QCoreApplication::setOrganizationName("MoonlightWeb");

    // Dock (macOS) / taskbar icon fallback: without this the Dock shows an
    // empty icon when the bundle .icns is missing or unreadable.
    {
        QIcon appIcon = TrayManager::loadAppIcon();
        if (!appIcon.isNull()) app.setWindowIcon(appIcon);
    }

    // The Windows release build is windowless (no console): capture Qt messages
    // and default to a log file. --log overrides the path. The file lives in the
    // per-user data dir (next to settings.json/cert), NOT next to the exe: an
    // admin install under Program Files is not writable by the user session, so
    // a log there would silently fail to open. Platform paths:
    //   Windows: %AppData%\MoonlightWeb\MoonlightWeb\logs
    //   macOS:   ~/Library/Application Support/MoonlightWeb/MoonlightWeb/logs
    //   Linux:   ~/.local/share/MoonlightWeb/MoonlightWeb/logs
    qInstallMessageHandler(mwMessageHandler);
    {
        const QString logDir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
        QDir().mkpath(logDir);
        Logger::instance()->setLogFile(logDir + "/moonlightweb.log");
    }

    // Install the crash handler before anything can crash: on Windows it writes a
    // minidump (call stacks + modules) next to the log so a hard C++ crash leaves
    // a post-mortem the .pdb can symbolize. Free until an actual crash → safe in
    // production. Dumps land in the per-user data dir (writable under an admin
    // install), same rationale as the log path above.
    CrashHandler::install(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                          "/crashes");

    // Load .env file before anything reads environment variables, then fall back
    // to any values baked in at build time (CI secrets) for vars still unset.
    loadEnvFile();
    applyEmbeddedEnvDefaults();

    // Parse command line
    QCommandLineParser parser;
    parser.setApplicationDescription("MoonlightWeb streaming server");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOption("port", "HTTP server port", "port", "80");
    parser.addOption(portOption);

    QCommandLineOption logOption("log", "Log file path", "path");
    parser.addOption(logOption);

    QCommandLineOption wsPortOption("ws-port", "WebRTC signaling WebSocket port", "port", "48001");
    parser.addOption(wsPortOption);

    // Set by every automatic launcher (XDG autostart, LaunchAgent, Windows logon
    // task, installer post-install start): suppresses the browser auto-open a
    // manual launch performs.
    QCommandLineOption autostartOption("autostart",
                                       "Started automatically at login (don't open the browser)");
    parser.addOption(autostartOption);

    parser.process(app);

    // Configure logging
    if (parser.isSet(logOption)) Logger::instance()->setLogFile(parser.value(logOption));

    Logger::info("MoonlightWeb server starting...");
    Logger::info("Version: " + QCoreApplication::applicationVersion());

    // Force Qt's TLS backend to OpenSSL. On Windows Qt defaults to Schannel,
    // which cannot import the public ACME cert's PEM private key — handshakes on
    // the public domain fail with SEC_E_CERT_UNKNOWN (0x80090327) and the browser
    // ends up served the LAN self-signed cert (no public-domain SAN), yielding
    // ERR_CERT_COMMON_NAME_INVALID. The OpenSSL plugin + libssl/libcrypto DLLs are
    // shipped next to the exe (CMakeLists), so the PEM cert/key load reliably.
    // Must run before any QSslSocket use locks the active backend.
    if (QSslSocket::activeBackend() != QStringLiteral("openssl")) {
        if (QSslSocket::availableBackends().contains(QStringLiteral("openssl"))) {
            if (QSslSocket::setActiveBackend(QStringLiteral("openssl")))
                Logger::info("Qt TLS backend set to OpenSSL");
            else
                Logger::warning("Failed to set Qt TLS backend to OpenSSL — using " +
                                QSslSocket::activeBackend());
        } else {
            Logger::warning("OpenSSL TLS backend unavailable (plugin/DLLs missing) — "
                            "using " +
                            QSslSocket::activeBackend());
        }
    }

    // Read HTTP/HTTPS port preferences from persisted settings.
    // CLI --port overrides the persisted HTTP port when explicitly provided.
    AppSettings appSettings;
    appSettings.seedDocumentedDefaults(); // write documented file-only keys if absent
    quint16 httpPort = appSettings.httpPort(80);
    if (parser.isSet("port")) httpPort = parser.value("port").toUShort();

    // ── Single instance ──────────────────────────────────────────────────────
    // The app has no window: launching it again (Desktop shortcut / Apps /
    // Start-menu click) must not spawn a duplicate server on fallback ports.
    // Surface the running instance's admin page instead, then exit cleanly
    // (exit 0 = no supervisor relaunch).
    QLockFile instanceLock(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                           "/moonlightweb.lock");
    instanceLock.setStaleLockTime(0); // stale detection by PID liveness only
    if (!instanceLock.tryLock(100)) {
        Logger::info("Another instance is already running");
        if (hasGuiSession() && !parser.isSet(autostartOption)) {
            quint16 p = appSettings.httpsPort(443); // running instance persisted its port
            const QString base = p == 443 ? QStringLiteral("https://localhost")
                                          : QStringLiteral("https://localhost:%1").arg(p);
            // Prefer redirecting an already-open tab (no duplicate). Only when the
            // running instance can't be reached do we open the admin page here.
            Logger::info("Asking the running instance to surface the admin page: " + base);
            if (!requestFocusAdmin(base)) {
                Logger::info("Running instance unreachable — opening the admin page directly");
                openInBrowser(base + QStringLiteral("/admin"));
            }
        }
        return 0;
    }

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
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
                             OPENSSL_INIT_NO_ATEXIT,
                         nullptr);
        OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
        Logger::info("OpenSSL initialized");
    }

    // Read remaining persistent settings
    quint16 httpsPort = appSettings.httpsPort(443);
    VideoCodec preferredCodec = appSettings.videoCodec();
    bool upnpEnabled = appSettings.upnpEnabled();
    QString stunServer = appSettings.stunServer();
    Logger::info("[main] Settings: http_port=" + QString::number(httpPort) +
                 ", https_port=" + QString::number(httpsPort) +
                 ", video_codec=" + AppSettings::videoCodecToString(preferredCodec) +
                 ", upnp_enabled=" + (upnpEnabled ? "true" : "false") +
                 ", stun_server=" + stunServer);

    // ── AuthManager (PIN-based + certificate authentication) ────────────────
    AuthManager authManager(&appSettings);

    // Generate certificate token on first startup (persisted forever)
    if (authManager.certificateToken().isEmpty()) {
        QString token = authManager.generateCertificateToken();
        Logger::info(
            QString("[Auth] Initial certificate token generated (%1 chars)").arg(token.size()));
    } else {
        Logger::info("[Auth] Certificate token already exists");
    }

    Logger::info("[Auth] No PIN set by default — admin must generate one");
    Logger::info("[Auth] Access the admin page at https://localhost/");
    Logger::info("[Auth] Remote access requires a generated PIN");
    server.setAuthManager(&authManager);

    QObject::connect(&authManager, &AuthManager::pinChanged, [](const QString& pin) {
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
    // UUID of the host the active relay streams from. Needed by teardown paths
    // that have no HTTP request to read the host from (revoked-device kill).
    QString g_ActiveHostUuid;

    // Suspend host polling whenever a relay is active, so we stop hammering
    // Sunshine's HTTP server while a stream is running (avoids wedging it and
    // making the host appear offline to native clients).
    // g_ActiveRelayRoot stays valid through a take-over teardown (until the old
    // relay is fully destroyed), and g_ActiveSession covers the gap between a
    // /start request and the new relay being created (deferred start). Both keep
    // polling suspended across the whole transition, so a poll never opens an
    // HTTPS 47984 socket that would linger ~120s and hide the host from native
    // clients.
    computerManager.setStreamActivePredicate([&g_ActiveRelay, &g_ActiveMediaTrackRelay,
                                              &g_ActiveStreamRelay, &g_ActiveRelayRoot,
                                              &g_ActiveSession]() {
        return !g_ActiveRelay.isNull() || !g_ActiveMediaTrackRelay.isNull() ||
               !g_ActiveStreamRelay.isNull() || !g_ActiveRelayRoot.isNull() ||
               !g_ActiveSession.isNull();
    });

    // ── Revoked-device kill-switch ─────────────────────────────────────────────
    // Revoking a device whose session is actively streaming must stop that
    // stream immediately — the revoked browser must not keep receiving video
    // just because its relay was established before the revocation. Teardown
    // mirrors the proven /quit route (notify client, stop shim + relay,
    // deleteLater), then cancels the Sunshine session keyed by the revoked
    // browser's uniqueid. sessionEnded stays connected: its auto-quit lambda is
    // re-entrant safe (stop() guards, deleteLater is idempotent) and a double
    // Sunshine /cancel is harmless.
    QObject::connect(
        &authManager, &AuthManager::streamingSessionRevoked, qApp,
        [&computerManager, &g_ActiveRelay, &g_ActiveMediaTrackRelay, &g_ActiveStreamRelay,
         &g_ActiveSession, &g_ActiveClientUniqueId, &g_ActiveHostUuid]() {
            qInfo() << "[main] Streaming session revoked — tearing down active stream";
            bool relayStopped = false;

            if (g_ActiveRelay) {
                DataChannelRelay* relay = g_ActiveRelay;
                g_ActiveRelay = nullptr;
                relay->notifyClientRevoked();
                if (relay->moonlightShim()) relay->moonlightShim()->stopConnection();
                relay->stop();
                relay->deleteLater();
                relayStopped = true;
            }

            if (g_ActiveMediaTrackRelay) {
                MediaTrackRelay* relay = g_ActiveMediaTrackRelay;
                g_ActiveMediaTrackRelay = nullptr;
                relay->notifyClientRevoked();
                if (relay->moonlightShim()) relay->moonlightShim()->stopConnection();
                relay->stop();
                relay->deleteLater();
                relayStopped = true;
            }

            if (g_ActiveStreamRelay) {
                StreamRelay* relay = g_ActiveStreamRelay;
                g_ActiveStreamRelay = nullptr;
                relay->notifyClientRevoked();
                relay->stop();
                relay->deleteLater();
                relayStopped = true;
            }

            if (g_ActiveSession) {
                g_ActiveSession->deleteLater();
                g_ActiveSession = nullptr;
            }

            if (!relayStopped) {
                qInfo() << "[main] Revoke teardown: no live relay (already stopped)";
                return;
            }

            // Cancel the revoked browser's Sunshine session (keyed like /launch).
            // Nobody legitimately resumes it, so leaving it alive is pointless.
            NvComputer* host = computerManager.getHost(g_ActiveHostUuid);
            if (host) {
                auto* identity = IdentityManager::get();
                auto* quitReply = computerManager.http()->quitAppAsync(
                    host->activeAddress, host->activeHttpsPort, identity->getCertificate(),
                    identity->getPrivateKey(), g_ActiveClientUniqueId);
                QObject::connect(quitReply, &QNetworkReply::finished, quitReply,
                                 &QNetworkReply::deleteLater);
            }
            g_ActiveClientUniqueId.clear();
            g_ActiveHostUuid.clear();
        });

    InternetAccessManager internetAccess(&appSettings);
    GeoIpService geoIpService;
    UpdateChecker updateChecker(QCoreApplication::applicationVersion());

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
            if (certData.isEmpty()) certData = QByteArray(MW_CERT_PEM);
#endif
            QByteArray keyData = qgetenv("MW_CERT_KEY");
#ifdef MW_CERT_KEY
            if (keyData.isEmpty()) keyData = QByteArray(MW_CERT_KEY);
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

    // GET /api/update/check — is a newer MoonlightWeb release available? Returns
    // the cached GitHub Releases result (current/latest/update_available plus the
    // exact installer download URL for this OS/arch); a stale cache refreshes in
    // the background without blocking this handler.
    server.router()->get("/api/update/check", [&updateChecker](const HttpRequest&) {
        return HttpResponse::json(updateChecker.statusJson());
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

    registerAuthRoutes(server, authManager, geoIpService);

    server.router()->get("/api/server/status", [&server](const HttpRequest&) {
        QJsonObject obj;
        obj["status"] = "running";
        obj["version"] = QCoreApplication::applicationVersion();
        obj["http_port"] = static_cast<int>(server.httpPort());
        obj["https_port"] = static_cast<int>(server.activeHttpsPort());
        return HttpResponse::json(obj);
    });

    registerHostRoutes(server, computerManager);

    // Phase 5: Start streaming — launch app + RTSP handshake
    auto effectiveUpnpEnabled = upnpEnabled; // Capture by value for the lambda

    server.router()->postAsync("/api/hosts/:id/start", [&computerManager, signalingPort,
                                                        &g_ActiveRelay, &g_ActiveStreamRelay,
                                                        &g_ActiveMediaTrackRelay, &g_ActiveSession,
                                                        &g_ActiveRelayRoot, &g_ActiveClientUniqueId,
                                                        &g_ActiveHostUuid, &server, &appSettings,
                                                        &authManager, effectiveUpnpEnabled,
                                                        stunServer](const HttpRequest& req,
                                                                    ResponseCallback respond) {
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
        g_ActiveHostUuid.clear();

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

        bool reqGamingMode =
            body.contains("gaming_mode") ? body["gaming_mode"].toBool() : appSettings.gamingMode();

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
        QString reqAspect =
            body.contains("stream_aspect") && !body["stream_aspect"].toString().isEmpty()
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

        bool reqYuv444 = body.contains("chroma_444_enabled") ? body["chroma_444_enabled"].toBool()
                                                             : appSettings.chroma444Enabled();

        bool reqHdr =
            body.contains("hdr_enabled") ? body["hdr_enabled"].toBool() : appSettings.hdrEnabled();

        // Mobile clients request lower-bandwidth audio (10ms Opus frames).
        bool reqLowAudio = body.contains("low_audio") && body["low_audio"].toBool();

        // Mute host PC speakers while streaming (localAudioPlayMode). Default true.
        bool reqMuteHost = body.contains("mute_host_audio") ? body["mute_host_audio"].toBool()
                                                            : appSettings.muteHostAudio();

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
            if (u.isDigit() || (u >= 'A' && u <= 'F')) reqClientUniqueId += u;
            if (reqClientUniqueId.size() >= 32) break;
        }

        qInfo() << "[Session] Per-request streaming settings:"
                << "codec=" << AppSettings::videoCodecToString(reqCodec)
                << "gaming=" << reqGamingMode << "bitrate=" << reqBitrate << "height=" << reqHeight
                << "fps=" << reqFps << "yuv444=" << reqYuv444 << "hdr=" << reqHdr
                << "videoEnhancement=" << reqVideoEnhancement;

        // Determine signaling host from the browser's Host header.
        // Works for both LAN (localhost:443) and remote access via moonlightweb.top.
        QString serverHost = req.headers.value("host");
        int colon = serverHost.indexOf(':');
        if (colon >= 0) serverHost = serverHost.left(colon);

        // Is the streaming client on our own LAN? True for loopback / RFC1918,
        // which also covers a LAN client reaching us through the public URL:
        // the router source-NATs the hairpinned connection to a private gateway
        // IP (e.g. 192.168.1.254). When true, the relay may also advertise its
        // private host ICE candidate so a local client can connect directly
        // (routers rarely hairpin UDP); it is never advertised to internet
        // clients (public source IP), so the LAN IP is not leaked.
        const bool clientIsLocal = [&req]() {
            QString ip = req.clientAddress;
            if (ip.startsWith("::ffff:")) ip = ip.mid(7);
            QHostAddress addr(ip);
            if (addr.isNull()) return false;
            if (addr.isLoopback()) return true;
            if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
                quint32 v = addr.toIPv4Address();
                return (v & 0xFF000000) == 0x0A000000 || // 10.0.0.0/8
                       (v & 0xFFF00000) == 0xAC100000 || // 172.16.0.0/12
                       (v & 0xFFFF0000) == 0xC0A80000;   // 192.168.0.0/16
            }
            return false;
        }();

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
        if (transportMode.isEmpty()) transportMode = "auto";

        // A per-request transport_mode (sent by the browser) overrides the admin
        // setting. The browser does NOT normally send it; it only sends
        // transport_index to walk the fallback chain on relaunch.
        if (body.contains("transport_mode") && !body["transport_mode"].toString().isEmpty())
            transportMode = body["transport_mode"].toString();

        // Index into the fallback chain that this attempt targets (the browser
        // increments it and relaunches when a transport fails to connect).
        int reqTransportIndex =
            body.contains("transport_index") ? body["transport_index"].toInt(0) : 0;

        // ── Auto-mode: priority-ordered transport list from header ─────

        // Helper: does the host support a given codec?
        auto hostSupportsCodec = [](NvComputer* h, VideoCodec c) -> bool {
            int support = h->serverCodecModeSupport;
            // Use the canonical SCM_MASK_* values (Limelight.h). The old GFE-era
            // literals (HEVC 0x02, AV1 0x20) are wrong for Sunshine, which reports
            // HEVC at 0x100 and AV1 at 0x10000 → HEVC/AV1 were seen as unsupported.
            switch (c) {
            case VideoCodec::H264: return (support & SCM_MASK_H264) != 0;
            case VideoCodec::HEVC: return (support & SCM_MASK_HEVC) != 0;
            case VideoCodec::AV1: return (support & SCM_MASK_AV1) != 0;
            default: return true;
            }
        };

        // Helper: filter transport list by codec compatibility.
        //
        // Rules:
        //   - H.264 works on all transports.
        //   - HEVC and AV1 are skipped for MediaTrack modes (<video> element
        //     codec support varies across browsers; DataChannel WebCodecs
        //     is more reliable).
        //   - If the host doesn't support the selected codec, try H.264
        //     instead (if host supports it).
        auto filterTransportsByCodec = [&](const QStringList& transports, VideoCodec codec,
                                           NvComputer* h) -> QStringList {
            // Determine effective codec (resolve Auto → HEVC if host supports)
            VideoCodec effective = codec;
            if (effective == VideoCodec::Auto) {
                effective =
                    hostSupportsCodec(h, VideoCodec::HEVC) ? VideoCodec::HEVC : VideoCodec::H264;
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
                if ((effective == VideoCodec::AV1 || effective == VideoCodec::HEVC) &&
                    t.startsWith("webrtc-media")) {
                    qInfo() << "[Auto] Skipping" << t << "(MediaTrack only supports H.264, codec is"
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
                sibling = transportMode.left(transportMode.length() - 4) + QStringLiteral("-tcp");
            if (!sibling.isEmpty() && autoOrder.contains(sibling)) transportChain.append(sibling);
            for (const QString& m : autoOrder)
                if (m != transportMode && m != sibling) transportChain.append(m);
        }
        qInfo() << "[Session] Transport chain:" << transportChain
                << "requested index=" << reqTransportIndex;

        if (transportChain.isEmpty() || reqTransportIndex < 0 ||
            reqTransportIndex >= transportChain.size()) {
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
        qInfo() << "[Session] Attempt" << (reqTransportIndex + 1) << "/" << transportChain.size()
                << ":" << chainMode << "internal=" << internalTransport
                << "iceTcp=" << enableIceTcp;

        // ── Helper: attach lifecycle relay tracking for a new session ───────────
        // Adds the standard relay-created and session-ended connections that
        // maintain the global relay pointers (g_ActiveRelay, etc.) and send a
        // best-effort HTTPS quit to Sunshine when a session ends unexpectedly.
        auto attachRelayTracking = [&](StreamSession* s) {
            // Per-browser uniqueid so an unexpected-end auto-quit cancels only THIS
            // browser's Sunshine session (keyed like /launch), never a co-located
            // iOS/Qt client's session. Empty → shared id (legacy fallback).
            const QString& quitUid = reqClientUniqueId;

            // WSS mode: StreamRelay tracking
            QObject::connect(s, &StreamSession::streamRelayCreated,
                             [&g_ActiveStreamRelay, &g_ActiveRelayRoot, &g_ActiveClientUniqueId,
                              &g_ActiveHostUuid, &computerManager, &authManager, sessionToken, host,
                              quitUid](StreamRelay* r) {
                                 qInfo() << "[main] streamRelayCreated, relay=" << r;
                                 g_ActiveStreamRelay = r;
                                 g_ActiveRelayRoot = r;
                                 g_ActiveClientUniqueId = quitUid;
                                 g_ActiveHostUuid = host->uuid;
                                 authManager.setSessionStreaming(sessionToken, true);

                                 // Context = qApp so the lambda runs on the main thread: the relay
                                 // emits sessionEnded from its dedicated thread, but quitAppAsync()
                                 // touches the shared QNAM that lives on the main thread.
                                 QObject::connect(
                                     r, &StreamRelay::sessionEnded, qApp,
                                     [r, &g_ActiveStreamRelay, &computerManager, &authManager,
                                      sessionToken, host, quitUid]() {
                                         qInfo() << "[main] StreamRelay sessionEnded";
                                         authManager.setSessionStreaming(sessionToken, false);
                                         auto* identity = IdentityManager::get();
                                         auto* quitReply = computerManager.http()->quitAppAsync(
                                             host->activeAddress, host->activeHttpsPort,
                                             identity->getCertificate(), identity->getPrivateKey(),
                                             quitUid);
                                         QObject::connect(quitReply, &QNetworkReply::finished,
                                                          quitReply, &QNetworkReply::deleteLater);
                                         // The StreamSession is ephemeral (self-deletes once
                                         // streaming starts), so its own sessionEnded->quit()
                                         // handler is gone by the time the client disconnects —
                                         // this qApp lambda is the only surviving teardown owner.
                                         // Stop the shim FIRST (while the relay is alive) so
                                         // moonlight stops calling back before destruction (no
                                         // UAF), then stop + deleteLater. destroyed() frees the
                                         // signaling port and lets a deferred start() proceed.
                                         if (r->moonlightShim())
                                             r->moonlightShim()->stopConnection();
                                         r->stop();
                                         r->deleteLater();
                                         if (g_ActiveStreamRelay == r)
                                             g_ActiveStreamRelay = nullptr;
                                     });
                             });

            // WebRTC DataChannel mode: DataChannelRelay tracking
            QObject::connect(
                s, &StreamSession::relayCreated,
                [&g_ActiveRelay, &g_ActiveRelayRoot, &g_ActiveClientUniqueId, &g_ActiveHostUuid,
                 &computerManager, &authManager, sessionToken, host, quitUid](DataChannelRelay* r) {
                    qInfo() << "[main] relayCreated, relay=" << r;
                    g_ActiveRelay = r;
                    g_ActiveRelayRoot = r;
                    g_ActiveClientUniqueId = quitUid;
                    g_ActiveHostUuid = host->uuid;
                    authManager.setSessionStreaming(sessionToken, true);

                    // Context = qApp: see StreamRelay note above (run on main thread).
                    QObject::connect(r, &DataChannelRelay::sessionEnded, qApp,
                                     [r, &g_ActiveRelay, &computerManager, &authManager,
                                      sessionToken, host, quitUid]() {
                                         qInfo() << "[main] sessionEnded fired, relay=" << r;
                                         authManager.setSessionStreaming(sessionToken, false);
                                         auto* identity = IdentityManager::get();
                                         auto* quitReply = computerManager.http()->quitAppAsync(
                                             host->activeAddress, host->activeHttpsPort,
                                             identity->getCertificate(), identity->getPrivateKey(),
                                             quitUid);
                                         QObject::connect(quitReply, &QNetworkReply::finished,
                                                          quitReply, &QNetworkReply::deleteLater);
                                         // The StreamSession is ephemeral (self-deletes once
                                         // streaming starts), so its own sessionEnded->quit()
                                         // handler is gone by the time the client disconnects —
                                         // this qApp lambda is the only surviving teardown owner.
                                         // Stop the shim FIRST (while the relay is alive) so
                                         // moonlight stops calling back before destruction (no
                                         // UAF), then stop + deleteLater. destroyed() frees the
                                         // signaling port and lets a deferred start() proceed.
                                         if (r->moonlightShim())
                                             r->moonlightShim()->stopConnection();
                                         r->stop();
                                         r->deleteLater();
                                         if (g_ActiveRelay == r) {
                                             g_ActiveRelay = nullptr;
                                         }
                                     });
                });

            // WebRTC Media Track mode: MediaTrackRelay tracking
            QObject::connect(
                s, &StreamSession::mediaTrackRelayCreated,
                [&g_ActiveMediaTrackRelay, &g_ActiveRelayRoot, &g_ActiveClientUniqueId,
                 &g_ActiveHostUuid, &computerManager, &authManager, sessionToken, host,
                 quitUid](MediaTrackRelay* r) {
                    qInfo() << "[main] mediaTrackRelayCreated, relay=" << r;
                    g_ActiveMediaTrackRelay = r;
                    g_ActiveRelayRoot = r;
                    g_ActiveClientUniqueId = quitUid;
                    g_ActiveHostUuid = host->uuid;
                    authManager.setSessionStreaming(sessionToken, true);

                    // Context = qApp: see StreamRelay note above (run on main thread).
                    QObject::connect(
                        r, &MediaTrackRelay::sessionEnded, qApp,
                        [r, &g_ActiveMediaTrackRelay, &computerManager, &authManager, sessionToken,
                         host, quitUid]() {
                            qInfo() << "[main] MediaTrackRelay sessionEnded, relay=" << r;
                            authManager.setSessionStreaming(sessionToken, false);
                            auto* identity = IdentityManager::get();
                            auto* quitReply = computerManager.http()->quitAppAsync(
                                host->activeAddress, host->activeHttpsPort,
                                identity->getCertificate(), identity->getPrivateKey(), quitUid);
                            QObject::connect(quitReply, &QNetworkReply::finished, quitReply,
                                             &QNetworkReply::deleteLater);
                            // The StreamSession is ephemeral (self-deletes once
                            // streaming starts), so its own sessionEnded->quit()
                            // handler is gone by the time the client disconnects —
                            // this qApp lambda is the only surviving teardown owner.
                            // Stop the shim FIRST (while the relay is alive) so
                            // moonlight stops calling back before destruction (no
                            // UAF), then stop + deleteLater. destroyed() frees the
                            // signaling port and lets a deferred start() proceed.
                            if (r->moonlightShim()) r->moonlightShim()->stopConnection();
                            r->stop();
                            r->deleteLater();
                            if (g_ActiveMediaTrackRelay == r) g_ActiveMediaTrackRelay = nullptr;
                        });
                });
        };

        // ── Helper: create a session with the given transport mode and attach tracking ─
        // transportMode: full mode string ("webrtc-media-udp", "wss", etc.)
        // iceTcp: whether to enable ICE-TCP candidates
        auto createSession = [&](const QString& transportMode, bool iceTcp, ResponseCallback rsp,
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
                host, appId, computerManager.http(), std::move(rsp), signalingPort, serverHost,
                (codecOverride != VideoCodec::Auto) ? codecOverride : reqCodec, reqGamingMode,
                effectiveUpnpEnabled, internal, stunServer, reqHeight, reqWidth, reqFps, reqBitrate,
                reqYuv444, reqHdr);
            s->setHttpsPort(server.activeHttpsPort());
            s->setStreamRelayPort(signalingPort + 1);
            s->setTransportMode(transportMode); // Full mode for response
            s->setEnableIceTcp(iceTcp);
            s->setLowAudio(reqLowAudio);
            s->setMuteHostAudio(reqMuteHost);
            s->setClientUniqueId(reqClientUniqueId);
            s->setClientIsLocal(clientIsLocal);
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

            if (internalTransport == "webrtc-media" &&
                (effectiveCodec == VideoCodec::HEVC || effectiveCodec == VideoCodec::AV1)) {
                qInfo() << "[Session] MediaTrack attempt but codec is"
                        << AppSettings::videoCodecToString(effectiveCodec)
                        << "- forcing H.264 (MediaTrack only supports H.264)";
                originalCodec = effectiveCodec;
                effectiveCodec = VideoCodec::H264;
                codecOverridden = true;
            }

            auto* session =
                createSession(chainMode, enableIceTcp, std::move(respond), effectiveCodec);
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
                QObject::connect(g_ActiveRelayRoot, &QObject::destroyed, qApp, [session]() {
                    qInfo() << "[Session] Previous relay gone — starting deferred session"
                            << session;
                    session->start();
                });
            } else {
                session->start();
            }
        }
    });

    // Phase 5: Quit running app
    server.router()->postAsync(
        "/api/hosts/:id/quit",
        [&computerManager, &g_ActiveRelay, &g_ActiveStreamRelay, &g_ActiveMediaTrackRelay,
         &g_ActiveSession, &g_ActiveClientUniqueId,
         &g_ActiveHostUuid](const HttpRequest& req, const ResponseCallback& respond) {
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
            qInfo() << "[quit] Host found:" << host->name << host->activeAddress.address() << ":"
                    << host->activeHttpsPort;

            // Per-browser unique ID so the cancel targets this browser's own
            // session (must match the one used at /launch). Sanitized to hex.
            QString quitUniqueId;
            {
                QJsonObject qbody = QJsonDocument::fromJson(req.body).object();
                for (const QChar& c : qbody["client_uniqueid"].toString()) {
                    QChar u = c.toUpper();
                    if (u.isDigit() || (u >= 'A' && u <= 'F')) quitUniqueId += u;
                    if (quitUniqueId.size() >= 32) break;
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
            bool ownsSession = g_ActiveClientUniqueId.isEmpty() || quitUniqueId.isEmpty() ||
                               quitUniqueId == g_ActiveClientUniqueId;
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
                if (relay->moonlightShim()) relay->moonlightShim()->stopConnection();
                relay->stop();
                relay->deleteLater();
                relayStopped = true;
            }

            if (ownsSession && g_ActiveMediaTrackRelay) {
                qInfo() << "[quit] MediaTrackRelay exists, stopping relay="
                        << g_ActiveMediaTrackRelay.data();
                MediaTrackRelay* relay = g_ActiveMediaTrackRelay;
                g_ActiveMediaTrackRelay = nullptr;
                // Explicitly stop MoonlightShim before relay cleanup.
                if (relay->moonlightShim()) relay->moonlightShim()->stopConnection();
                relay->stop();
                relay->deleteLater();
                relayStopped = true;
            }

            if (ownsSession && g_ActiveStreamRelay) {
                qInfo() << "[quit] StreamRelay exists, stopping relay="
                        << g_ActiveStreamRelay.data();
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
                g_ActiveHostUuid.clear();
            }

            if (!relayStopped) {
                qInfo() << "[quit] No active relay (already stopped or never started)";
            }

            qInfo() << "[quit] Sending quitAppAsync to Sunshine ...";
            auto* identity = IdentityManager::get();
            QNetworkReply* reply = computerManager.http()->quitAppAsync(
                host->activeAddress, host->activeHttpsPort, identity->getCertificate(),
                identity->getPrivateKey(), quitUniqueId);
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

    if (!server.start(httpsPort)) return 1;

    // Persist active ports (may differ from preferred due to fallback)
    {
        quint16 activeHttps = server.activeHttpsPort();
        if (activeHttps > 0 && appSettings.httpsPort(0) != activeHttps)
            appSettings.setHttpsPort(activeHttps);

        quint16 activeHttp = server.httpPort();
        if (appSettings.httpPort(0) != activeHttp) appSettings.setHttpPort(activeHttp);
    }

    // Sync UPnP port mapping port with the actual server port
    internetAccess.setPorts(server.httpPort(), server.activeHttpsPort());

    // Port parity (external == internal): when the router-side default port is
    // owned by another instance, InternetAccessManager claims a fallback port so
    // the public URL port is exactly the port the router forwards. We ADD a
    // second HTTPS listener on that port for the domain rather than moving the
    // primary — the primary keeps serving localhost/LAN, so the admin page never
    // loses the origin it is loaded on (no reload race, no stranded page).
    internetAccess.setHttpsRebindCallback([&server](quint16 port) -> bool {
        qInfo() << "[main] Port parity: adding HTTPS listener for the public domain on" << port;
        return server.addSecondaryHttpsListener(port);
    });

    // URL for the host machine's own entry points (Desktop shortcut, installer
    // post-install page, Dock, tray, startup open). Once Internet Access is live
    // the full public domain is used (valid certificate, no warning); the
    // appended ?mwk=<host key> lets the frontend prove to the backend that this
    // browser runs on the host machine (over the domain the peer address is the
    // router, not loopback), unlocking the localhost-only admin functionality.
    // Before/without Internet Access, loopback over HTTPS is used — it works
    // even when DNS is down; the browser asks to accept the self-signed cert
    // once.
    auto entryUrl = [&](const QString& path) -> QString {
        if (internetAccess.isActive() && !internetAccess.domain().isEmpty()) {
            quint16 p = internetAccess.externalHttpsPort();
            if (p == 0) p = server.activeHttpsPort();
            const QString base =
                p == 443 ? QStringLiteral("https://%1").arg(internetAccess.domain())
                         : QStringLiteral("https://%1:%2").arg(internetAccess.domain()).arg(p);
            const QChar sep = path.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?');
            return base + path + sep + QStringLiteral("mwk=") + appSettings.localKey();
        }
        quint16 p = server.activeHttpsPort();
        return p == 443 ? QStringLiteral("https://localhost%1").arg(path)
                        : QStringLiteral("https://localhost:%1%2").arg(p).arg(path);
    };
    auto adminUrl = [entryUrl]() -> QString { return entryUrl(QStringLiteral("/admin")); };
    // First-run provisioning written by the installer (authorize Internet
    // Access, pair the local Sunshine). Runs before the auto-start below so a
    // freshly authorized instance brings Internet Access up immediately. When it
    // applied a provisioning.json, the installer is polling its status file: feed
    // the asynchronous A-record (domain ready) step into the live checklist.
    const bool provisioned = Provisioning::applyOnce(QCoreApplication::applicationDirPath(),
                                                     appSettings, computerManager);

    // Refresh the shortcut to the valid-certificate domain link once it is ready,
    // and (during a fresh install) mark the A-record checklist step done.
    QObject::connect(&internetAccess, &InternetAccessManager::ready, &app,
                     [&adminUrl, provisioned](const QString&, const QString&) {
                         // adminUrl() folds in the active HTTPS port (fallback port
                         // for a co-existing instance behind the same NAT).
                         writeAdminShortcut(adminUrl());
                         if (provisioned)
                             Provisioning::setStepStatus(QStringLiteral("arecord"),
                                                         QStringLiteral("done"));
                     });
    if (provisioned)
        QObject::connect(&internetAccess, &InternetAccessManager::error, &app, [](const QString&) {
            Provisioning::setStepStatus(QStringLiteral("arecord"), QStringLiteral("failed"));
        });

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
        // start() may resolve synchronously (no 'ready' signal emitted): reflect
        // the final A-record state into the installer checklist either way.
        if (provisioned)
            Provisioning::setStepStatus(QStringLiteral("arecord"), internetAccess.isActive()
                                                                       ? QStringLiteral("done")
                                                                       : QStringLiteral("failed"));
    }

    // Write the Desktop admin shortcut with the best URL known at this point.
    writeAdminShortcut(adminUrl());

    // Configure HttpServer to proxy WebSocket upgrades to the signaling server.
    // Both HTTPS and WebSocket signaling share the same port (443 by default).
    server.setSignalingPort(signalingPort);
    // Legacy WSS StreamRelay uses the next port for its local WS server.
    server.setStreamRelayPort(signalingPort + 1);

    // Single-tab dedup control channel: every open app tab keeps a WebSocket
    // (proxied at /ws/control) open here. A second launch asks us — over
    // POST /api/local/focus — to surface the admin page: redirect a connected
    // tab (no duplicate) if any, else open a fresh browser tab.
    ControlChannel controlChannel(signalingPort + 2);
    controlChannel.start();
    server.setControlPort(controlChannel.port());

    server.router()->post("/api/local/focus",
                          [&controlChannel, adminUrl](const HttpRequest& req) -> HttpResponse {
                              // Loopback only: this is the private IPC surface a second local
                              // launch uses, never something a remote peer should trigger.
                              if (!req.isLocal) return HttpResponse::error(403, "local only");
                              QJsonObject obj;
                              if (controlChannel.hasClients()) {
                                  controlChannel.broadcastFocusAdmin();
                                  obj["delivered"] = true;
                              } else {
                                  openInBrowser(adminUrl());
                                  obj["delivered"] = false;
                              }
                              return HttpResponse::json(obj, 200);
                          });

    // — Internet Access via PowerDNS —

    // The host key is single-use: after each redemption the rotated key must be
    // written back into the Desktop shortcut (tray/startup URLs read it live).
    registerSystemRoutes(server, appSettings, authManager, internetAccess, computerManager,
                         [adminUrl]() { writeAdminShortcut(adminUrl()); });

    // Phase N: System tray icon. Its entries open the public domain (with the
    // host key) once Internet Access is live, https://localhost otherwise.
    TrayManager trayManager(&server);
    trayManager.setUrlProvider([entryUrl](const QString& path) { return QUrl(entryUrl(path)); });
    trayManager.init();

    // Keep every host-side entry point current when the entry URL changes:
    // parity rebind moves the HTTPS port, 'ready' switches links to the domain.
    QObject::connect(&internetAccess, &InternetAccessManager::httpsPortChanged, &trayManager,
                     [&trayManager, adminUrl](quint16) {
                         writeAdminShortcut(adminUrl());
                         trayManager.refreshTooltip();
                     });
    QObject::connect(
        &internetAccess, &InternetAccessManager::ready, &trayManager,
        [&trayManager](const QString&, const QString&) { trayManager.refreshTooltip(); });

    // The app is windowless, so on a manual launch (Apps / Start-menu click) the
    // browser IS the app surface: open it — the setup wizard on first run
    // (macOS/Linux; Windows provisioning is owned by the Inno Setup installer),
    // the app page afterwards. Automatic launches (--autostart from the login
    // item / logon task / installer) and headless sessions stay silent.
    if (hasGuiSession() && !parser.isSet(autostartOption)) {
#ifdef Q_OS_WIN
        const QString path = QStringLiteral("/admin");
#else
        const QString path =
            appSettings.setupCompleted() ? QStringLiteral("/admin") : QStringLiteral("/setup");
#endif
        // Public domain when Internet Access is live (valid cert + host key),
        // HTTPS loopback otherwise; the browser asks to accept the self-signed
        // cert once. (If a user later reaches the hosts page over plain http://,
        // the frontend gates it with a secure link.)
        const QString url = entryUrl(path);
        // Defer so the TLS listener is fully accepting before the browser hits it.
        QTimer::singleShot(1200, &app, [url]() {
            qInfo() << "[main] Opening web UI:" << url;
            openInBrowser(url);
        });
    } else if (!appSettings.setupCompleted()) {
        qInfo() << "[main] Setup pending, browser not auto-opened — visit /setup";
    }

    Logger::info("Server ready. Open https://localhost" +
                 (httpsPort != 443 ? ":" + QString::number(server.activeHttpsPort()) : QString()) +
                 " in your browser.");

    return app.exec();
}