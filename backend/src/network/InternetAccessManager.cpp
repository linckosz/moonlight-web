#include "InternetAccessManager.h"
#include "server/AppSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QDateTime>
#include <QStandardPaths>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Periodic check interval: 5 minutes.
static constexpr int kPeriodicCheckMs = 5 * 60 * 1000;

/// Pending registration retry interval: 30 seconds.
static constexpr int kPendingRetryMs = 30 * 1000;

/// Parent domain registered on deSEC (must already exist).
/// Subdomains are created as A records under this domain.
static const QString kBaseDomain = QStringLiteral("moonlightweb.dedyn.io");

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
    , m_DeSec(this)
    , m_Stun(this)
    , m_Upnp(this)
    , m_Acme(this)
{
    // Periodic check timer (5 minutes)
    m_PeriodicCheckTimer = new QTimer(this);
    m_PeriodicCheckTimer->setSingleShot(false);
    m_PeriodicCheckTimer->setInterval(kPeriodicCheckMs);
    connect(m_PeriodicCheckTimer, &QTimer::timeout,
            this, &InternetAccessManager::onPeriodicCheck);

    // Pending registration retry timer (30 seconds)
    m_PendingRegistrationTimer = new QTimer(this);
    m_PendingRegistrationTimer->setSingleShot(true);
    connect(m_PendingRegistrationTimer, &QTimer::timeout,
            this, &InternetAccessManager::onPendingRegistrationRetry);

    // Connect ACME client signals
    connect(&m_Acme, &AcmeClient::progress,
            this, &InternetAccessManager::onAcmeProgress);
    connect(&m_Acme, &AcmeClient::errorOccurred,
            this, &InternetAccessManager::onAcmeError);
    connect(&m_Acme, &AcmeClient::finished,
            this, &InternetAccessManager::onAcmeFinished);

    // Restore persistent state
    m_UniqueId = m_Settings->uniqueId();
    m_Domain = m_Settings->domain();
    m_PublicIp = m_Settings->publicIp();

    // Eager UPnP discovery (deferred, non-blocking) so that upnp_available
    // is correctly reported even if Internet Access has never been enabled.
    QTimer::singleShot(2000, this, [this]() {
        if (!m_Upnp.isAvailable()) {
            qInfo() << "[InternetAccess] Eager UPnP discovery (2s deferred)";
            m_Upnp.discover(2000);

            // Capture local IP after UPnP discovery (or via fallback)
            char buf[64] = {};
            if (UPNPClient::getLocalIP(buf, sizeof(buf))) {
                m_LocalIp = QString::fromLatin1(buf);
                qInfo() << "[InternetAccess] Local LAN IP:" << m_LocalIp;
            }
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
    qInfo() << "[InternetAccess] baseDomain:" << kBaseDomain
            << "uniqueId:" << m_UniqueId
            << "fqdn:" << buildDomain();

    // Step 1: Generate or reuse unique ID
    if (m_UniqueId.isEmpty()) {
        m_UniqueId = generateUniqueId();
        m_Settings->setUniqueId(m_UniqueId);
    }
    m_Domain = buildDomain();
    m_Settings->setDomain(m_Domain);
    qInfo() << "[InternetAccess] Step 1 OK — domain:" << m_Domain;

    // Step 2: Set up the deSEC token
    QString token = effectiveToken();
    QString tokenSource = m_Settings->desecToken();
    if (tokenSource.isEmpty() || tokenSource == QStringLiteral("auto")) {
        tokenSource = QStringLiteral("env var DESEC_TOKEN");
    } else {
        tokenSource = QStringLiteral("settings (encrypted)");
    }
    qInfo() << "[InternetAccess] Step 2 — token source:" << tokenSource
            << "empty:" << token.isEmpty()
            << "length:" << token.length();

    if (token.isEmpty()) {
        m_LastError = QStringLiteral(
            "deSEC token is empty. Set the DESEC_TOKEN environment variable "
            "in Qt Creator (Projects → Run → Environment) or your shell.");
        qWarning() << "[InternetAccess]" << m_LastError;
        m_Settings->setPendingRegistration(true);
        m_PendingRegistrationTimer->start(kPendingRetryMs);
        return;
    }
    m_DeSec.setToken(token);

    // Step 3: Detect public IP via STUN (before A record creation)
    if (m_Settings->autoIpDetection()) {
        qInfo() << "[InternetAccess] Step 3 — detecting public IP via STUN...";
        detectPublicIp();
        qInfo() << "[InternetAccess] Step 3 — public IP:" << m_PublicIp;
    }

    // Step 4: Create or update A record (with real IP from STUN)
    qInfo() << "[InternetAccess] Step 4 — creating/verifying A record...";
    if (!createOrUpdateARecord()) {
        qWarning() << "[InternetAccess] Step 4 FAILED — A record creation failed,"
                   << "will retry in" << (kPendingRetryMs / 1000) << "s";
        m_Settings->setPendingRegistration(true);
        m_PendingRegistrationTimer->start(kPendingRetryMs);
        return;
    }
    m_Settings->setPendingRegistration(false);
    qInfo() << "[InternetAccess] Step 4 OK — A record exists";

    // Step 5: Initial DNS check (spaced to 24h thereafter)
    {
        QString resolvedIp = resolveDomain(m_Domain);
        if (!resolvedIp.isEmpty()) {
            qInfo() << "[InternetAccess] Step 5 — DNS check:" << m_Domain << "->" << resolvedIp;
        } else {
            qWarning() << "[InternetAccess] Step 5 — DNS resolution failed for" << m_Domain;
            if (pingDomain(m_Domain)) {
                qInfo() << "[InternetAccess] Domain" << m_Domain << "is reachable via ping despite DNS failure";
            } else {
                qWarning() << "[InternetAccess] Domain" << m_Domain << "not reachable via ping either";
            }
        }
        m_LastDnsCheck = QDateTime::currentDateTimeUtc();
    }

    // Step 6: Issue/renew TLS certificate
    checkCertificate();

    // Step 7: UPnP port mapping
    if (m_Settings->upnpEnabled()) {
        quint16 httpsPort = m_Settings->httpsPort(443);
        if (m_Upnp.discover()) {
            m_Upnp.addPortMapping(httpsPort, httpsPort, 3600,
                                  "Moonlight-Web HTTPS");

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
                    m_LastError = QStringLiteral(
                        "CGNAT detected: UPnP reports %1 but your public IP is %2. "
                        "Port forwarding may not work — contact your ISP.")
                        .arg(upnpIp, m_PublicIp);
                    qWarning() << "[InternetAccess]" << m_LastError;
                }
            }
        } else {
            qWarning() << "[InternetAccess] UPnP discovery failed — manual port forwarding may be needed";
        }
    }

    m_Active = true;
    emit ready(m_Domain, m_PublicIp);
    qInfo() << "[InternetAccess] Setup complete, domain:" << m_Domain
            << "public IP:" << m_PublicIp;

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
    qInfo() << "[InternetAccess] Stopped";
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
        qInfo() << "[InternetAccess] DNS resolution mismatch: resolved="
                << resolvedIp << "expected=" << m_PublicIp << "— updating A record";
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
    obj[QStringLiteral("upnp_enabled")] = m_Settings->upnpEnabled();
    obj[QStringLiteral("auto_ip_detection")] = m_Settings->autoIpDetection();
    obj[QStringLiteral("transport_mode")] = m_Settings->transportMode();
    obj[QStringLiteral("pending_registration")] = m_Settings->pendingRegistration();
    obj[QStringLiteral("cert_path")] = m_Settings->certPath();
    obj[QStringLiteral("cert_expiry")] = m_Settings->certExpiry();
    obj[QStringLiteral("cert_issuing")] = m_CertIssuing;
    obj[QStringLiteral("upnp_available")] = m_Upnp.isAvailable();
    obj[QStringLiteral("https_port")] = m_Settings->httpsPort(443);

    if (!m_LastError.isEmpty())
        obj[QStringLiteral("last_error")] = m_LastError;

    return obj;
}

// ---------------------------------------------------------------------------
// Token / ID helpers
// ---------------------------------------------------------------------------

QString InternetAccessManager::effectiveToken() const
{
    QString stored = m_Settings->desecToken();
    if (stored.isEmpty() || stored == QStringLiteral("auto")) {
        return AppSettings::defaultDesecToken();
    }
    return stored;
}

QString InternetAccessManager::buildDomain() const
{
    return m_UniqueId + QStringLiteral(".") + kBaseDomain;
}

QString InternetAccessManager::generateUniqueId()
{
    // Generate 8 random hex characters.
    QString hex(8, QChar('0'));
    for (int i = 0; i < 8; ++i) {
        hex[i] = QStringLiteral("0123456789abcdef")
                 .at(QRandomGenerator::global()->bounded(16));
    }
    qInfo() << "[InternetAccess] Generated unique ID:" << hex;
    return hex;
}

// ---------------------------------------------------------------------------
// A record management (subdomain under kBaseDomain)
// ---------------------------------------------------------------------------

bool InternetAccessManager::createOrUpdateARecord()
{
    qInfo() << "[InternetAccess] Checking A record for subdomain:" << m_UniqueId;

    QString errorMsg;
    bool available = m_DeSec.checkSubdomainAvailable(kBaseDomain, m_UniqueId, errorMsg);

    if (available) {
        // Subdomain is available — create it with the current public IP
        if (m_PublicIp.isEmpty()) {
            m_LastError = QStringLiteral(
                "Cannot create A record: public IP not detected. "
                "Enable auto IP detection or set a static public IP in Settings.");
            qWarning() << "[InternetAccess]" << m_LastError;
            emit error(m_LastError);
            return false;
        }
        qInfo() << "[InternetAccess] No existing A record, creating with" << m_PublicIp;
        if (!m_DeSec.createSubdomain(kBaseDomain, m_UniqueId, m_PublicIp, kDefaultTtl, errorMsg)) {
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
// Public IP detection via STUN
// ---------------------------------------------------------------------------

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
                if (ok && p > 0 && p <= 65535)
                    port = static_cast<quint16>(p);
            } else {
                host = stripped;
            }
        } else {
            int colon = configured.lastIndexOf(':');
            if (colon > 0) {
                host = configured.left(colon);
                bool ok;
                int p = configured.mid(colon + 1).toInt(&ok);
                if (ok && p > 0 && p <= 65535)
                    port = static_cast<quint16>(p);
            } else {
                host = configured;
            }
        }

        if (!host.isEmpty()) {
            // Prepend the configured STUN server as first priority
            servers.prepend({ host, port });
        }
    }

    QString detectedIp;
    if (m_Stun.detectPublicIp(servers, 3000, detectedIp)) {
        m_PublicIp = detectedIp;
        m_Settings->setPublicIp(m_PublicIp);
        qInfo() << "[InternetAccess] Public IP detected:" << m_PublicIp;
        return true;
    }

    // Fallback to stored public IP if STUN fails
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
// A record update via deSEC
// ---------------------------------------------------------------------------

bool InternetAccessManager::updateARecord()
{
    if (m_Domain.isEmpty() || m_PublicIp.isEmpty()) {
        qWarning() << "[InternetAccess] Cannot update A record: domain or IP empty";
        return false;
    }

    qInfo() << "[InternetAccess] Updating A record:" << m_Domain << "->" << m_PublicIp;

    QString errorMsg;
    if (!m_DeSec.updateSubdomain(kBaseDomain, m_UniqueId, m_PublicIp, kDefaultTtl, errorMsg)) {
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

    QString accountKeyPath = certDir + QStringLiteral("/account_key.pem");
    QString domainKeyPath  = certDir + QStringLiteral("/domain_key.pem");

    // Configure the ACME client
    m_Acme.setAccountKeyPath(accountKeyPath);
    m_Acme.setDomainKeyPath(domainKeyPath);
    m_Acme.setCertOutputDir(certDir);
    m_Acme.setHost(m_Domain);
    m_Acme.setBaseDomain(kBaseDomain);
    m_Acme.setDesecToken(effectiveToken());

    m_CertIssuing = true;
    qInfo() << "[InternetAccess] Starting ACME certificate issuance for" << m_Domain;

    m_Acme.start();
    return true;
}

bool InternetAccessManager::checkCertificate()
{
    QString certPath = m_Settings->certPath();
    QString certExpiry = m_Settings->certExpiry();

    if (certPath.isEmpty() || certExpiry.isEmpty()) {
        qInfo() << "[InternetAccess] No existing certificate — issuing new one";
        return issueCertificate();
    }

    // Check if certificate file exists
    if (!QFile::exists(certPath)) {
        qInfo() << "[InternetAccess] Certificate file not found — issuing new one";
        return issueCertificate();
    }

    // Check expiry
    QDateTime expiry = QDateTime::fromString(certExpiry, Qt::ISODate);
    if (!expiry.isValid()) {
        qInfo() << "[InternetAccess] Invalid certificate expiry — reissuing";
        return issueCertificate();
    }

    QDateTime now = QDateTime::currentDateTimeUtc();
    qint64 daysRemaining = now.daysTo(expiry);

    if (daysRemaining < kCertRenewalDays) {
        qInfo() << "[InternetAccess] Certificate expires in" << daysRemaining
                << "days (<" << kCertRenewalDays << ") — renewing";
        return issueCertificate();
    }

    qInfo() << "[InternetAccess] Certificate valid for" << daysRemaining << "more days";
    return true;
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
        QString srcDir  = appData + QStringLiteral("/cert/") + kAcmeCertDir;
        QString dstDir  = appData + QStringLiteral("/cert/");

        // Copy cert.pem to parent dir (for HttpServer discovery)
        QString srcCert = srcDir + QStringLiteral("/cert.pem");
        QString dstCert = dstDir + QStringLiteral("/cert.pem");
        if (QFile::exists(srcCert)) {
            QFile::remove(dstCert);
            QFile::copy(srcCert, dstCert);
        }

        // Copy key.pem to parent dir
        QString srcKey = srcDir + QStringLiteral("/key.pem");
        QString dstKey = dstDir + QStringLiteral("/key.pem");
        if (QFile::exists(srcKey)) {
            QFile::remove(dstKey);
            QFile::copy(srcKey, dstKey);
        }

        // Update settings
        m_Settings->setCertPath(srcDir + QStringLiteral("/fullchain.pem"));

        // Let's Encrypt certificates are valid for 90 days
        QDateTime now = QDateTime::currentDateTimeUtc();
        QDateTime expiry = now.addDays(89);
        m_Settings->setCertExpiry(expiry.toString(Qt::ISODate));

        qInfo() << "[InternetAccess] Certificate path:" << m_Settings->certPath()
                << "expires:" << m_Settings->certExpiry();

        emit certificateChanged(m_Settings->certPath());
    } else {
        qWarning() << "[InternetAccess] ACME issuance failed";
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
            qInfo() << "[InternetAccess] Public IP changed from" << oldIp
                    << "to" << m_PublicIp << "— updating DNS";
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
                    qInfo() << "[InternetAccess] DNS resolved to" << resolvedIp
                            << "but expected" << m_PublicIp << "— updating A record";
                    updateARecord();
                }
            } else {
                qWarning() << "[InternetAccess] DNS resolution failed for" << m_Domain;
                if (pingDomain(m_Domain)) {
                    qInfo() << "[InternetAccess] Domain" << m_Domain << "is reachable via ping despite DNS failure";
                } else {
                    qWarning() << "[InternetAccess] Domain" << m_Domain << "not reachable via ping either";
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
        quint16 httpsPort = m_Settings->httpsPort(443);
        m_Upnp.addPortMapping(httpsPort, httpsPort, 3600,
                              "Moonlight-Web HTTPS (renew)");
    }

    emit statusChanged(statusJson());
}

void InternetAccessManager::onPendingRegistrationRetry()
{
    m_PendingRetryCount++;

    if (m_PendingRetryCount > 3) {
        // Max retries exceeded — give up and disable Internet Access
        m_LastError = QStringLiteral(
            "deSEC domain registration failed after 3 attempts. "
            "Internet Access has been disabled. Check your network connectivity "
            "and deSEC token, then re-enable Internet Access in Settings.");
        qWarning() << "[InternetAccess]" << m_LastError;

        m_Settings->setPendingRegistration(false);
        m_Settings->setInternetAccessEnabled(false);
        m_PendingRegistrationTimer->stop();
        m_PendingRetryCount = 0;

        emit error(m_LastError);
        emit statusChanged(statusJson());
        return;
    }

    // Fixed retry delays: 3s each
    static constexpr int kRetryDelaysSec[] = { 3, 3, 3 };
    int delaySec = kRetryDelaysSec[m_PendingRetryCount - 1];
    int delayMs = delaySec * 1000;

    qInfo() << "[InternetAccess] Retrying pending domain registration..."
            << "attempt" << m_PendingRetryCount << "/3"
            << "next retry in" << delaySec << "s";

    // Regenerate unique ID and domain for each retry
    m_UniqueId = generateUniqueId();
    m_Settings->setUniqueId(m_UniqueId);
    m_Domain = buildDomain();
    m_Settings->setDomain(m_Domain);

    // Re-set token (env var may have been configured since last attempt)
    QString token = effectiveToken();
    if (token.isEmpty()) {
        qInfo() << "[InternetAccess] Token still empty — will retry in" << delaySec << "s";
        m_PendingRegistrationTimer->start(delayMs);
        return;
    }
    m_DeSec.setToken(token);

    if (m_Settings->autoIpDetection()) {
        detectPublicIp();
    }

    if (createOrUpdateARecord()) {
        m_Settings->setPendingRegistration(false);
        m_PendingRetryCount = 0;
        qInfo() << "[InternetAccess] A record created on retry:" << m_Domain;

        // Continue with the rest of the setup
        checkCertificate();

        m_Active = true;
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
    if (domain.isEmpty())
        return {};

    qInfo() << "[InternetAccess] Resolving domain:" << domain;

    QHostInfo info = QHostInfo::fromName(domain);
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
        qWarning() << "[InternetAccess] DNS resolution failed for" << domain
                   << ":" << info.errorString();
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
    if (domain.isEmpty())
        return false;

    int exitCode = QProcess::execute(QStringLiteral("ping"), {
        QStringLiteral("-n"), QStringLiteral("1"),
        QStringLiteral("-w"), QStringLiteral("3000"),
        domain
    });

    bool reachable = (exitCode == 0);
    qInfo() << "[InternetAccess] Ping" << domain << ":"
            << (reachable ? "reachable" : "unreachable")
            << "(exit code:" << exitCode << ")";
    return reachable;
}
