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

#include "AcmeClient.h"
#include "PdnsClient.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkReply>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

// OpenSSL C API (replaces openssl CLI calls)
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bn.h>
#include <openssl/err.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const QString kDefaultDirectoryUrl =
    QStringLiteral("https://acme-v02.api.letsencrypt.org/directory");

static constexpr int kHttpTimeoutMs = 30000;
static constexpr int kPollIntervalMs = 5000;
static constexpr int kMaxPollRetries = 36; // 36 x 5s = 3 min
static constexpr int kChallengeTtl = 60;   // TXT record TTL
// PowerDNS endpoint and zone come from PdnsClient (env-driven, single source).

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

void AcmeClient::setAccountKeyPath(const QString& path)
{
    m_AccountKeyPath = path;
}
void AcmeClient::setDomainKeyPath(const QString& path)
{
    m_DomainKeyPath = path;
}
void AcmeClient::setCertOutputDir(const QString& dir)
{
    m_CertOutputDir = dir;
}
void AcmeClient::setHost(const QString& host)
{
    m_Host = host;
}
void AcmeClient::setBaseDomain(const QString& domain)
{
    m_BaseDomain = domain;
}
void AcmeClient::setPdnsToken(const QString& token)
{
    m_PdnsToken = token;
}

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

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) {
        qWarning() << "[AcmeClient] Failed to create EVP_PKEY_CTX for keygen";
        return false;
    }

    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        qWarning() << "[AcmeClient] EVP_PKEY_keygen_init failed";
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) {
        qWarning() << "[AcmeClient] EVP_PKEY_CTX_set_rsa_keygen_bits failed";
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        qWarning() << "[AcmeClient] EVP_PKEY_keygen failed";
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    // Write PEM to file
    BIO* bio = BIO_new(BIO_s_mem());
    if (!PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr)) {
        qWarning() << "[AcmeClient] PEM_write_bio_PrivateKey failed";
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return false;
    }

    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "[AcmeClient] Cannot write key:" << path;
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return false;
    }
    f.write(data, len);
    f.close();

    BIO_free(bio);
    EVP_PKEY_free(pkey);
    qInfo() << "[AcmeClient] RSA key generated:" << path;
    return true;
}

// ---------------------------------------------------------------------------
// RSA helpers — native OpenSSL C API
// ---------------------------------------------------------------------------

QByteArray AcmeClient::parseRsaModulus()
{
    BIO* bio = BIO_new_file(m_AccountKeyPath.toUtf8().constData(), "r");
    if (!bio) {
        qWarning() << "[AcmeClient] Cannot open account key:" << m_AccountKeyPath;
        return {};
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        qWarning() << "[AcmeClient] Failed to parse account key for modulus";
        return {};
    }

    BIGNUM* n = nullptr;
    if (!EVP_PKEY_get_bn_param(pkey, "n", &n) || !n) {
        qWarning() << "[AcmeClient] EVP_PKEY_get_bn_param(n) failed";
        EVP_PKEY_free(pkey);
        return {};
    }

    int len = BN_num_bytes(n);
    QByteArray modulus(len, Qt::Uninitialized);
    BN_bn2bin(n, reinterpret_cast<unsigned char*>(modulus.data()));
    BN_free(n);
    EVP_PKEY_free(pkey);
    return modulus;
}

QByteArray AcmeClient::parseRsaExponent()
{
    BIO* bio = BIO_new_file(m_AccountKeyPath.toUtf8().constData(), "r");
    if (!bio) {
        qWarning() << "[AcmeClient] Cannot open account key:" << m_AccountKeyPath;
        return {};
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        qWarning() << "[AcmeClient] Failed to parse account key for exponent";
        return {};
    }

    BIGNUM* e = nullptr;
    if (!EVP_PKEY_get_bn_param(pkey, "e", &e) || !e) {
        qWarning() << "[AcmeClient] EVP_PKEY_get_bn_param(e) failed";
        EVP_PKEY_free(pkey);
        return {};
    }

    int len = BN_num_bytes(e);
    QByteArray exponent(len, Qt::Uninitialized);
    BN_bn2bin(e, reinterpret_cast<unsigned char*>(exponent.data()));
    BN_free(e);
    EVP_PKEY_free(pkey);
    return exponent;
}

// ---------------------------------------------------------------------------
// JWK helpers
// ---------------------------------------------------------------------------

QJsonObject AcmeClient::accountKeyJwk()
{
    QJsonObject jwk;
    jwk[QStringLiteral("kty")] = QStringLiteral("RSA");
    jwk[QStringLiteral("n")] = QString::fromUtf8(b64urlEncode(parseRsaModulus()));
    jwk[QStringLiteral("e")] = QString::fromUtf8(b64urlEncode(parseRsaExponent()));
    return jwk;
}

QByteArray AcmeClient::accountKeyThumbprint()
{
    QJsonObject jwk = accountKeyJwk();
    // Canonical JSON: keys sorted alphabetically → e, kty, n
    QJsonObject c;
    c[QStringLiteral("e")] = jwk.value(QStringLiteral("e"));
    c[QStringLiteral("kty")] = jwk.value(QStringLiteral("kty"));
    c[QStringLiteral("n")] = jwk.value(QStringLiteral("n"));

    QByteArray json = QJsonDocument(c).toJson(QJsonDocument::Compact);
    return b64urlEncode(QCryptographicHash::hash(json, QCryptographicHash::Sha256));
}

// ---------------------------------------------------------------------------
// JWS signing (RSA-SHA256 via native OpenSSL C API)
// ---------------------------------------------------------------------------

QByteArray AcmeClient::signRsaSha256(const QByteArray& data)
{
    // Load the account private key from PEM file
    BIO* bio = BIO_new_file(m_AccountKeyPath.toUtf8().constData(), "r");
    if (!bio) {
        qWarning() << "[AcmeClient] Cannot open account key for signing:" << m_AccountKeyPath;
        return {};
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        qWarning() << "[AcmeClient] Failed to parse account key for signing";
        return {};
    }

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        EVP_PKEY_free(pkey);
        return {};
    }

    if (EVP_DigestSignInit(mdctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
        qWarning() << "[AcmeClient] EVP_DigestSignInit failed";
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        return {};
    }

    if (EVP_DigestSignUpdate(mdctx, data.constData(), data.size()) <= 0) {
        qWarning() << "[AcmeClient] EVP_DigestSignUpdate failed";
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        return {};
    }

    // Get signature length
    size_t sigLen = 0;
    if (EVP_DigestSignFinal(mdctx, nullptr, &sigLen) <= 0) {
        qWarning() << "[AcmeClient] EVP_DigestSignFinal (length) failed";
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        return {};
    }

    QByteArray signature(static_cast<int>(sigLen), Qt::Uninitialized);
    if (EVP_DigestSignFinal(mdctx, reinterpret_cast<unsigned char*>(signature.data()), &sigLen) <=
        0) {
        qWarning() << "[AcmeClient] EVP_DigestSignFinal failed";
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        return {};
    }
    signature.resize(static_cast<int>(sigLen));

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    return signature;
}

QJsonObject AcmeClient::buildEabJws(const QString& newAccountUrl)
{
    if (m_EabKid.isEmpty() || m_EabHmacKey.isEmpty()) return {};

    // Inner JWS protected header: HS256, the CA-issued kid, and the newAccount URL.
    // Note: no "nonce" — the EAB inner JWS is not nonce-protected (RFC 8555 §7.3.4).
    QJsonObject header;
    header[QStringLiteral("alg")] = QStringLiteral("HS256");
    header[QStringLiteral("kid")] = m_EabKid;
    header[QStringLiteral("url")] = newAccountUrl;

    // Payload is the account public key JWK (the key being bound to the account).
    QByteArray protectedB64 = b64urlEncode(QJsonDocument(header).toJson(QJsonDocument::Compact));
    QByteArray payloadB64 =
        b64urlEncode(QJsonDocument(accountKeyJwk()).toJson(QJsonDocument::Compact));
    QByteArray signingInput = protectedB64 + '.' + payloadB64;

    // HMAC-SHA256 with the base64url-decoded EAB key.
    QByteArray hmacKey = b64urlDecode(m_EabHmacKey.toUtf8());
    QByteArray sig =
        QMessageAuthenticationCode::hash(signingInput, hmacKey, QCryptographicHash::Sha256);

    QJsonObject eab;
    eab[QStringLiteral("protected")] = QString::fromUtf8(protectedB64);
    eab[QStringLiteral("payload")] = QString::fromUtf8(payloadB64);
    eab[QStringLiteral("signature")] = QString::fromUtf8(b64urlEncode(sig));
    return eab;
}

QByteArray AcmeClient::buildJws(const QByteArray& payload, const QString& url, bool useKid)
{
    QJsonObject header;
    header[QStringLiteral("alg")] = QStringLiteral("RS256");
    header[QStringLiteral("nonce")] = QString::fromUtf8(m_Nonce);
    header[QStringLiteral("url")] = url;

    if (useKid && !m_AccountUrl.isEmpty()) {
        header[QStringLiteral("kid")] = m_AccountUrl;
    } else {
        header[QStringLiteral("jwk")] = accountKeyJwk();
    }

    QByteArray protectedB64 = b64urlEncode(QJsonDocument(header).toJson(QJsonDocument::Compact));
    QByteArray payloadB64 = b64urlEncode(payload);
    QByteArray signingInput = protectedB64 + '.' + payloadB64;

    QByteArray sig = signRsaSha256(signingInput);
    QByteArray sigB64 = b64urlEncode(sig);

    QJsonObject jws;
    jws[QStringLiteral("protected")] = QString::fromUtf8(protectedB64);
    jws[QStringLiteral("payload")] = QString::fromUtf8(payloadB64);
    jws[QStringLiteral("signature")] = QString::fromUtf8(sigB64);

    return QJsonDocument(jws).toJson(QJsonDocument::Compact);
}

// ---------------------------------------------------------------------------
// Fetch a fresh Replay-Nonce
// ---------------------------------------------------------------------------

void AcmeClient::fetchNonce(const std::function<void(bool)>& callback)
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
        if (m_Cancelled) {
            if (callback) callback(false);
            return;
        }

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

void AcmeClient::acmePost(
    const QString& url, const QByteArray& payload, bool useKid,
    const std::function<void(int, const QByteArray&, const QString&)>& callback, int retriesLeft)
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
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/jose+json"));

    QNetworkReply* reply = m_Nam->post(req, jwsBody);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, payload, useKid, callback, retriesLeft]() {
                reply->deleteLater();
                if (m_Cancelled) return;

                int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                QByteArray body = reply->readAll();
                QString location = reply->rawHeader("Location");
                QByteArray nonce = reply->rawHeader("Replay-Nonce");
                if (!nonce.isEmpty()) m_Nonce = nonce;

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
                               const std::function<void(int, const QByteArray&)>& callback)
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
    // Load domain private key from PEM file
    BIO* bio = BIO_new_file(m_DomainKeyPath.toUtf8().constData(), "r");
    if (!bio) {
        qWarning() << "[AcmeClient] Cannot open domain key for CSR:" << m_DomainKeyPath;
        return {};
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        qWarning() << "[AcmeClient] Failed to parse domain key for CSR";
        return {};
    }

    X509_REQ* req = X509_REQ_new();
    if (!req) {
        qWarning() << "[AcmeClient] X509_REQ_new failed";
        EVP_PKEY_free(pkey);
        return {};
    }

    X509_REQ_set_version(req, 0);
    X509_REQ_set_pubkey(req, pkey);

    // Subject: CN = host
    X509_NAME* name = X509_NAME_new();
    QByteArray cn = m_Host.toUtf8();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn.constData()), -1, -1, 0);
    X509_REQ_set_subject_name(req, name);
    X509_NAME_free(name); // req holds its own copy

    // Extension: subjectAltName = DNS:host
    GENERAL_NAMES* gens = sk_GENERAL_NAME_new_null();
    if (gens) {
        GENERAL_NAME* gen = GENERAL_NAME_new();
        if (gen) {
            ASN1_IA5STRING* ia5 = ASN1_IA5STRING_new();
            ASN1_STRING_set(ia5, cn.constData(), cn.size());
            GENERAL_NAME_set0_value(gen, GEN_DNS, ia5); // gen owns ia5
            sk_GENERAL_NAME_push(gens, gen);            // gens owns gen

            X509_EXTENSION* ext = X509V3_EXT_i2d(NID_subject_alt_name, 0, gens);
            if (ext) {
                STACK_OF(X509_EXTENSION)* exts = sk_X509_EXTENSION_new_null();
                sk_X509_EXTENSION_push(exts, ext);
                X509_REQ_add_extensions(req, exts);
                sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
            }
        }
        sk_GENERAL_NAME_pop_free(gens, GENERAL_NAME_free);
    }

    // Sign with SHA256
    X509_REQ_sign(req, pkey, EVP_sha256());

    // Write DER output
    BIO* out = BIO_new(BIO_s_mem());
    i2d_X509_REQ_bio(out, req);

    char* data = nullptr;
    long dataLen = BIO_get_mem_data(out, &data);
    QByteArray csrDer(data, static_cast<int>(dataLen));

    BIO_free(out);
    X509_REQ_free(req);
    EVP_PKEY_free(pkey);
    return csrDer;
}

// ---------------------------------------------------------------------------
// DNS-01 challenge helpers (PowerDNS API, synchronous)
// ---------------------------------------------------------------------------

bool AcmeClient::createChallengeTxtRecord(const QString& dnsValue)
{
    // Extract the label from m_Host by removing the base domain suffix.
    // m_Host = "92b8d127.moonlightweb.top"
    // m_BaseDomain = "moonlightweb.top"
    // label = "92b8d127"
    // subname = "_acme-challenge.92b8d127.moonlightweb.top." (FQDN with trailing dot)

    if (m_Host.isEmpty() || m_BaseDomain.isEmpty()) {
        qWarning() << "[AcmeClient] Cannot create TXT: missing host or base domain";
        return false;
    }

    QString label = m_Host;
    QString suffix = QStringLiteral(".") + m_BaseDomain;
    if (label.endsWith(suffix)) label = label.left(label.length() - suffix.length());

    m_TxtSubname = QStringLiteral("_acme-challenge.") + label + QStringLiteral(".") + m_BaseDomain +
                   QStringLiteral(".");

    qInfo() << "[AcmeClient] Creating TXT record" << m_TxtSubname;

    // Build PowerDNS PATCH body
    QJsonObject record;
    record[QStringLiteral("content")] = QChar('"') + dnsValue + QChar('"');
    record[QStringLiteral("disabled")] = false;

    QJsonArray records;
    records.append(record);

    QJsonObject rrset;
    rrset[QStringLiteral("name")] = m_TxtSubname;
    rrset[QStringLiteral("type")] = QStringLiteral("TXT");
    rrset[QStringLiteral("ttl")] = kChallengeTtl;
    rrset[QStringLiteral("changetype")] = QStringLiteral("REPLACE");
    rrset[QStringLiteral("records")] = records;

    QJsonArray rrsets;
    rrsets.append(rrset);

    QJsonObject body;
    body[QStringLiteral("rrsets")] = rrsets;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QUrl url(PdnsClient::apiBaseUrl() + QStringLiteral("/zones/") + PdnsClient::zoneName());
    QNetworkRequest req{url};
    req.setRawHeader("X-API-Key", m_PdnsToken.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QBuffer* buf = new QBuffer;
    buf->setData(payload);
    buf->open(QIODevice::ReadOnly);
    QNetworkReply* reply = m_Nam->sendCustomRequest(req, "PATCH", buf);
    buf->setParent(reply);

    // Synchronous wait
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(15000);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        qWarning() << "[AcmeClient] PowerDNS TXT creation timed out";
        return false;
    }
    timer.stop();

    int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (code == 204) {
        qInfo() << "[AcmeClient] TXT record created (HTTP 204)";
        return true;
    }
    qWarning() << "[AcmeClient] PowerDNS TXT creation failed (HTTP" << code << ")";
    return false;
}

bool AcmeClient::deleteChallengeTxtRecord()
{
    if (m_TxtSubname.isEmpty()) return false;

    qInfo() << "[AcmeClient] Deleting TXT record" << m_TxtSubname;

    // Build PowerDNS PATCH DELETE body
    QJsonObject rrset;
    rrset[QStringLiteral("name")] = m_TxtSubname;
    rrset[QStringLiteral("type")] = QStringLiteral("TXT");
    rrset[QStringLiteral("changetype")] = QStringLiteral("DELETE");
    rrset[QStringLiteral("records")] = QJsonArray();

    QJsonArray rrsets;
    rrsets.append(rrset);

    QJsonObject body;
    body[QStringLiteral("rrsets")] = rrsets;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QUrl url(PdnsClient::apiBaseUrl() + QStringLiteral("/zones/") + PdnsClient::zoneName());
    QNetworkRequest req{url};
    req.setRawHeader("X-API-Key", m_PdnsToken.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QBuffer* buf2 = new QBuffer;
    buf2->setData(payload);
    buf2->open(QIODevice::ReadOnly);
    QNetworkReply* reply = m_Nam->sendCustomRequest(req, "PATCH", buf2);
    buf2->setParent(reply);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(15000);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        return false;
    }
    timer.stop();

    int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (code == 204 || code == 404 || code == 422) {
        qInfo() << "[AcmeClient] TXT record deleted (HTTP" << code << ")";
        return true;
    }
    qWarning() << "[AcmeClient] PowerDNS TXT deletion failed (HTTP" << code << ")";
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
            emit errorOccurred(QStringLiteral("ACME directory request failed (HTTP %1)").arg(code));
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
    // "onlyReturnExisting" defaults to false, i.e. create the account if it
    // does not already exist.

    // External Account Binding (ZeroSSL / Google Trust Services require it).
    QJsonObject eab = buildEabJws(url);
    if (!eab.isEmpty()) {
        payload[QStringLiteral("externalAccountBinding")] = eab;
        qInfo() << "[AcmeClient] newAccount carries EAB (kid set)";
    }

    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    // First request: useKid=false (embed JWK)
    acmePost(
        url, body, false, [this](int code, const QByteArray& respBody, const QString& location) {
            if (m_Cancelled) return;

            if (code == 201) {
                emit progress(QStringLiteral("ACME account created"));
                qInfo() << "[AcmeClient] ACME account created";
            } else if (code == 200) {
                emit progress(QStringLiteral("Using existing ACME account"));
                qInfo() << "[AcmeClient] ACME account already exists";
            } else {
                QString err = QStringLiteral("ACME account creation failed (HTTP %1)").arg(code);
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
                // The ACME v2 account URL is only carried by the Location header,
                // never in the response body, so a missing header is fatal here.
                QJsonDocument d = QJsonDocument::fromJson(respBody);
                if (d.isObject()) {
                    emit errorOccurred(
                        QStringLiteral("Missing Location header in account response"));
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
    ident[QStringLiteral("type")] = QStringLiteral("dns");
    ident[QStringLiteral("value")] = m_Host;

    QJsonArray idents;
    idents.append(ident);

    QJsonObject payload;
    payload[QStringLiteral("identifiers")] = idents;

    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    acmePost(url, body, true, [this](int code, const QByteArray& respBody, const QString& loc) {
        if (m_Cancelled) return;

        if (code != 201) {
            QString err = QStringLiteral("ACME order creation failed (HTTP %1)").arg(code);
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
        // The order URL is the Location header of newOrder (RFC 8555 §7.4);
        // the order object body has no self-referential "url" field.
        m_OrderUrl = loc;
        qInfo() << "[AcmeClient] Order created, status:" << m_Order.value("status").toString()
                << "url:" << m_OrderUrl;

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

    acmePostAsGet(m_AuthorizationUrl, [this](int code, const QByteArray& body) {
        if (m_Cancelled) return;

        if (code != 200) {
            emit errorOccurred(QStringLiteral("ACME authorization failed (HTTP %1)").arg(code));
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
                m_ChallengeUrl = ch.value(QStringLiteral("url")).toString();
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
    QString keyAuth =
        m_ChallengeToken + QStringLiteral(".") + QString::fromUtf8(accountKeyThumbprint());

    // DNS-01 value = base64url(SHA256(keyAuthorization))
    QByteArray dnsValue =
        b64urlEncode(QCryptographicHash::hash(keyAuth.toUtf8(), QCryptographicHash::Sha256));

    qInfo() << "[AcmeClient] DNS-01 key authorization computed";

    // Create the TXT record via PowerDNS (synchronous)
    if (!createChallengeTxtRecord(QString::fromUtf8(dnsValue))) {
        emit errorOccurred(QStringLiteral("Failed to create DNS TXT record for challenge"));
        emit finished(false);
        return;
    }

    // Wait briefly for DNS propagation (PowerDNS is usually fast)
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
                         emit errorOccurred(
                             QStringLiteral("Challenge response failed (HTTP %1)").arg(code));
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
                      .arg(m_PollRetries + 1)
                      .arg(kMaxPollRetries));

    acmePostAsGet(m_AuthorizationUrl, [this](int code, const QByteArray& body) {
        if (m_Cancelled) {
            deleteChallengeTxtRecord();
            return;
        }

        if (code != 200) {
            emit errorOccurred(QStringLiteral("Authorization poll failed (HTTP %1)").arg(code));
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
                err += QStringLiteral(": ") + error.value(QStringLiteral("detail")).toString();
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

    acmePost(
        m_FinalizeUrl, body, true,
        [this](int code, const QByteArray& respBody, const QString& /*loc*/) {
            if (m_Cancelled) return;

            if (code != 200) {
                emit errorOccurred(QStringLiteral("Order finalization failed (HTTP %1)").arg(code));
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
                emit errorOccurred(
                    QStringLiteral("Certificate URL missing in valid order response"));
                emit finished(false);
                return;
            }

            // Order still processing — poll the order URL captured from the
            // newOrder Location header (the order body has no self "url" field).
            QString orderUrl = m_OrderUrl;
            if (orderUrl.isEmpty()) orderUrl = doc.object().value(QStringLiteral("url")).toString();
            if (orderUrl.isEmpty()) {
                emit errorOccurred(
                    QStringLiteral("Order URL missing (no Location header from newOrder)"));
                emit finished(false);
                return;
            }

            qInfo() << "[AcmeClient] Order still processing (" << status << "), will poll";
            emit progress(QStringLiteral("Waiting for order to be ready..."));

            // Poll the order URL until valid.
            // The recursive poll closure must outlive this callback's stack frame:
            // it is re-scheduled from inside its own async response, long after
            // stepFinalize() has returned. Hold it in a shared_ptr captured BY VALUE
            // so each lambda keeps it alive (capturing &pollOrder by reference would
            // dangle on the first re-schedule → use-after-free crash).
            m_PollRetries = 0;
            auto pollOrder = std::make_shared<std::function<void()>>();
            *pollOrder = [this, orderUrl, pollOrder]() {
                if (m_Cancelled) return;
                acmePostAsGet(orderUrl, [this, pollOrder](int code, const QByteArray& body) {
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
                        QTimer::singleShot(5000, this, *pollOrder);
                    } else {
                        emit errorOccurred(QStringLiteral("Order processing timed out"));
                        emit finished(false);
                    }
                });
            };

            QTimer::singleShot(3000, this, *pollOrder);
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

    acmePostAsGet(m_CertUrl, [this](int code, const QByteArray& body) {
        if (m_Cancelled) return;

        if (code != 200) {
            emit errorOccurred(QStringLiteral("Certificate download failed (HTTP %1)").arg(code));
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
        QString keyPath = m_CertOutputDir + QStringLiteral("/key.pem");

        emit progress(QStringLiteral("Certificate issued successfully"));
        emit certificateReady(fullchainPath, keyPath);
        emit finished(true);
    });
}
