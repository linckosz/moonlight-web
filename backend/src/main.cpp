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
#include <QLibraryInfo>
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
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QDateTime>
#include <QHostInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QRandomGenerator>

// isatty(): the operator CLI prompts for consent only when a human can answer.
#include <cstdio>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <termios.h>
#endif
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include "server/AppSettings.h"
#include "server/CertManager.h"
#include "server/Provisioning.h"
#include "server/HttpServer.h"
#include "server/ControlChannel.h"
#include "server/RestRouter.h"
#include "server/AuthManager.h"
#include "server/NetClassify.h"
#include "server/ShareManager.h"
#include "server/SessionPool.h"
#include "server/routes/AuthRoutes.h"
#include "server/routes/HostRoutes.h"
#include "server/routes/ShareRoutes.h"
#include "server/routes/SystemRoutes.h"
#include "common/Logger.h"
#include "common/CrashHandler.h"
#include "common/DesktopSession.h"
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
#include "streaming/StreamWorkerHost.h"
#include "streaming/worker/StreamWorkerMain.h"
#include "network/InternetAccessManager.h"
#include "network/GeoIpService.h"
#include "network/SelfUpdater.h"
#include "network/UpdateChecker.h"
#include "TrayManager.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

// Host + app of the last stream the OWNER started, remembered after their
// worker is gone. Leave (keep_host_session) ends the owner's leg without
// cancelling the Sunshine app, precisely so invited players carry on — but a
// player joining afterwards needs to know which app that is, and by then no
// owner slot says. File scope rather than a local in main(): the /start
// handler that sets it is a deeply nested lambda, and threading two more
// captures through would re-wrap a 600-line capture list for nothing.
static QString g_LastOwnerHostUuid;
static int g_LastOwnerAppId = 0;

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
#define MW_VERSION "0.2.1"
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

// True when a desktop session can show a browser/tray. Shared with the Sunshine
// installer (a headless box has no polkit agent to answer pkexec) and with the
// setup routes — see common/DesktopSession.h for the full rationale.
static bool hasGuiSession()
{
    return mw::hasDesktopSession();
}

// Pick the QPA platform plugin before QApplication exists. The server is
// windowless, but it is a QApplication (the tray needs QtWidgets), and Qt
// aborts at construction when the default xcb plugin finds no display:
//   "qt.qpa.plugin: Could not load the Qt platform plugin xcb"
// On a headless Linux box (server, container, systemd unit before login) the
// offscreen plugin runs the whole HTTP/streaming stack with no X11 or Wayland
// at all. An explicit QT_QPA_PLATFORM always wins — this only fills in a
// default.
//
// What is written is a fallback *list* ("xcb;offscreen"), not a single plugin:
// Qt walks it and keeps the first that initializes. Detecting the display
// ourselves cannot be made airtight — an X server that accepts the connection
// and then refuses the handshake (a gamescope session whose xauth cookie the
// autostart process never got) looks alive to any probe short of speaking X11 —
// and being wrong must not be fatal. With offscreen last, it never is: the
// plugin that cannot reach its display returns nullptr and Qt moves down the
// list instead of aborting. The order mirrors Qt's own preference, so a desktop
// still gets the platform it would have picked anyway.
//
// The packages ship libqoffscreen.so alongside libqxcb.so (EXTRA_PLATFORM_PLUGINS
// in .github/workflows/release.yml); if a hand-built Qt lacks it, Qt still emits
// its own diagnostic naming the plugins it did find.
#if defined(Q_OS_LINUX)
// Is a QPA platform plugin actually installed? Qt looks in
// QT_QPA_PLATFORM_PLUGIN_PATH (which points straight at a platforms directory),
// then QT_PLUGIN_PATH, then the plugins directory qt.conf names — inside an
// AppImage that is the AppDir's, not the build machine's.
static bool hasPlatformPlugin(const QString& name)
{
    const QString file = QStringLiteral("libq%1.so").arg(name);
    QStringList dirs;
    for (const char* var : {"QT_QPA_PLATFORM_PLUGIN_PATH", "QT_PLUGIN_PATH"}) {
        const QString value = qEnvironmentVariable(var);
        if (!value.isEmpty()) dirs += value.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    }
    dirs += QLibraryInfo::path(QLibraryInfo::PluginsPath);

    for (const QString& dir : std::as_const(dirs)) {
        if (QFile::exists(dir + QLatin1Char('/') + file)) return true;
        if (QFile::exists(dir + QStringLiteral("/platforms/") + file)) return true;
    }
    return false;
}
#endif

static void selectHeadlessPlatform()
{
#if defined(Q_OS_LINUX)
    if (!qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) return;

    QByteArray platforms;
    if (mw::hasDisplayServer()) {
        // Naming a plugin Qt cannot find costs a warning line on every launch,
        // and the Linux packages do not currently ship the Wayland client
        // plugins — so ask for wayland only when it is really there. That keeps
        // this function from having to agree with the packaging: whatever the
        // .deb, the .rpm, the AppImage or a distro's system Qt happens to
        // provide, the list adapts to it.
        //
        // Only wayland is gated this way, deliberately. A false negative here is
        // free (xcb serves the same session through XWayland), while the same
        // check on xcb would cost the tray on any Qt whose plugins sit somewhere
        // this lookup does not know about — so xcb stays unconditional and Qt's
        // own diagnostic remains the safety net for it.
        //
        // Two file names, one plugin: Qt 6.11 renamed libqwayland-generic.so to
        // libqwayland.so. Both register the platform name "wayland", and both
        // are in the wild — 6.11 is what CI builds against, while a distro
        // package still links whatever system Qt the distro ships.
        if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
            (hasPlatformPlugin(QStringLiteral("wayland")) ||
             hasPlatformPlugin(QStringLiteral("wayland-generic"))))
            platforms += "wayland;";
        if (!qEnvironmentVariableIsEmpty("DISPLAY")) platforms += "xcb;";
    }
    platforms += "offscreen";
    qputenv("QT_QPA_PLATFORM", platforms);
#endif
}

// True while the A-record checklist step is still open, i.e. some provisioning
// flow is waiting on it: the Windows installer's provisioning.json or the in-app
// setup wizard's /api/setup/apply. Both write the same status file, so the
// asynchronous completion handlers below don't need to know which one ran.
static bool arecordRunning()
{
    return Provisioning::stepStatus(QStringLiteral("arecord")) == QLatin1String("running");
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

// ── Headless operator CLI ───────────────────────────────────────────────────
// A headless install has no admin page within reach: the operator is on SSH,
// and the browser that will use the server lives on another machine. `--status`
// and `--enable-internet` drive the *running* instance through the very same
// loopback endpoints the admin page calls, and print the result as plain text.
//
// They are queries against another process, so they run before the
// single-instance lock is taken and never start a server of their own.

struct LoopbackReply
{
    bool ok = false;    // transport-level success AND HTTP 2xx
    int httpStatus = 0; // 0 when the request never completed
    QJsonObject json;   // parsed body (empty when not JSON)
    QString error;      // human-readable transport error
};

// One loopback HTTPS call. The loopback certificate is the self-signed LAN one,
// so peer verification is off — the peer is a socket on 127.0.0.1, which no
// certificate could authenticate better than the kernel already does.
static LoopbackReply loopbackRequest(const QString& base, const QString& path,
                                     const QByteArray& postBody = QByteArray(),
                                     int timeoutMs = 15000, const QString& adminKey = QString())
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(base + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!adminKey.isEmpty())
        req.setRawHeader(QByteArrayLiteral("X-MW-Admin-Key"), adminKey.toUtf8());
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(ssl);

    QNetworkReply* reply = postBody.isNull() ? nam.get(req) : nam.post(req, postBody);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    LoopbackReply out;
    if (!timer.isActive()) {
        out.error = QStringLiteral("timed out after %1 s").arg(timeoutMs / 1000);
    } else if (reply->error() != QNetworkReply::NoError) {
        out.error = reply->errorString();
    }
    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.json = QJsonDocument::fromJson(reply->readAll()).object();
    out.ok = out.error.isEmpty() && out.httpStatus >= 200 && out.httpStatus < 300;
    reply->deleteLater();
    return out;
}

// HTTPS port the running server answers on, or 0 when none does.
//
// The persisted port is the authority, but it is read from *this* user's
// settings.json — run as a different user than the service (a plain `sudo`-less
// `moonlightweb --status` against a root-owned systemd unit) that file is a
// different one, or absent. So fall back to the default 443 before giving up.
static quint16 findRunningHttpsPort(quint16 persistedPort)
{
    QList<quint16> candidates{persistedPort};
    if (!candidates.contains(443)) candidates.append(443);

    for (quint16 p : candidates) {
        const QString base = p == 443 ? QStringLiteral("https://127.0.0.1")
                                      : QStringLiteral("https://127.0.0.1:%1").arg(p);
        if (loopbackRequest(base, QStringLiteral("/api/health"), QByteArray(), 3000).ok) return p;
    }
    return 0;
}

// POST to an admin route from the terminal.
//
// Every mutating admin endpoint additionally requires the per-run admin key
// (RequestGuard treats a write without it as something another page drove
// through the user's browser). A terminal is not a browser, but the gate cannot
// tell — so fetch the key exactly as the frontend does, over the same loopback
// connection that already proves we are on this machine, and present it.
static LoopbackReply loopbackAdminPost(const QString& base, const QString& path,
                                       const QByteArray& body, int timeoutMs = 15000)
{
    const QString key =
        loopbackRequest(base, QStringLiteral("/api/admin/token"), QByteArray(), 5000)
            .json.value(QStringLiteral("token"))
            .toString();
    return loopbackRequest(base, path, body, timeoutMs, key);
}

// Loopback base URL of the running server, or empty when none answers.
static QString findRunningInstance(quint16 persistedPort)
{
    const quint16 p = findRunningHttpsPort(persistedPort);
    if (p == 0) return QString();
    return p == 443 ? QStringLiteral("https://127.0.0.1")
                    : QStringLiteral("https://127.0.0.1:%1").arg(p);
}

// The agreement the operator must accept before the machine is opened to the
// internet. Printed verbatim and sent verbatim as `consent_message`, so the
// audit log records the exact words that were shown — same contract as the
// wizard's checkbox text (frontend/js/ui/SetupView.js).
static const char* kCliInternetConsent =
    "MoonlightWeb will make this machine reachable from outside your local "
    "network. While the link is on, your router is asked (UPnP) to open a "
    "streaming port during each session, and whoever connects reaches this "
    "machine directly - each side of a peer-to-peer connection sees the "
    "other's public IP address. A future update will add an outgoing "
    "connection to the MoonlightWeb introduction server, which then sees this "
    "machine's public IP address and when it is online. No public DNS record "
    "is created, no certificate is issued for this machine, and ports 80/443 "
    "are not opened. You can turn the link off at any time from the Admin "
    "page. (An instance that already holds a moonlightweb.top sub-domain "
    "keeps it, and its existing behaviour, until the announced shutdown of "
    "the DNS service in February 2027.)";

// stdin is a terminal — i.e. there is a human who can answer a prompt.
static bool stdinIsInteractive()
{
#if defined(Q_OS_WIN)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

// `moonlightweb --status`: everything an operator needs to reach the server and
// to know what is still missing. Returns the process exit code.
static int runStatusCommand(quint16 persistedHttpsPort)
{
    QTextStream out(stdout);
    const QString base = findRunningInstance(persistedHttpsPort);
    if (base.isEmpty()) {
        out << "MoonlightWeb is not running (nothing answers on the loopback interface).\n"
            << "\n"
            << "  systemd:  sudo systemctl status moonlightweb\n"
            << "  logs:     ~/.local/share/MoonlightWeb/MoonlightWeb/logs/moonlightweb.log\n";
        out.flush();
        return 1;
    }

    const QJsonObject server = loopbackRequest(base, "/api/server/status").json;
    const QJsonObject setup = loopbackRequest(base, "/api/setup/status").json;
    const QJsonObject auth = loopbackRequest(base, "/api/auth/status").json;
    const QJsonObject inet = loopbackRequest(base, "/api/internet/status").json;

    const int httpsPort = setup.value("https_port").toInt(443);
    const QString portSuffix = httpsPort == 443 ? QString() : QStringLiteral(":%1").arg(httpsPort);
    const bool headless = setup.value("headless").toBool(false);

    out << "MoonlightWeb " << server.value("version").toString() << " — running";
    if (headless) out << " (headless)";
    out << "\n\n";

    out << "  This machine   https://localhost" << portSuffix << "\n";
    const QJsonArray localIps = inet.value("local_ips").toArray();
    bool firstIp = true;
    for (const QJsonValue& v : localIps) {
        out << (firstIp ? "  On your LAN    " : "                 ") << "https://" << v.toString()
            << portSuffix << "\n";
        firstIp = false;
    }

    const bool internetOn =
        inet.value("active").toBool(false) && !inet.value("domain").toString().isEmpty();
    if (internetOn) {
        const int extPort = inet.value("external_https_port").toInt(443);
        out << "  From internet  https://" << inet.value("domain").toString()
            << (extPort == 443 ? QString() : QStringLiteral(":%1").arg(extPort)) << "\n";
        if (inet.value("cert_issuing").toBool(false))
            out << "                 (certificate still being issued — retry in a minute)\n";
    } else {
        out << "  From internet  off — enable it with:  moonlightweb --enable-internet\n";
        // Only worth probing (and only actionable) while the link is off: it
        // tells the operator up front whether enabling it will configure the
        // router by itself or need a port forward typed in by hand. Nothing on
        // the LAN depends on any of this.
        const QJsonObject upnp =
            loopbackRequest(base, "/api/internet/upnp-probe", QByteArray(), 10000).json;
        if (upnp.value("available").toBool(false))
            out << "                 a UPnP router answered — the port will be mapped for you\n";
        else
            out << "                 no UPnP router — you would have to forward TCP " << httpsPort
                << " to this\n"
                << "                 machine yourself (nothing to do for LAN use)\n";
    }

    out << "\n";
    // The PIN is what a browser on another device is asked for, once. Localhost
    // never needs it — which is exactly why a headless operator, who only ever
    // reaches the server from another device, has to be told about it here.
    // "------" is the sentinel for "no PIN set" (AuthManager::clearPin).
    const QString pin = auth.value("pin").toString();
    if (pin.isEmpty() || pin == QLatin1String("------"))
        out << "  Access PIN     none set — create one with:  moonlightweb --new-pin\n";
    else if (auth.value("pin_consumed").toBool(false))
        out << "  Access PIN     already used — issue a new one with:  moonlightweb --new-pin\n";
    else
        out << "  Access PIN     " << pin << "   (asked once per browser)\n";

    // The admin page is localhost-only, so on a headless host the LAN password is
    // the only way anyone reaches it. There is no built-in default: until one is
    // set the door is shut, and the operator has no other way to find that out.
    if (auth.value("remote_admin_enabled").toBool(false)) {
        if (auth.value("admin_password_set").toBool(false))
            out << "  Admin password set   (change it with:  moonlightweb "
                   "--set-admin-password)\n";
        else
            out << "  Admin password none set — no computer on your network can open the admin\n"
                << "                 page. Set one with:  moonlightweb --set-admin-password\n";
    }

    const QJsonObject sun = setup.value("sunshine").toObject();
    if (headless)
        out << "  Sunshine       not applicable on a headless host — add your hosts by IP\n";
    else if (sun.value("installed").toBool(false))
        out << "  Sunshine       installed"
            << (sun.value("paired").toBool(false) ? ", paired\n" : ", not paired\n");

    out << "\n";
    out.flush();
    return 0;
}

// `moonlightweb --new-pin`: issue the PIN a browser on another device is asked
// for. On a headless host the admin page's "generate" button is out of reach
// (it is localhost-only, and localhost has no browser), so this is the only way
// to hand out access. Uses the non-revoking endpoint: existing devices keep
// their sessions.
static int runNewPinCommand(quint16 persistedHttpsPort)
{
    QTextStream out(stdout);
    const QString base = findRunningInstance(persistedHttpsPort);
    if (base.isEmpty()) {
        out << "MoonlightWeb is not running — start it first "
               "(sudo systemctl start moonlightweb).\n";
        out.flush();
        return 1;
    }

    const LoopbackReply reply =
        loopbackAdminPost(base, "/api/admin/pin/generate", QByteArrayLiteral("{}"));
    if (!reply.ok) {
        out << "Failed: "
            << (reply.error.isEmpty() ? QStringLiteral("HTTP %1").arg(reply.httpStatus)
                                      : reply.error)
            << "\n";
        out.flush();
        return 1;
    }

    out << "\n  Access PIN     " << reply.json.value("pin").toString() << "\n"
        << "\n"
        << "  Enter it once in the browser of each device you want to allow.\n"
        << "  It is single-use: run this again for the next device.\n\n";
    out.flush();
    return 0;
}

// Read a secret from the terminal without echoing it. When stdin is a pipe
// (the installer feeding a password it already collected) the line is simply
// read: there is no terminal to turn echo off on.
static QString readSecretFromStdin(const QString& prompt)
{
    QTextStream out(stdout);
    QTextStream in(stdin);

    if (!stdinIsInteractive()) return in.readLine();

    out << prompt;
    out.flush();

#if defined(Q_OS_WIN)
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool saved = GetConsoleMode(handle, &mode) != 0;
    if (saved) SetConsoleMode(handle, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    const QString line = in.readLine();
    if (saved) SetConsoleMode(handle, mode);
#else
    termios saved{};
    const bool haveTermios = tcgetattr(fileno(stdin), &saved) == 0;
    if (haveTermios) {
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(fileno(stdin), TCSAFLUSH, &quiet);
    }
    const QString line = in.readLine();
    if (haveTermios) tcsetattr(fileno(stdin), TCSAFLUSH, &saved);
#endif

    // The user's Enter never reached the screen, so end the line ourselves.
    out << "\n";
    out.flush();
    return line;
}

// `moonlightweb --set-admin-password`: set (or replace) the password a computer
// on the LAN spends to open the admin page.
//
// There is no built-in default password, so on a headless host this is the only
// way to open that door at all: the admin page it guards is localhost-only, and
// a headless box has no browser to open it with. Replacing the password signs
// out every machine that unlocked with the old one.
static int runSetAdminPasswordCommand(quint16 persistedHttpsPort)
{
    QTextStream out(stdout);
    const QString base = findRunningInstance(persistedHttpsPort);
    if (base.isEmpty()) {
        out << "MoonlightWeb is not running — start it first "
               "(sudo systemctl start moonlightweb).\n";
        out.flush();
        return 1;
    }

    const QString password = readSecretFromStdin(QStringLiteral("New admin password: "));
    if (password.size() < AuthManager::MIN_ADMIN_PASSWORD_LEN) {
        out << "Too short — the password must be at least " << AuthManager::MIN_ADMIN_PASSWORD_LEN
            << " characters. Nothing was changed.\n";
        out.flush();
        return 1;
    }
    // Only worth confirming when a human typed it blind; a piped password was
    // already confirmed by whatever collected it.
    if (stdinIsInteractive() && readSecretFromStdin(QStringLiteral("Repeat it: ")) != password) {
        out << "The two entries differ — nothing was changed.\n";
        out.flush();
        return 1;
    }

    QJsonObject body;
    body["password"] = password;
    // Setting a password is what the door is for: an operator running this means
    // to allow the unlock, so make sure the toggle is not left off.
    body["enabled"] = true;

    const LoopbackReply reply = loopbackAdminPost(
        base, "/api/admin/password", QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!reply.ok) {
        const QString apiError = reply.json.value("error").toString();
        out << "Failed: "
            << (!apiError.isEmpty()
                    ? apiError
                    : (reply.error.isEmpty() ? QStringLiteral("HTTP %1").arg(reply.httpStatus)
                                             : reply.error))
            << "\n";
        out.flush();
        return 1;
    }

    out << "\n  Admin password set.\n"
        << "\n"
        << "  A computer on this network can now open the admin page at the server's\n"
        << "  URL and unlock it with this password. Machines that used a previous\n"
        << "  password have been signed out.\n\n";
    out.flush();
    return 0;
}

// `moonlightweb --enable-internet`: record the operator's consent and enable
// the Internet link. A legacy instance re-publishes its sub-domain and renews
// its certificate; a fresh one authorizes the per-session router mapping and
// nothing more. Says plainly what to forward by hand when no UPnP-IGD answers.
// Returns the process exit code.
static int runEnableInternetCommand(quint16 persistedHttpsPort, bool assumeYes)
{
    QTextStream out(stdout);
    const QString base = findRunningInstance(persistedHttpsPort);
    if (base.isEmpty()) {
        out << "MoonlightWeb is not running — start it first "
               "(sudo systemctl start moonlightweb).\n";
        out.flush();
        return 1;
    }

    out << "\n" << kCliInternetConsent << "\n\n";
    if (!assumeYes) {
        if (!stdinIsInteractive()) {
            out << "Re-run with --yes to accept this and continue.\n";
            out.flush();
            return 1;
        }
        out << "Type 'yes' to continue: ";
        out.flush();
        QTextStream in(stdin);
        if (in.readLine().trimmed().compare(QStringLiteral("yes"), Qt::CaseInsensitive) != 0) {
            out << "Cancelled — nothing was registered.\n";
            out.flush();
            return 1;
        }
    }

    // Probe the router before enabling, so the closing advice is accurate even
    // if the enable itself half-fails.
    const bool upnp = loopbackRequest(base, "/api/internet/upnp-probe", QByteArray(), 10000)
                          .json.value("available")
                          .toBool(false);

    QJsonObject body;
    body["internet_access_enabled"] = true;
    body["consent_message"] = QString::fromUtf8(kCliInternetConsent);
    out << "\nEnabling the Internet link…\n";
    out.flush();

    // Generous: the handler returns only once the A record resolves, and DNS
    // propagation plus the ACME order are both network round-trips away.
    const LoopbackReply reply = loopbackAdminPost(
        base, "/api/internet/enable", QJsonDocument(body).toJson(QJsonDocument::Compact), 180000);
    if (!reply.ok) {
        out << "Failed: "
            << (reply.error.isEmpty() ? QStringLiteral("HTTP %1").arg(reply.httpStatus)
                                      : reply.error)
            << "\n";
        out.flush();
        return 1;
    }

    const QJsonObject st = reply.json;
    const QString domain = st.value("domain").toString();
    const int extPort = st.value("external_https_port").toInt(443);
    const QString extSuffix = extPort == 443 ? QString() : QStringLiteral(":%1").arg(extPort);

    if (!st.value("active").toBool(false)) {
        out << "Failed: " << st.value("last_error").toString(QStringLiteral("unknown error"))
            << "\n";
        out.flush();
        return 1;
    }

    if (domain.isEmpty()) {
        // Fresh instance: nothing is published — the link only authorizes the
        // per-session router mapping. The remote entry point arrives with the
        // introduction server.
        out << "\n  Internet link  enabled — streaming sessions may open a router port"
            << (upnp ? " (UPnP gateway found).\n"
                     : ".\n                 No UPnP gateway answered: remote streams will need a "
                       "manual port forward.\n");
        out << "  Web interface  stays on the LAN address (no sub-domain is registered,\n"
            << "                 no certificate is issued, ports 80/443 stay closed).\n";
    } else {
        out << "\n  Public URL     https://" << domain << extSuffix << "\n";
        if (st.value("cert_issuing").toBool(false))
            out << "                 certificate still being issued — it lands within a minute\n";

        out << "\n";
        if (upnp) {
            out << "  Router         UPnP gateway found — port " << extPort
                << "/tcp mapped automatically.\n";
        } else {
            // The only manual step on a headless internet install, and the one
            // that silently breaks everything when skipped. Spell it out.
            out << "  Router         no UPnP gateway answered. Forward this port by hand in your\n"
                << "                 router's admin page, or the public URL will not resolve to\n"
                << "                 this machine:\n"
                << "\n"
                << "                     TCP " << extPort << "  ->  "
                << st.value("local_ip").toString() << ":" << st.value("https_port").toInt(443)
                << "\n";
        }
    }
    out << "\n";
    out.flush();
    return 0;
}

// ── Tray-only client ────────────────────────────────────────────────────────
//
// A MoonlightWeb that already runs where nobody can see it still deserves a
// tray icon on the desktop: the Windows NSSM service lives in session 0, whose
// desktop no user is ever logged into, and a systemd unit has no session at
// all. Both draw their tray into the void — which is why a machine running the
// service showed no icon after a reboot even though the logon task fired.
//
// So a launch that loses the single-instance lock does not always exit. When
// the instance that owns the lock is headless and *this* one has a desktop, it
// stays alive with nothing but the tray, pointed at the loopback URL of the
// server it decorates.
//
// Guards, in order: only in a desktop session (a second service launch must
// still exit), only one client at a time (its own lock file — otherwise every
// desktop launch would stack another icon), and only when the running instance
// draws no tray of its own (else a plain desktop install would show two).

#ifdef Q_OS_WIN
// True when the process holding the single-instance lock cannot possibly have
// drawn a tray icon on THIS desktop: it lives in another Windows session (a
// service in session 0, whose desktop nobody is ever logged into), or it runs
// under an account this process may not even query — which amounts to the same
// thing.
//
// Asking Windows beats asking the server: it also works against an instance too
// old to report `headless` in /api/setup/status (≤ 0.2.3).
//
// One deliberate false positive: an elevated instance in our own session is
// unqueryable too, so a copy started with "Run as administrator" would get a
// second icon from us. Degenerate and purely cosmetic — not worth enumerating
// the SCM to rule out.
static bool lockOwnerHasNoDesktopHere(const QLockFile& lock)
{
    qint64 pid = 0;
    QString host, appName;
    if (!lock.getLockInfo(&pid, &host, &appName) || pid <= 0) return false;

    DWORD ourSession = 0;
    DWORD theirSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ourSession)) return false;
    if (!ProcessIdToSessionId(static_cast<DWORD>(pid), &theirSession))
        return true; // ERROR_ACCESS_DENIED: a service, or another user's process
    return theirSession != ourSession;
}
#endif

// Wait without a running event loop of our own.
static void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Loopback URL for the tray entries. "localhost", not 127.0.0.1: the loopback
// certificate carries localhost as a SAN, so the browser only has to accept a
// self-signed certificate, not a name mismatch on top of it. No host key is
// needed — loopback carries local privilege by itself.
static QUrl trayClientUrl(quint16 httpsPort, const QString& path)
{
    return QUrl(httpsPort == 443
                    ? QStringLiteral("https://localhost") + path
                    : QStringLiteral("https://localhost:%1%2").arg(httpsPort).arg(path));
}

static int runTrayClient(QApplication& app, quint16 persistedHttpsPort, bool ownerHasNoDesktopHere)
{
    QLockFile trayLock(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                       "/moonlightweb-tray.lock");
    trayLock.setStaleLockTime(0); // stale detection by PID liveness only
    if (!trayLock.tryLock(100)) {
        Logger::info("A tray client is already running — nothing to do");
        return 0;
    }

    // At logon the service and this process start within the same second, and
    // the service's TLS listener is a few seconds behind. Give it time before
    // concluding that nothing is there. A refused connection returns at once,
    // so this costs nothing in the common case.
    quint16 port = 0;
    for (int attempt = 0; attempt < 10 && port == 0; ++attempt) {
        if (attempt > 0) waitMs(2000);
        port = findRunningHttpsPort(persistedHttpsPort);
    }
    if (port == 0) {
        Logger::info("No tray client: the instance holding the lock never answered on loopback");
        return 0;
    }

    // Second opinion for the platforms with no session check of their own (a
    // systemd --user unit started outside a graphical session): the running
    // instance says itself whether it has a desktop to draw on.
    if (!ownerHasNoDesktopHere) {
        const QString base = port == 443 ? QStringLiteral("https://127.0.0.1")
                                         : QStringLiteral("https://127.0.0.1:%1").arg(port);
        const LoopbackReply setup =
            loopbackRequest(base, QStringLiteral("/api/setup/status"), QByteArray(), 5000);
        if (!setup.ok || !setup.json.value("headless").toBool(false)) {
            Logger::info("No tray client: the running instance has a desktop and its own tray");
            return 0;
        }
    }

    Logger::info(QStringLiteral("Tray-only client for the headless instance on port %1").arg(port));

    TrayManager tray(nullptr);
    tray.setClientMode(true);
    tray.setUrlProvider([&port](const QString& path) { return trayClientUrl(port, path); });

    // Restart / Quit act on the server we decorate, not on this tray. We reach it
    // the same way the CLI reaches an admin route: a loopback POST carrying the
    // per-run admin key (loopbackAdminPost fetches it). The server then exits on
    // its own — its supervisor respawns it for a restart, or lets it stay down
    // for a quit — so no elevation and no SCM rights are needed here. 127.0.0.1,
    // not "localhost", so the admin-key exchange rides the loopback the guard
    // already trusts. `port` is followed live by the poll below.
    tray.setRestartHandler([&port]() {
        const QString base = port == 443 ? QStringLiteral("https://127.0.0.1")
                                         : QStringLiteral("https://127.0.0.1:%1").arg(port);
        loopbackAdminPost(base, QStringLiteral("/api/system/restart"), QByteArrayLiteral("{}"));
    });
    tray.setQuitHandler([&port]() {
        const QString base = port == 443 ? QStringLiteral("https://127.0.0.1")
                                         : QStringLiteral("https://127.0.0.1:%1").arg(port);
        loopbackAdminPost(base, QStringLiteral("/api/system/quit"), QByteArrayLiteral("{}"));
    });

    if (!tray.init()) {
        Logger::warning("Tray client: no system tray available — exiting");
        return 0;
    }

    // Follow the server if it moves: a port-parity rebind or a service restart
    // on a fallback port would otherwise leave the tray pointing at nothing.
    // And do not outlive it: an icon for a server that is gone (service stopped
    // or uninstalled) is worse than no icon — it would also sit next to the
    // tray of whatever instance takes the lock next. A service restart is a
    // matter of seconds, so only a sustained silence counts.
    int silentPolls = 0;
    QTimer portPoll;
    portPoll.setInterval(60000);
    QObject::connect(&portPoll, &QTimer::timeout, &app, [&port, &tray, &silentPolls]() {
        const quint16 p = findRunningHttpsPort(port);
        if (p == 0) {
            if (++silentPolls < 5) return;
            Logger::info("Tray client: the server has been gone for 5 minutes — quitting");
            QApplication::quit();
            return;
        }
        silentPolls = 0;
        if (p == port) return;
        Logger::info(QStringLiteral("Tray client: server moved to port %1").arg(p));
        port = p;
        tray.refreshTooltip();
    });
    portPoll.start();

    return app.exec();
}

// Does @p h advertise support for codec @p c?
//
// Uses the canonical SCM_MASK_* values (Limelight.h). The old GFE-era literals
// (HEVC 0x02, AV1 0x20) are wrong for Sunshine, which reports HEVC at 0x100 and
// AV1 at 0x10000 → HEVC/AV1 were seen as unsupported.
static bool hostSupportsCodec(NvComputer* h, VideoCodec c)
{
    const int support = h->serverCodecModeSupport;
    switch (c) {
    case VideoCodec::H264: return (support & SCM_MASK_H264) != 0;
    case VideoCodec::HEVC: return (support & SCM_MASK_HEVC) != 0;
    case VideoCodec::AV1: return (support & SCM_MASK_AV1) != 0;
    default: return true;
    }
}

// Filter a transport list by codec compatibility.
//
// Rules:
//   - H.264 works on all transports.
//   - HEVC and AV1 are skipped for MediaTrack modes (<video> element codec
//     support varies across browsers; DataChannel WebCodecs is more reliable).
//   - If the host doesn't support the selected codec, try H.264 instead (if the
//     host supports it).
//
// Shared by the owner's /start and a player's join so both walk the same chain.
static QStringList filterTransportsByCodec(const QStringList& transports, VideoCodec codec,
                                           NvComputer* h)
{
    // Determine effective codec (resolve Auto → HEVC if host supports)
    VideoCodec effective = codec;
    if (effective == VideoCodec::Auto)
        effective = hostSupportsCodec(h, VideoCodec::HEVC) ? VideoCodec::HEVC : VideoCodec::H264;

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
        // Note: with video enhancement ON, MediaTrack is NOT skipped — it is
        // kept as a last resort (ordered last by TransportPriorities) and
        // streams WITHOUT enhancement (<video> cannot be processed by WebGPU).
        // The ordering is handled in orderedTransports().
        result.append(t);
    }
    return result;
}

// Default ports for --dev, chosen to stay clear of the production 80/443 and of
// the GameStream ranges (47984-48010 stock, 48100+ for MultiSeat seats).
static constexpr quint16 kDevHttpPort = 48080;
static constexpr quint16 kDevHttpsPort = 48443;
// Signaling base for --dev. It also seeds the control channel (+2) and the
// per-slot worker ports (+10 * slot), so it has to move as a block.
static constexpr quint16 kDevSignalingPort = 48501;
// WebRTC media UDP port base (all modes). Each concurrent slot binds a distinct
// port (base + slot) so simultaneous streams never collide on it, and UPnP maps
// exactly that port. Slot 0 keeps the historical 48010; mirrors
// SignalingServer::kUpnpPort. The dev firewall rule opens base..base+kMaxSlots-1.
static constexpr quint16 kMediaBasePort = 48010;

int main(int argc, char* argv[])
{
    // Before QApplication: on a headless Linux host this swaps the xcb platform
    // plugin (which would abort for want of a display) for offscreen.
    selectHeadlessPlatform();

    QApplication app(argc, argv);
    // The plugin that actually loaded is the last word on whether Qt can draw:
    // it overrides the environment probe for the tray, the browser auto-open,
    // the Sunshine installer and the `headless` flag the API reports.
    mw::confirmDisplayServer(app.platformName() != QLatin1String("offscreen"));
    // --dev has to be read straight from argv: the application name decides where
    // every piece of state lives (settings.json, the single-instance lock, logs,
    // the QSettings host list and client identity), and it must be set before any
    // of them is touched — long before QCommandLineParser runs.
    const bool devMode = [argc, argv]() {
        for (int i = 1; i < argc; ++i)
            if (qstrcmp(argv[i], "--dev") == 0) return true;
        return false;
    }();

    // A dev instance gets its own name, so AppDataLocation and QSettings both
    // move as one: it can never read or corrupt the installed service's state.
    QCoreApplication::setApplicationName(devMode ? "MoonlightWeb-dev" : "MoonlightWeb");
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
    // Route the console echo to stderr BEFORE anything logs (CrashHandler::install
    // below logs an INFO line), for every mode whose stdout is a payload rather
    // than a log: --stream-worker carries the JSON event protocol, and the
    // operator commands print a report a human (or a script) reads. The real
    // branches for all of them run after CLI parsing.
    // --help/--version included: QCommandLineParser writes them to stdout and
    // exits, and two INFO lines ahead of the usage text is exactly the kind of
    // noise that breaks `moonlightweb --help | less`.
    for (int i = 1; i < argc; ++i) {
        static const char* const kQuietStdout[] = {"--stream-worker",
                                                   "--status",
                                                   "--new-pin",
                                                   "--enable-internet",
                                                   "--set-admin-password",
                                                   "--help",
                                                   "--help-all",
                                                   "-h",
                                                   "-?",
                                                   "--version",
                                                   "-v"};
        for (const char* flag : kQuietStdout) {
            if (qstrcmp(argv[i], flag) == 0) {
                Logger::instance()->setConsoleToStderr(true);
                break;
            }
        }
    }
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

    // Internal: run as a per-session stream worker child process (spawned by the
    // parent server; hosts one relay graph + moonlight-common-c instance).
    QCommandLineOption streamWorkerOption("stream-worker",
                                          "Internal: host one streaming session as a child "
                                          "process of the main server");
    parser.addOption(streamWorkerOption);

    // Headless operator commands: query / configure the RUNNING instance from a
    // terminal, then exit. No server is started and no lock is taken.
    QCommandLineOption statusOption(
        "status", "Print the running server's URLs, access PIN and internet status, then exit");
    parser.addOption(statusOption);

    QCommandLineOption newPinOption(
        "new-pin", "Issue a new access PIN for a remote browser, print it, then exit");
    parser.addOption(newPinOption);

    QCommandLineOption setAdminPasswordOption(
        "set-admin-password",
        "Set the password another computer on this network uses to open the admin page, then exit");
    parser.addOption(setAdminPasswordOption);

    QCommandLineOption enableInternetOption(
        "enable-internet", "Enable the Internet link (asks for consent), then exit");
    parser.addOption(enableInternetOption);

    QCommandLineOption yesOption("yes", "Accept the --enable-internet agreement non-interactively");
    parser.addOption(yesOption);

    // Development instance: isolated state (see the applicationName switch at
    // startup), no single-instance lock, and alternate default ports — so it can
    // run alongside an installed service without touching it. Never for production.
    QCommandLineOption devOption("dev",
                                 "Run an isolated development instance (separate state, "
                                 "alternate ports, no single-instance lock)");
    parser.addOption(devOption);

    parser.process(app);

    // Configure logging
    if (parser.isSet(logOption)) Logger::instance()->setLogFile(parser.value(logOption));

    // ── Stream-worker child process ─────────────────────────────────────────
    // Branch BEFORE the single-instance lock (the worker is a deliberate second
    // process of the same binary) and log to a separate file (two processes on
    // one log file would interleave/rotate-race). stdout carries the worker's
    // JSON event protocol — Logger writes to file/stderr only.
    if (parser.isSet(streamWorkerOption)) {
        Logger::instance()->setConsoleToStderr(true);
        if (!parser.isSet(logOption)) {
            const QString logDir =
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
            Logger::instance()->setLogFile(logDir + QStringLiteral("/moonlightweb-worker-%1.log")
                                                        .arg(QCoreApplication::applicationPid()));
        }
        return runStreamWorker(app);
    }

    Logger::info("MoonlightWeb server starting...");
    Logger::info("Version: " + QCoreApplication::applicationVersion());
    // Which QPA plugin won the fallback list — "offscreen" is the whole story
    // behind a missing tray icon or an admin page that never opened by itself,
    // and the first thing to ask for when a headless install misbehaves.
    Logger::info("Qt platform: " + app.platformName() +
                 (mw::hasDisplayServer() ? QString() : QStringLiteral(" (no display — headless)")));

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

    // ── Headless operator commands ───────────────────────────────────────────
    // Talk to the instance that is already running and exit. Two orderings
    // matter here: before the single-instance lock, so these are never mistaken
    // for a second server launch (which would try to surface an admin page
    // nobody can see); and before seedDocumentedDefaults(), so querying the
    // service as a different user than it runs as does not leave a stray
    // settings.json behind in that user's home.
    if (parser.isSet(statusOption)) return runStatusCommand(appSettings.httpsPort(443));
    if (parser.isSet(newPinOption)) return runNewPinCommand(appSettings.httpsPort(443));
    if (parser.isSet(setAdminPasswordOption))
        return runSetAdminPasswordCommand(appSettings.httpsPort(443));
    if (parser.isSet(enableInternetOption))
        return runEnableInternetCommand(appSettings.httpsPort(443), parser.isSet(yesOption));

    appSettings.seedDocumentedDefaults(); // write documented file-only keys if absent
    quint16 httpPort = appSettings.httpPort(devMode ? kDevHttpPort : 80);
    if (parser.isSet("port")) httpPort = parser.value("port").toUShort();

    // ── Single instance ──────────────────────────────────────────────────────
    // The app has no window: launching it again (Desktop shortcut / Apps /
    // Start-menu click) must not spawn a duplicate server on fallback ports.
    // Surface the running instance's admin page instead, then exit cleanly
    // (exit 0 = no supervisor relaunch).
    QLockFile instanceLock(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                           "/moonlightweb.lock");
    instanceLock.setStaleLockTime(0); // stale detection by PID liveness only
    // A dev instance deliberately runs beside the installed one; it holds its own
    // lock file anyway (isolated state), so this only skips the "surface the
    // running admin page and exit" path.
    if (!devMode && !instanceLock.tryLock(100)) {
        Logger::info("Another instance is already running");
        if (!hasGuiSession()) return 0;
        if (!parser.isSet(autostartOption)) {
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
        // The server we just found may be a service with no desktop to draw on
        // (Windows session 0, systemd). Then this process stays as its tray.
#ifdef Q_OS_WIN
        const bool ownerHidden = lockOwnerHasNoDesktopHere(instanceLock);
#else
        const bool ownerHidden = false;
#endif
        return runTrayClient(app, appSettings.httpsPort(443), ownerHidden);
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
    quint16 httpsPort = appSettings.httpsPort(devMode ? kDevHttpsPort : 443);
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
    // In --dev the whole block moves: the signaling base also seeds the control
    // channel (base + 2) and the per-slot worker ports (base + 10 * slot), so
    // leaving it at the default would collide with an installed instance.
    quint16 signalingPort = parser.value("ws-port").toUShort();
    if (devMode && !parser.isSet(wsPortOption)) signalingPort = kDevSignalingPort;
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
    // Aspect ratio the owner's last launch settled on, per host. A guest cannot
    // know the host's screen format — its own monitor says nothing about it —
    // so its stream inherits what the owner's browser measured for that host
    // (frontend/js/stream/AspectProbe.js). Keyed by host so two owners on two
    // different hosts never hand each other's ratio to a player.
    QHash<QString, QString> g_HostAspect;

    // ── Concurrent stream worker slots ─────────────────────────────────────
    // With stream_worker_enabled (default), every session runs in its own
    // `--stream-worker` child process, which is what allows concurrent
    // sessions (moonlight-common-c is process-global). Slot 0 is the owner's
    // primary stream (/ws, 48001/48002); slot 1 the standby used by the
    // frontend's seamless quality switching (/ws1, 48011/48012); slots 2..4
    // belong to invited players (/ws2../ws4, 48021.. onwards), one each.
    //
    // Ports follow the slot: signaling = base + 10 * slot, relay = that + 1.
    // Slot 0 keeps the historical pair, which is why the formula starts there.
    static constexpr int kOwnerSlots = 2; // 0 = primary, 1 = standby
    static constexpr int kTotalSlots = kOwnerSlots + ShareManager::kSlotCount;
    // Slots up to kTotalSlots are addressed directly by index: 0/1 by the owner
    // paths, 2..4 by ShareManager when a player joins. Above that, acquire()
    // hands out slots to devices streaming a DIFFERENT host, which is what lets
    // independent sessions coexist instead of evicting each other.
    static constexpr int kExtraSessionSlots = 4;
    static constexpr int kMaxSlots = kTotalSlots + kExtraSessionSlots;
    SessionPool g_Pool(signalingPort, kTotalSlots, kMaxSlots);

    // Thin names kept over the pool: they read better at the call sites and
    // leave the slot arithmetic in one place.
    auto slotSignalingPort = [&g_Pool](int slot) -> quint16 {
        return g_Pool.signalingPort(slot);
    };
    auto slotWsPath = [](int slot) -> QString { return SessionPool::wsPath(slot); };
    // True when ANY slot other than @p slot still has a live worker. Sunshine
    // sessions share one running app, so a /cancel while this is true would
    // terminate the game for whoever is left.
    // Takes the host explicitly: the answer only means anything within one
    // host, and every caller uses it to decide whether a Sunshine /cancel is
    // safe. Passing the host at the call site keeps that choice visible.
    auto anyOtherSlotLive = [&g_Pool](int slot, const QString& hostUuid) {
        return g_Pool.anyOtherLiveOnHost(slot, hostUuid);
    };
    // Per-host result of the dual-stream capability probe (first standby
    // launch): missing = unknown (assume yes), false = host rejected a second
    // concurrent session → frontend falls back to the legacy relaunch.
    QHash<QString, bool> g_DualSupport;
    // When a standby (slot 1) launch starts: if slot 0 dies within ~3s of it,
    // the host "took over" instead of adding a session → mark no-dual.
    qint64 g_LastStandbyStartMs = 0;
    // Uniqueids whose Sunshine app session is (likely) still alive: set when a
    // worker reports streaming, cleared when a /cancel is actually sent.
    // Workers are fresh processes (their in-process resume hint is empty), so
    // the parent carries the hint: a launch for a live uid goes straight to
    // /resume — Sunshine rejects /launch while an app is running.
    QSet<QString> g_LiveSunshineUids;

    // Detach a worker slot from session-level bookkeeping and tear the child
    // down. Suppresses the slot's normal ended-cleanup (no Sunshine /cancel:
    // take-over and standby-restart both want the Sunshine session kept alive
    // for the /resume that follows). The host object self-deletes on exit.
    auto detachWorkerSlot = [&g_Pool, &authManager](int i, bool takenOver,
                                                           bool sessionEnded = false) {
        SessionPool::Slot& sl = g_Pool.at(i);
        StreamWorkerHost* old = g_Pool.workerAs<StreamWorkerHost>(i);
        const QString token = sl.sessionToken;
        sl.worker = nullptr;
        sl.clientUniqueId.clear();
        sl.hostUuid.clear();
        sl.sessionToken.clear();
        sl.appId = 0;
        if (!old) return static_cast<StreamWorkerHost*>(nullptr);
        QObject::disconnect(old, &StreamWorkerHost::ended, nullptr, nullptr);
        QObject::connect(old, &StreamWorkerHost::ended, qApp, [token, &authManager]() {
            authManager.setSessionStreaming(token, false);
        });
        // An invited player whose session the owner ended gets its own notice,
        // so their page can explain what happened instead of showing a
        // connection error.
        if (sessionEnded)
            old->notifySessionEnded();
        else if (takenOver)
            old->notifyTakenOver();
        else
            old->requestQuit();
        return old;
    };

    // ── Session sharing ────────────────────────────────────────────────────
    // Owns the share state machine (link + PIN + permissions per player slot);
    // the stream side of it lives here because only main.cpp drives workers.
    ShareManager shareManager;

    // Tear every player worker down and revoke their shares. Called when the
    // owner really stops — their session is the one the players resumed into.
    auto endPlayerSessions = [&g_Pool, &detachWorkerSlot,
                              &shareManager](ShareManager::EndReason reason) {
        for (int i = kOwnerSlots; i < kTotalSlots; ++i)
            if (g_Pool.at(i).worker) detachWorkerSlot(i, false, true);
        shareManager.deactivateAll(reason);
    };

    // The share manager decided a player's stream must end (the eight hours ran
    // out, or too many wrong PINs). It knows nothing about workers — this does.
    QObject::connect(&shareManager, &ShareManager::playerMustDisconnect, qApp,
                     [&g_Pool, &detachWorkerSlot](int slot, int) {
                         if (slot >= kOwnerSlots && slot < kTotalSlots &&
                             g_Pool.at(slot).worker)
                             detachWorkerSlot(slot, false, true);
                     });

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
                                              &g_ActiveSession, &g_Pool]() {
        if (!g_ActiveRelay.isNull() || !g_ActiveMediaTrackRelay.isNull() ||
            !g_ActiveStreamRelay.isNull() || !g_ActiveRelayRoot.isNull() ||
            !g_ActiveSession.isNull())
            return true;
        for (const auto& sl : g_Pool)
            if (!sl.worker.isNull()) return true;
        return false;
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
         &g_ActiveSession, &g_ActiveClientUniqueId, &g_ActiveHostUuid, &g_Pool]() {
            qInfo() << "[main] Streaming session revoked — tearing down active stream";
            bool relayStopped = false;

            // Worker-mode sessions: notify + teardown each slot's child. The
            // slot's normal ended handler sends the Sunshine /cancel and clears
            // the streaming flag.
            for (int i = 0; i < g_Pool.size(); ++i) {
                auto* w = g_Pool.workerAs<StreamWorkerHost>(i);
                if (!w) continue;
                qInfo() << "[main] Revoke teardown: stopping stream worker (uid="
                        << g_Pool.at(i).clientUniqueId << ")";
                w->notifyRevoked();
                relayStopped = true;
            }

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
    UpdateChecker updateChecker(QCoreApplication::applicationVersion(),
                                appSettings.updateRelayEnabled());
    SelfUpdater selfUpdater(&updateChecker);

    // Keep the release cache warm on our own clock instead of on client traffic.
    // statusJson() is stale-while-revalidate: the caller that finds the cache
    // stale gets update_available=false and only *then* kicks the fetch. A host
    // that is polled rarely — or a page that asks once, right after boot — would
    // therefore keep seeing "no update" long after a release landed.
    QTimer updatePollTimer;
    updatePollTimer.setInterval(UpdateChecker::kCacheHours * 3600 * 1000);
    QObject::connect(&updatePollTimer, &QTimer::timeout, &updateChecker, &UpdateChecker::refresh);
    updatePollTimer.start();
    // First fetch shortly after boot: late enough for the network stack (and any
    // ACME/DNS work above) to be up, early enough to precede the first client.
    QTimer::singleShot(5000, &updateChecker, &UpdateChecker::refresh);

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
                    if (CertManager::certMatchesDomain(certs.first(), domain)) {
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
    // the background without blocking this handler. `self_update` describes what
    // this host can do about it, so the web app can offer a one-click update (or
    // explain that the host must be updated by hand) without a second round-trip.
    server.router()->get("/api/update/check", [&updateChecker, &selfUpdater](const HttpRequest&) {
        QJsonObject obj = updateChecker.statusJson();
        obj["self_update"] = selfUpdater.capabilityJson();
        return HttpResponse::json(obj);
    });

    // POST /api/update/start — download and apply the update on the host, then
    // relaunch. Session-authenticated but deliberately NOT localhost-gated: the
    // point is to update the host from wherever the user happens to be.
    server.router()->post("/api/update/start", [&selfUpdater](const HttpRequest&) {
        const QString err = selfUpdater.start();
        if (!err.isEmpty()) return HttpResponse::error(409, err);
        return HttpResponse::json(selfUpdater.statusJson(), 202);
    });

    // GET /api/update/status — progress of the update started above. Polling
    // stops answering once the installer takes this process down; the client
    // then waits for the new build on /api/health (see HostListView).
    server.router()->get("/api/update/status", [&selfUpdater](const HttpRequest&) {
        return HttpResponse::json(selfUpdater.statusJson());
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
        // Session sharing is a build-time switch; the frontend hides every
        // entry point when it is off.
        obj["sharing"] = ShareManager::kSessionSharingEnabled;
        return HttpResponse::json(obj);
    });

    registerHostRoutes(server, computerManager);

    // Phase 5: Start streaming — launch app + RTSP handshake
    auto effectiveUpnpEnabled = upnpEnabled; // Capture by value for the lambda

    server.router()->postAsync("/api/hosts/:id/start", [&computerManager, signalingPort,
                                                        &g_ActiveRelay, &g_ActiveStreamRelay,
                                                        &g_ActiveMediaTrackRelay, &g_ActiveSession,
                                                        &g_ActiveRelayRoot, &g_ActiveClientUniqueId,
                                                        &g_ActiveHostUuid, &g_HostAspect, &g_Pool,
                                                        &g_DualSupport, &g_LastStandbyStartMs,
                                                        &g_LiveSunshineUids, &detachWorkerSlot,
                                                        &slotSignalingPort, &slotWsPath,
                                                        &anyOtherSlotLive, &server, &appSettings,
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

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();

        // ── Worker mode + slot selection ───────────────────────────────────────
        // stream_worker_enabled (default): sessions run in --stream-worker child
        // processes, allowing TWO concurrent sessions. A standby launch
        // (standby:true + session_slot) is the second leg of the frontend's
        // seamless quality switching: it must NOT take over the live stream.
        const bool workerMode = appSettings.streamWorkerEnabled();
        const bool standby = workerMode && body["standby"].toBool(false);

        // Slot selection.
        //
        // The owner pair (0 = primary, 1 = standby) is what the frontend's
        // seamless quality switching drives, so a standby launch still names
        // its own slot and a fresh launch still lands on 0 — that is the whole
        // single-owner flow, unchanged.
        //
        // What changes: slot 0 is only claimed when it is free or already on
        // THIS host. A device asking for a different host while someone streams
        // another one gets a slot of its own instead of evicting them. Both
        // sessions then run side by side, each with its own ports and /wsN.
        int reqSlot = standby ? qBound(0, body["session_slot"].toInt(1), 1) : 0;

        // A multi-user backend gives each device its own identity and its own
        // session upstream — Wolf pairs a certificate per device — so one
        // device's launch must not evict another's. Same-device take-over is
        // untouched: the frontend relies on it for quality switches and
        // transport fallback.
        bool backendIsMultiUser = false;
        if (auto probe = computerManager.backendForHost(uuid)) {
            backendIsMultiUser = probe->capabilities().multiUser;
        }
        // The pool stores the raw request value, so compare against the same.
        const QString reqDevice = body["client_uniqueid"].toString();
        auto slotHeldByAnotherDevice = [&](int i) {
            return backendIsMultiUser && g_Pool.live(i) && g_Pool.at(i).hostUuid == uuid &&
                   !g_Pool.at(i).clientUniqueId.isEmpty() &&
                   g_Pool.at(i).clientUniqueId != reqDevice;
        };

        if (!standby && workerMode) {
            const bool slot0Busy = (g_Pool.live(0) && !g_Pool.at(0).hostUuid.isEmpty() &&
                                    g_Pool.at(0).hostUuid != uuid) ||
                                   slotHeldByAnotherDevice(0);
            if (slot0Busy) {
                // Already streaming this host from this browser? Reuse that slot
                // rather than opening a second one for the same viewer.
                int mine = g_Pool.indexOfClientUniqueId(body["client_uniqueid"].toString());
                if (mine < 0 || g_Pool.at(mine).hostUuid != uuid) mine = -1;
                reqSlot = mine >= 0 ? mine : g_Pool.acquire();
                if (reqSlot < 0) {
                    respond(HttpResponse::error(
                        503, "All stream slots are busy — stop a running session first"));
                    return;
                }
                qInfo() << "[Session] Host" << uuid << "differs from slot 0 ("
                        << g_Pool.at(0).hostUuid << ") — using slot" << reqSlot;
            }
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
        //
        // Worker mode: a fresh (non-standby) launch keeps the same take-over
        // semantics — every slot's child is torn down (no Sunshine /cancel, the
        // newcomer /resumes). A STANDBY launch tears down only its own slot's
        // remnant and leaves the live stream untouched.
        //
        // Player slots (2..4) are never touched here: an owner relaunch — a
        // quality switch, a transport fallback — must leave invited players
        // streaming. Only an explicit Stop ends their session.
        StreamWorkerHost* previousWorker = nullptr;
        if (standby) {
            previousWorker = detachWorkerSlot(reqSlot, false);
        } else {
            for (int i = 0; i < kOwnerSlots; ++i) {
                // Take over only what streams the SAME host. A session on
                // another host is somebody else's — evicting it was the old
                // one-stream-at-a-time rule, not something callers want.
                const QString slotHost = g_Pool.at(i).hostUuid;
                if (g_Pool.live(i) && !slotHost.isEmpty() && slotHost != uuid) continue;
                // Same host, but another device on a multi-user backend: it owns
                // a separate session upstream, so tearing it down would be taking
                // over a stranger's stream rather than reclaiming our own.
                if (slotHeldByAnotherDevice(i)) continue;
                StreamWorkerHost* w = detachWorkerSlot(i, true);
                // Serialize the new worker behind whichever child still holds
                // the ports for the slot we are about to use.
                if (i == reqSlot) previousWorker = w;
            }
            // Slot claimed above kOwnerSlots (a second host): only its own
            // remnant is in the way.
            if (reqSlot >= kOwnerSlots) previousWorker = detachWorkerSlot(reqSlot, true);
        }
        if (!standby && g_ActiveRelay) {
            DataChannelRelay* old = g_ActiveRelay;
            g_ActiveRelay = nullptr;
            qInfo() << "[Session] Take-over: tearing down active DataChannelRelay" << old;
            if (old->isConnected()) old->notifyClientTakenOver();
            QObject::disconnect(old, &DataChannelRelay::sessionEnded, nullptr, nullptr);
            if (old->moonlightShim()) old->moonlightShim()->stopConnection();
            old->stop();
            old->deleteLater();
        }
        if (!standby && g_ActiveMediaTrackRelay) {
            MediaTrackRelay* old = g_ActiveMediaTrackRelay;
            g_ActiveMediaTrackRelay = nullptr;
            qInfo() << "[Session] Take-over: tearing down active MediaTrackRelay" << old;
            if (old->isConnected()) old->notifyClientTakenOver();
            QObject::disconnect(old, &MediaTrackRelay::sessionEnded, nullptr, nullptr);
            if (old->moonlightShim()) old->moonlightShim()->stopConnection();
            old->stop();
            old->deleteLater();
        }
        if (!standby && g_ActiveStreamRelay) {
            StreamRelay* old = g_ActiveStreamRelay;
            g_ActiveStreamRelay = nullptr;
            qInfo() << "[Session] Take-over: tearing down active StreamRelay" << old;
            if (old->isClientConnected()) old->notifyClientTakenOver();
            QObject::disconnect(old, &StreamRelay::sessionEnded, nullptr, nullptr);
            if (old->moonlightShim()) old->moonlightShim()->stopConnection();
            old->stop();
            old->deleteLater();
        }
        if (!standby) {
            g_ActiveSession = nullptr;
            g_ActiveClientUniqueId.clear();
            g_ActiveHostUuid.clear();
        }

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
        // the host's actual screen format so a non-16:9 host streams without the
        // black bars Sunshine would otherwise encode into the picture.
        //
        // The browser always sends a concrete "W:H" here: its Settings override,
        // or the ratio its AspectProbe measured from those very bars at stream
        // start (see frontend/js/stream/AspectProbe.js). Sunshine's serverinfo
        // carries no DisplayMode at all, so the "auto" branch below is only a
        // last-resort net for a backend that does report one — on Sunshine it
        // falls through to 16:9.
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
        // Players joining this host's share inherit it (see g_HostAspect).
        if (reqAspect.contains(':')) g_HostAspect[host->uuid] = reqAspect;

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

        // Is the streaming client inside a network we control? True for
        // loopback / RFC1918, which also covers a LAN client reaching us
        // through the public URL: the router source-NATs the hairpinned
        // connection to a private gateway IP (e.g. 192.168.1.254). Also true
        // for the tunnel range — a mesh VPN peer is inside our own network, and
        // suppressing its host candidate is what silently forced the media back
        // out to the public IP. When true, the relay may advertise its private
        // host ICE candidate so a local client can connect directly (routers
        // rarely hairpin UDP); never for a public peer, so the LAN IP is not
        // leaked.
        const bool clientIsLocal =
            NetClassify::isTrustedPeer(NetClassify::classify(req.clientAddress));

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
        // hostSupportsCodec / filterTransportsByCodec are file-level helpers so
        // a player's join builds its chain with the very same rules.

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
            // Which provider drives this host: plain GameStream unless it was
            // registered as a Wolf or MultiSeat backend.
            s->setBackend(std::shared_ptr<IStreamBackend>(
                computerManager.backendForHost(host->uuid).release()));
            attachRelayTracking(s);
            // Track the active session so a later take-over can quit() it (stops
            // the SignalingServer first → frees the fixed port). QPointer auto-
            // nulls when the session self-destructs on normal end.
            g_ActiveSession = s;
            return s;
        };

        // ═════════════════════════════════════════════════════════════════════
        // Worker mode: hand the whole session (Sunshine launch + relay graph +
        // moonlight-common-c) to a --stream-worker child process bound to the
        // requested slot. Falls back to the in-process path when the child
        // cannot be spawned (never for a standby attempt — that one simply
        // reports dual_unavailable and the frontend keeps the legacy relaunch).
        // ═════════════════════════════════════════════════════════════════════
        if (workerMode) {
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

            QJsonObject cfg;
            cfg["hostAddress"] = host->activeAddress.address();
            cfg["hostPort"] = static_cast<int>(host->activeAddress.port());
            cfg["hostHttpsPort"] = static_cast<int>(host->activeHttpsPort);
            cfg["hostName"] = host->name;
            cfg["hostUuid"] = host->uuid;
            cfg["appVersion"] = host->appVersion;
            cfg["gfeVersion"] = host->gfeVersion;
            cfg["serverCodecModeSupport"] = host->serverCodecModeSupport;
            // Which provider the worker should launch through. Empty means a
            // plain GameStream host, which is what the worker defaults to.
            cfg["backendType"] = host->backendType;
            cfg["backendApiUrl"] = host->backendApiUrl;
            cfg["backendApiToken"] = host->backendApiToken;
            // The worker rebuilds a bare NvComputer, and a backend refuses to
            // dial a host it believes unpaired. The pair state and the server
            // certificate have to travel with it.
            cfg["hostPairState"] = NvComputer::pairStateToString(host->pairState);
            cfg["hostServerCert"] = QString::fromUtf8(host->serverCertPem);
            cfg["appId"] = appId;
            cfg["codec"] = static_cast<int>(effectiveCodec);
            cfg["codecOverridden"] = codecOverridden;
            cfg["originalCodec"] = static_cast<int>(originalCodec);
            cfg["gamingMode"] = reqGamingMode;
            // UPnP is consent-gated: a router mapping only serves a peer coming
            // from the internet, and internet_access_enabled is the user's
            // answer to exactly that question. Read live (not baked into
            // effectiveUpnpEnabled at boot) so flipping the admin toggle takes
            // effect on the very next stream, without a restart. A LAN-only
            // user who declined keeps streaming — same-subnet peers connect on
            // the host candidate, the router is never in the path.
            cfg["upnpEnabled"] = effectiveUpnpEnabled && appSettings.internetAccessEnabled();
            cfg["internalTransport"] = internalTransport;
            cfg["transportMode"] = chainMode;
            cfg["stunServer"] = stunServer;
            cfg["height"] = reqHeight;
            cfg["width"] = reqWidth;
            cfg["fps"] = reqFps;
            cfg["bitrateKbps"] = reqBitrate;
            cfg["yuv444"] = reqYuv444;
            cfg["hdr"] = reqHdr;
            cfg["iceTcp"] = enableIceTcp;
            cfg["lowAudio"] = reqLowAudio;
            cfg["muteHostAudio"] = reqMuteHost;
            cfg["clientUniqueId"] = reqClientUniqueId;
            cfg["clientIsLocal"] = clientIsLocal;
            cfg["autoMode"] = true;
            // Straight to /resume when joining a live app session: every
            // standby joins the running app by definition, and any uid we know
            // to be live resumes its own session (Sunshine rejects /launch
            // while an app runs; the launch↔resume self-heal stays as backup).
            cfg["preferResume"] = standby || g_LiveSunshineUids.contains(reqClientUniqueId);
            cfg["serverHost"] = serverHost;
            cfg["serverHttpsPort"] = static_cast<int>(server.activeHttpsPort());
            // Every slot owns a port pair so all children can listen at once
            // (HttpServer proxies /ws1../ws4 to them).
            cfg["signalingPort"] = static_cast<int>(slotSignalingPort(reqSlot));
            cfg["streamRelayPort"] = static_cast<int>(slotSignalingPort(reqSlot) + 1);
            cfg["mediaPort"] = static_cast<int>(kMediaBasePort + reqSlot);
            cfg["wsPath"] = slotWsPath(reqSlot);
            QJsonArray chainArr;
            for (const QString& m : transportChain)
                chainArr.append(m);
            cfg["transportChain"] = chainArr;
            cfg["transportIndex"] = reqTransportIndex;

            auto* worker = new StreamWorkerHost(qApp);
            // The child self-deletes only once its process is gone — destroyed()
            // is the barrier a queued same-slot start waits on (ports are free).
            QObject::connect(worker, &StreamWorkerHost::exited, worker, &QObject::deleteLater);

            const QString hostUuidCopy = host->uuid;
            const QString uid = reqClientUniqueId;
            QObject::connect(
                worker, &StreamWorkerHost::responseReady, qApp,
                [respond, worker, &g_Pool, &g_DualSupport, &g_LiveSunshineUids, &authManager,
                 reqSlot, standby, hostUuidCopy, uid, sessionToken](int code, QJsonObject bodyObj) {
                    const bool ok =
                        code == 200 && bodyObj["status"].toString() == QLatin1String("streaming");
                    if (ok) {
                        bodyObj["slot"] = reqSlot;
                        bodyObj["dual_supported"] = g_DualSupport.value(hostUuidCopy, true);
                        authManager.setSessionStreaming(sessionToken, true);
                        g_LiveSunshineUids.insert(uid);
                        // A successful standby launch IS the capability probe.
                        if (standby) g_DualSupport[hostUuidCopy] = true;
                        respond(HttpResponse::json(bodyObj, 200));
                        return;
                    }
                    if (standby) {
                        // Probe outcome: the host refused (or failed) a second
                        // concurrent session. Report it as a distinct status —
                        // NOT an HTTP error — so the frontend can fall back to
                        // the legacy relaunch without treating it as a failed
                        // launch, and remember the answer for this host.
                        g_DualSupport[hostUuidCopy] = false;
                        QJsonObject r;
                        r["status"] = QStringLiteral("dual_unavailable");
                        r["reason"] = bodyObj.contains("error")
                                          ? bodyObj["error"].toString()
                                          : QStringLiteral("code %1").arg(code);
                        SessionPool::Slot& sl = g_Pool.at(reqSlot);
                        if (sl.worker == worker) {
                            sl.worker = nullptr;
                            sl.clientUniqueId.clear();
                            sl.hostUuid.clear();
                            sl.sessionToken.clear();
                        }
                        respond(HttpResponse::json(r, 200));
                        return;
                    }
                    respond(HttpResponse::json(bodyObj, code));
                });

            QObject::connect(
                worker, &StreamWorkerHost::ended, qApp,
                [worker, &g_Pool, &g_DualSupport, &g_LastStandbyStartMs, &g_LiveSunshineUids,
                 &anyOtherSlotLive, &computerManager, &authManager, reqSlot, host, hostUuidCopy,
                 uid, sessionToken]() {
                    qInfo() << "[main] Stream worker ended (slot" << reqSlot << ", uid=" << uid
                            << ")";
                    // Pathological probe outcome: the LIVE stream died within
                    // ~3s of a standby launch — the host "took over" instead of
                    // adding a session. Remember no-dual for this host.
                    if (reqSlot == 0 && g_LastStandbyStartMs > 0 &&
                        QDateTime::currentMSecsSinceEpoch() - g_LastStandbyStartMs < 3000) {
                        qWarning() << "[main] Live stream died right after a standby launch — "
                                      "marking host no-dual";
                        g_DualSupport[hostUuidCopy] = false;
                    }
                    authManager.setSessionStreaming(sessionToken, false);
                    // Best-effort Sunshine /cancel (mirrors the legacy
                    // sessionEnded auto-quit) — but ONLY when no sibling slot
                    // is still streaming: Sunshine sessions share the running
                    // app, and a /cancel would terminate it for the survivor.
                    const bool siblingLive = anyOtherSlotLive(reqSlot, hostUuidCopy);
                    if (!siblingLive) {
                        auto* identity = IdentityManager::get();
                        auto* quitReply = computerManager.http()->quitAppAsync(
                            host->activeAddress, host->activeHttpsPort, identity->getCertificate(),
                            identity->getPrivateKey(), uid);
                        // Report the outcome. This /cancel used to be pure
                        // fire-and-forget, so a refused or failed one left
                        // Sunshine holding the app with nothing in the log to
                        // say so — and the next /launch (a transport fallback,
                        // typically) stalled against a host we believed free.
                        QObject::connect(quitReply, &QNetworkReply::finished, quitReply,
                                         [quitReply] {
                                             quitReply->deleteLater();
                                             if (quitReply->error() != QNetworkReply::NoError)
                                                 qWarning() << "[main] Sunshine /cancel failed:"
                                                            << quitReply->errorString();
                                             else
                                                 qInfo() << "[main] Sunshine /cancel OK, body="
                                                         << quitReply->readAll().left(200);
                                         });
                        g_LiveSunshineUids.remove(uid);
                    } else {
                        qInfo() << "[main] Sibling slot still streaming — skipping Sunshine "
                                   "/cancel (shared app session)";
                    }
                    SessionPool::Slot& sl = g_Pool.at(reqSlot);
                    if (sl.worker == worker) {
                        sl.worker = nullptr;
                        sl.clientUniqueId.clear();
                        sl.hostUuid.clear();
                        sl.sessionToken.clear();
                    }
                });

            auto startWorker = [worker, cfg, respond, standby, reqSlot, appId, &g_Pool,
                                &g_LastStandbyStartMs, hostUuidCopy, uid, sessionToken]() {
                if (!worker->start(cfg)) {
                    worker->deleteLater();
                    // Spawn failure. For a standby attempt just report
                    // dual_unavailable; a primary launch has already torn the
                    // old session down, so surface the error (the in-process
                    // fallback would need the whole legacy block — the frontend
                    // retries and the next attempt hits the same code path).
                    QJsonObject r;
                    if (standby) {
                        r["status"] = QStringLiteral("dual_unavailable");
                        r["reason"] = QStringLiteral("worker spawn failed");
                        respond(HttpResponse::json(r, 200));
                    } else {
                        respond(HttpResponse::error(500, "Failed to spawn stream worker"));
                    }
                    return;
                }
                SessionPool::Slot& sl = g_Pool.at(reqSlot);
                sl.worker = worker;
                sl.clientUniqueId = uid;
                sl.hostUuid = hostUuidCopy;
                sl.sessionToken = sessionToken;
                sl.appId = appId;
                // Outlives the slot — see the declaration.
                g_LastOwnerHostUuid = hostUuidCopy;
                g_LastOwnerAppId = appId;
                if (standby) g_LastStandbyStartMs = QDateTime::currentMSecsSinceEpoch();
            };

            // Serialize with whatever still holds this slot's ports: a previous
            // worker child (wait for its process to die) or a legacy in-process
            // relay (wait for its destruction) — same rationale as the deferred
            // start below.
            if (previousWorker) {
                qInfo() << "[Session] Previous slot" << reqSlot
                        << "worker still tearing down — deferring worker start";
                QObject::connect(previousWorker, &QObject::destroyed, qApp, startWorker);
            } else if (reqSlot == 0 && g_ActiveRelayRoot) {
                qInfo() << "[Session] Previous in-process relay still tearing down — "
                           "deferring worker start";
                QObject::connect(g_ActiveRelayRoot, &QObject::destroyed, qApp, startWorker);
            } else {
                startWorker();
            }
            return;
        }

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
         &g_ActiveSession, &g_ActiveClientUniqueId, &g_ActiveHostUuid, &g_Pool,
         &g_LiveSunshineUids, &detachWorkerSlot,
         &endPlayerSessions](const HttpRequest& req, const ResponseCallback& respond) {
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
            QJsonObject qbody = QJsonDocument::fromJson(req.body).object();
            QString quitUniqueId;
            for (const QChar& c : qbody["client_uniqueid"].toString()) {
                QChar u = c.toUpper();
                if (u.isDigit() || (u >= 'A' && u <= 'F')) quitUniqueId += u;
                if (quitUniqueId.size() >= 32) break;
            }

            // ── Worker-mode sessions ───────────────────────────────────────
            // session_slot present → tear down ONLY that slot's child (the
            // frontend's seamless switcher retires one leg of the dual stream
            // while the other keeps playing). Absent → legacy semantics: stop
            // every slot this uniqueid owns (empty ids keep legacy behaviour).
            // keep_host_session: retire = disconnect only, NO Sunshine /cancel
            // — sessions share the running app, a /cancel would kill the
            // surviving leg's stream too.
            const int quitSlot = qbody.contains("session_slot")
                                     ? qBound(0, qbody["session_slot"].toInt(0), kOwnerSlots - 1)
                                     : -1;
            const bool keepHostSession = qbody["keep_host_session"].toBool(false);

            // The authenticated device asking to quit, read the same way /start
            // records it. Empty for localhost.
            QString quitSessionToken;
            {
                const QString cookie = req.headers.value("cookie");
                for (const QString& c : cookie.split(';')) {
                    const QString t = c.trimmed();
                    if (t.startsWith("mw_session=", Qt::CaseInsensitive)) {
                        quitSessionToken = t.mid(QStringLiteral("mw_session=").length());
                        break;
                    }
                }
            }

            // Anything that is not a retire ends with a Sunshine /cancel, which
            // kills the app for everyone — so the invited players go first.
            // Their workers must be gone before the owner's last slot ends, or
            // anyOtherSlotLive() would suppress that /cancel and leave the game
            // running with nobody attached. (A retire — keep_host_session — is
            // one leg of the owner's dual stream stepping aside; the app, and
            // every player on it, carries on.)
            if (!keepHostSession) endPlayerSessions(ShareManager::EndReason::OwnerStop);

            bool workerStopped = false;
            for (int i = 0; i < kOwnerSlots; ++i) {
                SessionPool::Slot& sl = g_Pool.at(i);
                if (!sl.worker) continue;
                if (quitSlot >= 0 && i != quitSlot) continue;
                const bool slotOwned = sl.clientUniqueId.isEmpty() || quitUniqueId.isEmpty() ||
                                       quitUniqueId == sl.clientUniqueId;
                if (!slotOwned) {
                    qInfo() << "[quit] Slot" << i << "owned by another uniqueid — skipping";
                    continue;
                }
                // Second, stronger guard: the authenticated device. A browser
                // picks its own uniqueid, so it alone cannot keep one device
                // from ending another's stream once sessions are independent.
                // Either side empty means "unknown" (localhost has no session
                // row), which keeps today's single-owner behaviour intact.
                if (!quitSessionToken.isEmpty() && !sl.sessionToken.isEmpty() &&
                    quitSessionToken != sl.sessionToken) {
                    qInfo() << "[quit] Slot" << i << "belongs to another device session — skipping";
                    continue;
                }
                qInfo() << "[quit] Stopping stream worker slot" << i
                        << (keepHostSession ? "(keeping host session)" : "");
                // Suppressed ended-cleanup: this handler owns the Sunshine
                // /cancel decision (keyed by quitUniqueId below; none at all
                // when retiring).
                detachWorkerSlot(i, false);
                workerStopped = true;
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

            if (!relayStopped && !workerStopped) {
                qInfo() << "[quit] No active relay (already stopped or never started)";
            }

            // Retire (keep_host_session): the transport is closed, the Sunshine
            // app session stays alive for the surviving leg — respond directly.
            if (keepHostSession) {
                qInfo() << "[quit] EXIT — retired without Sunshine /cancel";
                respond(HttpResponse::json(QJsonObject{{"status", "quit"}}));
                return;
            }

            qInfo() << "[quit] Sending quitAppAsync to Sunshine ...";
            g_LiveSunshineUids.remove(quitUniqueId);
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

    // POST /api/hosts/:id/stop-session — the escape hatch.
    //
    // The normal Stop is scoped to a browser's own session: it needs a live view
    // that knows its slot and uniqueid. When the owner cannot get back to their
    // stream — tab killed, machine rebooted, players holding the session open —
    // nothing in the UI can end the app any more, and Sunshine keeps it running.
    // This one is deliberately unscoped: it drops every slot and cancels every
    // Sunshine session we know to be live, whoever started it.
    server.router()->postAsync(
        "/api/hosts/:id/stop-session",
        [&computerManager, &g_Pool, &g_LiveSunshineUids, &detachWorkerSlot,
         &endPlayerSessions](const HttpRequest& req, const ResponseCallback& respond) {
            NvComputer* host = computerManager.getHost(req.pathParams.value("id"));
            if (!host) {
                respond(HttpResponse::error(404, "Host not found"));
                return;
            }

            qInfo() << "[stop-session] Forcing the host session down";
            endPlayerSessions(ShareManager::EndReason::OwnerStop);
            for (int i = 0; i < kOwnerSlots; ++i)
                detachWorkerSlot(i, false);

            // Sunshine's /cancel is keyed by the uniqueid that launched the
            // session, so every live one has to be named. The set is emptied
            // either way: whatever is left after this is not ours any more. A
            // trailing empty-uid /cancel sweeps a session this process never saw
            // (a previous run, a crash) — the case the button exists for.
            QStringList cancelQueue;
            for (const QString& uid : g_LiveSunshineUids)
                cancelQueue.append(uid);
            cancelQueue.append(QString()); // unscoped sweep, always last
            g_LiveSunshineUids.clear();
            auto* identity = IdentityManager::get();

            // Send the cancels SEQUENTIALLY, and only report "stopped" once the
            // last one has come back. Sunshine's HTTPS server is single-threaded
            // and its session teardown is not instant: firing the cancels
            // concurrently races that teardown, so a leg can return before the
            // app is actually gone. The frontend then re-enables the tile and the
            // next /launch hits a still-running session — which Sunshine answers
            // by blocking, not erroring, so it stalls for the full launch timeout.
            // Serialising and awaiting means the host is genuinely free before the
            // caller can relaunch.
            auto sendNext = std::make_shared<std::function<void()>>();
            *sendNext = [&computerManager, host, identity, cancelQueue, sendNext, respond,
                         idx = 0]() mutable {
                if (idx >= cancelQueue.size()) {
                    respond(HttpResponse::json(QJsonObject{{"status", QStringLiteral("stopped")}}));
                    return;
                }
                const QString uid = cancelQueue.at(idx++);
                auto* reply = computerManager.http()->quitAppAsync(
                    host->activeAddress, host->activeHttpsPort, identity->getCertificate(),
                    identity->getPrivateKey(), uid);
                QObject::connect(reply, &QNetworkReply::finished, reply, [reply, sendNext]() {
                    reply->deleteLater();
                    (*sendNext)();
                });
            };
            (*sendNext)();
        });

    // ── Session sharing: the player side of the stream lifecycle ───────────
    // A player never launches anything: they resume into the app the owner is
    // already streaming, on a slot of their own. Everything that is not the
    // resolution is decided here — the browser on the other end is a stranger's.

    // The owner's live context (host + app), taken from whichever owner slot is
    // up. Empty uuid means nothing is running and no player can join.
    auto ownerContext = [&g_Pool, &g_ActiveHostUuid]() {
        for (int i = 0; i < kOwnerSlots; ++i)
            if (g_Pool.at(i).worker && !g_Pool.at(i).hostUuid.isEmpty())
                return std::pair<QString, int>{g_Pool.at(i).hostUuid, g_Pool.at(i).appId};
        // No owner leg left — they pressed Leave, or their stream dropped. The
        // app is still on Sunshine, so fall back to what it was.
        if (!g_LastOwnerHostUuid.isEmpty())
            return std::pair<QString, int>{g_LastOwnerHostUuid, g_LastOwnerAppId};
        return std::pair<QString, int>{g_ActiveHostUuid, 0};
    };

    // Is there a Sunshine app a player could resume into? Not "is the owner
    // watching": Leave (keep_host_session) takes the owner's leg down without a
    // /cancel exactly so the guests carry on, and a guest handed a link before
    // that must still be able to use it. g_LiveSunshineUids is the registry of
    // sessions we hold and have not cancelled, so it answers the question the
    // owner's slots cannot.
    auto ownerStreamAlive = [&g_Pool, &g_LiveSunshineUids, &g_ActiveRelay,
                             &g_ActiveMediaTrackRelay, &g_ActiveStreamRelay]() {
        for (int i = 0; i < kOwnerSlots; ++i)
            if (!g_Pool.at(i).worker.isNull()) return true;
        if (!g_LiveSunshineUids.isEmpty()) return true;
        // Legacy in-process path (stream_worker_enabled off).
        return !g_ActiveRelay.isNull() || !g_ActiveMediaTrackRelay.isNull() ||
               !g_ActiveStreamRelay.isNull();
    };

    ShareRoutesDeps shareDeps;
    shareDeps.ownerStreamAlive = ownerStreamAlive;
    shareDeps.currentOwnerContext = ownerContext;
    shareDeps.machineName = []() {
#ifdef Q_OS_WIN
        wchar_t buf[256];
        DWORD sz = static_cast<DWORD>(sizeof(buf) / sizeof(wchar_t));
        if (GetComputerNameW(buf, &sz)) return QString::fromWCharArray(buf, static_cast<int>(sz));
        return qEnvironmentVariable("COMPUTERNAME", QStringLiteral("PC"));
#else
        return QHostInfo::localHostName();
#endif
    };
    shareDeps.publicOrigin = [&internetAccess, &appSettings, &server]() -> QString {
        // Same bar as the Desktop shortcut: the domain is only worth handing out
        // once it is published AND covered by a real certificate. Otherwise the
        // caller falls back to the LAN address — never loopback, the player is
        // on another machine.
        if (!internetAccess.isActive() || internetAccess.domain().isEmpty() ||
            internetAccess.certificateIssuing() || appSettings.certPem().isEmpty())
            return {};
        quint16 p = internetAccess.externalHttpsPort();
        if (p == 0) p = server.activeHttpsPort();
        return p == 443 ? QStringLiteral("https://%1").arg(internetAccess.domain())
                        : QStringLiteral("https://%1:%2").arg(internetAccess.domain()).arg(p);
    };
    shareDeps.stopPlayerStream = [&g_Pool, &detachWorkerSlot,
                                  &shareManager](int slot, bool notifyEnded) {
        if (slot < kOwnerSlots || slot >= kTotalSlots) return;
        if (!g_Pool.at(slot).worker) return;
        detachWorkerSlot(slot, false, notifyEnded);
        // detachWorkerSlot severs the worker's ended() handlers, so the state
        // has to be settled here: without it a player who left would stay
        // "streaming" forever and never be able to rejoin their own slot.
        shareManager.setStreaming(slot, false);
    };
    shareDeps.startPlayerStream =
        [&computerManager, &g_Pool, &g_LiveSunshineUids, &shareManager, &detachWorkerSlot,
         &anyOtherSlotLive, &slotSignalingPort, &slotWsPath, &server, &appSettings, signalingPort,
         stunServer, &g_HostAspect](int slot, int height, QString aspect,
                                    ShareManager::Permissions perms, QString serverHost,
                                    ResponseCallback respond) {
            // The share is bound to ONE host at activation (ShareManager::activate).
            // A player is routed there and nowhere else — never to "whichever owner
            // slot is up", which is what let a link minted on one host reach a
            // different machine. Resolve the bound host, then require the owner to
            // actually be streaming it right now.
            const QString hostUuid = shareManager.hostForSlot(slot);
            if (hostUuid.isEmpty()) {
                respond(HttpResponse::json(QJsonObject{{"error", "session_ended"}}, 409));
                return;
            }
            int appId = shareManager.appForSlot(slot);
            bool ownerOnBoundHost = false;
            for (int i = 0; i < kOwnerSlots; ++i)
                if (g_Pool.at(i).worker && g_Pool.at(i).hostUuid == hostUuid) {
                    appId = g_Pool.at(i).appId; // the app currently up on that host
                    ownerOnBoundHost = true;
                    break;
                }
            // Owner pressed Leave but kept the Sunshine session alive for guests:
            // the owner's leg is gone, the session on the bound host is not.
            if (!ownerOnBoundHost && g_LastOwnerHostUuid == hostUuid &&
                !g_LiveSunshineUids.isEmpty()) {
                if (appId < 0) appId = g_LastOwnerAppId;
                ownerOnBoundHost = true;
            }
            NvComputer* host =
                ownerOnBoundHost ? computerManager.getHost(hostUuid) : nullptr;
            if (!host) {
                respond(HttpResponse::json(QJsonObject{{"error", "session_ended"}}, 409));
                return;
            }

            // A player joining twice replaces its own worker, never anyone
            // else's — the slot is theirs alone. (The join route refuses while
            // the slot is streaming, so this only ever collects a remnant; the
            // state is settled anyway since detaching severs ended().)
            StreamWorkerHost* previousWorker = detachWorkerSlot(slot, false);
            if (previousWorker) shareManager.setStreaming(slot, false);

            // Fixed profile: 60 fps, SDR, 4:2:0, HEVC→H.264 (Auto never resolves
            // to AV1). The width follows the HOST's screen aspect ("W:H", 16:9
            // fallback), which the owner's session already worked out for this
            // host — a guest has no way to know it and its own monitor is beside
            // the point. The bitrate is the standard auto estimate for the
            // height, halved — a guest should not eat the whole uplink.
            if (!aspect.contains(':')) aspect = g_HostAspect.value(hostUuid);
            double aspectRatio = 16.0 / 9.0;
            if (aspect.contains(':')) {
                const QStringList parts = aspect.split(':');
                int aw = parts.value(0).toInt(), ah = parts.value(1).toInt();
                if (aw > 0 && ah > 0) aspectRatio = static_cast<double>(aw) / ah;
            }
            const int width = static_cast<int>(height * aspectRatio + 0.5) & ~1;
            const int autoKbps =
                qRound(20000.0 * (static_cast<double>(height) * height) / (1080.0 * 1080.0));
            const int bitrateKbps = qMax(1000, autoKbps / 2);

            // Players always get the enhancement-aware ordering: their profile
            // runs SGSR1 until it proves too expensive (frontend governor).
            const QStringList chain = filterTransportsByCodec(
                TransportPriorities::orderedTransports(true), VideoCodec::Auto, host);
            if (chain.isEmpty()) {
                respond(HttpResponse::error(502, "No usable transport"));
                return;
            }

            // Its own Sunshine identity: a distinct uniqueid is what makes
            // Sunshine build a separate session_t instead of reassigning the
            // owner's stream. 32 hex chars, like the browser-side ids.
            const QString uid = QUuid::createUuid().toString(QUuid::Id128);

            QJsonObject cfg;
            cfg["hostAddress"] = host->activeAddress.address();
            cfg["hostPort"] = static_cast<int>(host->activeAddress.port());
            cfg["hostHttpsPort"] = static_cast<int>(host->activeHttpsPort);
            cfg["hostName"] = host->name;
            cfg["hostUuid"] = host->uuid;
            cfg["appVersion"] = host->appVersion;
            cfg["gfeVersion"] = host->gfeVersion;
            cfg["serverCodecModeSupport"] = host->serverCodecModeSupport;
            // Which provider the worker should launch through. Empty means a
            // plain GameStream host, which is what the worker defaults to.
            cfg["backendType"] = host->backendType;
            cfg["backendApiUrl"] = host->backendApiUrl;
            cfg["backendApiToken"] = host->backendApiToken;
            // The worker rebuilds a bare NvComputer, and a backend refuses to
            // dial a host it believes unpaired. The pair state and the server
            // certificate have to travel with it.
            cfg["hostPairState"] = NvComputer::pairStateToString(host->pairState);
            cfg["hostServerCert"] = QString::fromUtf8(host->serverCertPem);
            cfg["appId"] = appId;
            cfg["codec"] = static_cast<int>(VideoCodec::Auto);
            cfg["codecOverridden"] = false;
            cfg["originalCodec"] = static_cast<int>(VideoCodec::Auto);
            cfg["gamingMode"] = true;
            cfg["upnpEnabled"] = false; // the owner's session owns the mapping
            cfg["internalTransport"] = chain.first().startsWith(QStringLiteral("webrtc-media"))
                                           ? QStringLiteral("webrtc-media")
                                       : chain.first().startsWith(QStringLiteral("webrtc-dc"))
                                           ? QStringLiteral("webrtc")
                                           : QStringLiteral("wss");
            cfg["transportMode"] = chain.first();
            cfg["stunServer"] = stunServer;
            cfg["height"] = height;
            cfg["width"] = width;
            cfg["fps"] = 60;
            cfg["bitrateKbps"] = bitrateKbps;
            cfg["yuv444"] = false;
            cfg["hdr"] = false;
            cfg["iceTcp"] = chain.first().endsWith(QStringLiteral("-tcp"));
            cfg["lowAudio"] = false;
            cfg["muteHostAudio"] = false;
            cfg["clientUniqueId"] = uid;
            cfg["clientIsLocal"] = false;
            cfg["autoMode"] = true;
            // Never /launch: Sunshine refuses it while an app runs, and a player
            // has no business starting one anyway.
            cfg["preferResume"] = true;
            // The host the player typed, so the signaling URL they get back
            // points at the same place — an empty one made the browser fall
            // back to /ws and land on the owner's signaling server.
            cfg["serverHost"] = serverHost;
            cfg["serverHttpsPort"] = static_cast<int>(server.activeHttpsPort());
            cfg["signalingPort"] = static_cast<int>(slotSignalingPort(slot));
            cfg["streamRelayPort"] = static_cast<int>(slotSignalingPort(slot) + 1);
            cfg["mediaPort"] = static_cast<int>(kMediaBasePort + slot);
            cfg["wsPath"] = slotWsPath(slot);
            // What this player may send. Enforced in the worker, where a forged
            // datachannel message cannot get around it.
            cfg["inputPolicy"] = perms.toJson();
            // Gamepads from different sessions would all arrive as controller 0;
            // offset each player so they land on distinct virtual pads.
            cfg["gamepadOffset"] = slot - kOwnerSlots + 1;
            QJsonArray chainArr;
            for (const QString& m : chain)
                chainArr.append(m);
            cfg["transportChain"] = chainArr;
            cfg["transportIndex"] = 0;

            auto* worker = new StreamWorkerHost(qApp);
            QObject::connect(worker, &StreamWorkerHost::exited, worker, &QObject::deleteLater);

            QObject::connect(worker, &StreamWorkerHost::responseReady, qApp,
                             [respond, &g_LiveSunshineUids, &shareManager, slot,
                              uid](int code, QJsonObject bodyObj) {
                                 const bool ok = code == 200 && bodyObj["status"].toString() ==
                                                                    QLatin1String("streaming");
                                 if (ok) {
                                     bodyObj["slot"] = slot;
                                     g_LiveSunshineUids.insert(uid);
                                     shareManager.setStreaming(slot, true);
                                 }
                                 respond(HttpResponse::json(bodyObj, ok ? 200 : code));
                             });

            QObject::connect(worker, &StreamWorkerHost::ended, qApp,
                             [worker, &g_Pool, &g_LiveSunshineUids, &shareManager,
                              &anyOtherSlotLive, &computerManager, slot, host, uid]() {
                                 qInfo() << "[main] Player worker ended (slot" << slot << ")";
                                 // The share survives the stream: a dropped connection must
                                 // not end the invitation, only an owner action does.
                                 shareManager.setStreaming(slot, false);
                                 // Same rule as the owner slots — the Sunshine app is shared,
                                 // so /cancel only once nothing else is streaming.
                                 if (!anyOtherSlotLive(slot, host->uuid)) {
                                     auto* identity = IdentityManager::get();
                                     auto* quitReply = computerManager.http()->quitAppAsync(
                                         host->activeAddress, host->activeHttpsPort,
                                         identity->getCertificate(), identity->getPrivateKey(),
                                         uid);
                                     QObject::connect(quitReply, &QNetworkReply::finished,
                                                      quitReply, &QNetworkReply::deleteLater);
                                 }
                                 g_LiveSunshineUids.remove(uid);
                                 SessionPool::Slot& sl = g_Pool.at(slot);
                                 if (sl.worker == worker) {
                                     sl.worker = nullptr;
                                     sl.clientUniqueId.clear();
                                     sl.hostUuid.clear();
                                     sl.sessionToken.clear();
                                     sl.appId = 0;
                                 }
                             });

            const QString hostUuidCopy = host->uuid;
            auto startWorker = [worker, cfg, respond, slot, appId, &g_Pool, hostUuidCopy,
                                uid]() {
                if (!worker->start(cfg)) {
                    worker->deleteLater();
                    respond(HttpResponse::error(500, "Failed to spawn stream worker"));
                    return;
                }
                SessionPool::Slot& sl = g_Pool.at(slot);
                sl.worker = worker;
                sl.clientUniqueId = uid;
                sl.hostUuid = hostUuidCopy;
                sl.appId = appId;
            };

            // Wait for this slot's previous child to release its ports.
            if (previousWorker)
                QObject::connect(previousWorker, &QObject::destroyed, qApp, startWorker);
            else
                startWorker();
        };

    registerShareRoutes(server, shareManager, shareDeps);

    // /ws2../ws4 carry a player's video and input. Only the mw_player cookie
    // that matches THAT slot's live activation opens them — the session-cookie
    // check the owner's slots go through would reject every player.
    server.setPlayerSlotAuthorizer([&shareManager](const HttpRequest& req, int slot) {
        return shareManager.slotForCookie(
                   HttpServer::cookieFromRequest(req, QStringLiteral("mw_player"))) == slot;
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
    // The domain is only worth handing to a browser once it is BOTH published
    // and covered by a real certificate: ready() fires as soon as the A-record
    // resolves, while the ACME order finishes seconds to minutes later. In that
    // window the domain is still served with the self-signed fallback and the
    // browser shows ERR_CERT_AUTHORITY_INVALID, so stay on loopback — checking
    // certPem() as well as certificateIssuing() keeps us there when issuance
    // finished by failing. Every caller re-reads this on certificateChanged.
    auto entryUrl = [&](const QString& path) -> QString {
        if (internetAccess.isActive() && !internetAccess.domain().isEmpty() &&
            !internetAccess.certificateIssuing() && !appSettings.certPem().isEmpty()) {
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
    // applyOnce() opens that step itself, which is what the handlers below key
    // off — no need for its return value here.
    Provisioning::applyOnce(QCoreApplication::applicationDirPath(), appSettings, computerManager);

    // Refresh the shortcut to the valid-certificate domain link once it is ready,
    // and (during a fresh install) mark the A-record checklist step done.
    //
    // "done" means the address is usable in a browser, which takes BOTH the
    // A-record and the certificate. ready() only covers the former, so when an
    // ACME order is still in flight the step stays "running" and is closed by
    // the certificateChanged handler below — otherwise the installer declares
    // success and opens the admin page on the still-self-signed domain.
    //
    // Gated on the step being "running" rather than on `provisioned`: the
    // in-app setup wizard (macOS/Linux) opens the very same step from
    // /api/setup/apply, and it needs the certificate folded into "done" for
    // exactly the same reason the installer does.
    QObject::connect(&internetAccess, &InternetAccessManager::ready, &app,
                     [&adminUrl, &internetAccess](const QString&, const QString&) {
                         // adminUrl() folds in the active HTTPS port (fallback port
                         // for a co-existing instance behind the same NAT).
                         writeAdminShortcut(adminUrl());
                         if (arecordRunning() && !internetAccess.certificateIssuing())
                             Provisioning::setStepStatus(QStringLiteral("arecord"),
                                                         QStringLiteral("done"));
                     });
    // The certificate landed (and the TLS reload connected earlier already
    // swapped it in): the domain URL is now trusted, so republish it and close
    // the checklist step.
    QObject::connect(
        &internetAccess, &InternetAccessManager::certificateChanged, &app, [&adminUrl]() {
            writeAdminShortcut(adminUrl());
            if (arecordRunning())
                Provisioning::setStepStatus(QStringLiteral("arecord"), QStringLiteral("done"));
        });
    QObject::connect(&internetAccess, &InternetAccessManager::error, &app, [](const QString&) {
        if (arecordRunning())
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
        // the final A-record state into the installer checklist either way. An
        // ACME order still in flight keeps the step running — certificateChanged
        // (or the installer's own timeout) closes it.
        if (arecordRunning() && !internetAccess.certificateIssuing())
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
    // Concurrent stream slots: slot 1 is the owner's standby (dual-stream
    // seamless switching), slots 2.. belong to invited players. Each child
    // listens on +10*slot / +10*slot+1, proxied at /wsN and /wsN/stream.
    for (int slot = 1; slot < kTotalSlots; ++slot)
        server.setSlotPorts(slot, slotSignalingPort(slot), slotSignalingPort(slot) + 1);
    // Slots acquired for a second host live above kTotalSlots; register their
    // ports up front so the proxy can route /wsN the moment one is handed out.
    for (int slot = kTotalSlots; slot < kMaxSlots; ++slot)
        server.setSlotPorts(slot, slotSignalingPort(slot), slotSignalingPort(slot) + 1);
    server.setFirstPlayerSlot(kOwnerSlots);
    // Player slots end at kTotalSlots; slots above it are the owner's own extra
    // sessions on a second host, so the WS-upgrade auth treats them as the owner
    // (session / local privilege), not as players demanding an mw_player cookie.
    server.setFirstExtraSlot(kTotalSlots);

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
    // Skipped under a service supervisor: Windows accepts a Shell_NotifyIcon
    // from session 0 and reports success, so the icon is "created" on a desktop
    // no one is logged into — a phantom that only ever showed up in the log.
    // The desktop session gets its tray from runTrayClient() instead.
    TrayManager trayManager(&server);
    trayManager.setUrlProvider([entryUrl](const QString& path) { return QUrl(entryUrl(path)); });
    if (hasGuiSession()) trayManager.init();

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

    // Every address the UI answers on, not just the loopback one. On a headless
    // or autostarted instance this block is the only thing that ever tells the
    // operator where to point a browser, and localhost is the one address that
    // is useless from anywhere but the host itself. Same three sections as
    // `moonlightweb --status`, so both agree on what is reachable.
    //
    // The port is taken from the ACTIVE listener rather than the configured one:
    // a second instance behind the same NAT rebinds to a fallback port, and
    // printing the configured 443 would send the user to the wrong place.
    {
        const quint16 activePort = server.activeHttpsPort();
        const QString portSuffix =
            activePort == 443 ? QString() : QStringLiteral(":%1").arg(activePort);

        Logger::info("Server ready. Open https://localhost" + portSuffix + " in your browser.");

        // Multi-homed hosts (Hyper-V, VirtualBox, WSL) get several: only one is
        // on the shared LAN, the others reach their own VMs. Which is which is
        // not ours to guess, so list them best-first and let the user pick.
        for (const QString& ip : internetAccess.localIps())
            Logger::info("  From another machine on this network: https://" + ip + portSuffix);

        if (internetAccess.isActive() && !internetAccess.domain().isEmpty()) {
            quint16 extPort = internetAccess.externalHttpsPort();
            if (extPort == 0) extPort = activePort;
            Logger::info("  From the internet: https://" + internetAccess.domain() +
                         (extPort == 443 ? QString() : QStringLiteral(":%1").arg(extPort)) +
                         (internetAccess.certificateIssuing()
                              ? QStringLiteral(" (certificate still being issued — retry in a "
                                               "minute)")
                              : QString()));
        }
        // Nothing is printed when Internet Access is off: the operator turned it
        // off, or never turned it on. --status is where that gets explained.
    }

    return app.exec();
}