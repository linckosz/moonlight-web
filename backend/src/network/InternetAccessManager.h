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

#pragma once

#include <functional>

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

    /// True when @p label collides with a subdomain the PowerDNS stack owns
    /// (apex, www, api, stats, ns1/ns2, mail) or an internal token label
    /// (anything starting with '_', e.g. _owner / _acme-challenge). A
    /// per-instance unique_id must be rejected when this returns true, otherwise
    /// it would hijack the DNS server's own records. Case-insensitive.
    static bool isReservedSubdomain(const QString& label);

    /// The registered domain name (e.g. "92b8d127.example.com").
    QString domain() const { return m_Domain; }

    /// Current public IP.
    QString publicIp() const { return m_PublicIp; }

    /// External (router-side) HTTPS port this instance is reachable on from the
    /// internet. Equals the internal HTTPS port (443) for the first instance to
    /// claim it; a deterministic fallback port for further instances behind the
    /// same NAT. 0 until UPnP mapping has run.
    quint16 externalHttpsPort() const { return m_ExternalHttpsPort; }

    /// External (router-side) HTTP port (used for the HTTP→HTTPS redirect).
    quint16 externalHttpPort() const { return m_ExternalHttpPort; }

    /// UPnP client (exposed for integration with existing session code).
    UPNPClient* upnpClient() { return &m_Upnp; }

    /// PowerDNS client (for direct API access if needed).
    PdnsClient* pdnsClient() { return &m_Pdns; }

    /// Set the actual HTTP and HTTPS ports the server is listening on.
    /// Must be called before start() so UPnP mappings use the correct ports.
    void setPorts(quint16 httpPort, quint16 httpsPort);

    /// Callback used to move the HTTPS listener to a new port when port parity
    /// requires it (external router port must equal the local HTTPS port).
    /// Returns true when the rebind succeeded. Must be set before start().
    void setHttpsRebindCallback(std::function<bool(quint16)> cb)
    {
        m_HttpsRebindCallback = std::move(cb);
    }

signals:
    /// Emitted when Internet Access becomes fully operational.
    void ready(const QString& domain, const QString& publicIp);

    /// Emitted when a non-fatal error occurs (status update for UI).
    void statusChanged(const QJsonObject& status);

    /// Emitted when a fatal error prevents Internet Access from working.
    void error(const QString& message);

    /// Emitted when TLS certificate is renewed or changed.
    void certificateChanged();

    /// Emitted after the HTTPS listener was successfully moved to a new port
    /// (port parity rebind). Entry points depending on the port (Desktop
    /// shortcut, tray tooltip) must refresh.
    void httpsPortChanged(quint16 port);

private slots:
    /// Called every 5 minutes for periodic checks.
    void onPeriodicCheck();

    /// Called when pending registration is active. Retries with fixed delay (3s, 3s, 3s), max 3
    /// attempts.
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

    /// Release the previously registered subdomain when unique_id changed, so an
    /// owner never holds more than one live subdomain. Deletes the old A record
    /// and its _owner TXT, but only after verifying we own it (TXT == ownerToken).
    void releaseOldSubdomain();

    /// Detect public IP via STUN (with fallback chain).
    bool detectPublicIp();

    /// Fallback: detect public IP via HTTP (ipify.org / icanhazip.com).
    /// Used when STUN servers all fail to respond.
    QString detectPublicIpViaHttp();

    /// Update the A record on PowerDNS with the current public IP.
    bool updateARecord();

    /// Append a traceability entry to the dedicated audit log every time an
    /// A-record registration request is sent to the PowerDNS API. Records the
    /// timestamp, unique ID, domain, public IP and the user's consent (exact
    /// agreement text, when and where it was accepted).
    void logDnsRegistrationAudit(const QString& action);

    /// Issue a TLS certificate via the native ACME client.
    bool issueCertificate();

    /// Read certificate expiry date directly from the PEM file.
    /// Returns ISO 8601 string, or empty string if file missing/invalid.
    static QString readCertExpiry(const QString& certPath);

    /// Check certificate expiry and renew if < 30 days remaining.
    bool checkCertificate();

    /// Build the domain name from unique ID.
    QString buildDomain() const;

    /// Deterministic fallback external port derived from unique_id, so two
    /// instances behind the same NAT never converge on the same port (the router
    /// forwards each external port to a single host).
    quint16 fallbackExternalPort(quint16 internalPort) const;

    /// Map an external port to the given internal port, preferring the internal
    /// port itself (clean URL) and falling back to a deterministic per-instance
    /// port when another device already owns it. Never evicts another device's
    /// mapping. Returns the external port actually mapped, or 0 on failure.
    quint16 mapPortWithFallback(quint16 internalPort, const char* protocol, const char* desc);

    /// Map the HTTPS port with strict external==internal parity: the router-side
    /// port always equals the local listener port (443→443, 48123→48123, ...).
    /// When the preferred port is owned by another device on the router, a
    /// deterministic fallback port is chosen and the HTTPS listener is moved to
    /// it via m_HttpsRebindCallback (deferred, out of the current call stack).
    /// Returns the external (== eventual internal) port, or 0 on failure.
    quint16 mapHttpsPortParity();

    /// True when the given TCP port can be bound locally (quick listen test).
    static bool isLocalPortBindable(quint16 port);

    /// Test whether the host can reach its own public endpoint (domain +
    /// external HTTPS port) — i.e. whether the router supports NAT hairpin /
    /// loopback. A short TCP connect from this machine to its public address is
    /// exactly what a browser on the same host would attempt. Blocks up to a few
    /// seconds; call only from timers, never from the HTTP request path.
    bool testHairpinReachable();

    /// Re-run the hairpin test, store the result, and emit statusChanged when it
    /// changed so live admin pages pick it up.
    void updateHairpinStatus();

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
    QString
        m_LocalIp; ///< LAN IP address of this host (discovered via UPnP or GetAdaptersAddresses)
    QString m_UniqueId;
    QString m_LastError;
    QString m_Phase; ///< Current activation step (drives the UI loader). See statusJson "phase".
    bool m_CertIssuing = false;      ///< True while ACME issuance is in progress
    bool m_HairpinReachable = false; ///< True when the host can reach its own public
                                     ///< endpoint (router supports NAT hairpin). Drives
                                     ///< the host-machine redirect to the public domain.
    quint16 m_HttpPort = 0;          ///< Actual HTTP server port
    quint16 m_HttpsPort = 0;         ///< Actual HTTPS server port
    quint16 m_ExternalHttpsPort = 0; ///< Router-side external HTTPS port (443 or fallback)
    quint16 m_ExternalHttpPort = 0;  ///< Router-side external HTTP port (80 or fallback)
    bool m_ServiceManaged = false; ///< True when launched by a service supervisor (MW_SERVICE set);
                                   ///< such an instance never steals a port mapping owned by
                                   ///< another device — only a manual launch takes over.

    // HTTPS listener rebind hook (port parity), set from main.cpp.
    std::function<bool(quint16)> m_HttpsRebindCallback;

    // Retry state
    int m_PendingRetryCount = 0; ///< Current retry attempt (0..3) for pending registration

    // Last DNS check timestamp (spaced to 24h)
    QDateTime m_LastDnsCheck;

    // Timers
    QTimer* m_PeriodicCheckTimer = nullptr;
    QTimer* m_PendingRegistrationTimer = nullptr;
};
