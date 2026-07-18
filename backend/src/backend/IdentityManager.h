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

class IdentityManager
{
public:
    // PEM certificate and private key bytes
    QByteArray getCertificate();
    QByteArray getPrivateKey();

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

    QByteArray m_CachedPemCert;
    QByteArray m_CachedPrivateKey;
    QString m_CachedUniqueId;

    X509* m_Cert = nullptr;
    EVP_PKEY* m_Key = nullptr;
    bool m_CredentialsLoaded = false;

    static IdentityManager* s_Instance;
};
