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

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QDirIterator>
#include "streaming/TransportPriorities.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QDateTime>
#include <QStandardPaths>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Periodic check interval: 5 minutes.
static constexpr int kPeriodicCheckMs = 5 * 60 * 1000;

/// Pending registration retry interval: 30 seconds.
static constexpr int kPendingRetryMs = 30 * 1000;

/// Parent domain used as suffix for A records.
/// Read from MW_DOMAIN env var (fallback: "moonlightweb.top").
static QString baseDomain()
{
    QString env = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    return env.isEmpty() ? QStringLiteral("moonlightweb.top") : env;
}

/// Default TTL for A records (5 minutes).
static constexpr int kDefaultTtl = 300;

/// Initial DNS check retries: a freshly created A record needs to propagate
/// through PowerDNS and the local resolver before it resolves. Without retries
/// the single check almost always fails right after creation, which misleads a
/// developer reading the logs into thinking DNS is broken.
static constexpr int kDnsCheckRetries = 10;

/// Delay between DNS check retries (3s → up to ~30s total for propagation).
static constexpr int kDnsCheckRetryMs = 3000;

/// TLS certificate renewal threshold: 30 days.
static constexpr int kCertRenewalDays = 30;

/// ACME certificate output directory (under AppData/cert/).
static const QString kAcmeCertDir = QStringLiteral("letsencrypt");

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

InternetAccessManager::InternetAccessManager(AppSettings* settings, QObject* parent)
    : QObject(parent)
    , m_Settings(settings)
    , m_Pdns(this)
    , m_Stun(this)
    , m_Upnp(this)
    , m_Acme(this)
{
    // Periodic check timer (5 minutes)
    m_PeriodicCheckTimer = new QTimer(this);
    m_PeriodicCheckTimer->setSingleShot(false);
    m_PeriodicCheckTimer->setInterval(kPeriodicCheckMs);
    connect(m_PeriodicCheckTimer, &QTimer::timeout, this, &InternetAccessManager::onPeriodicCheck);

    // Pending registration retry timer (30 seconds)
    m_PendingRegistrationTimer = new QTimer(this);
    m_PendingRegistrationTimer->setSingleShot(true);
    connect(m_PendingRegistrationTimer, &QTimer::timeout, this,
            &InternetAccessManager::onPendingRegistrationRetry);

    // Connect ACME client signals
    connect(&m_Acme, &AcmeClient::progress, this, &InternetAccessManager::onAcmeProgress);
    connect(&m_Acme, &AcmeClient::errorOccurred, this, &InternetAccessManager::onAcmeError);
    connect(&m_Acme, &AcmeClient::finished, this, &InternetAccessManager::onAcmeFinished);

    // After a parity rebind the HTTPS listener sits on the new port: re-test the
    // hairpin against it (the test at the end of start() ran while the listener
    // was still on the old port).
    connect(this, &InternetAccessManager::httpsPortChanged, this,
            &InternetAccessManager::updateHairpinStatus);

    // Restore persistent state
    m_UniqueId = m_Settings->uniqueId();
    m_Domain = m_Settings->domain();
    m_PublicIp = m_Settings->publicIp();

    // Service-managed instances (systemd/launchd/NSSM set MW_SERVICE=1) must not
    // fight over UPnP port mappings on an auto-restart: only a manual launch is
    // allowed to take a mapping over from another device on the LAN.
    m_ServiceManaged = !qEnvironmentVariableIsEmpty("MW_SERVICE");
    qInfo() << "[InternetAccess] Service-managed launch:" << m_ServiceManaged;

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
    qInfo() << "[InternetAccess] baseDomain:" << baseDomain() << "uniqueId:" << m_UniqueId
            << "fqdn:" << buildDomain();

    // Clear any stale error from a previous attempt so the UI only reflects the
    // outcome of this run (the frontend auto-unchecks the toggle on last_error).
    m_LastError.clear();
    // Phase drives the UI activation loader (read back via statusJson polling).
    m_Phase = QStringLiteral("starting");

    // Step 1: Ensure identifiers exist (already done eagerly at startup,
    // but called again here in case setUniqueId was changed via API).
    ensureIdentifiers();
    qInfo() << "[InternetAccess] Step 1 OK — domain:" << m_Domain
            << "legacy:" << m_LegacyDns << "custom:" << m_CustomDomain;

    // Consent gate. A consent record without a version was obtained for the
    // retired DNS mechanism ("create an A record pointing at your IP") — it
    // does not cover what enabling does today, so the UI has to ask again with
    // the current wording. Legacy instances still run that mechanism until the
    // announced shutdown, so their v1 consent remains exactly right; a custom
    // domain involves no service of ours to consent to.
    if (!m_LegacyDns && !m_CustomDomain && m_Settings->internetConsentVersion() < 2 &&
        !m_Settings->internetConsent().isEmpty()) {
        qInfo() << "[InternetAccess] Stored consent predates the current mechanism —"
                << "waiting for renewed consent before opening anything";
        m_Phase = QStringLiteral("consent_required");
        return;
    }

    // Step 2: Read the PowerDNS token from MW_PDNS_TOKEN env var. Only a legacy
    // instance still writes to PowerDNS; a fresh install registers nothing and a
    // user-owned domain is not ours to write to.
    if (m_LegacyDns) {
        QString token = QString::fromUtf8(qgetenv("MW_PDNS_TOKEN"));
        qInfo() << "[InternetAccess] Step 2 — token source: env var MW_PDNS_TOKEN"
                << "empty:" << token.isEmpty() << "length:" << token.length();

        if (token.isEmpty()) {
            m_LastError = QStringLiteral(
                "PowerDNS token is empty. Set the MW_PDNS_TOKEN environment variable "
                "in Qt Creator (Projects → Run → Environment) or your shell.");
            qWarning() << "[InternetAccess]" << m_LastError;
            m_Phase = QStringLiteral("pending");
            m_Settings->setPendingRegistration(true);
            m_PendingRegistrationTimer->start(kPendingRetryMs);
            return;
        }
        m_Pdns.setToken(token);
    }

    // Step 3: Detect public IP via STUN (before A record creation)
    m_Phase = QStringLiteral("detecting_ip");
    if (m_Settings->autoIpDetection()) {
        qInfo() << "[InternetAccess] Step 3 — detecting public IP via STUN...";
        detectPublicIp();
        qInfo() << "[InternetAccess] Step 3 — public IP:" << m_PublicIp;
    }

    // Step 3.5: Pre-check DNS — if the domain already resolves, skip PowerDNS A record creation.
    // This is critical when STUN fails (e.g. IPv6 XOR-MAPPED-ADDRESS not supported):
    // the existing A record is still valid, so no need to touch PowerDNS at all.
    if (m_LegacyDns) {
        bool skipARecordStep = false;
        QString resolvedIp = resolveDomain(m_Domain);
        if (!resolvedIp.isEmpty()) {
            if (m_PublicIp.isEmpty() || resolvedIp == m_PublicIp) {
                qInfo() << "[InternetAccess] Domain already resolves to" << resolvedIp
                        << "via DNS — skipping PowerDNS A record creation";
                m_Settings->setPendingRegistration(false);
                skipARecordStep = true;
            } else {
                qInfo() << "[InternetAccess] Domain resolves to" << resolvedIp << "but public IP is"
                        << m_PublicIp << "— will update via PowerDNS";
            }
        }

        if (!skipARecordStep) {
            // Step 4: Create or update A record (with real IP from STUN)
            m_Phase = QStringLiteral("registering_dns");
            qInfo() << "[InternetAccess] Step 4 — creating/verifying A record...";
            if (!createOrUpdateARecord()) {
                qWarning() << "[InternetAccess] Step 4 FAILED — A record creation failed,"
                           << "will retry in" << (kPendingRetryMs / 1000) << "s";
                m_Phase = QStringLiteral("pending");
                m_Settings->setPendingRegistration(true);
                m_PendingRegistrationTimer->start(kPendingRetryMs);
                return;
            }
            m_Settings->setPendingRegistration(false);
            qInfo() << "[InternetAccess] Step 4 OK — A record exists";
        }
    }

    // Step 5: Initial DNS check (spaced to 24h thereafter). A freshly created A
    // record needs time to propagate, so retry a few times before giving up —
    // otherwise the first check fails and misleads anyone reading the logs.
    if (m_LegacyDns) {
        m_Phase = QStringLiteral("checking_dns");
        QString resolvedIp;
        for (int attempt = 1; attempt <= kDnsCheckRetries; ++attempt) {
            resolvedIp = resolveDomain(m_Domain);
            if (!resolvedIp.isEmpty()) {
                qInfo() << "[InternetAccess] Step 5 — DNS check OK:" << m_Domain << "->"
                        << resolvedIp << "(attempt" << attempt << "/" << kDnsCheckRetries << ")";
                break;
            }
            if (attempt < kDnsCheckRetries) {
                qInfo() << "[InternetAccess] Step 5 — DNS not propagated yet for" << m_Domain
                        << "(attempt" << attempt << "/" << kDnsCheckRetries << "), retrying in"
                        << (kDnsCheckRetryMs / 1000) << "s...";
                // Wait without freezing the event loop (keeps status polling alive).
                QEventLoop wait;
                QTimer::singleShot(kDnsCheckRetryMs, &wait, &QEventLoop::quit);
                wait.exec();
            }
        }

        if (resolvedIp.isEmpty()) {
            qWarning() << "[InternetAccess] Step 5 — DNS resolution still failing for" << m_Domain
                       << "after" << kDnsCheckRetries << "attempts";
            if (pingDomain(m_Domain)) {
                qInfo() << "[InternetAccess] Domain" << m_Domain
                        << "is reachable via ping despite DNS failure";
            } else {
                qWarning() << "[InternetAccess] Domain" << m_Domain
                           << "not reachable via ping either";
            }
        }
        m_LastDnsCheck = QDateTime::currentDateTimeUtc();
    }

    // Step 6: Issue/renew TLS certificate. Only a legacy instance holds a
    // public certificate; a fresh install serves its self-signed one on the
    // LAN, and a user-owned domain's certificate is the user's (CertManager
    // just loads what they dropped in the cert directory).
    if (m_LegacyDns) {
        m_Phase = QStringLiteral("issuing_certificate");
        QString existingCert = m_Settings->certPem();
        qInfo() << "[InternetAccess] Step 6 — checking certificate: cert_pem=\"" << existingCert
                << "\"";
        checkCertificate();
    }

    // Step 7: UPnP port mapping. Only a legacy instance forwards its web ports
    // — its public URL depends on them until the DNS stack shuts down. A fresh
    // install exposes no admin surface: the media port is mapped per-session by
    // the signaling server (and only with this same consent), and a custom
    // domain's 443 forward is the user's own router configuration.
    m_Phase = QStringLiteral("configuring_ports");
    if (m_Settings->upnpEnabled()) {
        if (m_Upnp.discover()) {
            if (m_LegacyDns) {
                // Co-existence: never evict another device's mapping. HTTPS uses
                // strict external==internal parity: when another instance holds
                // the default router port, this instance claims a deterministic
                // fallback port AND moves its HTTPS listener to it (deferred
                // rebind), so the advertised URL port is exactly the
                // router-forwarded port.
                m_ExternalHttpsPort = mapHttpsPortParity();

                // Map HTTP port too, so the HTTP→HTTPS redirect works from the
                // internet. Without this mapping, external clients cannot reach
                // the HTTP redirect server through the NAT gateway.
                quint16 httpPort = m_HttpPort > 0 ? m_HttpPort : m_Settings->httpPort(80);
                m_ExternalHttpPort = mapPortWithFallback(httpPort, "TCP", "MoonlightWeb HTTP");

                m_ExternalUdpPort = mapPortWithFallback(47999, "UDP", "MoonlightWeb UDP Stream");
            }

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
    // blocks cross-origin subresources there). When a parity rebind is pending,
    // the listener is not on the new port yet: httpsPortChanged re-tests once it
    // has moved, and the periodic check / next page load self-heal regardless.
    updateHairpinStatus();

    // Start periodic checks
    m_PeriodicCheckTimer->start();
}

void InternetAccessManager::stop()
{
    m_PeriodicCheckTimer->stop();
    m_PendingRegistrationTimer->stop();
    m_PendingRetryCount = 0;

    // Cancel any in-progress ACME issuance
    m_Acme.cancel();
    m_CertIssuing = false;

    // Close our router mappings now. The consent toggle is the master switch
    // for internet reachability: leaving the forwards to die with their lease
    // would keep the NAT hole open for up to an hour after the user said no.
    // Only ports we mapped ourselves are touched (mapPortWithFallback and
    // mapHttpsPortParity both refuse to evict another device's mapping, so
    // these external ports are ours by construction).
    if (m_Upnp.isAvailable()) {
        if (m_ExternalHttpsPort > 0) {
            qInfo() << "[InternetAccess] Removing HTTPS mapping" << m_ExternalHttpsPort;
            m_Upnp.removePortMapping(m_ExternalHttpsPort, "TCP");
        }
        if (m_ExternalHttpPort > 0) {
            qInfo() << "[InternetAccess] Removing HTTP mapping" << m_ExternalHttpPort;
            m_Upnp.removePortMapping(m_ExternalHttpPort, "TCP");
        }
        if (m_ExternalUdpPort > 0) {
            qInfo() << "[InternetAccess] Removing UDP stream mapping" << m_ExternalUdpPort;
            m_Upnp.removePortMapping(m_ExternalUdpPort, "UDP");
        }
    }
    m_ExternalHttpsPort = 0;
    m_ExternalHttpPort = 0;
    m_ExternalUdpPort = 0;

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

quint16 InternetAccessManager::fallbackExternalPort(quint16 internalPort) const
{
    // High-range port (40000..49999) derived from unique_id + the internal port,
    // kept well away from well-known ports so two instances on the same LAN don't
    // collide on their fallback. Uses a stable FNV-1a hash (NOT qHash, whose seed
    // is randomized per process) so the same instance keeps the same URL across
    // restarts.
    const QByteArray seed = (m_UniqueId + QString::number(internalPort)).toUtf8();
    quint32 h = 2166136261u;
    for (char c : seed) {
        h ^= static_cast<quint8>(c);
        h *= 16777619u;
    }
    return static_cast<quint16>(40000 + (h % 10000));
}

quint16 InternetAccessManager::mapPortWithFallback(quint16 internalPort, const char* protocol,
                                                   const char* desc)
{
    // Candidate external ports: the internal port first (so the first instance to
    // claim it keeps a clean, port-less URL), then a deterministic fallback range.
    // We never evict a mapping owned by another device — a second instance behind
    // the same NAT simply takes its own external port, so both stay reachable from
    // the internet (one public IP forwards each external port to a single host).
    const quint16 fb = fallbackExternalPort(internalPort);
    const quint16 candidates[] = {internalPort, fb, static_cast<quint16>(fb + 1),
                                  static_cast<quint16>(fb + 2), static_cast<quint16>(fb + 3)};

    for (quint16 ext : candidates) {
        std::string existingClient, existingPort;
        if (m_Upnp.getExistingPortMapping(ext, protocol, existingClient, existingPort)) {
            if (existingClient != m_Upnp.lanAddress()) {
                // Owned by another device — try the next candidate (no eviction).
                qInfo() << "[InternetAccess] External port" << ext << protocol << "owned by"
                        << existingClient.c_str() << "— trying next candidate";
                continue;
            }
            // Already ours — re-add below to refresh the lease.
        }
        if (m_Upnp.addPortMapping(ext, internalPort, 3600, desc, protocol)) {
            if (ext != internalPort)
                qInfo() << "[InternetAccess] Using fallback external port" << ext << protocol
                        << "-> internal" << internalPort << "(another instance holds the default)";
            return ext;
        }
        // AddPortMapping may still fail (e.g. ConflictInMappingEntry on some
        // routers) even when GetSpecific reported the port free — try next.
    }

    qWarning() << "[InternetAccess] Could not map any external port for internal" << internalPort
               << protocol;
    return 0;
}

bool InternetAccessManager::isLocalPortBindable(quint16 port)
{
    QTcpServer probe;
    const bool ok = probe.listen(QHostAddress::Any, port);
    probe.close();
    return ok;
}

bool InternetAccessManager::testHairpinReachable()
{
    if (!m_Active || m_Domain.isEmpty()) return false;

    quint16 port = m_ExternalHttpsPort > 0
                       ? m_ExternalHttpsPort
                       : (m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443));

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

quint16 InternetAccessManager::mapHttpsPortParity()
{
    // Strict parity: the router-side external port must equal the local HTTPS
    // listener port, so the public URL port is the one the router forwards
    // (443→443, 48123→48123, ...). Candidates: the current port first, then the
    // deterministic per-instance fallback range. We never evict another device's
    // mapping; when the current port is owned elsewhere on the router, we take a
    // fallback port AND move our own HTTPS listener to it (deferred rebind).
    const quint16 current = m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443);
    const quint16 fb = fallbackExternalPort(current);
    const quint16 candidates[] = {current, fb, static_cast<quint16>(fb + 1),
                                  static_cast<quint16>(fb + 2), static_cast<quint16>(fb + 3)};

    for (quint16 c : candidates) {
        std::string existingClient, existingPort;
        if (m_Upnp.getExistingPortMapping(c, "TCP", existingClient, existingPort)) {
            if (existingClient != m_Upnp.lanAddress()) {
                qInfo() << "[InternetAccess] External port" << c << "TCP owned by"
                        << existingClient.c_str() << "— trying next candidate";
                continue;
            }
            // Already ours — re-add below to refresh the lease.
        }
        // The listener must be able to move to c on this machine too.
        if (c != current && !isLocalPortBindable(c)) {
            qInfo() << "[InternetAccess] Port" << c << "not bindable locally — trying next";
            continue;
        }
        if (!m_Upnp.addPortMapping(c, c, 3600, "MoonlightWeb HTTPS", "TCP")) continue;

        if (c != current) {
            qInfo() << "[InternetAccess] Port parity: external" << c
                    << "claimed — adding a public-domain HTTPS listener on" << c;
            // Adding a second listener is non-destructive (the primary keeps
            // serving localhost/LAN), so it is safe to do synchronously inside
            // the enable request's call stack — no deferral, no torn-down origin.
            // Publish the external port first so entry points (shortcut, tray)
            // that react to httpsPortChanged read the right port.
            m_ExternalHttpsPort = c;
            if (!m_HttpsRebindCallback) {
                qWarning() << "[InternetAccess] No HTTPS listener callback set — the public domain"
                           << "may be unreachable on external port" << c;
            } else if (m_HttpsRebindCallback(c)) {
                // Full parity: adopt c as this instance's canonical HTTPS port so
                // the host's own local URLs (localhost, LAN IP) use the same port
                // as the public domain — there is a single https_port for host and
                // router, not 443 locally / c externally. The secondary listener
                // already accepts on c across every interface, so :c works right
                // now; persisting it makes the next boot bind the primary listener
                // straight to c (converging onto one port).
                m_HttpsPort = c;
                m_Settings->setHttpsPort(c);
                emit httpsPortChanged(c);
            } else {
                qWarning() << "[InternetAccess] Failed to add public-domain HTTPS listener on" << c;
            }
        }
        return c;
    }

    qWarning() << "[InternetAccess] Could not map any parity port for HTTPS (current:" << current
               << ")";
    return 0;
}

void InternetAccessManager::forceRefresh()
{
    qInfo() << "[InternetAccess] Force refresh triggered";

    // Re-detect public IP
    if (m_Settings->autoIpDetection()) {
        QString oldIp = m_PublicIp;
        detectPublicIp();

        if (m_PublicIp != oldIp && !m_PublicIp.isEmpty()) {
            // IP changed — update A record
            updateARecord();
            m_Settings->setPublicIp(m_PublicIp);
        }
    }

    // Check DNS resolution — legacy registrations only.
    if (m_LegacyDns) {
        QString resolvedIp = resolveDomain(m_Domain);
        if (!resolvedIp.isEmpty() && resolvedIp != m_PublicIp && !m_PublicIp.isEmpty()) {
            // DNS mismatch — update A record
            qInfo() << "[InternetAccess] DNS resolution mismatch: resolved=" << resolvedIp
                    << "expected=" << m_PublicIp << "— updating A record";
            updateARecord();
        }
    }

    // Check certificate
    checkCertificate();

    emit statusChanged(statusJson());
}

void InternetAccessManager::renewCertificate()
{
    qInfo() << "[InternetAccess] Manual certificate renewal requested";
    issueCertificate();
}

QJsonObject InternetAccessManager::statusJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("active")] = m_Active;
    obj[QStringLiteral("domain")] = m_Domain;
    // True when `domain` in settings.json is a user-owned FQDN: no DNS
    // registration, no ACME — only IP detection, port mapping and hairpin.
    obj[QStringLiteral("custom_domain")] = m_CustomDomain;
    // True when this instance still runs the retiring DNS mechanism (registered
    // subdomain + public certificate). Drives the legacy-only admin UI: the
    // activation checklist's DNS steps, the pending-registration banner, and
    // the February 2027 shutdown notice.
    obj[QStringLiteral("legacy_dns")] = m_LegacyDns;
    // Version of the stored consent record (0 when none). A record without a
    // version was worded for the DNS mechanism; the admin UI must re-ask
    // before a non-legacy instance opens anything (phase "consent_required").
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
    obj[QStringLiteral("available_transports")] =
        QJsonArray::fromStringList(TransportPriorities::orderedTransports());
    obj[QStringLiteral("pending_registration")] = m_Settings->pendingRegistration();
    obj[QStringLiteral("cert_pem")] = m_Settings->certPem();
    obj[QStringLiteral("cert_key")] = m_Settings->certKey();
    obj[QStringLiteral("cert_issuing")] = m_CertIssuing;
    obj[QStringLiteral("upnp_available")] = m_Upnp.isAvailable();
    // Whether the host can reach its own public endpoint (router NAT hairpin).
    // Drives the host-machine redirect from https://localhost to the domain.
    obj[QStringLiteral("hairpin_reachable")] = m_HairpinReachable;
    obj[QStringLiteral("https_port")] = m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443);
    // External (router-side) HTTPS port for the public domain URL: the mapped
    // port when known, otherwise the local HTTPS port. Differs from https_port
    // only for a second instance behind the same NAT (fallback port).
    obj[QStringLiteral("external_https_port")] =
        m_ExternalHttpsPort > 0 ? m_ExternalHttpsPort
                                : (m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443));

    if (!m_LastError.isEmpty()) obj[QStringLiteral("last_error")] = m_LastError;

    return obj;
}

// ---------------------------------------------------------------------------
// Token / ID helpers
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

bool InternetAccessManager::isReservedSubdomain(const QString& label)
{
    const QString l = label.trimmed().toLower();
    if (l.isEmpty()) return true;                    // apex ("@")
    if (l.startsWith(QLatin1Char('_'))) return true; // _owner / _acme-challenge tokens
    // Labels the DNS stack itself publishes under the base domain — keep in sync
    // with deploy/powerdns/pdns/init.sh (plus "stats" for the Umami analytics
    // host, "updates" for the release relay, and "app" for the bootstrap, which
    // is a CNAME to GitHub Pages rather than a record this stack serves).
    static const char* const kReserved[] = {
        "www", "api", "dnsapi", "stats", "stream", "ns1", "ns2", "mail", "updates", "app",
    };
    for (const char* r : kReserved)
        if (l == QLatin1String(r)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Identifiers — eagerly initialized at startup, without touching DNS
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
    // A hand-edited settings.json can carry any unique_id; the REST route is the
    // only other writer and already rejects reserved labels. Regenerate instead
    // of registering a label the DNS stack owns (www/api/stats/stream/...).
    if (!m_UniqueId.isEmpty() && isReservedSubdomain(m_UniqueId)) {
        qWarning() << "[InternetAccess] unique_id from settings is a reserved subdomain:"
                   << m_UniqueId << "— regenerating";
        m_UniqueId.clear();
    }

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

    // Bring-your-own-domain: a valid FQDN in settings.json that is not the
    // computed one is the user's, so keep it verbatim — rewriting the sentinel
    // here would silently drop it at every boot, and CertManager would then
    // CN-check their certificate against a domain they never asked for.
    const QString stored = m_Settings->domain();
    m_CustomDomain = AppSettings::isValidFqdn(stored) && stored != buildDomain();
    if (m_CustomDomain) {
        m_LegacyDns = false;
        m_Domain = stored;
        qInfo() << "[InternetAccess] Custom domain from settings:" << m_Domain
                << "— DNS registration and ACME issuance are disabled";
        return;
    }

    // Only an instance that actually registered a subdomain under the retiring
    // DNS mechanism keeps a public domain. registered_uid is written on the
    // first successful A-record registration (shipped since v0.1.0) and never
    // cleared, so it is the one reliable marker — the skip-when-already-resolving
    // path never runs before a first registration succeeded.
    m_LegacyDns = !m_Settings->registeredUid().isEmpty();
    if (!m_LegacyDns) {
        // Fresh instance: unique_id stays (it seeds the deterministic UPnP
        // fallback port, so two instances on one LAN never collide) but it
        // never leaves this machine — no domain, no ownership token, nothing
        // to publish. An empty domain is what keeps every entry point
        // (shortcut, tray, hairpin test) on the LAN address.
        m_Domain.clear();
        return;
    }

    // Store sentinel — the real domain is always derivable from uniqueId + baseDomain.
    m_Domain = buildDomain();
    m_Settings->setDomain(QStringLiteral("MW_DOMAIN"));

    // Make the ownership token available on the PowerDNS client before any write
    // (release/claim/create), so every PATCH carries the X-MW-Owner header.
    ensureOwnerToken();
}

// ---------------------------------------------------------------------------
// A record management (subdomain under kBaseDomain)
// ---------------------------------------------------------------------------

QString InternetAccessManager::ensureOwnerToken()
{
    // Per-instance random token, generated once and persisted. It is presented
    // as the X-MW-Owner header on every write and enforced server-side by the
    // mw-proxy middleware, so it is NEVER written to a public TXT record.
    QString myToken = m_Settings->ownerToken();
    if (myToken.isEmpty()) {
        QByteArray raw(32, '\0');
        for (int i = 0; i < 32; ++i)
            raw[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
        myToken = QString::fromLatin1(raw.toBase64(QByteArray::OmitTrailingEquals));
        m_Settings->setOwnerToken(myToken);
    }
    m_Pdns.setOwnerToken(myToken);
    return myToken;
}

bool InternetAccessManager::claimOrVerifyOwnership(QString& errorMsg)
{
    Q_UNUSED(errorMsg);
    // Ownership is enforced by mw-proxy: the first writer of a subdomain claims
    // it (Trust-On-First-Use), and every later write must carry the matching
    // X-MW-Owner token. We only need to make sure the token is ready; if this
    // instance does not own the subdomain, the middleware rejects the A-record
    // write with an explicit error surfaced through PdnsClient.
    ensureOwnerToken();
    return true;
}

void InternetAccessManager::releaseOldSubdomain()
{
    const QString oldUid = m_Settings->registeredUid();
    if (oldUid.isEmpty() || oldUid == m_UniqueId) return; // nothing to release, or unchanged

    const QString ownerFqdn =
        QStringLiteral("_owner.") + oldUid + QLatin1Char('.') + baseDomain() + QLatin1Char('.');

    // Only release a subdomain we can prove we own. If the TXT exists but does
    // not match our token, another instance took it over — leave it alone.
    QString existing, getErr;
    if (m_Pdns.getTxtRecord(ownerFqdn, existing, getErr)) {
        if (!existing.isEmpty() && existing != m_Settings->ownerToken()) {
            qInfo() << "[InternetAccess] Old subdomain" << oldUid
                    << "now owned by another instance — not releasing";
            return;
        }
    } else {
        // Transient DNS API error — skip release this round, retry next time.
        qWarning() << "[InternetAccess] Could not verify old subdomain ownership"
                   << "(skipping release):" << getErr;
        return;
    }

    qInfo() << "[InternetAccess] Releasing previous subdomain:" << oldUid;
    QString delErr;
    m_Pdns.deleteSubdomain(oldUid, delErr);
    m_Pdns.deleteTxtRecord(ownerFqdn, delErr);
}

bool InternetAccessManager::createOrUpdateARecord()
{
    if (!m_LegacyDns) return false; // user-owned zone, or nothing registered — never write

    qInfo() << "[InternetAccess] Checking A record for subdomain:" << m_UniqueId;

    // One subdomain per owner: free the previously registered one if unique_id
    // changed, before claiming the new one.
    releaseOldSubdomain();

    // Cooperative ownership guard: never clobber another instance's subdomain.
    QString ownErr;
    if (!claimOrVerifyOwnership(ownErr)) {
        m_LastError = ownErr;
        emit error(ownErr);
        return false;
    }

    // We now hold the _owner TXT for m_UniqueId — record it as the registered
    // subdomain so a future unique_id change releases this one.
    m_Settings->setRegisteredUid(m_UniqueId);

    QString errorMsg;
    bool available = m_Pdns.checkSubdomainAvailable(m_UniqueId, errorMsg);

    if (available) {
        // Subdomain is available — create it with the current public IP
        if (m_PublicIp.isEmpty()) {
            m_LastError =
                QStringLiteral("Cannot create A record: public IP not detected. "
                               "Enable auto IP detection or set a static public IP in Settings.");
            qWarning() << "[InternetAccess]" << m_LastError;
            emit error(m_LastError);
            return false;
        }
        qInfo() << "[InternetAccess] No existing A record, creating with" << m_PublicIp;
        logDnsRegistrationAudit(QStringLiteral("create"));
        if (!m_Pdns.createOrUpdateSubdomain(m_UniqueId, m_PublicIp, kDefaultTtl, errorMsg)) {
            m_LastError = errorMsg;
            qWarning() << "[InternetAccess] A record creation failed:" << errorMsg;
            emit error(errorMsg);
            return false;
        }
        qInfo() << "[InternetAccess] A record created:" << m_Domain << "->" << m_PublicIp;
        return true;
    } else {
        if (errorMsg.contains("already has an A record")) {
            // Subdomain already exists — update A record with current public IP
            qInfo() << "[InternetAccess] A record already exists — updating to" << m_PublicIp;
            if (m_PublicIp.isEmpty()) {
                qWarning() << "[InternetAccess] Cannot update A record: public IP not detected";
                return false;
            }
            return updateARecord();
        }
        m_LastError = errorMsg;
        qWarning() << "[InternetAccess] Subdomain check failed:" << errorMsg;
        emit error(errorMsg);
        return false;
    }
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
            // Prepend the configured STUN server as first priority
            servers.prepend({host, port});
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
// A record update via PowerDNS
// ---------------------------------------------------------------------------

void InternetAccessManager::logDnsRegistrationAudit(const QString& action)
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/internet-access-audit.log"));
    if (!f.open(QIODevice::Append)) {
        qWarning() << "[InternetAccess] Cannot open audit log:" << f.fileName();
        return;
    }

    // One JSON object per line (JSONL): machine-parseable, append-only.
    QJsonObject entry;
    entry[QStringLiteral("datetime")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    entry[QStringLiteral("action")] = action; // "create" | "update"
    entry[QStringLiteral("unique_id")] = m_UniqueId;
    entry[QStringLiteral("domain")] = m_Domain;
    entry[QStringLiteral("public_ip")] = m_PublicIp;
    entry[QStringLiteral("local_ip")] = m_LocalIp;
    entry[QStringLiteral("app_version")] = QCoreApplication::applicationVersion();
    const QJsonObject consent = m_Settings->internetConsent();
    entry[QStringLiteral("consent_message")] = consent.value(QStringLiteral("message")).toString();
    entry[QStringLiteral("consent_at")] = consent.value(QStringLiteral("at")).toString();
    entry[QStringLiteral("consent_source")] = consent.value(QStringLiteral("source")).toString();

    f.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + "\n");
    f.close();
}

bool InternetAccessManager::updateARecord()
{
    // Only a legacy registration is ours to update — the user's own zone is
    // not ours to write to, and a fresh install has no record at all (the
    // periodic IP-change path lands here directly).
    if (!m_LegacyDns) return false;

    if (m_Domain.isEmpty() || m_PublicIp.isEmpty()) {
        qWarning() << "[InternetAccess] Cannot update A record: domain or IP empty";
        return false;
    }

    qInfo() << "[InternetAccess] Updating A record:" << m_Domain << "->" << m_PublicIp;

    logDnsRegistrationAudit(QStringLiteral("update"));
    QString errorMsg;
    if (!m_Pdns.createOrUpdateSubdomain(m_UniqueId, m_PublicIp, kDefaultTtl, errorMsg)) {
        m_LastError = errorMsg;
        qWarning() << "[InternetAccess] A record update failed:" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    qInfo() << "[InternetAccess] A record updated successfully";
    return true;
}

// ---------------------------------------------------------------------------
// TLS certificate management via native ACME client (DNS-01)
// ---------------------------------------------------------------------------

bool InternetAccessManager::issueCertificate()
{
    // ACME DNS-01 writes the challenge through the PowerDNS API, which only
    // covers a legacy subdomain — a manual /api/internet/renew-cert on a
    // user-owned domain (their cert) or a fresh install (no public domain)
    // must not even try.
    if (!m_LegacyDns) {
        qWarning() << "[InternetAccess] Certificate issuance skipped — no legacy DNS registration"
                   << (m_CustomDomain ? "(custom domain is managed by the user)" : "");
        return false;
    }

    if (m_CertIssuing) {
        qInfo() << "[InternetAccess] ACME issuance already in progress";
        return true;
    }

    if (m_Domain.isEmpty()) {
        qWarning() << "[InternetAccess] Cannot issue certificate: no domain configured";
        return false;
    }

    // Certificate storage: %AppData%/mw-server/cert/letsencrypt/
    // HttpServer::findCertDir() also checks %AppData%/cert/ so we copy there on success.
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString certDir = appData + QStringLiteral("/cert/") + kAcmeCertDir;
    QDir().mkpath(certDir);

    qInfo() << "[InternetAccess] issueCertificate: domain=" << m_Domain << "certDir=" << certDir
            << "existing keys: account=" << QFile::exists(certDir + "/account_key.pem")
            << "domain=" << QFile::exists(certDir + "/domain_key.pem");

    QString accountKeyPath = certDir + QStringLiteral("/account_key.pem");
    QString domainKeyPath = certDir + QStringLiteral("/domain_key.pem");

    // Configure the ACME client
    m_Acme.setAccountKeyPath(accountKeyPath);
    m_Acme.setDomainKeyPath(domainKeyPath);
    m_Acme.setCertOutputDir(certDir);
    m_Acme.setHost(m_Domain);
    m_Acme.setBaseDomain(baseDomain());
    m_Acme.setPdnsToken(QString::fromUtf8(qgetenv("MW_PDNS_TOKEN")));
    // Prove subdomain ownership to mw-proxy for the _acme-challenge TXT writes.
    m_Acme.setOwnerToken(ensureOwnerToken());

    // ACME CA selection. When ZeroSSL EAB credentials are present, issue from
    // ZeroSSL (USERTrust/Sectigo root — same trust as PositiveSSL) so corporate
    // networks that reject the Let's Encrypt root still accept the certificate.
    // Each instance gets its own key; no shared wildcard. Falls back to Let's
    // Encrypt when EAB is not configured.
    const QString eabKid = QString::fromUtf8(qgetenv("MW_ZEROSSL_EAB_KID"));
    const QString eabHmac = QString::fromUtf8(qgetenv("MW_ZEROSSL_EAB_HMAC"));
    if (!eabKid.isEmpty() && !eabHmac.isEmpty()) {
        QString dir = QString::fromUtf8(qgetenv("MW_ACME_DIRECTORY"));
        if (dir.isEmpty()) dir = QStringLiteral("https://acme.zerossl.com/v2/DV90");
        m_Acme.setDirectoryUrl(dir);
        m_Acme.setExternalAccountBinding(eabKid, eabHmac);
        qInfo() << "[InternetAccess] ACME provider: ZeroSSL (EAB set), directory=" << dir;
    } else {
        qInfo() << "[InternetAccess] ACME provider: Let's Encrypt (no EAB configured)";
    }

    m_CertIssuing = true;
    qInfo() << "[InternetAccess] Starting ACME certificate issuance for" << m_Domain;

    m_Acme.start();
    return true;
}

// Read certificate expiry date directly from the PEM file on disk.
// Avoids storing a redundant cert_expiry field in settings.json.
QString InternetAccessManager::readCertExpiry(const QString& certPath)
{
    if (certPath.isEmpty()) return {};

    QFile f(certPath);
    if (!f.open(QIODevice::ReadOnly)) return {};

    QList<QSslCertificate> certs = QSslCertificate::fromDevice(&f, QSsl::Pem);
    f.close();

    if (certs.isEmpty()) return {};

    QDateTime expiry = certs.first().expiryDate();
    if (!expiry.isValid()) return {};

    return expiry.toUTC().toString(Qt::ISODate);
}

bool InternetAccessManager::checkCertificate()
{
    // User-owned domain: the certificate is theirs (dropped in the cert
    // directory or pointed at by cert_pem/cert_key). Never reissue over it.
    // Fresh install: no public domain, nothing to certify — the LAN serves
    // the self-signed certificate.
    if (!m_LegacyDns) {
        qInfo() << "[InternetAccess] No legacy DNS registration — certificate lifecycle"
                << (m_CustomDomain ? "left to the user" : "not applicable (self-signed on LAN)");
        return false;
    }

    QString certPem = m_Settings->certPem();
    QString certKey = m_Settings->certKey();
    QString currentDomain = m_Domain;

    qInfo() << "[InternetAccess] checkCertificate: cert_pem=\"" << certPem << "\" cert_key=\""
            << certKey << "\" domain=\"" << currentDomain << "\"";

    if (certPem.isEmpty()) {
        qInfo() << "[InternetAccess] cert_pem is empty — issuing new certificate";
        return issueCertificate();
    }

    // Case 1: cert_pem is an env var name (e.g. "MW_CERT_PEM") — try to resolve it
    QByteArray certData = qgetenv(certPem.toUtf8());
    if (!certData.isEmpty()) {
        // Certificate is managed via environment variables — check the key too
        QByteArray keyData = qgetenv(certKey.toUtf8());
        if (keyData.isEmpty()) {
            qInfo() << "[InternetAccess] cert_pem resolved from env var but cert_key (" << certKey
                    << ") is empty — cannot validate, skipping ACME";
            return false; // User manages certs manually, don't interfere
        }

        // Parse cert from env data to check CN
        QList<QSslCertificate> certs = QSslCertificate::fromData(certData, QSsl::Pem);
        if (certs.isEmpty()) {
            qInfo() << "[InternetAccess] Invalid certificate data from env var" << certPem;
            return false;
        }

        QString certCn = certs.first().subjectInfo(QSslCertificate::CommonName).value(0);
        QDateTime expiry = certs.first().expiryDate();

        qInfo() << "[InternetAccess] Env cert: CN=\"" << certCn
                << "\" expires=" << expiry.toString(Qt::ISODate);

        if (!currentDomain.isEmpty() && certCn.compare(currentDomain, Qt::CaseInsensitive) != 0) {
            qInfo() << "[InternetAccess] Env cert CN mismatch — reissuing";
            return issueCertificate();
        }

        // Don't auto-renew env-managed certs — user handles their own lifecycle
        qInfo() << "[InternetAccess] Certificate managed via env vars — ACME skipped";
        return false;
    }

    // Case 2: cert_pem is a file path (ACME managed)
    const QString& certPath = certPem;
    QString certExpiry = readCertExpiry(certPath);

    if (certExpiry.isEmpty()) {
        qInfo() << "[InternetAccess] Cannot read expiry from" << certPath << "— issuing new one";
        return issueCertificate();
    }

    // Check if certificate file exists on disk
    if (!QFile::exists(certPath)) {
        qInfo() << "[InternetAccess] Certificate file not found at" << certPath
                << "— issuing new one";
        return issueCertificate();
    }

    // Verify the cert_key also points to a valid key file
    {
        QByteArray keyData = qgetenv(certKey.toUtf8());
        if (keyData.isEmpty()) {
            // cert_key is a file path — check it exists
            if (!QFile::exists(certKey)) {
                qInfo() << "[InternetAccess] Key file not found at" << certKey << "— reissuing";
                return issueCertificate();
            }
        }
    }

    // Parse certificate to verify CN
    QString certCn;
    {
        QFile f(certPath);
        if (f.open(QIODevice::ReadOnly)) {
            QList<QSslCertificate> certs = QSslCertificate::fromDevice(&f, QSsl::Pem);
            f.close();
            if (!certs.isEmpty())
                certCn = certs.first().subjectInfo(QSslCertificate::CommonName).value(0);
        }
    }

    qInfo() << "[InternetAccess] Certificate CN=\"" << certCn << "\"";

    if (!currentDomain.isEmpty() && !certCn.isEmpty()) {
        if (certCn.compare(currentDomain, Qt::CaseInsensitive) != 0) {
            qInfo() << "[InternetAccess] Certificate CN mismatch: got \"" << certCn
                    << "\", expected \"" << currentDomain << "\" — reissuing";
            return issueCertificate();
        }
        qInfo() << "[InternetAccess] Certificate CN matches domain: " << currentDomain;
    } else if (!currentDomain.isEmpty() && certCn.isEmpty()) {
        qInfo() << "[InternetAccess] Could not extract CN from certificate — will reissue";
        return issueCertificate();
    }

    // Check expiry
    QDateTime expiry = QDateTime::fromString(certExpiry, Qt::ISODate);
    if (!expiry.isValid()) {
        qInfo() << "[InternetAccess] Invalid certificate expiry string \"" << certExpiry
                << "\" — reissuing";
        return issueCertificate();
    }

    QDateTime now = QDateTime::currentDateTimeUtc();
    qint64 daysRemaining = now.daysTo(expiry);
    qInfo() << "[InternetAccess] Certificate expires in" << daysRemaining
            << "days (threshold:" << kCertRenewalDays << ")";

    if (daysRemaining < kCertRenewalDays) {
        qInfo() << "[InternetAccess] Certificate renewal needed — expires in" << daysRemaining
                << "days";
        return issueCertificate();
    }

    qInfo() << "[InternetAccess] Certificate valid for" << daysRemaining
            << "more days — no action needed";
    return true;
}

// ---------------------------------------------------------------------------
// Key-certificate pair validation
// ---------------------------------------------------------------------------

/// Verify that the private key PEM file matches the certificate PEM file.
/// Returns true if the key's public key matches the certificate's public key.
static bool validateCertKeyPair(const QString& certPath, const QString& keyPath)
{
    if (certPath.isEmpty() || keyPath.isEmpty()) return false;

    // Load certificate
    BIO* certBio = BIO_new_file(certPath.toUtf8().constData(), "r");
    if (!certBio) {
        qWarning() << "[InternetAccess] Cannot open cert for validation:" << certPath;
        return false;
    }
    X509* cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO_free(certBio);
    if (!cert) {
        qWarning() << "[InternetAccess] Failed to parse certificate for validation:" << certPath;
        return false;
    }

    // Extract public key from leaf certificate
    EVP_PKEY* certPubKey = X509_get_pubkey(cert);
    if (!certPubKey) {
        qWarning() << "[InternetAccess] Failed to extract public key from certificate";
        X509_free(cert);
        return false;
    }

    // Load private key
    BIO* keyBio = BIO_new_file(keyPath.toUtf8().constData(), "r");
    if (!keyBio) {
        qWarning() << "[InternetAccess] Cannot open key for validation:" << keyPath;
        EVP_PKEY_free(certPubKey);
        X509_free(cert);
        return false;
    }
    EVP_PKEY* privKey = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    BIO_free(keyBio);
    if (!privKey) {
        qWarning() << "[InternetAccess] Failed to parse private key for validation:" << keyPath;
        EVP_PKEY_free(certPubKey);
        X509_free(cert);
        return false;
    }

    // Compare public keys: returns 1 if matching
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    int match = EVP_PKEY_eq(certPubKey, privKey);
#else
    int match = EVP_PKEY_cmp(certPubKey, privKey);
#endif
    bool ok = (match == 1);

    if (!ok) {
        qWarning() << "[InternetAccess] Key-cert MISMATCH: cert=" << certPath << "key=" << keyPath;
    }

    EVP_PKEY_free(privKey);
    EVP_PKEY_free(certPubKey);
    X509_free(cert);
    return ok;
}

// ---------------------------------------------------------------------------
// ACME client signal handlers
// ---------------------------------------------------------------------------

void InternetAccessManager::onAcmeProgress(const QString& message)
{
    qInfo() << "[AcmeClient]" << message;
}

void InternetAccessManager::onAcmeError(const QString& message)
{
    m_LastError = message;
    qWarning() << "[InternetAccess] ACME error:" << message;
    emit error(message);
}

void InternetAccessManager::onAcmeFinished(bool success)
{
    m_CertIssuing = false;

    if (success) {
        qInfo() << "[InternetAccess] TLS certificate issued successfully";

        // ACME client saves to: AppData/mw-server/cert/letsencrypt/{cert,fullchain,key}.pem
        // HttpServer::findCertDir() looks for cert.pem + key.pem in AppData/mw-server/cert/
        // so we copy them one level up.
        QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QString srcDir = appData + QStringLiteral("/cert/") + kAcmeCertDir;
        // No trailing slash: child paths add their own "/" (avoids "cert//key.pem").
        QString dstDir = appData + QStringLiteral("/cert");

        qInfo() << "[InternetAccess] onAcmeFinished: appData=" << appData << "srcDir=" << srcDir
                << "dstDir=" << dstDir;

        // Copy cert.pem to parent dir (for HttpServer discovery)
        QString srcCert = srcDir + QStringLiteral("/cert.pem");
        QString dstCert = dstDir + QStringLiteral("/cert.pem");
        bool certCopied = false;
        if (QFile::exists(srcCert)) {
            QFile::remove(dstCert);
            certCopied = QFile::copy(srcCert, dstCert);
        }
        qInfo() << "[InternetAccess] cert.pem: src_exists=" << QFile::exists(srcCert)
                << "copied=" << certCopied;

        // Copy key.pem to parent dir
        QString srcKey = srcDir + QStringLiteral("/key.pem");
        QString dstKey = dstDir + QStringLiteral("/key.pem");
        bool keyCopied = false;
        if (QFile::exists(srcKey)) {
            QFile::remove(dstKey);
            keyCopied = QFile::copy(srcKey, dstKey);
            if (keyCopied) {
                // Restrict the copied key to the owner (QFile::copy resets perms).
                QFile::setPermissions(dstKey, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            }
        }
        // Also lock down the ACME working-dir key.
        QFile::setPermissions(srcKey, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        qInfo() << "[InternetAccess] key.pem: src_exists=" << QFile::exists(srcKey)
                << "copied=" << keyCopied;

        // Copy fullchain.pem to parent dir (for HttpServer discovery with chain)
        QString srcFullchain = srcDir + QStringLiteral("/fullchain.pem");
        QString dstFullchain = dstDir + QStringLiteral("/fullchain.pem");
        bool fullchainCopied = false;
        if (QFile::exists(srcFullchain)) {
            QFile::remove(dstFullchain);
            fullchainCopied = QFile::copy(srcFullchain, dstFullchain);
        }
        qInfo() << "[InternetAccess] fullchain.pem: src_exists=" << QFile::exists(srcFullchain)
                << "copied=" << fullchainCopied;

        // Determine which cert+key pair to use.
        // Priority: dstDir (copied to stable location) > srcDir (ACME working dir).
        bool useDst = false;
        QString fullchainPath;
        QString certKeyPath;

        // Prefer dstDir fullchain if it was copied successfully
        if (fullchainCopied && QFile::exists(dstFullchain)) {
            fullchainPath = dstFullchain;
            // Prefer dstDir key.pem if copied successfully; fall back to srcDir/domain_key.pem
            if (keyCopied && QFile::exists(dstKey)) {
                certKeyPath = dstKey;
            } else {
                QString dk = srcDir + QStringLiteral("/domain_key.pem");
                certKeyPath = QFile::exists(dk) ? dk : srcDir + QStringLiteral("/key.pem");
            }
            useDst = true;
        } else {
            // Fallback: use srcDir directly (ACME output dir)
            fullchainPath = srcDir + QStringLiteral("/fullchain.pem");
            QString dk = srcDir + QStringLiteral("/domain_key.pem");
            certKeyPath = QFile::exists(dk) ? dk : srcDir + QStringLiteral("/key.pem");
        }

        qInfo() << "[InternetAccess] final cert_pem=" << fullchainPath << "cert_key=" << certKeyPath
                << "useDst=" << useDst;

        // Validate that the private key matches the certificate BEFORE updating settings.
        // This catches the case where ACME issued a cert for one key but we're trying to
        // pair it with a different key (e.g. stale domain_key.pem from a previous run).
        if (!QFile::exists(fullchainPath)) {
            qWarning() << "[InternetAccess] fullchain.pem NOT FOUND at" << fullchainPath;
            m_LastError =
                QStringLiteral("ACME completed but certificate file missing at ") + fullchainPath;
            emit error(m_LastError);
        } else if (!QFile::exists(certKeyPath)) {
            qWarning() << "[InternetAccess] Key file NOT FOUND at" << certKeyPath;
            m_LastError = QStringLiteral("ACME completed but key file missing at ") + certKeyPath;
            emit error(m_LastError);
        } else if (!validateCertKeyPair(fullchainPath, certKeyPath)) {
            // Key does not match certificate — this is a critical error.
            // Do NOT update settings or emit certificateChanged, as it would cause
            // the next reloadTls() to fail with "key values mismatch".
            // Self-heal: delete the stale domain_key.pem so the NEXT ACME run
            // generates a fresh keypair that matches the new certificate.
            QString domainKeyPath = srcDir + QStringLiteral("/domain_key.pem");
            if (QFile::remove(domainKeyPath)) {
                qInfo() << "[InternetAccess] Deleted stale domain_key.pem at" << domainKeyPath
                        << "— next ACME issuance will generate a fresh keypair";
            }
            m_LastError =
                QStringLiteral("ACME issued a certificate but the private key does not match. "
                               "A fresh keypair will be generated on the next renewal attempt.");
            qWarning() << "[InternetAccess]" << m_LastError;
            emit error(m_LastError);
        } else {
            // Key-cert pair validated — persist the paths
            m_Settings->setCertPem(fullchainPath);
            m_Settings->setCertKey(certKeyPath);

            qInfo() << "[InternetAccess] cert_pem:" << m_Settings->certPem()
                    << "cert_key:" << m_Settings->certKey()
                    << "expires:" << readCertExpiry(fullchainPath);

            emit certificateChanged();
        }
    } else {
        qWarning() << "[InternetAccess] ACME issuance failed — cert_pem/cert_key remain empty, "
                      "check previous ACME errors";
        // Surface it: the domain stays on the self-signed fallback, and callers
        // waiting on the certificate (the installer checklist, the entry URL)
        // would otherwise wait out their whole budget for a signal never coming.
        m_LastError = QStringLiteral("TLS certificate issuance failed — the public address "
                                     "falls back to an untrusted certificate.");
        emit error(m_LastError);
    }

    emit statusChanged(statusJson());
}

// ---------------------------------------------------------------------------
// Periodic checks
// ---------------------------------------------------------------------------

void InternetAccessManager::onPeriodicCheck()
{
    qInfo() << "[InternetAccess] Periodic check triggered";

    // 1. Re-detect public IP if auto-detection is enabled
    if (m_Settings->autoIpDetection()) {
        QString oldIp = m_PublicIp;
        detectPublicIp();

        if (m_PublicIp != oldIp && !m_PublicIp.isEmpty()) {
            qInfo() << "[InternetAccess] Public IP changed from" << oldIp << "to" << m_PublicIp
                    << "— updating DNS";
            updateARecord();
            m_Settings->setPublicIp(m_PublicIp);
        }
    }

    // 2. Check DNS resolution (max once every 24h) — legacy registrations only,
    // a fresh install has no domain to resolve.
    if (m_LegacyDns) {
        QDateTime now = QDateTime::currentDateTimeUtc();
        if (!m_LastDnsCheck.isValid() || m_LastDnsCheck.secsTo(now) >= 86400) {
            QString resolvedIp = resolveDomain(m_Domain);
            if (!resolvedIp.isEmpty()) {
                if (!m_PublicIp.isEmpty() && resolvedIp != m_PublicIp) {
                    qInfo() << "[InternetAccess] DNS resolved to" << resolvedIp << "but expected"
                            << m_PublicIp << "— updating A record";
                    updateARecord();
                }
            } else {
                qWarning() << "[InternetAccess] DNS resolution failed for" << m_Domain;
                if (pingDomain(m_Domain)) {
                    qInfo() << "[InternetAccess] Domain" << m_Domain
                            << "is reachable via ping despite DNS failure";
                } else {
                    qWarning() << "[InternetAccess] Domain" << m_Domain
                               << "not reachable via ping either";
                }
            }
            m_LastDnsCheck = now;
        }
    }

    // 3. Check certificate renewal (skip if issuance already in progress)
    if (!m_CertIssuing) {
        checkCertificate();
    } else {
        qInfo() << "[InternetAccess] ACME issuance in progress, skipping cert check";
    }

    // 4. Re-verify UPnP mappings (legacy only — a fresh install forwards no web
    // ports). Idempotent and self-healing: if our external port was taken over
    // since last time, the next free candidate is re-derived — no eviction, so
    // instances never fight over a port. HTTPS keeps strict external==internal
    // parity (rebinding the listener if needed).
    if (m_LegacyDns && m_Settings->upnpEnabled() && m_Upnp.isAvailable()) {
        m_ExternalHttpsPort = mapHttpsPortParity();

        quint16 httpPort = m_HttpPort > 0 ? m_HttpPort : m_Settings->httpPort(80);
        m_ExternalHttpPort = mapPortWithFallback(httpPort, "TCP", "MoonlightWeb HTTP (renew)");

        m_ExternalUdpPort = mapPortWithFallback(47999, "UDP", "MoonlightWeb UDP Stream (renew)");
    }

    // 5. Refresh NAT hairpin reachability (router config can change).
    updateHairpinStatus();

    emit statusChanged(statusJson());
}

void InternetAccessManager::onPendingRegistrationRetry()
{
    m_PendingRetryCount++;

    if (m_PendingRetryCount > 3) {
        // Max retries exceeded — give up and disable Internet Access
        m_LastError =
            QStringLiteral("PowerDNS domain registration failed after 3 attempts. "
                           "Internet Access has been disabled. Check your network connectivity "
                           "and PowerDNS token, then re-enable Internet Access in Settings.");
        qWarning() << "[InternetAccess]" << m_LastError;

        m_Settings->setPendingRegistration(false);
        m_Settings->setInternetAccessEnabled(false);
        m_Phase = QStringLiteral("error");
        m_PendingRegistrationTimer->stop();
        m_PendingRetryCount = 0;

        emit error(m_LastError);
        emit statusChanged(statusJson());
        return;
    }

    // Fixed retry delays: 3s each
    static constexpr int kRetryDelaysSec[] = {3, 3, 3};
    int delaySec = kRetryDelaysSec[m_PendingRetryCount - 1];
    int delayMs = delaySec * 1000;

    qInfo() << "[InternetAccess] Retrying pending domain registration..."
            << "attempt" << m_PendingRetryCount << "/3"
            << "next retry in" << delaySec << "s";

    // Keep existing unique ID — do NOT regenerate on retry, otherwise
    // a new subdomain is created each time and the old domain is abandoned.
    // Re-set token (env var may have been configured since last attempt)
    QString token = QString::fromUtf8(qgetenv("MW_PDNS_TOKEN"));
    if (token.isEmpty()) {
        qInfo() << "[InternetAccess] Token still empty — will retry in" << delaySec << "s";
        m_PendingRegistrationTimer->start(delayMs);
        return;
    }
    m_Pdns.setToken(token);

    if (m_Settings->autoIpDetection()) {
        detectPublicIp();
    }

    if (createOrUpdateARecord()) {
        m_Settings->setPendingRegistration(false);
        m_PendingRetryCount = 0;
        qInfo() << "[InternetAccess] A record created on retry:" << m_Domain;

        // Continue with the rest of the setup
        m_Phase = QStringLiteral("issuing_certificate");
        checkCertificate();

        m_Active = true;
        m_Phase = QStringLiteral("active");
        m_PeriodicCheckTimer->start();

        emit ready(m_Domain, m_PublicIp);
    } else {
        qInfo() << "[InternetAccess] Registration retry failed (" << m_PendingRetryCount
                << "/3) — will retry in" << delaySec << "s";
        m_PendingRegistrationTimer->start(delayMs);
    }
}

// ---------------------------------------------------------------------------
// DNS resolution
// ---------------------------------------------------------------------------

QString InternetAccessManager::resolveDomain(const QString& domain)
{
    if (domain.isEmpty()) return {};

    qInfo() << "[InternetAccess] Resolving domain:" << domain;

    QHostInfo info = QHostInfo::fromName(domain);
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
        qWarning() << "[InternetAccess] DNS resolution failed for" << domain << ":"
                   << info.errorString();
        return {};
    }

    // Return the first A record result
    for (const QHostAddress& addr : info.addresses()) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
            QString ip = addr.toString();
            qInfo() << "[InternetAccess] Domain resolved:" << domain << "->" << ip;
            return ip;
        }
    }

    qWarning() << "[InternetAccess] No IPv4 address found for" << domain;
    return {};
}

// ---------------------------------------------------------------------------
// DNS fallback: ping the domain
// ---------------------------------------------------------------------------

bool InternetAccessManager::pingDomain(const QString& domain)
{
    if (domain.isEmpty()) return false;

    int exitCode = QProcess::execute(QStringLiteral("ping"),
                                     {QStringLiteral("-n"), QStringLiteral("1"),
                                      QStringLiteral("-w"), QStringLiteral("3000"), domain});

    bool reachable = (exitCode == 0);
    qInfo() << "[InternetAccess] Ping" << domain << ":" << (reachable ? "reachable" : "unreachable")
            << "(exit code:" << exitCode << ")";
    return reachable;
}
