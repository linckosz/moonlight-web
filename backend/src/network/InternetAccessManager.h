#pragma once

#include <QObject>
#include <QTimer>
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include "PdnsClient.h"
#include "StunClient.h"
#include "UPNPClient.h"
#include "AcmeClient.h"

class AppSettings;

/**
 * @brief Orchestrates the full Internet Access feature.
 *
 * Responsibilities:
 *   1. Unique ID generation & PowerDNS domain registration
 *   2. Public IP detection via STUN (with fallback chain)
 *   3. A record management through PowerDNS API
 *   4. Periodic checks every 5 minutes (IP change, DNS resolution)
 *   5. TLS certificate management via native ACMEv2 client (DNS-01)
 *   6. UPnP port mapping delegation
 *   7. Pending registration retry every 30s
 *
 * Lifecycle:
 *   - Created early in main(), registered with API routes.
 *   - start() is called when Internet Access is enabled.
 *   - stop() is called when Internet Access is disabled.
 *   - Periodic checks run automatically while enabled.
 *
 * Thread safety: all methods should be called from the Qt main thread only.
 */
class InternetAccessManager : public QObject
{
    Q_OBJECT

public:
    explicit InternetAccessManager(AppSettings* settings, QObject* parent = nullptr);
    ~InternetAccessManager() override;

    /// Enable Internet Access: register domain, detect IP, set up A record.
    void start();

    /// Disable Internet Access: stop timers, clean up.
    void stop();

    /// Force re-check public IP and update DNS immediately.
    void forceRefresh();

    /// Force TLS certificate renewal immediately.
    void renewCertificate();

    /// Get current status as JSON object (for API responses).
    QJsonObject statusJson() const;

    /// Whether the manager is currently active (Internet Access enabled).
    bool isActive() const { return m_Active; }

    /// The registered domain name (e.g. "92b8d127.example.com").
    QString domain() const { return m_Domain; }

    /// Current public IP.
    QString publicIp() const { return m_PublicIp; }

    /// UPnP client (exposed for integration with existing session code).
    UPNPClient* upnpClient() { return &m_Upnp; }

    /// PowerDNS client (for direct API access if needed).
    PdnsClient* pdnsClient() { return &m_Pdns; }

    /// Set the actual HTTP and HTTPS ports the server is listening on.
    /// Must be called before start() so UPnP mappings use the correct ports.
    void setPorts(quint16 httpPort, quint16 httpsPort);

signals:
    /// Emitted when Internet Access becomes fully operational.
    void ready(const QString& domain, const QString& publicIp);

    /// Emitted when a non-fatal error occurs (status update for UI).
    void statusChanged(const QJsonObject& status);

    /// Emitted when a fatal error prevents Internet Access from working.
    void error(const QString& message);

    /// Emitted when TLS certificate is renewed or changed.
    void certificateChanged();

private slots:
    /// Called every 5 minutes for periodic checks.
    void onPeriodicCheck();

    /// Called when pending registration is active. Retries with fixed delay (3s, 3s, 3s), max 3 attempts.
    void onPendingRegistrationRetry();

    /// Called when ACME client reports progress.
    void onAcmeProgress(const QString& message);

    /// Called when ACME client errors out.
    void onAcmeError(const QString& message);

    /// Called when ACME client finishes (success or failure).
    void onAcmeFinished(bool success);

private:
    /// Generate a unique 8-char hex ID.
    QString generateUniqueId();

    /// Eager init: ensure unique_id and domain exist, without touching DNS.
    void ensureIdentifiers();

    /// Create or verify the A record under the existing parent domain.
    bool createOrUpdateARecord();

    /// Claim (if unowned) or verify ownership of this instance's subdomain via a
    /// _owner.<uid> TXT record. Returns false only when another instance owns it.
    bool claimOrVerifyOwnership(QString& errorMsg);

    /// Detect public IP via STUN (with fallback chain).
    bool detectPublicIp();

    /// Fallback: detect public IP via HTTP (ipify.org / icanhazip.com).
    /// Used when STUN servers all fail to respond.
    QString detectPublicIpViaHttp();

    /// Update the A record on PowerDNS with the current public IP.
    bool updateARecord();

    /// Issue a TLS certificate via the native ACME client.
    bool issueCertificate();

    /// Read certificate expiry date directly from the PEM file.
    /// Returns ISO 8601 string, or empty string if file missing/invalid.
    static QString readCertExpiry(const QString& certPath);

    /// Check certificate expiry and renew if < 30 days remaining.
    bool checkCertificate();

    /// Build the domain name from unique ID.
    QString buildDomain() const;

    /// Resolve the domain via system DNS.
    QString resolveDomain(const QString& domain);

    /// Ping the domain (fallback when DNS fails).
    bool pingDomain(const QString& domain);

    // Owned sub-clients
    PdnsClient m_Pdns;
    StunClient m_Stun;
    UPNPClient m_Upnp;
    AcmeClient m_Acme;

    // Settings reference (not owned)
    AppSettings* m_Settings = nullptr;

    // State
    bool m_Active = false;
    QString m_Domain;
    QString m_PublicIp;
    QString m_LocalIp;       ///< LAN IP address of this host (discovered via UPnP or GetAdaptersAddresses)
    QString m_UniqueId;
    QString m_LastError;
    bool m_CertIssuing = false;  ///< True while ACME issuance is in progress
    quint16 m_HttpPort = 0;      ///< Actual HTTP server port
    quint16 m_HttpsPort = 0;     ///< Actual HTTPS server port

    // Retry state
    int m_PendingRetryCount = 0;          ///< Current retry attempt (0..3) for pending registration

    // Last DNS check timestamp (spaced to 24h)
    QDateTime m_LastDnsCheck;

    // Timers
    QTimer* m_PeriodicCheckTimer = nullptr;
    QTimer* m_PendingRegistrationTimer = nullptr;
};
