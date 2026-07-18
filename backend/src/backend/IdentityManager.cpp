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

#include "IdentityManager.h"
#include "common/Logger.h"

#include <QSettings>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/rand.h>
#include <openssl/bio.h>

#define SER_UNIQUEID "uniqueid"
#define SER_CERT "certificate"
#define SER_KEY "key"

IdentityManager* IdentityManager::s_Instances[2] = {nullptr, nullptr};

IdentityManager* IdentityManager::get(int index)
{
    if (index < 0 || index > 1) index = 0;
    if (!s_Instances[index]) s_Instances[index] = new IdentityManager(index);
    return s_Instances[index];
}

// Storage-key suffix: the primary identity keeps the historical unsuffixed
// keys; the secondary (dual-stream standby) appends "2".
static QString keyFor(const char* base, int index)
{
    return index == 0 ? QString::fromLatin1(base) : QString::fromLatin1(base) + QLatin1Char('2');
}

IdentityManager::IdentityManager(int index)
    : m_Index(index)
{
    loadOrGenerate();
}

IdentityManager::~IdentityManager()
{
    if (m_Cert) X509_free(m_Cert);
    if (m_Key) EVP_PKEY_free(m_Key);
}

void IdentityManager::loadOrGenerate()
{
    QSettings settings;

    m_CachedPemCert = settings.value(keyFor(SER_CERT, m_Index)).toByteArray();
    m_CachedPrivateKey = settings.value(keyFor(SER_KEY, m_Index)).toByteArray();
    m_CachedUniqueId = settings.value(keyFor(SER_UNIQUEID, m_Index)).toString();

    if (m_CachedPemCert.isEmpty() || m_CachedPrivateKey.isEmpty()) {
        Logger::info("No existing credentials found, generating new identity...");
        createCredentials();
        return;
    }

    // Validate stored credentials
    BIO* bio = BIO_new_mem_buf(m_CachedPemCert.data(), m_CachedPemCert.size());
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!cert) {
        Logger::warning("Stored certificate is invalid, regenerating...");
        createCredentials();
        return;
    }
    X509_free(cert);

    bio = BIO_new_mem_buf(m_CachedPrivateKey.data(), m_CachedPrivateKey.size());
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!key) {
        Logger::warning("Stored private key is invalid, regenerating...");
        createCredentials();
        return;
    }
    EVP_PKEY_free(key);

    m_CredentialsLoaded = true;
    Logger::info("Identity credentials loaded from settings");

    // Always use the fixed unique ID, even if previously migrated. The primary
    // identity uses the Moonlight common ID; the secondary uses its own fixed
    // ID so the two never collide in Sunshine's per-uniqueid session keying.
    const QString fixedId =
        m_Index == 0 ? QStringLiteral("0123456789ABCDEF") : QStringLiteral("0123456789ABCDF1");
    if (m_CachedUniqueId != fixedId) {
        m_CachedUniqueId = fixedId;
        settings.setValue(keyFor(SER_UNIQUEID, m_Index), m_CachedUniqueId);
        Logger::info("Using fixed unique ID: " + m_CachedUniqueId);
    }
}

void IdentityManager::createCredentials()
{
    // Generate RSA 2048-bit keypair
    EVP_PKEY* pk = EVP_RSA_gen(2048);
    if (!pk) throw std::runtime_error("RSA key generation failed");

    // Create X.509 certificate
    X509* cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(pk);
        throw std::runtime_error("X509_new failed");
    }

    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 0);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24 * 365 * 20); // 20 yrs

    X509_set_pubkey(cert, pk);

    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<unsigned char*>(const_cast<char*>("NVIDIA GameStream Client")), -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);

    X509_sign(cert, pk, EVP_sha256());

    // Export private key PEM
    BIO* biokey = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(biokey, pk, nullptr, nullptr, 0, nullptr, nullptr);

    BUF_MEM* mem;
    BIO_get_mem_ptr(biokey, &mem);
    m_CachedPrivateKey = QByteArray(mem->data, static_cast<int>(mem->length));
    BIO_free(biokey);

    // Export certificate PEM
    BIO* biocert = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(biocert, cert);

    BIO_get_mem_ptr(biocert, &mem);
    m_CachedPemCert = QByteArray(mem->data, static_cast<int>(mem->length));
    BIO_free(biocert);

    X509_free(cert);
    EVP_PKEY_free(pk);

    // Fixed unique IDs: Moonlight common ID for the primary identity (enables
    // cross-client session management), a distinct fixed ID for the secondary.
    m_CachedUniqueId =
        m_Index == 0 ? QStringLiteral("0123456789ABCDEF") : QStringLiteral("0123456789ABCDF1");

    // Persist
    QSettings settings;
    settings.setValue(keyFor(SER_CERT, m_Index), m_CachedPemCert);
    settings.setValue(keyFor(SER_KEY, m_Index), m_CachedPrivateKey);
    settings.setValue(keyFor(SER_UNIQUEID, m_Index), m_CachedUniqueId);

    m_CredentialsLoaded = true;
    Logger::info("New identity credentials generated and persisted");
}

QByteArray IdentityManager::getCertificate()
{
    return m_CachedPemCert;
}

QByteArray IdentityManager::getPrivateKey()
{
    return m_CachedPrivateKey;
}

QString IdentityManager::getUniqueId()
{
    return m_CachedUniqueId;
}

X509* IdentityManager::getCertStruct()
{
    if (!m_Cert) {
        BIO* bio = BIO_new_mem_buf(m_CachedPemCert.data(), m_CachedPemCert.size());
        m_Cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
    }
    return m_Cert;
}

EVP_PKEY* IdentityManager::getKeyStruct()
{
    if (!m_Key) {
        BIO* bio = BIO_new_mem_buf(m_CachedPrivateKey.data(), m_CachedPrivateKey.size());
        m_Key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
    }
    return m_Key;
}
