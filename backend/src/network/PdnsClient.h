/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>

/**
 * @brief Client for the PowerDNS API (self-hosted).
 *
 * Manages A and TXT records for the zone "moonlightweb.top." through
 * the PowerDNS API at api.moonlightweb.top. The zone must already exist
 * in the PowerDNS server.
 *
 * All API calls are synchronous (blocking via QEventLoop).
 * Thread safety: call from the Qt main thread only.
 *
 * The PowerDNS API token is NEVER logged.
 */
class PdnsClient : public QObject
{
    Q_OBJECT

public:
    explicit PdnsClient(QObject* parent = nullptr);
    ~PdnsClient() override = default;

    /// Set the PowerDNS API key (X-API-Key header).
    void setToken(const QString& token) { m_Token = token; }
    QString token() const { return m_Token; }

    /// Base URL for the PowerDNS API.
    static QString apiBaseUrl() { return QStringLiteral("https://api.moonlightweb.top/api/v1/servers/localhost"); }

    /// Zone name with trailing dot (e.g. "moonlightweb.top.").
    static QString zoneName() { return QStringLiteral("moonlightweb.top."); }

    // ── Subdomain (A record) management ──────────────────────────────────────

    /**
     * @brief Check whether a subdomain A record already exists.
     *
     * GET /api/v1/servers/localhost/zones/{zone}?rrset_name={subname}.{zone}&rrset_type=A
     *
     * Parses the response JSON; if no matching A record is found, the
     * subdomain is considered available.
     *
     * @param subname  Subdomain label (e.g. "92b8d127").
     * @param[out] errorMsg Set on failure.
     * @return true  if no A record exists (available).
     *         false if a matching A record exists or on error.
     */
    bool checkSubdomainAvailable(const QString& subname,
                                 QString& errorMsg);

    /**
     * @brief Create or update an A record (unified via PATCH REPLACE).
     *
     * PATCH /api/v1/servers/localhost/zones/{zone}
     * Body: {"rrsets": [{"name": "{subname}.{zone}", "type": "A",
     *        "ttl": <ttl>, "changetype": "REPLACE",
     *        "records": [{"content": "<ip>", "disabled": false}]}]}
     *
     * PowerDNS PATCH is an upsert — works for both create and update.
     *
     * @return true on HTTP 204, false otherwise.
     */
    bool createOrUpdateSubdomain(const QString& subname,
                                  const QString& ip, int ttl,
                                  QString& errorMsg);

    // ── TXT record management (ACME DNS-01 challenge) ─────────────────────────

    /**
     * @brief Create/replace a TXT record via PATCH REPLACE.
     *
     * PATCH /api/v1/servers/localhost/zones/{zone}
     * Body: {"rrsets": [{"name": "{subname}.{zone}", "type": "TXT",
     *        "ttl": <ttl>, "changetype": "REPLACE",
     *        "records": [{"content": "<value>", "disabled": false}]}]}
     *
     * @return true on HTTP 204, false otherwise.
     */
    bool createTxtRecord(const QString& fqdnSubname, const QString& value,
                         int ttl, QString& errorMsg);

    /**
     * @brief Delete a TXT record via PATCH DELETE.
     *
     * PATCH /api/v1/servers/localhost/zones/{zone}
     * Body: {"rrsets": [{"name": "{subname}.{zone}", "type": "TXT",
     *        "changetype": "DELETE", "records": []}]}
     *
     * @return true on HTTP 204 (or 404 if already gone), false otherwise.
     */
    bool deleteTxtRecord(const QString& fqdnSubname, QString& errorMsg);

    /**
     * @brief Read a TXT record's value.
     *
     * GET .../zones/{zone}?rrset_name={fqdnSubname}&rrset_type=TXT
     *
     * @param[out] valueOut First TXT record content, unquoted; empty if absent.
     * @param[out] errorMsg Set only on network/HTTP error.
     * @return true if the query succeeded (record may still be absent),
     *         false on network/HTTP error.
     */
    bool getTxtRecord(const QString& fqdnSubname, QString& valueOut,
                      QString& errorMsg);

signals:
    void error(const QString& message);

private:
    /// Internal: synchronous PATCH with JSON body.
    QNetworkReply* sendPatch(const QString& url, const QByteArray& body,
                             int timeoutMs = 15000);

    /// Internal: synchronous GET.
    QNetworkReply* sendGet(const QString& url, int timeoutMs = 15000);

    /// Build the FQDN for a subname: "{subname}.moonlightweb.top."
    static QString fqdn(const QString& subname);

    QNetworkAccessManager* m_Nam = nullptr;
    QString m_Token;
};
