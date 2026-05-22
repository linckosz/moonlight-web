#pragma once

#include <QObject>
#include <QTimer>
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include "DeSecClient.h"
#include "StunClient.h"
#include "UPNPClient.h"
#include "AcmeClient.h"

class AppSettings;

/**
 * @brief Orchestrates the full Internet Access feature.
 *
 * Responsibilities:
 *   1. Unique ID generation & deSEC domain registration
 *   2. Public IP detection via STUN (with fallback chain)
 *   3. A record management through deSEC API
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

    /// The registered domain name (e.g. "moonlightweb-abc123.dedyn.io").
    QString domain() const { return m_Domain; }

    /// Current public IP.
    QString publicIp() const { return m_PublicIp; }

    /// UPnP client (exposed for integration with existing session code).
    UPNPClient* upnpClient() { return &m_Upnp; }

    /// DeSec client (for direct API access if needed).
    DeSecClient* desecClient() { return &m_DeSec; }

signals:
    /// Emitted when Internet Access becomes fully operational.
    void ready(const QString& domain, const QString& publicIp);

    /// Emitted when a non-fatal error occurs (status update for UI).
    void statusChanged(const QJsonObject& status);

    /// Emitted when a fatal error prevents Internet Access from working.
    void error(const QString& message);

    /// Emitted when TLS certificate is renewed or changed.
    void certificateChanged(const QString& certPath);

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

    /// Create or verify the A record under the existing parent domain.
    bool createOrUpdateARecord();

    /// Detect public IP via STUN (with fallback chain).
    bool detectPublicIp();

    /// Update the A record on deSEC with the current public IP.
    bool updateARecord();

    /// Issue a TLS certificate via the native ACME client.
    bool issueCertificate();

    /// Check certificate expiry and renew if < 30 days remaining.
    bool checkCertificate();

    /// Load or resolve the effective deSEC token (handles "auto").
    QString effectiveToken() const;

    /// Build the domain name from unique ID.
    QString buildDomain() const;

    /// Resolve the domain via system DNS.
    QString resolveDomain(const QString& domain);

    /// Ping the domain (fallback when DNS fails).
    bool pingDomain(const QString& domain);

    // Owned sub-clients
    DeSecClient m_DeSec;
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

    // Retry state
    int m_PendingRetryCount = 0;          ///< Current retry attempt (0..3) for pending registration

    // Last DNS check timestamp (spaced to 24h)
    QDateTime m_LastDnsCheck;

    // Timers
    QTimer* m_PeriodicCheckTimer = nullptr;
    QTimer* m_PendingRegistrationTimer = nullptr;
};
