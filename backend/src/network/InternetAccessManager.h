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

#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <QStringList>

#include "StunClient.h"
#include "UPNPClient.h"

class AppSettings;

/**
 * @brief Orchestrates the full Internet Access feature.
 *
 * Responsibilities:
 *   1. Public IP detection via STUN (with fallback chain)
 *   2. UPnP port mapping delegation
 *   3. NAT-hairpin verdict for the host machine's own entry points
 *   4. Periodic checks every 5 minutes (IP change, mappings, hairpin)
 *
 * This manager publishes nothing. It registers no name, writes no DNS record
 * and issues no certificate: a machine is reached from the internet through
 * the rendezvous server, which needs none of that. What is left here is the
 * router half — knowing this host's public address, opening the ports a
 * session needs, and telling the host's own shortcuts whether the router
 * reflects its public address back to the LAN.
 *
 * Bring-your-own-domain: when settings.json's `domain` holds a valid FQDN, it
 * is served as-is and the certificate for it is the user's to provide. Nothing
 * changes here — this manager never owned a zone in the first place.
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

    /// Enable Internet Access: detect the public IP, map the router ports.
    void start();

    /// Disable Internet Access: stop timers, clean up.
    void stop();

    /// Force re-check public IP immediately.
    void forceRefresh();

    /// Get current status as JSON object (for API responses).
    QJsonObject statusJson() const;

    /// Whether the manager is currently active (Internet Access enabled).
    bool isActive() const { return m_Active; }

    /// True when start() stopped at the consent gate: a stored consent that
    /// predates the current mechanism, so nothing may be opened until the user
    /// has been asked again with today's wording.
    ///
    /// Callers that open anything of their own — the rendezvous line is one —
    /// must honour this too. The switch being "on" is not sufficient: it can be
    /// on because of a consent given for a mechanism that no longer runs.
    bool consentRequired() const { return m_Phase == QLatin1String("consent_required"); }

    /// The user-owned FQDN from settings.json, or empty — which is the normal
    /// case. This instance registers no name of its own: it is reached through
    /// the rendezvous server.
    QString domain() const { return m_Domain; }

    /// True when settings.json carries a valid FQDN the user owns. It is served
    /// as-is; the certificate for it is theirs to drop in the cert directory.
    /// This manager still detects the public IP and tests NAT hairpin, so the
    /// same one-click toggle configures the router. The 443 forward is the
    /// user's to set up.
    bool customDomain() const { return m_CustomDomain; }

    /// Current public IP.
    QString publicIp() const { return m_PublicIp; }

    /// Every IPv4 another machine on the LAN can reach this host on, best first
    /// (default route, then gateway-carrying adapters). Filled in the
    /// constructor, so it is usable before — and without — Internet Access.
    QStringList localIps() const { return m_LocalIps; }

    /// Router-side HTTPS port a user-owned domain is forwarded to. This manager
    /// maps no web port of its own, so it is simply the local listener port —
    /// the forward is the user's own router configuration.
    quint16 externalHttpsPort() const { return m_HttpsPort; }

    /// True when this host can reach its own public endpoint, i.e. the router
    /// reflects it back to the LAN (NAT hairpin). False until the first test has
    /// run, which is the safe default: an entry point on the host machine then
    /// stays on loopback instead of handing a browser an address that times out.
    bool hairpinReachable() const { return m_HairpinReachable; }

    /// UPnP client (exposed for integration with existing session code).
    UPNPClient* upnpClient() { return &m_Upnp; }

    /// Set the actual HTTP and HTTPS ports the server is listening on.
    /// Must be called before start() so the hairpin test probes the right port.
    void setPorts(quint16 httpPort, quint16 httpsPort);

signals:
    /// Emitted when Internet Access becomes fully operational.
    void ready(const QString& domain, const QString& publicIp);

    /// Emitted when a non-fatal error occurs (status update for UI).
    void statusChanged(const QJsonObject& status);

    /// Emitted when a fatal error prevents Internet Access from working.
    void error(const QString& message);

    /// Emitted when the NAT-hairpin verdict flips. Host-side entry points (tray,
    /// Desktop shortcut) pick the public domain or loopback based on it, so they
    /// must be rebuilt when it changes.
    void hairpinChanged(bool reachable);

private slots:
    /// Called every 5 minutes for periodic checks.
    void onPeriodicCheck();

private:
    /// Generate a unique 8-char hex ID.
    QString generateUniqueId();

    /// Eager init: ensure unique_id exists and settle whether a user-owned
    /// domain is configured. Touches nothing on the network.
    void ensureIdentifiers();

    /// Re-read this host's IPv4 addresses into m_LocalIps / m_LocalIp (best first).
    void refreshLocalAddresses();

    /// Detect public IP via STUN (with fallback chain).
    bool detectPublicIp();

    /// Fallback: detect public IP via HTTP (ipify.org / icanhazip.com).
    /// Used when STUN servers all fail to respond.
    QString detectPublicIpViaHttp();

    /// The name this instance would have carried under the retired DNS
    /// mechanism. Never served: it exists only so a `domain` left in
    /// settings.json by such an install is recognised as that name rather than
    /// mistaken for a domain the user owns.
    QString buildDomain() const;

    /// Test whether the host can reach its own public endpoint (domain +
    /// HTTPS port) — i.e. whether the router supports NAT hairpin /
    /// loopback. A short TCP connect from this machine to its public address is
    /// exactly what a browser on the same host would attempt. Blocks up to a few
    /// seconds; call only from timers, never from the HTTP request path.
    bool testHairpinReachable();

    /// Re-run the hairpin test, store the result, and emit statusChanged when it
    /// changed so live admin pages pick it up.
    void updateHairpinStatus();

    // Owned sub-clients
    StunClient m_Stun;
    UPNPClient m_Upnp;

    // Settings reference (not owned)
    AppSettings* m_Settings = nullptr;

    // State
    bool m_Active = false;
    QString m_Domain;
    bool m_CustomDomain = false; ///< True when settings.json carries a user-owned FQDN. It is
                                 ///< served as-is; we own neither the zone nor the certificate.
    QString m_PublicIp;
    QString m_LocalIp;      ///< Best LAN IP of this host: the default-route address (= first
                            ///< entry of m_LocalIps). What the shared access URL points at.
    QStringList m_LocalIps; ///< Every IPv4 another machine can reach us on, best first. Includes
                            ///< host-only virtual switches, only reachable from their own VMs.
    QString m_UniqueId;
    QString m_LastError;
    QString m_Phase; ///< Current activation step (drives the UI loader). See statusJson "phase".
    bool m_HairpinReachable = false; ///< True when the host can reach its own public
                                     ///< endpoint (router supports NAT hairpin). Drives
                                     ///< the host-machine redirect to the public domain.
    quint16 m_HttpPort = 0;          ///< Actual HTTP server port
    quint16 m_HttpsPort = 0;         ///< Actual HTTPS server port

    // Timers
    QTimer* m_PeriodicCheckTimer = nullptr;
};
