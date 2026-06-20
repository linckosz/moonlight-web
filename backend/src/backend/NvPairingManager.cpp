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

#include "NvPairingManager.h"
#include "IdentityManager.h"
#include "NvHTTP.h"
#include "common/Logger.h"

#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QUuid>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/bio.h>

#define REQUEST_TIMEOUT_MS 5000
#define PAIRING_PIN_WAIT_MS  60000  // Stage 1 blocks until user enters PIN in Sunshine

NvPairingManager::NvPairingManager(const QString& appVersion,
                                   const QString& host, quint16 httpPort, quint16 httpsPort)
    : m_ServerVersion(appVersion.toUtf8())
    , m_Host(host)
    , m_HttpPort(httpPort)
    , m_HttpsPort(httpsPort)
{
    // Load client cert
    QByteArray certPem = IdentityManager::get()->getCertificate();
    BIO* bio = BIO_new_mem_buf(certPem.data(), certPem.size());
    m_Cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!m_Cert)
        throw std::runtime_error("Unable to load client certificate");

    // Load private key
    QByteArray keyPem = IdentityManager::get()->getPrivateKey();
    bio = BIO_new_mem_buf(keyPem.data(), keyPem.size());
    m_PrivateKey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!m_PrivateKey)
        throw std::runtime_error("Unable to load private key");

    // Determine server generation
    QVector<int> quad = NvHTTP::parseQuad(appVersion);
    if (!quad.isEmpty())
        m_ServerMajorVersion = quad[0];

    // Gen 7+ uses SHA-256, older uses SHA-1
    if (m_ServerMajorVersion >= 7) {
        m_HashLength = 32;
    } else {
        m_HashLength = 20;
    }

    m_Nam = new QNetworkAccessManager();
    m_Nam->setProxy(QNetworkProxy::NoProxy);
}

NvPairingManager::~NvPairingManager()
{
    if (m_Cert)
        X509_free(m_Cert);
    if (m_PrivateKey)
        EVP_PKEY_free(m_PrivateKey);
    delete m_Nam;
}

// --- Synchronous HTTP request ---

QString NvPairingManager::openConnection(const QString& scheme, const QString& command,
                                          const QString& arguments, int timeoutMs)
{
    QUrl url;
    url.setScheme(scheme);
    url.setHost(m_Host);
    url.setPort(scheme == "https" ? m_HttpsPort : m_HttpPort);
    url.setPath("/" + command);

    QString query = "uniqueid=" + IdentityManager::get()->getUniqueId()
                    + "&uuid=" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!arguments.isEmpty())
        query += "&" + arguments;
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "Moonlight-Web/0.1");
    req.setTransferTimeout(timeoutMs > 0 ? timeoutMs : REQUEST_TIMEOUT_MS);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

    if (scheme == "https") {
        // Mutual TLS: present client certificate as expected by Sunshine
        auto* identity = IdentityManager::get();
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(QSslCertificate(identity->getCertificate(), QSsl::Pem));
        ssl.setPrivateKey(QSslKey(identity->getPrivateKey(), QSsl::Rsa, QSsl::Pem));
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(ssl);
    }

    // Ignore SSL errors (self-signed cert)
    auto sslConn = QObject::connect(m_Nam, &QNetworkAccessManager::sslErrors,
        [](QNetworkReply* reply, const QList<QSslError>&) {
            reply->ignoreSslErrors();
        });

    QNetworkReply* reply = m_Nam->get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);

    if (timeoutMs > 0)
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);

    loop.exec(QEventLoop::ExcludeUserInputEvents);

    QObject::disconnect(sslConn);

    if (!reply->isFinished())
        reply->abort();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        Logger::warning(QString("Pairing request failed: %1 %2 → %3")
                            .arg(scheme, command, err));
        delete reply;
        throw std::runtime_error(err.toStdString());
    }

    QString body = QString::fromUtf8(reply->readAll());
    delete reply;
    return body;
}

// --- Crypto helpers ---

QByteArray NvPairingManager::generateRandomBytes(int length)
{
    QByteArray result(length, 0);
    RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), length);
    return result;
}

QByteArray NvPairingManager::saltPin(const QByteArray& salt, const QString& pin)
{
    QByteArray result;
    result.append(salt);
    result.append(pin.toUtf8());
    return result;
}

QByteArray NvPairingManager::encrypt(const QByteArray& plaintext, const QByteArray& key)
{
    QByteArray ciphertext(plaintext.size(), 0);

    EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();
    EVP_EncryptInit(cipher, EVP_aes_128_ecb(),
                    reinterpret_cast<const unsigned char*>(key.data()), nullptr);
    EVP_CIPHER_CTX_set_padding(cipher, 0);

    int ciphertextLen = 0;
    EVP_EncryptUpdate(cipher,
                      reinterpret_cast<unsigned char*>(ciphertext.data()),
                      &ciphertextLen,
                      reinterpret_cast<const unsigned char*>(plaintext.data()),
                      plaintext.size());

    EVP_CIPHER_CTX_free(cipher);
    return ciphertext;
}

QByteArray NvPairingManager::decrypt(const QByteArray& ciphertext, const QByteArray& key)
{
    QByteArray plaintext(ciphertext.size(), 0);

    EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();
    EVP_DecryptInit(cipher, EVP_aes_128_ecb(),
                    reinterpret_cast<const unsigned char*>(key.data()), nullptr);
    EVP_CIPHER_CTX_set_padding(cipher, 0);

    int plaintextLen = 0;
    EVP_DecryptUpdate(cipher,
                      reinterpret_cast<unsigned char*>(plaintext.data()),
                      &plaintextLen,
                      reinterpret_cast<const unsigned char*>(ciphertext.data()),
                      ciphertext.size());

    EVP_CIPHER_CTX_free(cipher);
    return plaintext;
}

QByteArray NvPairingManager::getSignatureFromCert(X509* cert)
{
    const ASN1_BIT_STRING* asnSignature;
    X509_get0_signature(&asnSignature, nullptr, cert);

    return QByteArray(
        reinterpret_cast<const char*>(ASN1_STRING_get0_data(asnSignature)),
        ASN1_STRING_length(asnSignature));
}

QByteArray NvPairingManager::getSignatureFromPemCert(const QByteArray& certificate)
{
    BIO* bio = BIO_new_mem_buf(certificate.data(), certificate.size());
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    QByteArray signature = getSignatureFromCert(cert);
    X509_free(cert);
    return signature;
}

bool NvPairingManager::verifySignature(const QByteArray& data,
                                        const QByteArray& signature,
                                        const QByteArray& serverCertificate)
{
    BIO* bio = BIO_new_mem_buf(serverCertificate.data(), serverCertificate.size());
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert)
        return false;

    EVP_PKEY* pubKey = X509_get_pubkey(cert);
    if (!pubKey) {
        X509_free(cert);
        return false;
    }

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pubKey);
    EVP_DigestVerifyUpdate(mdctx, data.data(), data.size());
    int result = EVP_DigestVerifyFinal(mdctx,
        reinterpret_cast<unsigned char*>(const_cast<char*>(signature.data())),
        signature.size());

    EVP_PKEY_free(pubKey);
    EVP_MD_CTX_free(mdctx);
    X509_free(cert);

    return result > 0;
}

QByteArray NvPairingManager::signMessage(const QByteArray& message)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, m_PrivateKey);
    EVP_DigestSignUpdate(ctx,
        reinterpret_cast<unsigned char*>(const_cast<char*>(message.data())),
        message.size());

    size_t signatureLength = 0;
    EVP_DigestSignFinal(ctx, nullptr, &signatureLength);

    QByteArray signature(static_cast<int>(signatureLength), 0);
    EVP_DigestSignFinal(ctx,
        reinterpret_cast<unsigned char*>(signature.data()),
        &signatureLength);

    EVP_MD_CTX_free(ctx);
    return signature;
}

// --- Pairing protocol ---

NvPairingManager::InitResult NvPairingManager::initiatePairing()
{
    if (m_Stage1Done)
        return INIT_OK;

    m_Salt = generateRandomBytes(16);

    try {
        QString args = "devicename=roth&updateState=1&phrase=getservercert&salt="
                       + m_Salt.toHex() + "&clientcert="
                       + IdentityManager::get()->getCertificate().toHex();

        QString response = openConnection("http", "pair", args, PAIRING_PIN_WAIT_MS);

        NvHTTP::verifyResponseStatus(response);

        if (NvHTTP::getXmlString(response, "paired") != "1") {
            Logger::warning("Pairing stage #1 failed: paired != 1");
            return INIT_FAILED;
        }

        QByteArray plainCert = NvHTTP::getXmlStringFromHex(response, "plaincert");
        if (plainCert.isEmpty()) {
            Logger::warning("Server likely already pairing — no plaincert received");
            try {
                openConnection("http", "unpair", QString(), REQUEST_TIMEOUT_MS);
            } catch (...) {}
            return INIT_ALREADY_IN_PROGRESS;
        }

        m_ServerCertPem = plainCert;
        m_Stage1Done = true;
        Logger::info("Pairing stage #1 OK — received server certificate");
        return INIT_OK;

    } catch (const std::exception& e) {
        Logger::warning(QString("Pairing stage #1 exception: %1").arg(e.what()));
        return INIT_FAILED;
    }
}

NvPairingManager::PairState NvPairingManager::completePairing(const QString& pin,
                                                               QByteArray& outServerCertPem)
{
    QCryptographicHash::Algorithm hashAlgo =
        m_ServerMajorVersion >= 7 ? QCryptographicHash::Sha256
                                  : QCryptographicHash::Sha1;

    QByteArray aesKey = QCryptographicHash::hash(saltPin(m_Salt, pin), hashAlgo);
    aesKey.truncate(16);

    try {
        // --- Stage 2: client challenge ---

        QByteArray randomChallenge = generateRandomBytes(16);
        QByteArray encryptedChallenge = encrypt(randomChallenge, aesKey);

        QString challengeXml = openConnection("http", "pair",
            "devicename=roth&updateState=1&clientchallenge=" + encryptedChallenge.toHex(),
            REQUEST_TIMEOUT_MS);

        NvHTTP::verifyResponseStatus(challengeXml);

        if (NvHTTP::getXmlString(challengeXml, "paired") != "1") {
            // PIN not yet entered in Sunshine — retryable
            Logger::warning("Pairing stage #2: PIN not accepted yet");
            return PIN_WRONG;
        }

        QByteArray challengeResponseData = decrypt(
            NvHTTP::getXmlStringFromHex(challengeXml, "challengeresponse"), aesKey);

        QByteArray clientSecretData = generateRandomBytes(16);
        QByteArray challengeResponse;

        // serverResponse = first hashLength bytes of decrypted challenge response
        QByteArray serverResponse(challengeResponseData.data(), m_HashLength);

        // Build our challenge response: remaining 16 bytes + cert sig + client secret
        challengeResponse.append(challengeResponseData.data() + m_HashLength, 16);
        challengeResponse.append(getSignatureFromCert(m_Cert));
        challengeResponse.append(clientSecretData);

        QByteArray paddedHash = QCryptographicHash::hash(challengeResponse, hashAlgo);
        paddedHash.resize(32);
        QByteArray encryptedChallengeResponseHash = encrypt(paddedHash, aesKey);

        // --- Stage 3: server challenge response ---

        QString respXml = openConnection("http", "pair",
            "devicename=roth&updateState=1&serverchallengeresp="
            + encryptedChallengeResponseHash.toHex(),
            REQUEST_TIMEOUT_MS);

        NvHTTP::verifyResponseStatus(respXml);

        if (NvHTTP::getXmlString(respXml, "paired") != "1") {
            Logger::warning("Pairing stage #3 failed");
            return PIN_WRONG;
        }

        QByteArray pairingSecret = NvHTTP::getXmlStringFromHex(respXml, "pairingsecret");
        QByteArray serverSecret = pairingSecret.left(16);
        QByteArray serverSignature = pairingSecret.mid(16);

        // Verify server signature (anti-MITM)
        if (!verifySignature(serverSecret, serverSignature, m_ServerCertPem)) {
            Logger::warning("MITM detected — server signature verification failed");
            openConnection("http", "unpair", QString(), REQUEST_TIMEOUT_MS);
            return FAILED;
        }

        // Verify PIN
        QByteArray expectedResponseData;
        expectedResponseData.append(randomChallenge);
        expectedResponseData.append(getSignatureFromPemCert(m_ServerCertPem));
        expectedResponseData.append(serverSecret);

        if (QCryptographicHash::hash(expectedResponseData, hashAlgo) != serverResponse) {
            Logger::warning("Incorrect PIN");
            openConnection("http", "unpair", QString(), REQUEST_TIMEOUT_MS);
            return PIN_WRONG;
        }

        // --- Stage 4: client pairing secret ---

        QByteArray clientPairingSecret;
        clientPairingSecret.append(clientSecretData);
        clientPairingSecret.append(signMessage(clientSecretData));

        QString secretRespXml = openConnection("http", "pair",
            "devicename=roth&updateState=1&clientpairingsecret="
            + clientPairingSecret.toHex(),
            REQUEST_TIMEOUT_MS);

        NvHTTP::verifyResponseStatus(secretRespXml);

        if (NvHTTP::getXmlString(secretRespXml, "paired") != "1") {
            Logger::warning("Pairing stage #4 failed");
            openConnection("http", "unpair", QString(), REQUEST_TIMEOUT_MS);
            return FAILED;
        }

        // --- Stage 5: pair challenge (HTTPS — mutual TLS verification) ---
        // This confirms the client cert is recognized by Sunshine over TLS.

        try {
            QString pairChallengeXml = openConnection("https", "pair",
                "devicename=roth&updateState=1&phrase=pairchallenge",
                REQUEST_TIMEOUT_MS);

            NvHTTP::verifyResponseStatus(pairChallengeXml);

            if (NvHTTP::getXmlString(pairChallengeXml, "paired") != "1") {
                Logger::warning("Pairing stage #5: paired != 1");
            }
        } catch (const std::exception& e) {
            // Stage 5 is non-fatal — pairing is already complete after stage 4
            Logger::warning(QString("Pairing stage #5 failed (non-fatal): %1").arg(e.what()));
        }

        // Success — stages 1-4 completed the actual pairing
        outServerCertPem = m_ServerCertPem;
        Logger::info("Pairing completed successfully");
        return PAIRED;

    } catch (const std::exception& e) {
        // If stage 1 already succeeded, the Sunshine session was
        // destroyed after a prior successful stage 4 — that means we're paired
        if (m_Stage1Done) {
            Logger::info("Session already completed — paired successfully");
            outServerCertPem = m_ServerCertPem;
            return PAIRED;
        }

        Logger::warning(QString("Pairing exception: %1").arg(e.what()));
        try {
            openConnection("http", "unpair", QString(), REQUEST_TIMEOUT_MS);
        } catch (...) {}
        return FAILED;
    }
}
