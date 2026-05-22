#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>

/**
 * @brief Client for the deSEC.io Dynamic DNS API.
 *
 * Works with an already-registered parent domain (e.g. "moonlightweb.dedyn.io").
 * Creates and manages A records for subdomains like "92b8d127.moonlightweb.dedyn.io"
 * by adding RRsets with the subname ("92b8d127") under the parent domain.
 *
 * All API calls are synchronous (blocking via QEventLoop).
 * Thread safety: call from the Qt main thread only.
 *
 * The deSEC API token is NEVER logged.
 */
class DeSecClient : public QObject
{
    Q_OBJECT

public:
    explicit DeSecClient(QObject* parent = nullptr);
    ~DeSecClient() override = default;

    /// Set the deSEC API token (bearer token for Authorization header).
    void setToken(const QString& token) { m_Token = token; }
    QString token() const { return m_Token; }

    /// Base URL for all deSEC API calls.
    static QString apiBaseUrl() { return QStringLiteral("https://desec.io/api/v1"); }

    // ── Subdomain (A record) management ──────────────────────────────────────

    /**
     * @brief Check whether a subdomain A record already exists.
     *
     * GET /api/v1/domains/{domain}/rrsets/{subname}/A/
     *
     * @param domain  Parent domain (e.g. "moonlightweb.dedyn.io").
     * @param subname Subdomain label (e.g. "92b8d127").
     * @param[out] errorMsg Set on failure.
     * @return true  if the A record does NOT exist yet (HTTP 404 — available).
     *         false if it exists (HTTP 200) or on error.
     */
    bool checkSubdomainAvailable(const QString& domain, const QString& subname,
                                 QString& errorMsg);

    /**
     * @brief Create a new A record (subdomain) under the parent domain.
     *
     * POST /api/v1/domains/{domain}/rrsets/
     * Body: {"subname": "<subname>", "type": "A", "ttl": <ttl>, "records": ["<ip>"]}
     *
     * @return true on HTTP 201, false otherwise.
     */
    bool createSubdomain(const QString& domain, const QString& subname,
                         const QString& ip, int ttl, QString& errorMsg);

    /**
     * @brief Update the IP of an existing A record.
     *
     * PUT /api/v1/domains/{domain}/rrsets/{subname}/A/
     * Body: {"ttl": <ttl>, "records": ["<ip>"]}
     *
     * @return true on HTTP 200, false otherwise.
     */
    bool updateSubdomain(const QString& domain, const QString& subname,
                         const QString& ip, int ttl, QString& errorMsg);

    // ── TXT record management (ACME DNS-01 challenge) ─────────────────────────

    /**
     * @brief Create a TXT record for ACME DNS-01 challenge.
     *
     * POST /api/v1/domains/{domain}/rrsets/
     * Body: {"subname": "<subname>", "type": "TXT", "ttl": 60, "records": ["<value>"]}
     *
     * Typically subname is "_acme-challenge.{subdomain}" (e.g. "_acme-challenge.92b8d127").
     * The parent domain is the base domain (e.g. "moonlightweb.dedyn.io").
     *
     * @return true on HTTP 201, false otherwise.
     */
    bool createTxtRecord(const QString& domain, const QString& subname,
                         const QString& value, int ttl, QString& errorMsg);

    /**
     * @brief Delete a TXT record (cleanup after ACME challenge).
     *
     * DELETE /api/v1/domains/{domain}/rrsets/{subname}/TXT/
     *
     * @return true on HTTP 200/204, false otherwise.
     */
    bool deleteTxtRecord(const QString& domain, const QString& subname,
                         QString& errorMsg);

signals:
    void error(const QString& message);

private:
    QNetworkReply* sendRequest(const QString& method, const QString& path,
                               const QByteArray& body = QByteArray(),
                               int timeoutMs = 10000);
    /// Log a warning if the reply indicates HTTP 429 (Rate Limited) using headers only.
    void logRateLimit(QNetworkReply* reply);
    QString authHeader() const { return QStringLiteral("Token ") + m_Token; }

    QNetworkAccessManager* m_Nam = nullptr;
    QString m_Token;
};
