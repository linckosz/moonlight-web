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

// Forward declarations — OpenSSL types
struct x509_st;
struct evp_pkey_st;
typedef struct x509_st X509;
typedef struct evp_pkey_st EVP_PKEY;

/// One client identity: the PEM pair presented to a GameStream host.
struct ClientIdentity
{
    QByteArray certPem;
    QByteArray keyPem;

    bool isValid() const { return !certPem.isEmpty() && !keyPem.isEmpty(); }
};

class IdentityManager
{
public:
    // PEM certificate and private key bytes
    QByteArray getCertificate();
    QByteArray getPrivateKey();

    /// A distinct identity per seat, minted on first use and persisted.
    ///
    /// Wolf resolves a client from its TLS certificate and keys running
    /// sessions on it, so two seats sharing one certificate share one session —
    /// player B would take over player A's screen. A seat therefore needs its
    /// own certificate, and each has to be paired separately (cheap, since
    /// pairing needs no human).
    ///
    /// The default identity is deliberately left alone. Sunshine keys sessions
    /// by uniqueid and every browser already has its own, so giving plain hosts
    /// per-seat certificates would force each browser to pair again.
    ///
    /// An empty seatId returns the default identity.
    ClientIdentity identityForSeat(const QString& seatId);

    // Unique client ID, persisted in QSettings
    QString getUniqueId();

    // OpenSSL parsed structs (caller must NOT free)
    X509* getCertStruct();
    EVP_PKEY* getKeyStruct();

    static IdentityManager* get();

private:
    IdentityManager();
    ~IdentityManager();

    void createCredentials();
    void loadOrGenerate();

    /// Mint a fresh self-signed GameStream client identity. Pure: it persists
    /// nothing and touches no member, so both the default identity and per-seat
    /// ones are produced by the same code.
    static ClientIdentity generateIdentity();

    QByteArray m_CachedPemCert;
    QByteArray m_CachedPrivateKey;
    QString m_CachedUniqueId;

    X509* m_Cert = nullptr;
    EVP_PKEY* m_Key = nullptr;
    bool m_CredentialsLoaded = false;

    static IdentityManager* s_Instance;
};
