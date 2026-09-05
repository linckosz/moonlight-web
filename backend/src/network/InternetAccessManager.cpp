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

#include "InternetAccessManager.h"
#include "server/AppSettings.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSslSocket>
#include "streaming/TransportPriorities.h"
#include <QTcpSocket>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Periodic check interval: 5 minutes.
static constexpr int kPeriodicCheckMs = 5 * 60 * 1000;

/// Parent domain of the retired DNS mechanism, kept only to recognise a name
/// this instance would have carried under it (see buildDomain()).
/// Read from MW_DOMAIN env var (fallback: "moonlightweb.top").
static QString baseDomain()
{
    QString env = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    return env.isEmpty() ? QStringLiteral("moonlightweb.top") : env;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

InternetAccessManager::InternetAccessManager(AppSettings* settings, QObject* parent)
    : QObject(parent)
    , m_Settings(settings)
    , m_Stun(this)
    , m_Upnp(this)
{
    // Periodic check timer (5 minutes)
    m_PeriodicCheckTimer = new QTimer(this);
    m_PeriodicCheckTimer->setSingleShot(false);
    m_PeriodicCheckTimer->setInterval(kPeriodicCheckMs);
    connect(m_PeriodicCheckTimer, &QTimer::timeout, this, &InternetAccessManager::onPeriodicCheck);

    // Restore persistent state
    m_UniqueId = m_Settings->uniqueId();
    m_Domain = m_Settings->domain();
    m_PublicIp = m_Settings->publicIp();

    // Eager init: ensure unique_id and domain exist even when Internet Access
    // is disabled, so the UI can display the URL immediately.
    ensureIdentifiers();

    // Synchronous local IP detection for admin UI display
    refreshLocalAddresses();

    // Eager UPnP discovery (deferred, non-blocking) so that upnp_available
    // is correctly reported even if Internet Access has never been enabled.
    QTimer::singleShot(2000, this, [this]() {
        if (!m_Upnp.isAvailable()) {
            qInfo() << "[InternetAccess] Eager UPnP discovery (2s deferred)";
            m_Upnp.discover(2000);
        }
    });
}

InternetAccessManager::~InternetAccessManager()
{
    stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void InternetAccessManager::start()
{
    if (m_Active) {
        qInfo() << "[InternetAccess] Already active";
        return;
    }

    qInfo() << "[InternetAccess] ═══ STARTING Internet Access setup ═══";
    qInfo() << "[InternetAccess] uniqueId:" << m_UniqueId;

    // Clear any stale error from a previous attempt so the UI only reflects the
    // outcome of this run (the frontend auto-unchecks the toggle on last_error).
    m_LastError.clear();
    // Phase drives the UI activation loader (read back via statusJson polling).
    m_Phase = QStringLiteral("starting");

    // Step 1: Ensure identifiers exist (already done eagerly at startup,
    // but called again here in case setUniqueId was changed via API).
    ensureIdentifiers();
    qInfo() << "[InternetAccess] Step 1 OK — domain:" << m_Domain << "custom:" << m_CustomDomain;

    // Consent gate. A consent record without a version was obtained for the
    // retired DNS mechanism ("create an A record pointing at your IP") — it
    // does not cover what enabling does today, so the UI has to ask again with
    // the current wording. That applies to every upgraded install: the DNS
    // mechanism no longer runs anywhere, so no consent worded for it can still
    // cover what happens now. The price is one re-tick per existing user, on
    // the admin page, the first time they open Internet Access after
    // upgrading. A custom domain involves no service of ours to consent to.
    if (!m_CustomDomain && m_Settings->internetConsentVersion() < 2 &&
        !m_Settings->internetConsent().isEmpty()) {
        qInfo() << "[InternetAccess] Stored consent predates the current mechanism —"
                << "waiting for renewed consent before opening anything";
        m_Phase = QStringLiteral("consent_required");
        return;
    }

    // Step 2: Detect public IP via STUN. It is what the double-NAT check below
    // compares the router's own answer against, and what the admin page shows.
    m_Phase = QStringLiteral("detecting_ip");
    if (m_Settings->autoIpDetection()) {
        qInfo() << "[InternetAccess] Step 2 — detecting public IP via STUN...";
        detectPublicIp();
        qInfo() << "[InternetAccess] Step 2 — public IP:" << m_PublicIp;
    }

    // Step 3: UPnP discovery. No web port is forwarded here: this instance
    // exposes no admin surface to the internet — the media port is mapped
    // per-session by the signaling server (under this same consent), and a
    // custom domain's 443 forward is the user's own router configuration.
    // Discovery still runs, because the per-session mapping needs the gateway
    // and the admin page reports whether one answered.
    m_Phase = QStringLiteral("configuring_ports");
    if (m_Settings->upnpEnabled()) {
        if (m_Upnp.discover()) {
            // Capture local LAN IP for the UI (port mapping display)
            refreshLocalAddresses();

            // Double NAT detection: UPnP external IP vs STUN public IP
            std::string upnpExternalIp = m_Upnp.getExternalIPAddress();
            if (!upnpExternalIp.empty() && !m_PublicIp.isEmpty()) {
                QString upnpIp = QString::fromStdString(upnpExternalIp);
                if (upnpIp != m_PublicIp) {
                    m_LastError =
                        QStringLiteral("CGNAT detected: UPnP reports %1 but your public IP is %2. "
                                       "Port forwarding may not work — contact your ISP.")
                            .arg(upnpIp, m_PublicIp);
                    qWarning() << "[InternetAccess]" << m_LastError;
                }
            }
        } else {
            qWarning()
                << "[InternetAccess] UPnP discovery failed — manual port forwarding may be needed";
        }
    }

    m_Active = true;
    m_Phase = QStringLiteral("active");
    emit ready(m_Domain, m_PublicIp);
    qInfo() << "[InternetAccess] Setup complete, domain:" << m_Domain << "public IP:" << m_PublicIp;

    // Test NAT hairpin now so the enable response (and the status the admin page
    // reads right after) already carries hairpin_reachable — the frontend cannot
    // probe the domain itself from the untrusted-cert localhost page (Chrome
    // blocks cross-origin subresources there). Only a user-owned domain has an
    // endpoint to test; without one the verdict is false and stays false.
    updateHairpinStatus();

    // Start periodic checks
    m_PeriodicCheckTimer->start();
}

void InternetAccessManager::stop()
{
    m_PeriodicCheckTimer->stop();

    // No mapping to close: this manager forwards no port of its own. The only
    // hole it ever authorises is the per-session media port, opened and closed
    // by the signaling server for the length of a stream.
    m_Active = false;
    m_Phase.clear();
    qInfo() << "[InternetAccess] Stopped";
}

void InternetAccessManager::setPorts(quint16 httpPort, quint16 httpsPort)
{
    m_HttpPort = httpPort;
    m_HttpsPort = httpsPort;
    qInfo() << "[InternetAccess] Ports set: http=" << m_HttpPort << "https=" << m_HttpsPort;
}

bool InternetAccessManager::testHairpinReachable()
{
    // Only a user-owned domain has a public endpoint of ours to test. Without
    // one there is nothing to reflect back, and the verdict stays false — which
    // is what keeps the host's own entry points on the LAN address.
    if (!m_Active || m_Domain.isEmpty()) return false;

    const quint16 port = m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443);

    // A plain TCP connect to our own public endpoint is enough to know whether
    // the router reflects it back to this LAN host (NAT hairpin). This is
    // exactly what a browser opened on the host would attempt when following the
    // public-domain URL. Connecting by hostname also exercises DNS resolution.
    QTcpSocket sock;
    sock.connectToHost(m_Domain, port);
    const bool ok = sock.waitForConnected(2500);
    sock.abort();
    qInfo() << "[InternetAccess] Hairpin test" << m_Domain << ":" << port << "->"
            << (ok ? "reachable" : "unreachable");
    return ok;
}

void InternetAccessManager::updateHairpinStatus()
{
    const bool ok = testHairpinReachable();
    if (ok != m_HairpinReachable) {
        m_HairpinReachable = ok;
        // Two audiences: a live admin page polls the status, while the host-side
        // entry points (tray, Desktop shortcut) only care about this one flag —
        // they rebuild on hairpinChanged rather than on every status update.
        emit statusChanged(statusJson());
        emit hairpinChanged(ok);
    }
}

void InternetAccessManager::forceRefresh()
{
    qInfo() << "[InternetAccess] Force refresh triggered";

    // Re-detect the public IP. Nothing is published with it — it is shown on
    // the admin page and compared against what the router claims.
    if (m_Settings->autoIpDetection()) {
        detectPublicIp();
        m_Settings->setPublicIp(m_PublicIp);
    }

    // A router reconfigured since the last check can start (or stop) reflecting
    // the host's own public address back to the LAN.
    updateHairpinStatus();

    emit statusChanged(statusJson());
}

QJsonObject InternetAccessManager::statusJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("active")] = m_Active;
    obj[QStringLiteral("domain")] = m_Domain;
    // True when `domain` in settings.json is a user-owned FQDN, served as-is
    // with a certificate the user provides.
    obj[QStringLiteral("custom_domain")] = m_CustomDomain;
    // Version of the stored consent record (0 when none). A record without a
    // version was worded for the retired DNS mechanism; the admin UI must
    // re-ask before anything is opened (phase "consent_required").
    obj[QStringLiteral("consent_version")] = m_Settings->internetConsentVersion();
    obj[QStringLiteral("local_ip")] = m_LocalIp;
    // Every address another machine can reach this host on, best first: a
    // multi-homed host (Hyper-V, VirtualBox, WSL) needs all of them shown —
    // only one is on the shared LAN, the others reach their own VMs.
    obj[QStringLiteral("local_ips")] = QJsonArray::fromStringList(m_LocalIps);
    obj[QStringLiteral("public_ip")] = m_PublicIp;
    obj[QStringLiteral("unique_id")] = m_UniqueId;
    obj[QStringLiteral("internet_access_enabled")] = m_Settings->internetAccessEnabled();
    obj[QStringLiteral("phase")] = m_Phase;
    obj[QStringLiteral("upnp_enabled")] = m_Settings->upnpEnabled();
    obj[QStringLiteral("auto_ip_detection")] = m_Settings->autoIpDetection();
    obj[QStringLiteral("transport_mode")] = m_Settings->transportMode();
    // The admin transport selector. `wss` is deliberately absent from release
    // builds: it carries video over the TLS connection that served the page, so
    // it can only run when the page was opened on the host's LAN IP directly —
    // never through the rendezvous, which is how every user reaches the app.
    // Offering it there is offering a choice that silently becomes another one.
    // The capability itself is untouched: it stays the last rung of the `auto`
    // fallback chain for a LAN-IP browser, and a settings.json written by hand
    // is still honoured.
    QStringList availableTransports = TransportPriorities::orderedTransports();
#ifndef QT_DEBUG
    availableTransports.removeAll(QStringLiteral("wss"));
#endif
    obj[QStringLiteral("available_transports")] = QJsonArray::fromStringList(availableTransports);
    // The certificate this host serves. Empty on a plain install (the
    // self-signed one is generated and found by CertManager); a path when the
    // user dropped their own next to a domain they own.
    obj[QStringLiteral("cert_pem")] = m_Settings->certPem();
    obj[QStringLiteral("cert_key")] = m_Settings->certKey();
    obj[QStringLiteral("upnp_available")] = m_Upnp.isAvailable();
    // Whether the host can reach its own public endpoint (router NAT hairpin).
    // Drives the host-machine redirect from https://localhost to the domain.
    obj[QStringLiteral("hairpin_reachable")] = m_HairpinReachable;
    const int httpsPort = m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443);
    obj[QStringLiteral("https_port")] = httpsPort;
    // Router-side HTTPS port. This manager maps no web port of its own, so it
    // is the local listener port: a user-owned domain is forwarded to it by the
    // user's own router configuration. Kept as a separate field because every
    // reader of a public URL builds it from this one.
    obj[QStringLiteral("external_https_port")] = httpsPort;

    if (!m_LastError.isEmpty()) obj[QStringLiteral("last_error")] = m_LastError;

    return obj;
}

// ---------------------------------------------------------------------------
// Identity helpers
// ---------------------------------------------------------------------------

QString InternetAccessManager::buildDomain() const
{
    return m_UniqueId + QStringLiteral(".") + baseDomain();
}

QString InternetAccessManager::generateUniqueId()
{
    // Generate 8 random lowercase hex characters from the OS CSPRNG.
    // Lowercase matches the reuse-from-domain check; CSPRNG avoids the
    // predictable sequence of the shared global PRNG.
    QString hex(8, QChar('0'));
    for (int i = 0; i < 8; ++i) {
        hex[i] = QStringLiteral("0123456789abcdef").at(QRandomGenerator::system()->bounded(16));
    }
    qInfo() << "[InternetAccess] Generated unique ID:" << hex;
    return hex;
}

// ---------------------------------------------------------------------------
// Identifiers — eagerly initialized at startup, without touching the network
// ---------------------------------------------------------------------------

void InternetAccessManager::refreshLocalAddresses()
{
    const QStringList addresses = UPNPClient::getLocalIPs();
    if (addresses.isEmpty()) return; // keep the previous value rather than blanking the UI

    m_LocalIps = addresses;
    m_LocalIp = addresses.first();
    qInfo() << "[InternetAccess] Local LAN IP:" << m_LocalIp << "— all reachable:" << m_LocalIps;
}

void InternetAccessManager::ensureIdentifiers()
{
    // If unique_id is missing, try to extract from saved domain first
    if (m_UniqueId.isEmpty()) {
        QString dotBaseDomain = QStringLiteral(".") + baseDomain();
        if (!m_Domain.isEmpty() && m_Domain.endsWith(dotBaseDomain)) {
            QString subname = m_Domain.left(m_Domain.length() - baseDomain().length() - 1);
            if (subname.length() == 8) {
                bool allHex = true;
                for (const QChar& c : subname) {
                    if ((c < QLatin1Char('0') || c > QLatin1Char('9')) &&
                        (c < QLatin1Char('a') || c > QLatin1Char('f'))) {
                        allHex = false;
                        break;
                    }
                }
                if (allHex) {
                    m_UniqueId = subname;
                    m_Settings->setUniqueId(m_UniqueId);
                    qInfo() << "[InternetAccess] Reused unique ID from saved domain:" << m_UniqueId;
                }
            }
        }
        if (m_UniqueId.isEmpty()) {
            m_UniqueId = generateUniqueId();
            m_Settings->setUniqueId(m_UniqueId);
        }
    }

    // Bring-your-own-domain: a valid FQDN in settings.json is the user's, so
    // keep it verbatim — rewriting the sentinel here would silently drop it at
    // every boot, and CertManager would then CN-check their certificate against
    // a domain they never asked for.
    //
    // One name is excluded: the {unique_id}.{MW_DOMAIN} a pre-0.3 install wrote
    // there when it registered a sub-domain. That name is not the user's and
    // nothing points at it any more, so serving it as a "custom domain" would
    // hand every entry point a URL that does not resolve.
    const QString stored = m_Settings->domain();
    m_CustomDomain = AppSettings::isValidFqdn(stored) && stored != buildDomain();
    if (m_CustomDomain) {
        m_Domain = stored;
        qInfo() << "[InternetAccess] Custom domain from settings:" << m_Domain
                << "— we own neither its zone nor its certificate";
        return;
    }

    // No name of our own. This build registers nothing: it is reached through
    // the rendezvous, which needs no sub-domain, no DNS write and no public
    // certificate. An empty domain is what keeps every entry point (shortcut,
    // tray, hairpin test) on the LAN address, with the rendezvous for the rest.
    //
    // unique_id stays as this instance's local identity — shown on the admin
    // page, and what tells a name left behind by a pre-0.3 install apart from
    // a domain the user owns. It never leaves this machine.
    m_Domain.clear();
}

// ---------------------------------------------------------------------------
// Public IP detection via STUN (with HTTP fallback)
// ---------------------------------------------------------------------------

QString InternetAccessManager::detectPublicIpViaHttp()
{
    // HTTPS endpoints (port 443). TLS is verified against the system trust store,
    // so a network MITM can neither read the request nor forge the public IP we
    // would then publish in the A record. This is a fallback after STUN.
    const QString hosts[] = {
        QStringLiteral("api.ipify.org"),
        QStringLiteral("icanhazip.com"),
        QStringLiteral("checkip.amazonaws.com"),
    };

    for (const QString& host : hosts) {
        qInfo() << "[InternetAccess] Trying HTTPS IP detection:" << host;

        QSslSocket socket;
        socket.connectToHostEncrypted(host, 443);
        if (!socket.waitForEncrypted(5000)) {
            qWarning() << "[InternetAccess] HTTPS handshake failed to" << host << ":"
                       << socket.errorString();
            continue;
        }

        QByteArray request = QStringLiteral("GET / HTTP/1.1\r\n"
                                            "Host: %1\r\n"
                                            "User-Agent: MoonlightWeb/1.0\r\n"
                                            "Connection: close\r\n\r\n")
                                 .arg(host)
                                 .toUtf8();

        socket.write(request);
        socket.waitForBytesWritten();

        // Read until the server closes the connection (Connection: close).
        QByteArray response;
        while (socket.state() == QAbstractSocket::ConnectedState && socket.waitForReadyRead(5000))
            response += socket.readAll();
        response += socket.readAll();
        socket.close();

        if (response.isEmpty()) {
            qWarning() << "[InternetAccess] HTTPS read timeout from" << host;
            continue;
        }

        // Parse HTTP response body (after \r\n\r\n)
        int headerEnd = response.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            qWarning() << "[InternetAccess] Invalid HTTP response from" << host;
            continue;
        }

        QByteArray body = response.mid(headerEnd + 4).trimmed();

        // Validate that it looks like an IPv4 address
        QHostAddress ha;
        if (!ha.setAddress(QString::fromUtf8(body))) {
            qWarning() << "[InternetAccess] HTTP response is not a valid IP:" << body;
            continue;
        }
        if (ha.protocol() != QAbstractSocket::IPv4Protocol) {
            qWarning() << "[InternetAccess] HTTP response is not IPv4:" << body;
            continue;
        }

        QString ip = ha.toString();
        qInfo() << "[InternetAccess] Public IP detected via HTTPS:" << ip << "from" << host;
        return ip;
    }

    qWarning() << "[InternetAccess] All HTTP IP detection services failed";
    return {};
}

bool InternetAccessManager::detectPublicIp()
{
    qInfo() << "[InternetAccess] Detecting public IP via STUN...";

    // Try default STUN servers
    QList<StunClient::StunServer> servers = StunClient::defaultServers();

    // Also try the user-configured STUN server if different
    QString configured = m_Settings->stunServer();
    if (!configured.isEmpty()) {
        // Parse "stun:host:port" or "host:port"
        QString host;
        quint16 port = 3478;

        if (configured.startsWith("stun:", Qt::CaseInsensitive)) {
            QString stripped = configured.mid(5);
            int colon = stripped.lastIndexOf(':');
            if (colon > 0) {
                host = stripped.left(colon);
                bool ok;
                int p = stripped.mid(colon + 1).toInt(&ok);
                if (ok && p > 0 && p <= 65535) port = static_cast<quint16>(p);
            } else {
                host = stripped;
            }
        } else {
            int colon = configured.lastIndexOf(':');
            if (colon > 0) {
                host = configured.left(colon);
                bool ok;
                int p = configured.mid(colon + 1).toInt(&ok);
                if (ok && p > 0 && p <= 65535) port = static_cast<quint16>(p);
            } else {
                host = configured;
            }
        }

        if (!host.isEmpty()) {
            // Prepend the configured STUN server as first priority — unless the
            // default chain already leads with it, which it does whenever
            // stun_server is unset and both fall back to our own server. Two
            // entries for one host would cost a second timeout before the
            // fallback when that host is the one that is down.
            bool already = false;
            for (const StunClient::StunServer& s : servers) {
                if (s.host.compare(host, Qt::CaseInsensitive) == 0 && s.port == port) {
                    already = true;
                    break;
                }
            }
            if (!already) servers.prepend({host, port});
        }
    }

    QString detectedIp;
    if (m_Stun.detectPublicIp(servers, 3000, detectedIp)) {
        m_PublicIp = detectedIp;
        m_Settings->setPublicIp(m_PublicIp);
        qInfo() << "[InternetAccess] Public IP detected:" << m_PublicIp;
        return true;
    }

    // Fallback 1: HTTP IP detection when STUN fails
    qInfo() << "[InternetAccess] STUN failed, trying HTTP IP detection...";
    QString httpIp = detectPublicIpViaHttp();
    if (!httpIp.isEmpty()) {
        m_PublicIp = httpIp;
        m_Settings->setPublicIp(m_PublicIp);
        return true;
    }

    // Fallback 2: stored public IP if all network detection fails
    QString stored = m_Settings->publicIp();
    if (!stored.isEmpty()) {
        m_PublicIp = stored;
        qInfo() << "[InternetAccess] Using stored public IP as fallback:" << m_PublicIp;
        return true;
    }

    m_LastError = QStringLiteral("Failed to detect public IP via STUN");
    qWarning() << "[InternetAccess]" << m_LastError;
    emit error(m_LastError);
    return false;
}

// ---------------------------------------------------------------------------
// Periodic checks
// ---------------------------------------------------------------------------

void InternetAccessManager::onPeriodicCheck()
{
    qInfo() << "[InternetAccess] Periodic check triggered";

    // 1. Re-detect public IP if auto-detection is enabled. Nothing is published
    // with it: it is shown on the admin page, and it is what a CGNAT diagnosis
    // compares the router's own answer against.
    if (m_Settings->autoIpDetection()) {
        QString oldIp = m_PublicIp;
        detectPublicIp();

        if (m_PublicIp != oldIp && !m_PublicIp.isEmpty()) {
            qInfo() << "[InternetAccess] Public IP changed from" << oldIp << "to" << m_PublicIp;
            m_Settings->setPublicIp(m_PublicIp);
        }
    }

    // 2. Refresh NAT hairpin reachability (router config can change).
    updateHairpinStatus();

    emit statusChanged(statusJson());
}
