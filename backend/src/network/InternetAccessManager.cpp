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
    char buf[64] = {};
    if (UPNPClient::getLocalIP(buf, sizeof(buf))) {
        m_LocalIp = QString::fromLatin1(buf);
        qInfo() << "[InternetAccess] Local LAN IP:" << m_LocalIp;
    }

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
    qInfo() << "[InternetAccess] Step 1 OK — domain:" << m_Domain;

    // Step 2: Read the PowerDNS token from MW_PDNS_TOKEN env var
    QString token = QString::fromUtf8(qgetenv("MW_PDNS_TOKEN"));
    qInfo() << "[InternetAccess] Step 2 — token source: env var MW_PDNS_TOKEN"
            << "empty:" << token.isEmpty() << "length:" << token.length();

    if (token.isEmpty()) {
        m_LastError =
            QStringLiteral("PowerDNS token is empty. Set the MW_PDNS_TOKEN environment variable "
                           "in Qt Creator (Projects → Run → Environment) or your shell.");
        qWarning() << "[InternetAccess]" << m_LastError;
        m_Phase = QStringLiteral("pending");
        m_Settings->setPendingRegistration(true);
        m_PendingRegistrationTimer->start(kPendingRetryMs);
        return;
    }
    m_Pdns.setToken(token);

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
    {
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

    // Step 5: Initial DNS check (spaced to 24h thereafter)
    m_Phase = QStringLiteral("checking_dns");
    {
        QString resolvedIp = resolveDomain(m_Domain);
        if (!resolvedIp.isEmpty()) {
            qInfo() << "[InternetAccess] Step 5 — DNS check:" << m_Domain << "->" << resolvedIp;
        } else {
            qWarning() << "[InternetAccess] Step 5 — DNS resolution failed for" << m_Domain;
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

    // Step 6: Issue/renew TLS certificate
    m_Phase = QStringLiteral("issuing_certificate");
    {
        QString existingCert = m_Settings->certPem();
        qInfo() << "[InternetAccess] Step 6 — checking certificate: cert_pem=\"" << existingCert
                << "\"";
    }
    checkCertificate();

    // Step 7: UPnP port mapping
    m_Phase = QStringLiteral("configuring_ports");
    if (m_Settings->upnpEnabled()) {
        // Use the real server port if set, otherwise fall back to settings
        quint16 httpsPort = m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443);
        if (m_Upnp.discover()) {
            // Check for port conflicts before adding mappings
            auto checkAndMap = [this](quint16 port, const std::string& protocol, const char* desc) {
                std::string existingClient, existingPort;
                if (m_Upnp.getExistingPortMapping(port, protocol, existingClient, existingPort)) {
                    if (existingClient != m_Upnp.lanAddress()) {
                        if (m_ServiceManaged) {
                            // Service auto-restart: never steal a mapping owned by another
                            // device — only a manual launch is allowed to take over.
                            qWarning()
                                << "[InternetAccess] Port" << port << protocol.c_str()
                                << "already mapped to" << existingClient.c_str() << ":"
                                << existingPort.c_str() << "— another device owns this mapping";
                            m_LastError =
                                QStringLiteral(
                                    "Port %1/%2 already forwarded to %3 on this router. "
                                    "Free it in your router settings or use a different port.")
                                    .arg(port)
                                    .arg(QString::fromStdString(protocol))
                                    .arg(QString::fromStdString(existingClient));
                            emit error(m_LastError);
                            return false;
                        }
                        // Manual launch wins: evict the previous owner's mapping, then
                        // claim the port for this host (the last instance started wins).
                        qInfo() << "[InternetAccess] Port" << port << protocol.c_str() << "owned by"
                                << existingClient.c_str() << ":" << existingPort.c_str()
                                << "— taking over (manual launch)";
                        m_Upnp.removePortMapping(port, protocol);
                    } else {
                        // Same host — re-add to refresh the lease
                        qInfo() << "[InternetAccess] Port" << port << protocol.c_str()
                                << "already mapped to us (" << existingClient.c_str()
                                << ") — refreshing lease";
                    }
                }
                return m_Upnp.addPortMapping(port, port, 3600, desc, protocol);
            };

            checkAndMap(httpsPort, "TCP", "MoonlightWeb HTTPS");

            // Map HTTP port too, so the HTTP→HTTPS redirect works from the internet.
            // Without this mapping, external clients cannot reach the HTTP redirect
            // server through the NAT gateway.
            {
                quint16 httpPort = m_HttpPort > 0 ? m_HttpPort : m_Settings->httpPort(80);
                checkAndMap(httpPort, "TCP", "MoonlightWeb HTTP");
            }

            checkAndMap(47999, "UDP", "MoonlightWeb UDP Stream");

            // Capture local LAN IP for the UI (port mapping display)
            char buf[64] = {};
            if (UPNPClient::getLocalIP(buf, sizeof(buf))) {
                m_LocalIp = QString::fromLatin1(buf);
                qInfo() << "[InternetAccess] Local LAN IP:" << m_LocalIp;
            }

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

    // Check DNS resolution
    QString resolvedIp = resolveDomain(m_Domain);
    if (!resolvedIp.isEmpty() && resolvedIp != m_PublicIp && !m_PublicIp.isEmpty()) {
        // DNS mismatch — update A record
        qInfo() << "[InternetAccess] DNS resolution mismatch: resolved=" << resolvedIp
                << "expected=" << m_PublicIp << "— updating A record";
        updateARecord();
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
    obj[QStringLiteral("local_ip")] = m_LocalIp;
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
    obj[QStringLiteral("https_port")] = m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443);

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

// ---------------------------------------------------------------------------
// Identifiers — eagerly initialized at startup, without touching DNS
// ---------------------------------------------------------------------------

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

    // Store sentinel — the real domain is always derivable from uniqueId + baseDomain.
    // A custom FQDN (different from the computed one) would be stored by other paths.
    m_Domain = buildDomain();
    m_Settings->setDomain(QStringLiteral("MW_DOMAIN"));
}

// ---------------------------------------------------------------------------
// A record management (subdomain under kBaseDomain)
// ---------------------------------------------------------------------------

bool InternetAccessManager::claimOrVerifyOwnership(QString& errorMsg)
{
    // Per-instance random token, generated once and persisted.
    QString myToken = m_Settings->ownerToken();
    if (myToken.isEmpty()) {
        QByteArray raw(32, '\0');
        for (int i = 0; i < 32; ++i)
            raw[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
        myToken = QString::fromLatin1(raw.toBase64(QByteArray::OmitTrailingEquals));
        m_Settings->setOwnerToken(myToken);
    }

    const QString ownerFqdn =
        QStringLiteral("_owner.") + m_UniqueId + QLatin1Char('.') + baseDomain() + QLatin1Char('.');

    QString existing;
    if (!m_Pdns.getTxtRecord(ownerFqdn, existing, errorMsg)) {
        // Network/HTTP error: don't brick the user's own registration over a
        // transient DNS API issue — proceed (best-effort cooperative ownership).
        qWarning() << "[InternetAccess] Ownership check failed (proceeding):" << errorMsg;
        return true;
    }

    if (existing.isEmpty()) {
        // Unclaimed → claim it now.
        QString claimErr;
        if (!m_Pdns.createTxtRecord(ownerFqdn, myToken, kDefaultTtl, claimErr))
            qWarning() << "[InternetAccess] Failed to write ownership TXT:" << claimErr;
        else
            qInfo() << "[InternetAccess] Claimed subdomain ownership:" << m_UniqueId;
        return true;
    }

    if (existing == myToken) return true; // we already own it

    // Owned by a different instance — refuse to touch the A record.
    errorMsg =
        QStringLiteral("Subdomain %1 is already registered by another MoonlightWeb instance. "
                       "Pick a different subdomain (unique_id) for this machine.")
            .arg(m_Domain);
    qWarning() << "[InternetAccess]" << errorMsg;
    return false;
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

bool InternetAccessManager::updateARecord()
{
    if (m_Domain.isEmpty() || m_PublicIp.isEmpty()) {
        qWarning() << "[InternetAccess] Cannot update A record: domain or IP empty";
        return false;
    }

    qInfo() << "[InternetAccess] Updating A record:" << m_Domain << "->" << m_PublicIp;

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

    // 2. Check DNS resolution (max once every 24h)
    {
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

    // 4. Re-verify UPnP mappings
    if (m_Settings->upnpEnabled() && m_Upnp.isAvailable()) {
        // Same conflict check as in start() — detect if another device
        // claimed the port since our last mapping.
        auto checkAndRenew = [this](quint16 port, const std::string& protocol, const char* desc) {
            std::string existingClient, existingPort;
            if (m_Upnp.getExistingPortMapping(port, protocol, existingClient, existingPort)) {
                if (existingClient != m_Upnp.lanAddress()) {
                    qWarning() << "[InternetAccess] Port" << port << protocol.c_str()
                               << "now mapped to" << existingClient.c_str()
                               << "— port was taken over by another device";
                    m_LastError =
                        QStringLiteral("Port %1/%2 has been taken over by %3. "
                                       "Free it in your router settings or use a different port.")
                            .arg(port)
                            .arg(QString::fromStdString(protocol))
                            .arg(QString::fromStdString(existingClient));
                    emit error(m_LastError);
                    return;
                }
            }
            m_Upnp.addPortMapping(port, port, 3600, desc, protocol);
        };

        quint16 httpsPort = m_HttpsPort > 0 ? m_HttpsPort : m_Settings->httpsPort(443);
        checkAndRenew(httpsPort, "TCP", "MoonlightWeb HTTPS (renew)");

        {
            quint16 httpPort = m_HttpPort > 0 ? m_HttpPort : m_Settings->httpPort(80);
            checkAndRenew(httpPort, "TCP", "MoonlightWeb HTTP (renew)");
        }

        checkAndRenew(47999, "UDP", "MoonlightWeb UDP Stream (renew)");
    }

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
