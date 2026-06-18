#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QCryptographicHash>
#include <functional>

/**
 * @brief Native ACMEv2 client (RFC 8555) with DNS-01 via PowerDNS API.
 *
 * Replaces the acme.sh shell-script dependency with a pure C++/Qt
 * implementation. Uses QNetworkAccessManager for async HTTP and
 * native OpenSSL C API for RSA operations (key gen, signing, CSR).
 *
 * Flow (all async — start() returns immediately):
 *   1. Ensure RSA keys exist (account + domain)
 *   2. GET ACME directory
 *   3. Create/retrieve ACME account
 *   4. Create certificate order
 *   5. Get DNS-01 authorization challenge
 *   6. Create _acme-challenge TXT record via PowerDNS API
 *   7. Respond to challenge (POST to challenge URL)
 *   8. Poll authorization until valid
 *   9. Clean up TXT record
 *  10. Generate CSR and finalize order
 *  11. Download certificate chain
 *
 * Output: {certOutputDir}/{cert.pem, key.pem, fullchain.pem}
 */
class AcmeClient : public QObject
{
    Q_OBJECT

public:
    explicit AcmeClient(QObject* parent = nullptr);
    ~AcmeClient() override;

    /// Path to the ACME account RSA-2048 private key. Generated if missing.
    void setAccountKeyPath(const QString& path);
    QString accountKeyPath() const { return m_AccountKeyPath; }

    /// Path to the domain RSA-2048 private key (certificate key). Generated if missing.
    void setDomainKeyPath(const QString& path);

    /// Output directory for cert.pem, key.pem, fullchain.pem.
    void setCertOutputDir(const QString& dir);

    /// FQDN for the certificate, e.g. "92b8d127.moonlightweb.top".
    void setHost(const QString& host);

    /// Base domain for PowerDNS DNS-01 zone, e.g. "moonlightweb.top".
    void setBaseDomain(const QString& domain);

    /// PowerDNS API key for DNS-01 TXT record creation/deletion.
    void setPdnsToken(const QString& token);

    /// ACME directory URL (default: Let's Encrypt production).
    void setDirectoryUrl(const QString& url) { m_DirectoryUrl = url; }

    /// External Account Binding credentials (RFC 8555 §7.3.4), required by CAs
    /// such as ZeroSSL / Google Trust Services. @p hmacKeyB64Url is the EAB HMAC
    /// key as provided by the CA (base64url, no padding). When both are set, the
    /// newAccount request carries an "externalAccountBinding" field.
    void setExternalAccountBinding(const QString& kid, const QString& hmacKeyB64Url) {
        m_EabKid = kid;
        m_EabHmacKey = hmacKeyB64Url;
    }

    /// Start the certificate issuance. Async — returns immediately.
    void start();

    /// Cancel any in-progress operation. No further signals will fire.
    void cancel();

signals:
    /// Successful issuance: fullchain path and private key path.
    void certificateReady(const QString& fullchainPath, const QString& keyPath);

    /// Progress message for logging or UI display.
    void progress(const QString& message);

    /// Fatal error during issuance.
    void errorOccurred(const QString& message);

    /// Emitted at the end of every issuance attempt (success or failure).
    void finished(bool success);

private:
    // ── Step chain (async, each calls the next via queued invocation) ─────────

    void stepCheckKeys();
    void stepGetDirectory();
    void stepCreateAccount();
    void stepCreateOrder();
    void stepGetAuthorization();
    void stepRespondChallenge();
    void stepPollAuthorization();
    void stepFinalize();
    void stepDownloadCert();

    // ── ACME HTTP helpers ─────────────────────────────────────────────────────

    /// ACME response data passed to callbacks.
    struct AcmeResponse {
        int statusCode = 0;
        QByteArray body;
        QString location;
    };

    /// Build a JWS JSON body: { "protected": b64, "payload": b64, "signature": b64 }.
    /// If useKid is false, embeds the full JWK instead of the kid.
    QByteArray buildJws(const QByteArray& payload, const QString& url, bool useKid);

    /// Build the External Account Binding inner JWS (HS256 over the account JWK).
    /// Returns an empty object when no EAB credentials are configured.
    QJsonObject buildEabJws(const QString& newAccountUrl);

    /// POST a JWS to an ACME endpoint with Replay-Nonce handling.
    /// Automatically retries on badNonce (up to 3 times).
    /// @param callback receives (int statusCode, QByteArray body, QString location)
    void acmePost(const QString& url, const QByteArray& payload, bool useKid,
                  std::function<void(int, const QByteArray&, const QString&)> callback,
                  int retriesLeft = 3);

    /// POST-as-GET equivalent: JWS with empty payload.
    void acmePostAsGet(const QString& url,
                       std::function<void(int, const QByteArray&)> callback);

    /// Get a fresh Replay-Nonce via HEAD to newNonce endpoint.
    void fetchNonce(std::function<void(bool)> callback);

    // ── RSA helpers (native OpenSSL C API) ────────────────────────────────────

    /// Sign data with RSA-SHA256 using the account key (openssl CLI).
    QByteArray signRsaSha256(const QByteArray& data);

    /// Generate an RSA-2048 key and save to path.
    static bool generateRsaKey(const QString& path);

    // ── Account key → JWK helpers ─────────────────────────────────────────────

    /// Parse the RSA modulus (big-endian bytes) from openssl -text output.
    QByteArray parseRsaModulus();

    /// Parse the RSA public exponent (big-endian bytes) from openssl -text.
    QByteArray parseRsaExponent();

    /// Build JWK JSON: { "kty": "RSA", "n": b64url(mod), "e": b64url(exp) }.
    QJsonObject accountKeyJwk();

    /// Compute JWK SHA-256 thumbprint (RFC 7638) for DNS-01 key authorization.
    QByteArray accountKeyThumbprint();

    // ── DNS challenge helpers (PowerDNS API) ──────────────────────────────────

    /// Create _acme-challenge.{subdomain} TXT record with DNS-01 value.
    bool createChallengeTxtRecord(const QString& dnsValue);

    /// Delete the TXT record after challenge validation.
    bool deleteChallengeTxtRecord();

    // ── CSR helpers ───────────────────────────────────────────────────────────

    /// Generate a PKCS#10 CSR in DER format for m_Host.
    QByteArray generateCsr();

    // ── Certificate helpers ───────────────────────────────────────────────────

    /// Save fullchain.pem, cert.pem, and copy key.pem to output directory.
    bool saveCertificate(const QByteArray& pemChain);

    // ── Utility ───────────────────────────────────────────────────────────────

    static QByteArray b64urlEncode(const QByteArray& data);
    static QByteArray b64urlDecode(const QByteArray& data);

    // ── Members ───────────────────────────────────────────────────────────────

    QNetworkAccessManager* m_Nam = nullptr;

    // Configuration
    QString m_AccountKeyPath;
    QString m_DomainKeyPath;
    QString m_CertOutputDir;
    QString m_Host;        // FQDN: "92b8d127.moonlightweb.top"
    QString m_BaseDomain;  // PowerDNS zone (e.g. "moonlightweb.top")
    QString m_PdnsToken;

    QString m_DirectoryUrl;
    QString m_EabKid;      // External Account Binding key identifier
    QString m_EabHmacKey;  // EAB HMAC key (base64url, as given by the CA)

    // ACME protocol state
    QJsonObject m_Directory;
    QJsonObject m_Order;
    QByteArray m_Nonce;
    QString m_AccountUrl;
    QString m_FinalizeUrl;
    QString m_OrderUrl;       // from newOrder Location header (for polling)
    QString m_CertUrl;
    QString m_ChallengeToken;
    QString m_ChallengeUrl;
    QString m_AuthorizationUrl;
    QString m_TxtSubname;     // "_acme-challenge.92b8d127"
    int m_PollRetries = 0;
    bool m_Cancelled = false;
};
