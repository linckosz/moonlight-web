#include "AcmeClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const QString kDefaultDirectoryUrl =
    QStringLiteral("https://acme-v02.api.letsencrypt.org/directory");

static constexpr int kHttpTimeoutMs  = 30000;
static constexpr int kPollIntervalMs = 5000;
static constexpr int kMaxPollRetries = 36;     // 36 x 5s = 3 min
static constexpr int kChallengeTtl   = 60;     // TXT record TTL
static constexpr int kOpensslTimeout = 15000;  // 15s for openssl CLI

static const QString kDeSecBase =
    QStringLiteral("https://desec.io/api/v1");

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

AcmeClient::AcmeClient(QObject* parent)
    : QObject(parent)
    , m_DirectoryUrl(kDefaultDirectoryUrl)
{
    m_Nam = new QNetworkAccessManager(this);
}

AcmeClient::~AcmeClient()
{
    cancel();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void AcmeClient::setAccountKeyPath(const QString& path)  { m_AccountKeyPath = path; }
void AcmeClient::setDomainKeyPath(const QString& path)   { m_DomainKeyPath = path; }
void AcmeClient::setCertOutputDir(const QString& dir)    { m_CertOutputDir = dir; }
void AcmeClient::setHost(const QString& host)            { m_Host = host; }
void AcmeClient::setBaseDomain(const QString& domain)    { m_BaseDomain = domain; }
void AcmeClient::setDesecToken(const QString& token)     { m_DesecToken = token; }

void AcmeClient::start()
{
    m_Cancelled = false;
    m_Nonce.clear();
    m_AccountUrl.clear();
    m_FinalizeUrl.clear();
    m_CertUrl.clear();
    m_ChallengeToken.clear();
    m_ChallengeUrl.clear();
    m_AuthorizationUrl.clear();
    m_TxtSubname.clear();
    m_PollRetries = 0;
    m_Directory = QJsonObject();
    m_Order = QJsonObject();

    emit progress(QStringLiteral("Starting ACME certificate issuance for ") + m_Host);
    QTimer::singleShot(0, this, &AcmeClient::stepCheckKeys);
}

void AcmeClient::cancel()
{
    m_Cancelled = true;
}

// ---------------------------------------------------------------------------
// Base64url (RFC 4648 §5, no padding)
// ---------------------------------------------------------------------------

QByteArray AcmeClient::b64urlEncode(const QByteArray& data)
{
    return data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray AcmeClient::b64urlDecode(const QByteArray& data)
{
    return QByteArray::fromBase64(data, QByteArray::Base64UrlEncoding);
}

// ---------------------------------------------------------------------------
// RSA key generation
// ---------------------------------------------------------------------------

bool AcmeClient::generateRsaKey(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QProcess p;
    p.start(QStringLiteral("openssl"),
            { QStringLiteral("genrsa"), QStringLiteral("2048") });
    if (!p.waitForStarted(5000)) {
        qWarning() << "[AcmeClient] Cannot start openssl for keygen";
        return false;
    }
    if (!p.waitForFinished(kOpensslTimeout)) {
        p.kill(); p.waitForFinished(5000);
        qWarning() << "[AcmeClient] openssl genrsa timed out";
        return false;
    }
    if (p.exitCode() != 0) {
        qWarning() << "[AcmeClient] openssl genrsa failed:" << p.readAll();
        return false;
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "[AcmeClient] Cannot write key:" << path;
        return false;
    }
    f.write(p.readAllStandardOutput());
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
// RSA helpers — parse modulus / exponent from "openssl rsa -pubout -text"
// ---------------------------------------------------------------------------

QByteArray AcmeClient::parseRsaModulus()
{
    QProcess p;
    p.start(QStringLiteral("openssl"),
            { QStringLiteral("rsa"), QStringLiteral("-in"), m_AccountKeyPath,
              QStringLiteral("-pubout"), QStringLiteral("-text"),
              QStringLiteral("-noout") });
    if (!p.waitForStarted(5000) || !p.waitForFinished(10000) || p.exitCode() != 0)
        return {};

    QString out = QString::fromUtf8(p.readAllStandardOutput());
    int idx = out.indexOf(QStringLiteral("Modulus:"));
    if (idx < 0) return {};
    QString hex = out.mid(idx + 8);
    int eIdx = hex.indexOf(QStringLiteral("Exponent:"));
    if (eIdx >= 0) hex = hex.left(eIdx);

    hex.remove(QLatin1Char(':'));
    hex.remove(QLatin1Char(' '));
    hex.remove(QLatin1Char('\t'));
    hex.remove(QLatin1Char('\n'));
    hex.remove(QLatin1Char('\r'));

    return QByteArray::fromHex(hex.toUtf8());
}

QByteArray AcmeClient::parseRsaExponent()
{
    QProcess p;
    p.start(QStringLiteral("openssl"),
            { QStringLiteral("rsa"), QStringLiteral("-in"), m_AccountKeyPath,
              QStringLiteral("-pubout"), QStringLiteral("-text"),
              QStringLiteral("-noout") });
    if (!p.waitForStarted(5000) || !p.waitForFinished(10000) || p.exitCode() != 0)
        return {};

    QString out = QString::fromUtf8(p.readAllStandardOutput());
    int idx = out.indexOf(QStringLiteral("Exponent:"));
    if (idx < 0) return {};

    // Format: "Exponent: 65537 (0x10001)"
    QString expStr = out.mid(idx + 9).trimmed();
    int sp = expStr.indexOf(QLatin1Char(' '));
    if (sp > 0) expStr = expStr.left(sp);

    bool ok = false;
    quint64 val = expStr.toULongLong(&ok);
    if (!ok) return {};

    QByteArray result;
    while (val > 0) {
        result.prepend(static_cast<char>(val & 0xFF));
        val >>= 8;
    }
    return result;
}

// ---------------------------------------------------------------------------
// JWK helpers
// ---------------------------------------------------------------------------

QJsonObject AcmeClient::accountKeyJwk()
{
    QJsonObject jwk;
    jwk[QStringLiteral("kty")] = QStringLiteral("RSA");
    jwk[QStringLiteral("n")]   = QString::fromUtf8(b64urlEncode(parseRsaModulus()));
    jwk[QStringLiteral("e")]   = QString::fromUtf8(b64urlEncode(parseRsaExponent()));
    return jwk;
}

QByteArray AcmeClient::accountKeyThumbprint()
{
    QJsonObject jwk = accountKeyJwk();
    // Canonical JSON: keys sorted alphabetically → e, kty, n
    QJsonObject c;
    c[QStringLiteral("e")]   = jwk.value(QStringLiteral("e"));
    c[QStringLiteral("kty")] = jwk.value(QStringLiteral("kty"));
    c[QStringLiteral("n")]   = jwk.value(QStringLiteral("n"));

    QByteArray json = QJsonDocument(c).toJson(QJsonDocument::Compact);
    return b64urlEncode(QCryptographicHash::hash(json, QCryptographicHash::Sha256));
}

// ---------------------------------------------------------------------------
// JWS signing (RSA-SHA256 via openssl CLI)
// ---------------------------------------------------------------------------

QByteArray AcmeClient::signRsaSha256(const QByteArray& data)
{
    QProcess p;
    p.start(QStringLiteral("openssl"),
            { QStringLiteral("dgst"), QStringLiteral("-sha256"),
              QStringLiteral("-sign"), m_AccountKeyPath });
    if (!p.waitForStarted(5000)) return {};
    p.write(data);
    p.closeWriteChannel();
    if (!p.waitForFinished(10000)) return {};
    if (p.exitCode() != 0) return {};
    return p.readAllStandardOutput();
}

QByteArray AcmeClient::buildJws(const QByteArray& payload, const QString& url, bool useKid)
{
    QJsonObject header;
    header[QStringLiteral("alg")]   = QStringLiteral("RS256");
    header[QStringLiteral("nonce")] = QString::fromUtf8(m_Nonce);
    header[QStringLiteral("url")]   = url;

    if (useKid && !m_AccountUrl.isEmpty()) {
        header[QStringLiteral("kid")] = m_AccountUrl;
    } else {
        header[QStringLiteral("jwk")] = accountKeyJwk();
    }

    QByteArray protectedB64 = b64urlEncode(
        QJsonDocument(header).toJson(QJsonDocument::Compact));
    QByteArray payloadB64  = b64urlEncode(payload);
    QByteArray signingInput = protectedB64 + '.' + payloadB64;

    QByteArray sig   = signRsaSha256(signingInput);
    QByteArray sigB64 = b64urlEncode(sig);

    QJsonObject jws;
    jws[QStringLiteral("protected")] = QString::fromUtf8(protectedB64);
    jws[QStringLiteral("payload")]   = QString::fromUtf8(payloadB64);
    jws[QStringLiteral("signature")] = QString::fromUtf8(sigB64);

    return QJsonDocument(jws).toJson(QJsonDocument::Compact);
}

// ---------------------------------------------------------------------------
// Fetch a fresh Replay-Nonce
// ---------------------------------------------------------------------------

void AcmeClient::fetchNonce(std::function<void(bool)> callback)
{
    QString url = m_Directory.value(QStringLiteral("newNonce")).toString();
    if (url.isEmpty()) {
        qWarning() << "[AcmeClient] No newNonce in ACME directory";
        if (callback) callback(false);
        return;
    }

    QNetworkRequest req{QUrl(url)};
    QNetworkReply* reply = m_Nam->head(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        if (m_Cancelled) { if (callback) callback(false); return; }

        QByteArray nonce = reply->rawHeader("Replay-Nonce");
        if (!nonce.isEmpty()) {
            m_Nonce = nonce;
            if (callback) callback(true);
        } else {
            qWarning() << "[AcmeClient] No Replay-Nonce in HEAD response";
            if (callback) callback(false);
        }
    });
}

// ---------------------------------------------------------------------------
// ACME POST (async, with badNonce retry)
// ---------------------------------------------------------------------------

void AcmeClient::acmePost(const QString& url, const QByteArray& payload, bool useKid,
                          std::function<void(int, const QByteArray&, const QString&)> callback,
                          int retriesLeft)
{
    if (m_Cancelled) return;

    // Ensure we have a nonce; fetch one if needed
    if (m_Nonce.isEmpty()) {
        fetchNonce([this, url, payload, useKid, callback, retriesLeft](bool ok) {
            if (!ok) {
                emit errorOccurred(QStringLiteral("Failed to obtain ACME Replay-Nonce"));
                emit finished(false);
                return;
            }
            acmePost(url, payload, useKid, callback, retriesLeft);
        });
        return;
    }

    QByteArray jwsBody = buildJws(payload, url, useKid);

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/jose+json"));

    QNetworkReply* reply = m_Nam->post(req, jwsBody);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, payload, useKid, callback, retriesLeft]() {
        reply->deleteLater();
        if (m_Cancelled) return;

        int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray body = reply->readAll();
        QString location = reply->rawHeader("Location");
        QByteArray nonce = reply->rawHeader("Replay-Nonce");
        if (!nonce.isEmpty())
            m_Nonce = nonce;

        // Retry on badNonce
        if (code == 400 && retriesLeft > 0 && body.contains("badNonce")) {
            qInfo() << "[AcmeClient] badNonce, retries left:" << (retriesLeft - 1);
            m_Nonce.clear();
            acmePost(url, payload, useKid, callback, retriesLeft - 1);
            return;
        }

        callback(code, body, location);
    });
}

void AcmeClient::acmePostAsGet(const QString& url,
                               std::function<void(int, const QByteArray&)> callback)
{
    // POST-as-GET: empty payload per RFC 8555 §6.3
    acmePost(url, QByteArray(), !m_AccountUrl.isEmpty(),
             [callback](int code, const QByteArray& body, const QString& /*loc*/) {
        if (callback) callback(code, body);
    });
}

// ---------------------------------------------------------------------------
// CSR generation
// ---------------------------------------------------------------------------

QByteArray AcmeClient::generateCsr()
{
    QTemporaryFile cfg(QDir::tempPath() + QStringLiteral("/acme-csr-XXXXXX.cnf"));
    if (!cfg.open()) {
        qWarning() << "[AcmeClient] Cannot create temporary openssl config";
        return {};
    }

    QByteArray config =
        "[req]\n"
        "distinguished_name = req\n"
        "prompt = no\n"
        "req_extensions = v3_req\n"
        "\n"
        "[v3_req]\n"
        "keyUsage = keyEncipherment, digitalSignature\n"
        "extendedKeyUsage = serverAuth\n"
        "subjectAltName = DNS:" + m_Host.toUtf8() + "\n";

    cfg.write(config);
    cfg.close();

    QProcess p;
    p.start(QStringLiteral("openssl"), {
        QStringLiteral("req"), QStringLiteral("-new"),
        QStringLiteral("-key"),  m_DomainKeyPath,
        QStringLiteral("-subj"), QStringLiteral("/CN=") + m_Host,
        QStringLiteral("-config"), cfg.fileName(),
        QStringLiteral("-extensions"), QStringLiteral("v3_req"),
        QStringLiteral("-outform"), QStringLiteral("DER")
    });

    if (!p.waitForStarted(5000) || !p.waitForFinished(kOpensslTimeout)) {
        p.kill(); p.waitForFinished(5000);
        qWarning() << "[AcmeClient] CSR generation failed";
        return {};
    }
    if (p.exitCode() != 0) {
        qWarning() << "[AcmeClient] openssl req failed:" << p.readAll();
        return {};
    }

    return p.readAllStandardOutput();  // DER-encoded CSR
}

// ---------------------------------------------------------------------------
// DNS-01 challenge helpers (deSEC API, synchronous)
// ---------------------------------------------------------------------------

bool AcmeClient::createChallengeTxtRecord(const QString& dnsValue)
{
    // Extract the label from m_Host by removing the base domain suffix.
    // m_Host = "92b8d127.moonlightweb.dedyn.io"
    // m_BaseDomain = "moonlightweb.dedyn.io"
    // label = "92b8d127"
    // subname = "_acme-challenge.92b8d127"

    if (m_Host.isEmpty() || m_BaseDomain.isEmpty()) {
        qWarning() << "[AcmeClient] Cannot create TXT: missing host or base domain";
        return false;
    }

    QString label = m_Host;
    QString suffix = QStringLiteral(".") + m_BaseDomain;
    if (label.endsWith(suffix))
        label = label.left(label.length() - suffix.length());

    m_TxtSubname = QStringLiteral("_acme-challenge.") + label;

    qInfo() << "[AcmeClient] Creating TXT record"
            << m_TxtSubname << "." << m_BaseDomain;

    // Build deSEC API request
    QUrl apiUrl(kDeSecBase + QStringLiteral("/domains/") + m_BaseDomain
                + QStringLiteral("/rrsets/"));
    QNetworkRequest req{apiUrl};
    req.setRawHeader("Authorization", (QStringLiteral("Token ") + m_DesecToken).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));

    QJsonObject body;
    body[QStringLiteral("subname")] = m_TxtSubname;
    body[QStringLiteral("type")]    = QStringLiteral("TXT");
    body[QStringLiteral("ttl")]     = kChallengeTtl;
    QJsonArray records;
    records.append(dnsValue);
    body[QStringLiteral("records")] = records;

    QNetworkReply* reply = m_Nam->post(req,
        QJsonDocument(body).toJson(QJsonDocument::Compact));

    // Synchronous wait
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(15000);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort(); reply->deleteLater();
        qWarning() << "[AcmeClient] deSEC TXT creation timed out";
        return false;
    }
    timer.stop();

    int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // Log deSEC rate limiting
    if (code == 429) {
        QByteArray retryAfter = reply->rawHeader("Retry-After");
        qWarning() << "[AcmeClient] deSEC TXT creation HTTP 429 (Rate Limited) —"
                   << "Retry-After:" << QString::fromUtf8(retryAfter);
    }
    reply->deleteLater();

    if (code == 201 || code == 200) {
        qInfo() << "[AcmeClient] TXT record created (HTTP" << code << ")";
        return true;
    }
    qWarning() << "[AcmeClient] deSEC TXT creation failed (HTTP" << code << ")";
    return false;
}

bool AcmeClient::deleteChallengeTxtRecord()
{
    if (m_TxtSubname.isEmpty() || m_BaseDomain.isEmpty())
        return false;

    qInfo() << "[AcmeClient] Deleting TXT record"
            << m_TxtSubname << "." << m_BaseDomain;

    QUrl url(kDeSecBase + QStringLiteral("/domains/") + m_BaseDomain
             + QStringLiteral("/rrsets/") + m_TxtSubname + QStringLiteral("/TXT/"));
    QNetworkRequest req{url};
    req.setRawHeader("Authorization", (QStringLiteral("Token ") + m_DesecToken).toUtf8());

    QNetworkReply* reply = m_Nam->deleteResource(req);

    QEventLoop loop; QTimer timer;
    timer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(15000);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort(); reply->deleteLater();
        return false;
    }
    timer.stop();

    int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // Log deSEC rate limiting
    if (code == 429) {
        QByteArray retryAfter = reply->rawHeader("Retry-After");
        qWarning() << "[AcmeClient] deSEC TXT deletion HTTP 429 (Rate Limited) —"
                   << "Retry-After:" << QString::fromUtf8(retryAfter);
    }
    reply->deleteLater();

    if (code == 200 || code == 204 || code == 404) {
        qInfo() << "[AcmeClient] TXT record deleted (HTTP" << code << ")";
        return true;
    }
    qWarning() << "[AcmeClient] deSEC TXT deletion failed (HTTP" << code << ")";
    return false;
}

// ---------------------------------------------------------------------------
// Certificate file saving
// ---------------------------------------------------------------------------

bool AcmeClient::saveCertificate(const QByteArray& pemChain)
{
    QDir().mkpath(m_CertOutputDir);

    // fullchain.pem — the complete chain as returned by ACME
    QFile fc(m_CertOutputDir + QStringLiteral("/fullchain.pem"));
    if (!fc.open(QIODevice::WriteOnly)) {
        qWarning() << "[AcmeClient] Cannot write fullchain.pem";
        return false;
    }
    fc.write(pemChain);
    fc.close();

    // cert.pem — extract the leaf (first PEM block)
    QFile cl(m_CertOutputDir + QStringLiteral("/cert.pem"));
    if (!cl.open(QIODevice::WriteOnly)) {
        qWarning() << "[AcmeClient] Cannot write cert.pem";
        return false;
    }
    int s = pemChain.indexOf("-----BEGIN CERTIFICATE-----");
    if (s >= 0) {
        int e = pemChain.indexOf("-----END CERTIFICATE-----", s);
        if (e >= 0) {
            cl.write(pemChain.mid(s, e - s + 25));
            cl.write("\n");
        }
    }
    cl.close();

    // key.pem — copy the domain private key
    if (QFile::exists(m_DomainKeyPath)) {
        QString keyOut = m_CertOutputDir + QStringLiteral("/key.pem");
        QFile::remove(keyOut);
        if (!QFile::copy(m_DomainKeyPath, keyOut)) {
            qWarning() << "[AcmeClient] Failed to copy key.pem";
            return false;
        }
    }

    qInfo() << "[AcmeClient] Certificates saved to" << m_CertOutputDir;
    return true;
}

// ===========================================================================
// Step chain
// ===========================================================================

// ---------------------------------------------------------------------------
// Step 1: Ensure both RSA keys exist
// ---------------------------------------------------------------------------

void AcmeClient::stepCheckKeys()
{
    if (m_Cancelled) return;

    if (!QFile::exists(m_AccountKeyPath)) {
        emit progress(QStringLiteral("Generating ACME account key..."));
        if (!generateRsaKey(m_AccountKeyPath)) {
            emit errorOccurred(QStringLiteral("Failed to generate ACME account key"));
            emit finished(false);
            return;
        }
        qInfo() << "[AcmeClient] Account key created:" << m_AccountKeyPath;
    }

    if (!QFile::exists(m_DomainKeyPath)) {
        emit progress(QStringLiteral("Generating domain key..."));
        if (!generateRsaKey(m_DomainKeyPath)) {
            emit errorOccurred(QStringLiteral("Failed to generate domain key"));
            emit finished(false);
            return;
        }
        qInfo() << "[AcmeClient] Domain key created:" << m_DomainKeyPath;
    }

    stepGetDirectory();
}

// ---------------------------------------------------------------------------
// Step 2: GET ACME directory
// ---------------------------------------------------------------------------

void AcmeClient::stepGetDirectory()
{
    if (m_Cancelled) return;

    emit progress(QStringLiteral("Fetching ACME directory..."));

    QNetworkRequest req{QUrl(m_DirectoryUrl)};
    QNetworkReply* reply = m_Nam->get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_Cancelled) return;

        // Save nonce from directory response
        QByteArray nonce = reply->rawHeader("Replay-Nonce");
        if (!nonce.isEmpty()) m_Nonce = nonce;

        int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code != 200) {
            emit errorOccurred(QStringLiteral("ACME directory request failed (HTTP %1)")
                               .arg(code));
            emit finished(false);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            emit errorOccurred(QStringLiteral("Invalid ACME directory JSON"));
            emit finished(false);
            return;
        }

        m_Directory = doc.object();
        qInfo() << "[AcmeClient] Got ACME directory";

        stepCreateAccount();
    });
}

// ---------------------------------------------------------------------------
// Step 3: Create or retrieve ACME account
// ---------------------------------------------------------------------------

void AcmeClient::stepCreateAccount()
{
    if (m_Cancelled) return;

    QString url = m_Directory.value(QStringLiteral("newAccount")).toString();
    if (url.isEmpty()) {
        emit errorOccurred(QStringLiteral("ACME directory missing newAccount"));
        emit finished(false);
        return;
    }

    emit progress(QStringLiteral("Creating ACME account..."));

    QJsonObject payload;
    payload[QStringLiteral("termsOfServiceAgreed")] = true;
    // Also set "onlyReturnExisting": false — this is the default
    // but we'll be explicit: we want to create if not exists
    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    // First request: useKid=false (embed JWK)
    acmePost(url, body, false,
             [this](int code, const QByteArray& respBody, const QString& location) {
        if (m_Cancelled) return;

        if (code == 201) {
            emit progress(QStringLiteral("ACME account created"));
            qInfo() << "[AcmeClient] ACME account created";
        } else if (code == 200) {
            emit progress(QStringLiteral("Using existing ACME account"));
            qInfo() << "[AcmeClient] ACME account already exists";
        } else {
            QString err = QStringLiteral("ACME account creation failed (HTTP %1)")
                          .arg(code);
            if (!respBody.isEmpty())
                err += QStringLiteral(": ") + QString::fromUtf8(respBody.left(200));
            emit errorOccurred(err);
            emit finished(false);
            return;
        }

        // Store the account URL from the Location header (or response body)
        if (!location.isEmpty()) {
            m_AccountUrl = location;
        } else {
            // Fallback: try to extract from response body
            QJsonDocument d = QJsonDocument::fromJson(respBody);
            if (d.isObject()) {
                QStringList keys = d.object().keys();
                // ACME v2 response may include the account URL in the body
                // as a child of the "keyChange" or similar field
                // But typically Location header is set. Let's try to parse it.
                // Actually the response body doesn't contain the account URL.
                // Let's set it from the URL we posted to, with the account ID from body.
                emit errorOccurred(QStringLiteral("Missing Location header in account response"));
                emit finished(false);
                return;
            }
        }

        stepCreateOrder();
    });
}

// ---------------------------------------------------------------------------
// Step 4: Create certificate order
// ---------------------------------------------------------------------------

void AcmeClient::stepCreateOrder()
{
    if (m_Cancelled) return;

    QString url = m_Directory.value(QStringLiteral("newOrder")).toString();
    if (url.isEmpty()) {
        emit errorOccurred(QStringLiteral("ACME directory missing newOrder"));
        emit finished(false);
        return;
    }

    emit progress(QStringLiteral("Creating certificate order..."));

    QJsonObject ident;
    ident[QStringLiteral("type")]  = QStringLiteral("dns");
    ident[QStringLiteral("value")] = m_Host;

    QJsonArray idents;
    idents.append(ident);

    QJsonObject payload;
    payload[QStringLiteral("identifiers")] = idents;

    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    acmePost(url, body, true,
             [this](int code, const QByteArray& respBody, const QString& /*loc*/) {
        if (m_Cancelled) return;

        if (code != 201) {
            QString err = QStringLiteral("ACME order creation failed (HTTP %1)")
                          .arg(code);
            if (!respBody.isEmpty())
                err += QStringLiteral(": ") + QString::fromUtf8(respBody.left(200));
            emit errorOccurred(err);
            emit finished(false);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(respBody);
        if (!doc.isObject()) {
            emit errorOccurred(QStringLiteral("Invalid order response JSON"));
            emit finished(false);
            return;
        }

        m_Order = doc.object();
        m_FinalizeUrl = m_Order.value(QStringLiteral("finalize")).toString();
        qInfo() << "[AcmeClient] Order created, status:" << m_Order.value("status").toString();

        // Get the first authorization URL
        QJsonArray auths = m_Order.value(QStringLiteral("authorizations")).toArray();
        if (auths.isEmpty()) {
            emit errorOccurred(QStringLiteral("No authorizations in order response"));
            emit finished(false);
            return;
        }

        m_AuthorizationUrl = auths.first().toString();

        stepGetAuthorization();
    });
}

// ---------------------------------------------------------------------------
// Step 5: Get authorization (find DNS-01 challenge)
// ---------------------------------------------------------------------------

void AcmeClient::stepGetAuthorization()
{
    if (m_Cancelled) return;

    emit progress(QStringLiteral("Getting DNS authorization..."));

    acmePostAsGet(m_AuthorizationUrl,
                  [this](int code, const QByteArray& body) {
        if (m_Cancelled) return;

        if (code != 200) {
            emit errorOccurred(QStringLiteral("ACME authorization failed (HTTP %1)")
                               .arg(code));
            emit finished(false);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit errorOccurred(QStringLiteral("Invalid authorization JSON"));
            emit finished(false);
            return;
        }

        QJsonObject auth = doc.object();
        QString status = auth.value(QStringLiteral("status")).toString();

        // If it's already valid (cached), skip to finalize
        if (status == QStringLiteral("valid")) {
            qInfo() << "[AcmeClient] Authorization already valid (cached)";
            stepFinalize();
            return;
        }

        // Find the DNS-01 challenge
        QJsonArray challenges = auth.value(QStringLiteral("challenges")).toArray();
        for (const QJsonValue& v : challenges) {
            QJsonObject ch = v.toObject();
            if (ch.value(QStringLiteral("type")).toString() == QStringLiteral("dns-01")) {
                m_ChallengeToken = ch.value(QStringLiteral("token")).toString();
                m_ChallengeUrl   = ch.value(QStringLiteral("url")).toString();
                break;
            }
        }

        if (m_ChallengeToken.isEmpty() || m_ChallengeUrl.isEmpty()) {
            emit errorOccurred(QStringLiteral("No DNS-01 challenge in authorization"));
            emit finished(false);
            return;
        }

        qInfo() << "[AcmeClient] Found DNS-01 challenge, token:" << m_ChallengeToken.left(16);

        stepRespondChallenge();
    });
}

// ---------------------------------------------------------------------------
// Step 6: Create TXT record and respond to challenge
// ---------------------------------------------------------------------------

void AcmeClient::stepRespondChallenge()
{
    if (m_Cancelled) return;

    emit progress(QStringLiteral("Setting up DNS-01 challenge..."));

    // Compute key authorization: token + "." + thumbprint
    QString keyAuth = m_ChallengeToken + QStringLiteral(".") +
                      QString::fromUtf8(accountKeyThumbprint());

    // DNS-01 value = base64url(SHA256(keyAuthorization))
    QByteArray dnsValue = b64urlEncode(
        QCryptographicHash::hash(keyAuth.toUtf8(), QCryptographicHash::Sha256));

    qInfo() << "[AcmeClient] DNS-01 key authorization computed";

    // Create the TXT record via deSEC (synchronous)
    if (!createChallengeTxtRecord(QString::fromUtf8(dnsValue))) {
        emit errorOccurred(QStringLiteral("Failed to create DNS TXT record for challenge"));
        emit finished(false);
        return;
    }

    // Wait briefly for DNS propagation (deSEC is usually fast)
    emit progress(QStringLiteral("Waiting for DNS propagation (5s)..."));
    QTimer::singleShot(5000, this, [this]() {
        if (m_Cancelled) return;

        // Post to challenge URL to indicate readiness
        emit progress(QStringLiteral("Responding to ACME challenge..."));

        // Respond with empty payload JSON to start the challenge
        acmePost(m_ChallengeUrl, QByteArray("{}"), true,
                 [this](int code, const QByteArray& /*body*/, const QString& /*loc*/) {
            if (m_Cancelled) return;

            if (code != 200) {
                emit errorOccurred(QStringLiteral("Challenge response failed (HTTP %1)")
                                   .arg(code));
                deleteChallengeTxtRecord();
                emit finished(false);
                return;
            }

            qInfo() << "[AcmeClient] Challenge posted, starting poll";

            m_PollRetries = 0;
            stepPollAuthorization();
        });
    });
}

// ---------------------------------------------------------------------------
// Step 7: Poll authorization until valid/invalid
// ---------------------------------------------------------------------------

void AcmeClient::stepPollAuthorization()
{
    if (m_Cancelled) return;

    if (m_PollRetries >= kMaxPollRetries) {
        emit errorOccurred(QStringLiteral("Authorization polling timed out (3 min)"));
        deleteChallengeTxtRecord();
        emit finished(false);
        return;
    }

    emit progress(QStringLiteral("Waiting for DNS validation (attempt %1/%2)...")
                  .arg(m_PollRetries + 1).arg(kMaxPollRetries));

    acmePostAsGet(m_AuthorizationUrl,
                  [this](int code, const QByteArray& body) {
        if (m_Cancelled) {
            deleteChallengeTxtRecord();
            return;
        }

        if (code != 200) {
            emit errorOccurred(QStringLiteral("Authorization poll failed (HTTP %1)")
                               .arg(code));
            deleteChallengeTxtRecord();
            emit finished(false);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit errorOccurred(QStringLiteral("Invalid authorization poll response"));
            deleteChallengeTxtRecord();
            emit finished(false);
            return;
        }

        QString status = doc.object().value(QStringLiteral("status")).toString();

        if (status == QStringLiteral("valid")) {
            qInfo() << "[AcmeClient] Authorization valid!";
            emit progress(QStringLiteral("DNS validation successful"));

            // Clean up TXT record
            deleteChallengeTxtRecord();

            stepFinalize();
            return;
        }

        if (status == QStringLiteral("invalid")) {
            QString err = QStringLiteral("DNS validation failed (authorization invalid)");
            QJsonObject error = doc.object().value(QStringLiteral("error")).toObject();
            if (!error.isEmpty()) {
                err += QStringLiteral(": ") +
                       error.value(QStringLiteral("detail")).toString();
            }
            emit errorOccurred(err);
            deleteChallengeTxtRecord();
            emit finished(false);
            return;
        }

        // Still pending — poll again after interval
        m_PollRetries++;
        QTimer::singleShot(kPollIntervalMs, this, &AcmeClient::stepPollAuthorization);
    });
}

// ---------------------------------------------------------------------------
// Step 8: Finalize order (submit CSR)
// ---------------------------------------------------------------------------

void AcmeClient::stepFinalize()
{
    if (m_Cancelled) return;

    if (m_FinalizeUrl.isEmpty()) {
        emit errorOccurred(QStringLiteral("No finalize URL in order"));
        emit finished(false);
        return;
    }

    emit progress(QStringLiteral("Generating CSR..."));

    QByteArray csrDer = generateCsr();
    if (csrDer.isEmpty()) {
        emit errorOccurred(QStringLiteral("CSR generation failed"));
        emit finished(false);
        return;
    }

    emit progress(QStringLiteral("Finalizing order..."));

    QJsonObject payload;
    payload[QStringLiteral("csr")] = QString::fromUtf8(b64urlEncode(csrDer));
    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    acmePost(m_FinalizeUrl, body, true,
             [this](int code, const QByteArray& respBody, const QString& /*loc*/) {
        if (m_Cancelled) return;

        if (code != 200) {
            emit errorOccurred(QStringLiteral("Order finalization failed (HTTP %1)")
                               .arg(code));
            emit finished(false);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(respBody);
        if (!doc.isObject()) {
            emit errorOccurred(QStringLiteral("Invalid finalize response JSON"));
            emit finished(false);
            return;
        }

        QString status = doc.object().value(QStringLiteral("status")).toString();
        m_CertUrl = doc.object().value(QStringLiteral("certificate")).toString();

        qInfo() << "[AcmeClient] Order finalized, status:" << status;

        if (status == QStringLiteral("valid") && !m_CertUrl.isEmpty()) {
            stepDownloadCert();
            return;
        }

        if (status == QStringLiteral("valid") && m_CertUrl.isEmpty()) {
            emit errorOccurred(QStringLiteral("Certificate URL missing in valid order response"));
            emit finished(false);
            return;
        }

        // Order still processing — poll the order URL
        QString orderUrl = doc.object().value(QStringLiteral("url")).toString();
        if (orderUrl.isEmpty()) {
            emit errorOccurred(QStringLiteral("Order URL missing in finalize response"));
            emit finished(false);
            return;
        }

        qInfo() << "[AcmeClient] Order still processing (" << status << "), will poll";
        emit progress(QStringLiteral("Waiting for order to be ready..."));

        // Poll the order URL until valid
        m_PollRetries = 0;
        std::function<void()> pollOrder;
        pollOrder = [this, orderUrl, &pollOrder]() {
            if (m_Cancelled) return;
            acmePostAsGet(orderUrl,
                [this, &pollOrder](int code, const QByteArray& body) {
                    if (m_Cancelled) return;
                    if (code != 200) {
                        emit errorOccurred(QStringLiteral("Order poll failed (HTTP %1)").arg(code));
                        emit finished(false);
                        return;
                    }
                    QJsonDocument d = QJsonDocument::fromJson(body);
                    if (!d.isObject()) {
                        emit errorOccurred(QStringLiteral("Invalid order poll response"));
                        emit finished(false);
                        return;
                    }
                    QString s = d.object().value(QStringLiteral("status")).toString();
                    if (s == QStringLiteral("valid")) {
                        m_CertUrl = d.object().value(QStringLiteral("certificate")).toString();
                        if (!m_CertUrl.isEmpty()) {
                            stepDownloadCert();
                        } else {
                            emit errorOccurred(QStringLiteral("Certificate URL missing in order"));
                            emit finished(false);
                        }
                        return;
                    }
                    if (s == QStringLiteral("invalid")) {
                        emit errorOccurred(QStringLiteral("Order became invalid"));
                        emit finished(false);
                        return;
                    }
                    // Still processing — poll again
                    if (++m_PollRetries < 12) { // 12 * 5s = 60s
                        QTimer::singleShot(5000, this, pollOrder);
                    } else {
                        emit errorOccurred(QStringLiteral("Order processing timed out"));
                        emit finished(false);
                    }
                });
        };

        QTimer::singleShot(3000, this, pollOrder);
    });
}

// ---------------------------------------------------------------------------
// Step 9: Download certificate
// ---------------------------------------------------------------------------

void AcmeClient::stepDownloadCert()
{
    if (m_Cancelled) return;

    if (m_CertUrl.isEmpty()) {
        emit errorOccurred(QStringLiteral("No certificate URL to download from"));
        emit finished(false);
        return;
    }

    emit progress(QStringLiteral("Downloading certificate..."));

    acmePostAsGet(m_CertUrl,
                  [this](int code, const QByteArray& body) {
        if (m_Cancelled) return;

        if (code != 200) {
            emit errorOccurred(QStringLiteral("Certificate download failed (HTTP %1)")
                               .arg(code));
            emit finished(false);
            return;
        }

        qInfo() << "[AcmeClient] Certificate downloaded (" << body.size() << "bytes)";

        if (!saveCertificate(body)) {
            emit errorOccurred(QStringLiteral("Failed to save certificate files"));
            emit finished(false);
            return;
        }

        QString fullchainPath = m_CertOutputDir + QStringLiteral("/fullchain.pem");
        QString keyPath       = m_CertOutputDir + QStringLiteral("/key.pem");

        emit progress(QStringLiteral("Certificate issued successfully"));
        emit certificateReady(fullchainPath, keyPath);
        emit finished(true);
    });
}
