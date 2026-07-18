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

#include <QString>
#include <QByteArray>
#include <QNetworkAccessManager>

#include <functional>

struct x509_st;
struct evp_pkey_st;
typedef struct x509_st X509;
typedef struct evp_pkey_st EVP_PKEY;

// Drives the Sunshine/GameStream challenge-response pairing handshake.
//
// Fully asynchronous: every HTTP request is issued via QNetworkAccessManager
// and resolved through a callback, so no method ever spins a nested Qt event
// loop. This matters because pairing is driven from the single-threaded HTTP
// server — a nested loop there would re-enter request dispatch and free objects
// still held on the stack (use-after-free). The owner must keep the instance
// alive until the final callback fires.
class NvPairingManager
{
public:
    enum PairState
    {
        PAIRED,
        PIN_WRONG,
        FAILED,
        ALREADY_IN_PROGRESS
    };

    enum InitResult
    {
        INIT_OK,
        INIT_FAILED,
        INIT_ALREADY_IN_PROGRESS
    };

    /// identityIndex selects the client identity to pair (0 = primary,
    /// 1 = secondary for the dual-stream standby slot).
    NvPairingManager(const QString& appVersion, const QString& host, quint16 httpPort,
                     quint16 httpsPort, int identityIndex = 0);
    ~NvPairingManager();

    // Owns raw OpenSSL handles (X509*/EVP_PKEY*) and a QNetworkAccessManager*
    // freed in the destructor — non-copyable to avoid double-free.
    NvPairingManager(const NvPairingManager&) = delete;
    NvPairingManager& operator=(const NvPairingManager&) = delete;

    // Stage 1 (async): send salt + client cert → server displays PIN.
    // The getservercert request blocks server-side up to 60s until the user
    // enters the PIN in Sunshine; `cb` fires when the request resolves. Fully
    // event-driven — no nested Qt event loop (see class comment).
    void initiatePairing(std::function<void(InitResult)> cb);

    // Stages 2-5 (async): complete pairing with the user-provided PIN.
    // `cb` receives the final state and, on PAIRED, the server certificate PEM.
    void completePairing(const QString& pin,
                         std::function<void(PairState, const QByteArray& serverCertPem)> cb);

private:
    QByteArray generateRandomBytes(int length);
    QByteArray saltPin(const QByteArray& salt, const QString& pin);
    QByteArray encrypt(const QByteArray& plaintext, const QByteArray& key);
    QByteArray decrypt(const QByteArray& ciphertext, const QByteArray& key);
    QByteArray getSignatureFromCert(X509* cert);
    QByteArray getSignatureFromPemCert(const QByteArray& certificate);
    bool verifySignature(const QByteArray& data, const QByteArray& signature,
                         const QByteArray& serverCertificate);
    QByteArray signMessage(const QByteArray& message);

    // Asynchronous HTTP request. Invokes cb(body, error) when the reply
    // finishes; `error` is empty on success, otherwise the reply error string
    // (and `body` is empty). Never blocks the calling thread.
    void openConnection(const QString& scheme, const QString& command, const QString& arguments,
                        int timeoutMs,
                        std::function<void(const QString& body, const QString& error)> cb);

    // Fire-and-forget unpair (used to abort a half-completed handshake).
    void unpair();

    QByteArray m_ServerVersion; // "7.1.431" from serverinfo
    QString m_Host;
    quint16 m_HttpPort;
    quint16 m_HttpsPort;

    X509* m_Cert = nullptr;
    EVP_PKEY* m_PrivateKey = nullptr;
    QNetworkAccessManager* m_Nam = nullptr;

    // State preserved between initiatePairing() and completePairing()
    QByteArray m_Salt;          // 16 random bytes from stage 1
    QByteArray m_ServerCertPem; // server cert PEM received in stage 1
    bool m_Stage1Done = false;

    int m_HashLength = 32;
    int m_ServerMajorVersion = 7;
    int m_IdentityIndex = 0;
};
