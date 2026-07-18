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

#include "NvPairingManager.h"
#include "IdentityManager.h"
#include "NvHTTP.h"
#include "common/Logger.h"

#include <QUrl>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QUuid>
#include <QCryptographicHash>
#include <stdexcept>
#include <utility>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/bio.h>

#define REQUEST_TIMEOUT_MS 5000
#define PAIRING_PIN_WAIT_MS 60000 // Stage 1 blocks until user enters PIN in Sunshine

NvPairingManager::NvPairingManager(const QString& appVersion, const QString& host, quint16 httpPort,
                                   quint16 httpsPort, int identityIndex)
    : m_ServerVersion(appVersion.toUtf8())
    , m_Host(host)
    , m_HttpPort(httpPort)
    , m_HttpsPort(httpsPort)
    , m_IdentityIndex(identityIndex)
{
    // Load client cert
    QByteArray certPem = IdentityManager::get(identityIndex)->getCertificate();
    BIO* bio = BIO_new_mem_buf(certPem.data(), certPem.size());
    m_Cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!m_Cert) throw std::runtime_error("Unable to load client certificate");

    // Load private key
    QByteArray keyPem = IdentityManager::get(identityIndex)->getPrivateKey();
    bio = BIO_new_mem_buf(keyPem.data(), keyPem.size());
    m_PrivateKey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!m_PrivateKey) throw std::runtime_error("Unable to load private key");

    // Determine server generation
    QVector<int> quad = NvHTTP::parseQuad(appVersion);
    if (!quad.isEmpty()) m_ServerMajorVersion = quad[0];

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
    if (m_Cert) X509_free(m_Cert);
    if (m_PrivateKey) EVP_PKEY_free(m_PrivateKey);
    delete m_Nam;
}

// --- Asynchronous HTTP request ---

void NvPairingManager::openConnection(const QString& scheme, const QString& command,
                                      const QString& arguments, int timeoutMs,
                                      std::function<void(const QString&, const QString&)> cb)
{
    QUrl url;
    url.setScheme(scheme);
    url.setHost(m_Host);
    url.setPort(scheme == "https" ? m_HttpsPort : m_HttpPort);
    url.setPath("/" + command);

    QString query = "uniqueid=" + IdentityManager::get(m_IdentityIndex)->getUniqueId() +
                    "&uuid=" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!arguments.isEmpty()) query += "&" + arguments;
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "MoonlightWeb/0.1");
    // On timeout the reply finishes with OperationCanceledError — surfaced to
    // `cb` as an error string, no separate timer needed.
    req.setTransferTimeout(timeoutMs > 0 ? timeoutMs : REQUEST_TIMEOUT_MS);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

    if (scheme == "https") {
        // Mutual TLS: present client certificate as expected by Sunshine
        auto* identity = IdentityManager::get(m_IdentityIndex);
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(QSslCertificate(identity->getCertificate(), QSsl::Pem));
        ssl.setPrivateKey(QSslKey(identity->getPrivateKey(), QSsl::Rsa, QSsl::Pem));
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(ssl);
    }

    QNetworkReply* reply = m_Nam->get(req);

    // Ignore SSL errors (self-signed cert) — scoped to this reply.
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });

    QObject::connect(
        reply, &QNetworkReply::finished, reply, [reply, scheme, command, cb = std::move(cb)]() {
            QString body;
            QString error;
            if (reply->error() != QNetworkReply::NoError) {
                error = reply->errorString();
                Logger::warning(
                    QString("Pairing request failed: %1 %2 → %3").arg(scheme, command, error));
            } else {
                body = QString::fromUtf8(reply->readAll());
            }
            reply->deleteLater();
            cb(body, error);
        });
}

void NvPairingManager::unpair()
{
    openConnection("http", "unpair", QString(), REQUEST_TIMEOUT_MS,
                   [](const QString&, const QString&) {});
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
    EVP_EncryptInit(cipher, EVP_aes_128_ecb(), reinterpret_cast<const unsigned char*>(key.data()),
                    nullptr);
    EVP_CIPHER_CTX_set_padding(cipher, 0);

    int ciphertextLen = 0;
    EVP_EncryptUpdate(cipher, reinterpret_cast<unsigned char*>(ciphertext.data()), &ciphertextLen,
                      reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size());

    EVP_CIPHER_CTX_free(cipher);
    return ciphertext;
}

QByteArray NvPairingManager::decrypt(const QByteArray& ciphertext, const QByteArray& key)
{
    QByteArray plaintext(ciphertext.size(), 0);

    EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();
    EVP_DecryptInit(cipher, EVP_aes_128_ecb(), reinterpret_cast<const unsigned char*>(key.data()),
                    nullptr);
    EVP_CIPHER_CTX_set_padding(cipher, 0);

    int plaintextLen = 0;
    EVP_DecryptUpdate(cipher, reinterpret_cast<unsigned char*>(plaintext.data()), &plaintextLen,
                      reinterpret_cast<const unsigned char*>(ciphertext.data()), ciphertext.size());

    EVP_CIPHER_CTX_free(cipher);
    return plaintext;
}

QByteArray NvPairingManager::getSignatureFromCert(X509* cert)
{
    const ASN1_BIT_STRING* asnSignature;
    X509_get0_signature(&asnSignature, nullptr, cert);

    return QByteArray(reinterpret_cast<const char*>(ASN1_STRING_get0_data(asnSignature)),
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

bool NvPairingManager::verifySignature(const QByteArray& data, const QByteArray& signature,
                                       const QByteArray& serverCertificate)
{
    BIO* bio = BIO_new_mem_buf(serverCertificate.data(), serverCertificate.size());
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) return false;

    EVP_PKEY* pubKey = X509_get_pubkey(cert);
    if (!pubKey) {
        X509_free(cert);
        return false;
    }

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pubKey);
    EVP_DigestVerifyUpdate(mdctx, data.data(), data.size());
    int result = EVP_DigestVerifyFinal(
        mdctx, reinterpret_cast<unsigned char*>(const_cast<char*>(signature.data())),
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
    EVP_DigestSignUpdate(ctx, reinterpret_cast<unsigned char*>(const_cast<char*>(message.data())),
                         message.size());

    size_t signatureLength = 0;
    EVP_DigestSignFinal(ctx, nullptr, &signatureLength);

    QByteArray signature(static_cast<int>(signatureLength), 0);
    EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(signature.data()), &signatureLength);

    EVP_MD_CTX_free(ctx);
    return signature;
}

// --- Pairing protocol ---

void NvPairingManager::initiatePairing(std::function<void(InitResult)> cb)
{
    if (m_Stage1Done) {
        cb(INIT_OK);
        return;
    }

    m_Salt = generateRandomBytes(16);

    QString args = "devicename=roth&updateState=1&phrase=getservercert&salt=" + m_Salt.toHex() +
                   "&clientcert=" + IdentityManager::get(m_IdentityIndex)->getCertificate().toHex();

    openConnection(
        "http", "pair", args, PAIRING_PIN_WAIT_MS,
        [this, cb = std::move(cb)](const QString& response, const QString& error) {
            if (!error.isEmpty()) {
                Logger::warning(QString("Pairing stage #1 failed: %1").arg(error));
                cb(INIT_FAILED);
                return;
            }

            try {
                NvHTTP::verifyResponseStatus(response);

                if (NvHTTP::getXmlString(response, "paired") != "1") {
                    Logger::warning("Pairing stage #1 failed: paired != 1");
                    cb(INIT_FAILED);
                    return;
                }

                QByteArray plainCert = NvHTTP::getXmlStringFromHex(response, "plaincert");
                if (plainCert.isEmpty()) {
                    Logger::warning("Server likely already pairing — no plaincert received");
                    unpair();
                    cb(INIT_ALREADY_IN_PROGRESS);
                    return;
                }

                m_ServerCertPem = plainCert;
                m_Stage1Done = true;
                Logger::info("Pairing stage #1 OK — received server certificate");
                cb(INIT_OK);
            } catch (const std::exception& e) {
                Logger::warning(QString("Pairing stage #1 exception: %1").arg(e.what()));
                cb(INIT_FAILED);
            }
        });
}

void NvPairingManager::completePairing(const QString& pin,
                                       std::function<void(PairState, const QByteArray&)> cb)
{
    QCryptographicHash::Algorithm hashAlgo =
        m_ServerMajorVersion >= 7 ? QCryptographicHash::Sha256 : QCryptographicHash::Sha1;

    QByteArray aesKey = QCryptographicHash::hash(saltPin(m_Salt, pin), hashAlgo);
    aesKey.truncate(16);

    QByteArray randomChallenge = generateRandomBytes(16);
    QByteArray encryptedChallenge = encrypt(randomChallenge, aesKey);

    // Preserved from the original synchronous flow: a transport error or an
    // unexpected parse failure *after* stage 1 succeeded means Sunshine most
    // likely tore the pairing session down following an earlier successful
    // handshake — treat the host as paired. The subsequent app-list HTTPS fetch
    // re-validates the client cert and resets the pair state if this was wrong.
    auto assumePaired = [this, cb](const QString& why) {
        Logger::info(QString("Session already completed — paired successfully (%1)").arg(why));
        cb(PAIRED, m_ServerCertPem);
    };

    // --- Stage 2: client challenge ---
    openConnection(
        "http", "pair",
        "devicename=roth&updateState=1&clientchallenge=" + encryptedChallenge.toHex(),
        REQUEST_TIMEOUT_MS,
        [this, cb, aesKey, randomChallenge, hashAlgo, assumePaired](const QString& challengeXml,
                                                                    const QString& error) {
            if (!error.isEmpty()) {
                assumePaired(error);
                return;
            }
            try {
                NvHTTP::verifyResponseStatus(challengeXml);

                if (NvHTTP::getXmlString(challengeXml, "paired") != "1") {
                    // PIN not yet entered in Sunshine — retryable
                    Logger::warning("Pairing stage #2: PIN not accepted yet");
                    cb(PIN_WRONG, QByteArray());
                    return;
                }

                QByteArray challengeResponseData =
                    decrypt(NvHTTP::getXmlStringFromHex(challengeXml, "challengeresponse"), aesKey);

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
                openConnection(
                    "http", "pair",
                    "devicename=roth&updateState=1&serverchallengeresp=" +
                        encryptedChallengeResponseHash.toHex(),
                    REQUEST_TIMEOUT_MS,
                    [this, cb, randomChallenge, clientSecretData, serverResponse, hashAlgo,
                     assumePaired](const QString& respXml, const QString& error) {
                        if (!error.isEmpty()) {
                            assumePaired(error);
                            return;
                        }
                        try {
                            NvHTTP::verifyResponseStatus(respXml);

                            if (NvHTTP::getXmlString(respXml, "paired") != "1") {
                                Logger::warning("Pairing stage #3 failed");
                                cb(PIN_WRONG, QByteArray());
                                return;
                            }

                            QByteArray pairingSecret =
                                NvHTTP::getXmlStringFromHex(respXml, "pairingsecret");
                            QByteArray serverSecret = pairingSecret.left(16);
                            QByteArray serverSignature = pairingSecret.mid(16);

                            // Verify server signature (anti-MITM)
                            if (!verifySignature(serverSecret, serverSignature, m_ServerCertPem)) {
                                Logger::warning(
                                    "MITM detected — server signature verification failed");
                                unpair();
                                cb(FAILED, QByteArray());
                                return;
                            }

                            // Verify PIN
                            QByteArray expectedResponseData;
                            expectedResponseData.append(randomChallenge);
                            expectedResponseData.append(getSignatureFromPemCert(m_ServerCertPem));
                            expectedResponseData.append(serverSecret);

                            if (QCryptographicHash::hash(expectedResponseData, hashAlgo) !=
                                serverResponse) {
                                Logger::warning("Incorrect PIN");
                                unpair();
                                cb(PIN_WRONG, QByteArray());
                                return;
                            }

                            // --- Stage 4: client pairing secret ---
                            QByteArray clientPairingSecret;
                            clientPairingSecret.append(clientSecretData);
                            clientPairingSecret.append(signMessage(clientSecretData));

                            openConnection(
                                "http", "pair",
                                "devicename=roth&updateState=1&clientpairingsecret=" +
                                    clientPairingSecret.toHex(),
                                REQUEST_TIMEOUT_MS,
                                [this, cb, assumePaired](const QString& secretRespXml,
                                                         const QString& error) {
                                    if (!error.isEmpty()) {
                                        assumePaired(error);
                                        return;
                                    }
                                    try {
                                        NvHTTP::verifyResponseStatus(secretRespXml);

                                        if (NvHTTP::getXmlString(secretRespXml, "paired") != "1") {
                                            Logger::warning(
                                                QString("Pairing stage #4 failed — response='%1'")
                                                    .arg(secretRespXml));
                                            unpair();
                                            cb(FAILED, QByteArray());
                                            return;
                                        }

                                        // --- Stage 5: pair challenge (HTTPS, non-fatal) ---
                                        // Confirms the client cert is recognized over mutual TLS;
                                        // pairing is already complete after stage 4.
                                        openConnection(
                                            "https", "pair",
                                            "devicename=roth&updateState=1&phrase=pairchallenge",
                                            REQUEST_TIMEOUT_MS,
                                            [this, cb](const QString& pairChallengeXml,
                                                       const QString& error) {
                                                if (!error.isEmpty()) {
                                                    Logger::warning(
                                                        QString("Pairing stage #5 failed "
                                                                "(non-fatal): %1")
                                                            .arg(error));
                                                } else {
                                                    try {
                                                        NvHTTP::verifyResponseStatus(
                                                            pairChallengeXml);
                                                        if (NvHTTP::getXmlString(pairChallengeXml,
                                                                                 "paired") != "1")
                                                            Logger::warning(
                                                                "Pairing stage #5: paired != 1");
                                                    } catch (const std::exception& e) {
                                                        Logger::warning(
                                                            QString("Pairing stage #5 failed "
                                                                    "(non-fatal): %1")
                                                                .arg(e.what()));
                                                    }
                                                }
                                                Logger::info("Pairing completed successfully");
                                                cb(PAIRED, m_ServerCertPem);
                                            });
                                    } catch (const std::exception& e) {
                                        assumePaired(e.what());
                                    }
                                });
                        } catch (const std::exception& e) {
                            assumePaired(e.what());
                        }
                    });
            } catch (const std::exception& e) {
                assumePaired(e.what());
            }
        });
}
