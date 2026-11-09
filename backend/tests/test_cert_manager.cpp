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

#include "test_framework.h"
#include "server/CertManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSslCertificate>
#include <QStandardPaths>

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

static QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/// Whether the cert file at @p certPath was signed for the key at @p keyPath
/// (what a TLS handshake needs; a mismatch is "key values mismatch").
static bool certMatchesKey(const QString& certPath, const QString& keyPath)
{
    const QByteArray certPem = readAll(certPath);
    const QByteArray keyPem = readAll(keyPath);
    BIO* certBio = BIO_new_mem_buf(certPem.constData(), static_cast<int>(certPem.size()));
    BIO* keyBio = BIO_new_mem_buf(keyPem.constData(), static_cast<int>(keyPem.size()));
    X509* cert = certBio ? PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr) : nullptr;
    EVP_PKEY* key = keyBio ? PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr) : nullptr;
    const bool match = cert && key && X509_check_private_key(cert, key) == 1;
    if (cert) X509_free(cert);
    if (key) EVP_PKEY_free(key);
    if (certBio) BIO_free(certBio);
    if (keyBio) BIO_free(keyBio);
    return match;
}

/// Re-sign the cert at @p certPath with the key at @p keyPath after changing it:
/// @p daysLeft sets its expiry from now, and @p dropLanSans leaves it naming
/// localhost only. Stands in for an old cert, or one from another network.
static bool rewriteCert(const QString& certPath, const QString& keyPath, long daysLeft,
                        bool dropLanSans)
{
    const QByteArray certPem = readAll(certPath);
    const QByteArray keyPem = readAll(keyPath);
    BIO* certBio = BIO_new_mem_buf(certPem.constData(), static_cast<int>(certPem.size()));
    BIO* keyBio = BIO_new_mem_buf(keyPem.constData(), static_cast<int>(keyPem.size()));
    X509* cert = certBio ? PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr) : nullptr;
    EVP_PKEY* key = keyBio ? PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr) : nullptr;
    bool ok = false;
    if (cert && key) {
        X509_gmtime_adj(X509_getm_notAfter(cert), daysLeft * 24 * 60 * 60);
        if (dropLanSans) {
            const int at = X509_get_ext_by_NID(cert, NID_subject_alt_name, -1);
            if (at >= 0) X509_EXTENSION_free(X509_delete_ext(cert, at));
            if (X509_EXTENSION* ext =
                    X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, "DNS:localhost")) {
                X509_add_ext(cert, ext, -1);
                X509_EXTENSION_free(ext);
            }
        }
        if (X509_sign(cert, key, EVP_sha256())) {
            BIO* out = BIO_new(BIO_s_mem());
            if (out && PEM_write_bio_X509(out, cert) == 1) {
                char* data = nullptr;
                const long len = BIO_get_mem_data(out, &data);
                QFile f(certPath);
                ok = len > 0 && f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                     f.write(data, len) == len;
            }
            if (out) BIO_free(out);
        }
    }
    if (cert) X509_free(cert);
    if (key) EVP_PKEY_free(key);
    if (certBio) BIO_free(certBio);
    if (keyBio) BIO_free(keyBio);
    return ok;
}

void run_cert_manager_tests()
{
    SECTION("CertManager: local cert keeps its key across restarts");

    // The runner's own AppData (main() names the app), emptied before and after.
    const QString certDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cert/";
    const QString certPath = certDir + "local-cert.pem";
    const QString keyPath = certDir + "local-key.pem";
    QDir(certDir).removeRecursively();

    // First start: a new key and a cert signed with it.
    CertManager first;
    first.ensureLocalSslConfig();
    const QByteArray key1 = readAll(keyPath);
    const QByteArray cert1 = readAll(certPath);
    CHECK(!key1.isEmpty());
    CHECK(!cert1.isEmpty());
    CHECK(certMatchesKey(certPath, keyPath));
    CHECK(!first.localConfig().localCertificate().isNull());
    CHECK(!first.localConfig().privateKey().isNull());

    // A restart: the same key (what a client pinned) and the same cert too — a
    // browser's "proceed anyway" is held for that one certificate, so a new one
    // would put the warning back after every restart.
    CertManager second;
    second.ensureLocalSslConfig();
    CHECK(readAll(keyPath) == key1);
    CHECK(readAll(certPath) == cert1);
    CHECK(certMatchesKey(certPath, keyPath));
    CHECK(!second.localConfig().localCertificate().isNull());
    CHECK(second.localConfig().localCertificate() == first.localConfig().localCertificate());

    // Close to expiry: renewed, with the same key.
    CHECK(rewriteCert(certPath, keyPath, CertManager::kLocalCertMinDaysLeft - 5, false));
    const QByteArray ageing = readAll(certPath);
    CertManager renewed;
    renewed.ensureLocalSslConfig();
    CHECK(readAll(certPath) != ageing);
    CHECK(readAll(keyPath) == key1);
    CHECK(certMatchesKey(certPath, keyPath));
    CHECK(renewed.localConfig().localCertificate().expiryDate() >
          QDateTime::currentDateTimeUtc().addDays(300));

    // Still valid for long but no longer naming this machine's addresses (a new
    // network, or a cert from an older build): renewed as well.
    CHECK(rewriteCert(certPath, keyPath, 200, true));
    const QByteArray elsewhere = readAll(certPath);
    CertManager moved;
    moved.ensureLocalSslConfig();
    CHECK(readAll(certPath) != elsewhere);
    CHECK(readAll(keyPath) == key1);
    CHECK(moved.localConfig()
              .localCertificate()
              .subjectAlternativeNames()
              .values(QSsl::IpAddressEntry)
              .contains(QStringLiteral("127.0.0.1")));

    // A valid cert left as it is when nothing changed since: kept again.
    const QByteArray settled = readAll(certPath);
    CertManager again;
    again.ensureLocalSslConfig();
    CHECK(readAll(certPath) == settled);

    // A key file that cannot be read: a fresh key, never a server without TLS.
    {
        QFile f(keyPath);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("not a key");
    }
    CertManager third;
    third.ensureLocalSslConfig();
    const QByteArray key3 = readAll(keyPath);
    CHECK(!key3.isEmpty());
    CHECK(key3 != key1);
    CHECK(certMatchesKey(certPath, keyPath));
    CHECK(!third.localConfig().privateKey().isNull());

    // The key file gone (the way to rotate it): a fresh key as well.
    QFile::remove(keyPath);
    CertManager fourth;
    fourth.ensureLocalSslConfig();
    CHECK(!readAll(keyPath).isEmpty());
    CHECK(readAll(keyPath) != key3);
    CHECK(certMatchesKey(certPath, keyPath));

    QDir(certDir).removeRecursively();
}
