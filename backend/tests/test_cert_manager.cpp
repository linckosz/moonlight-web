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
#include <QTemporaryDir>

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

/// Write a fresh EC key at @p keyPath and a certificate for @p cn signed with
/// it at @p certPath.
static bool makePair(const QString& certPath, const QString& keyPath, const char* cn)
{
    EVP_PKEY* key = EVP_EC_gen("P-256");
    X509* cert = X509_new();
    bool ok = false;
    if (key && cert) {
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 30L * 24 * 60 * 60);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
        X509_set_issuer_name(cert, name);
        if (X509_sign(cert, key, EVP_sha256())) {
            BIO* c = BIO_new(BIO_s_mem());
            BIO* k = BIO_new(BIO_s_mem());
            if (c && k && PEM_write_bio_X509(c, cert) == 1 &&
                PEM_write_bio_PrivateKey(k, key, nullptr, nullptr, 0, nullptr, nullptr) == 1) {
                char* data = nullptr;
                long len = BIO_get_mem_data(c, &data);
                QFile cf(certPath);
                ok = cf.open(QIODevice::WriteOnly) && cf.write(data, len) == len;
                len = BIO_get_mem_data(k, &data);
                QFile kf(keyPath);
                ok = ok && kf.open(QIODevice::WriteOnly) && kf.write(data, len) == len;
            }
            if (c) BIO_free(c);
            if (k) BIO_free(k);
        }
    }
    if (cert) X509_free(cert);
    if (key) EVP_PKEY_free(key);
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

    SECTION("CertManager: cert_pem / cert_key sources, and the key that goes with them");
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        const QString dir = tmp.path();
        const QString unset = QStringLiteral("MW_SFX_TEST_UNSET");
        qunsetenv("MW_SFX_TEST_UNSET");
        QList<QSslCertificate> chain;
        QSslKey key;
        QString described;

        // A and its key; B, another pair, in a folder of its own.
        QDir(dir).mkpath("a");
        QDir(dir).mkpath("b");
        const QString certA = dir + "/a/fullchain.pem";
        const QString keyA = dir + "/a/held-elsewhere.txt";
        const QString certB = dir + "/b/cert.pem";
        const QString keyB = dir + "/b/key.pem";
        CHECK(makePair(certA, keyA, "a.example"));
        CHECK(makePair(certB, keyB, "b.example"));
        const QSslCertificate a = QSslCertificate::fromPath(certA).value(0);
        CHECK(!a.isNull());

        // cert_key as a file path: its key is A's, loaded.
        CHECK(CertManager::pairFromSources(certA, keyA, QString(), &chain, &key, &described));
        CHECK(chain.value(0) == a);
        CHECK(!key.isNull());
        // cert_key names another pair's key: refused, never served.
        CHECK(!CertManager::pairFromSources(certA, keyB, QString(), &chain, &key));
        // The certificate must cover the domain when one is set.
        CHECK(!CertManager::pairFromSources(certA, keyA, "other.example", &chain, &key));
        CHECK(CertManager::pairFromSources(certA, keyA, "a.example", &chain, &key));

        // No candidate beside the certificate: nothing loaded.
        CHECK(!CertManager::pairFromSources(certA, unset, QString(), &chain, &key));
        // A key beside it that is not its own (key.pem): skipped, nothing loaded.
        CHECK(QFile::copy(keyB, dir + "/a/key.pem"));
        CHECK(!CertManager::pairFromSources(certA, unset, QString(), &chain, &key));
        // <base name>.key that is its own: found after the other one.
        CHECK(QFile::copy(keyA, dir + "/a/fullchain.key"));
        CHECK(CertManager::pairFromSources(certA, unset, QString(), &chain, &key, &described));
        CHECK(chain.value(0) == a);
        CHECK(described.endsWith("fullchain.key"));
        // privkey.pem comes first.
        CHECK(QFile::copy(keyA, dir + "/a/privkey.pem"));
        CHECK(CertManager::pairFromSources(certA, unset, QString(), &chain, &key, &described));
        CHECK(described.endsWith("privkey.pem"));
        // Never another name, nor another folder: B's cert finds only B's key.pem.
        CHECK(CertManager::pairFromSources(certB, unset, QString(), &chain, &key, &described));
        CHECK(described.endsWith("b/key.pem"));
        QFile::remove(keyB);
        CHECK(!CertManager::pairFromSources(certB, unset, QString(), &chain, &key));

        // Variables that hold a path to a PEM file.
        qputenv("MW_SFX_TEST_CERT", certA.toLocal8Bit());
        qputenv("MW_SFX_TEST_KEY", keyA.toLocal8Bit());
        CHECK(CertManager::pairFromSources("MW_SFX_TEST_CERT", "MW_SFX_TEST_KEY", QString(), &chain,
                                           &key, &described));
        CHECK(chain.value(0) == a);
        // Variables that hold the PEM itself.
        qputenv("MW_SFX_TEST_CERT", readAll(certA));
        qputenv("MW_SFX_TEST_KEY", readAll(keyA));
        CHECK(CertManager::pairFromSources("MW_SFX_TEST_CERT", "MW_SFX_TEST_KEY", QString(), &chain,
                                           &key));
        CHECK(chain.value(0) == a);
        // A variable that is neither PEM nor a regular file: nothing loaded.
        qputenv("MW_SFX_TEST_KEY", (dir + "/a/missing.pem").toLocal8Bit());
        CHECK(!CertManager::pairFromSources("MW_SFX_TEST_CERT", "MW_SFX_TEST_KEY", QString(),
                                            &chain, &key));
        qputenv("MW_SFX_TEST_KEY", (dir + "/a").toLocal8Bit());
        CHECK(!CertManager::pairFromSources("MW_SFX_TEST_CERT", "MW_SFX_TEST_KEY", QString(),
                                            &chain, &key));
        // A file over 1 MiB named by a variable is not read.
        {
            QFile big(dir + "/a/big.pem");
            CHECK(big.open(QIODevice::WriteOnly));
            QByteArray pem = readAll(keyA);
            big.write(pem);
            big.write(QByteArray(1024 * 1024, '\n'));
        }
        qputenv("MW_SFX_TEST_KEY", (dir + "/a/big.pem").toLocal8Bit());
        CHECK(!CertManager::pairFromSources("MW_SFX_TEST_CERT", "MW_SFX_TEST_KEY", QString(),
                                            &chain, &key));
        qunsetenv("MW_SFX_TEST_CERT");
        qunsetenv("MW_SFX_TEST_KEY");
    }

    SECTION("CertManager: the build's pair is used only as a pair");
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        const QString dir = tmp.path();
        qunsetenv("MW_SFX_TEST_UNSET");
        QDir(dir).mkpath("user");
        QDir(dir).mkpath("build");
        const QString certU = dir + "/user/fullchain.pem";
        const QString keyU = dir + "/user/privkey.pem";
        const QString certBuild = dir + "/build/cert.pem";
        const QString keyBuild = dir + "/build/key.pem";
        CHECK(makePair(certU, keyU, "user.example"));
        CHECK(makePair(certBuild, keyBuild, "build.example"));
        const QSslCertificate user = QSslCertificate::fromPath(certU).value(0);
        const QSslCertificate built = QSslCertificate::fromPath(certBuild).value(0);
        CertManager::BuiltInPair pair;
        pair.certPem = readAll(certBuild);
        pair.keyPem = readAll(keyBuild);
        QList<QSslCertificate> chain;
        QSslKey key;
        QString described;

        // The user's certificate file, no cert_key, the build's key around and
        // the right key beside the certificate: the one beside it is used.
        CHECK(CertManager::pairFromSources(certU, "MW_SFX_TEST_UNSET", QString(), &chain, &key,
                                           &described, pair));
        CHECK(chain.value(0) == user);
        CHECK(described.endsWith("user/privkey.pem"));
        CHECK(key.toPem() != QSslKey(pair.keyPem, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey).toPem());
        // Without the key beside it, the build's key is never tried with it.
        CHECK(QFile::remove(keyU));
        CHECK(!CertManager::pairFromSources(certU, "MW_SFX_TEST_UNSET", QString(), &chain, &key,
                                            nullptr, pair));
        CHECK(makePair(certU, keyU, "user.example"));
        const QSslCertificate user2 = QSslCertificate::fromPath(certU).value(0);

        // No cert_pem: the build's certificate, with the build's key only,
        // whatever cert_key names.
        CHECK(CertManager::pairFromSources("MW_SFX_TEST_UNSET", keyU, QString(), &chain, &key,
                                           &described, pair));
        CHECK(chain.value(0) == built);
        CHECK(chain.value(0) != user2);
        CHECK(described == "cert=the build's certificate, key=the build's key");
        // The build's certificate without its key and the user's key: nothing.
        CertManager::BuiltInPair halfPair;
        halfPair.certPem = pair.certPem;
        CHECK(!CertManager::pairFromSources("MW_SFX_TEST_UNSET", keyU, QString(), &chain, &key,
                                            nullptr, halfPair));
        CHECK(!CertManager::pairFromSources("MW_SFX_TEST_UNSET", keyBuild, QString(), &chain, &key,
                                            nullptr, halfPair));
    }
}
