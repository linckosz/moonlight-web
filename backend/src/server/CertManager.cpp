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

#include "CertManager.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QProcess>
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QStandardPaths>
#include <functional>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

// --- Helpers -------------------------------------------------------------------

/// Generate a self-signed RSA-2048 certificate + private key entirely via
/// libcrypto — no external openssl.exe dependency (CI artifacts run on clean
/// machines without OpenSSL on PATH). `sans` entries use the OpenSSL v3 conf
/// syntax ("DNS:localhost", "IP:127.0.0.1", ...). Writes PEM files.
static bool generateSelfSignedToFiles(const QString& certPath, const QString& keyPath,
                                      const QStringList& sans, const QString& commonName)
{
    bool ok = false;
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    X509* x509 = X509_new();

    do {
        if (!pkey || !x509) break;

        X509_set_version(x509, 2); // v3
        ASN1_INTEGER_set_uint64(X509_get_serialNumber(x509),
                                QRandomGenerator::global()->generate64());
        X509_gmtime_adj(X509_getm_notBefore(x509), 0);
        X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60 * 24 * 365); // 1 year
        X509_set_pubkey(x509, pkey);

        // Subject == issuer (self-signed), CN only.
        QByteArray cn = commonName.toUtf8();
        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(cn.constData()), -1, -1,
                                   0);
        X509_set_issuer_name(x509, name);

        // subjectAltName extension built from the SAN list.
        if (!sans.isEmpty()) {
            QByteArray sanStr = sans.join(',').toUtf8();
            if (X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                                                          sanStr.constData())) {
                X509_add_ext(x509, ext, -1);
                X509_EXTENSION_free(ext);
            }
        }

        if (!X509_sign(x509, pkey, EVP_sha256())) break;

        // Serialize to PEM in memory, then write via QFile (handles unicode paths
        // that BIO_new_file mishandles on Windows).
        auto writePem = [](const QString& path, const std::function<int(BIO*)>& dump) -> bool {
            BIO* bio = BIO_new(BIO_s_mem());
            if (!bio) return false;
            bool wrote = false;
            if (dump(bio) == 1) {
                char* data = nullptr;
                long len = BIO_get_mem_data(bio, &data);
                if (len > 0 && data) {
                    QFile f(path);
                    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                        wrote = (f.write(data, len) == len);
                }
            }
            BIO_free(bio);
            return wrote;
        };

        if (!writePem(keyPath, [pkey](BIO* b) {
                return PEM_write_bio_PrivateKey(b, pkey, nullptr, nullptr, 0, nullptr, nullptr);
            }))
            break;
        if (!writePem(certPath, [x509](BIO* b) { return PEM_write_bio_X509(b, x509); })) break;

        ok = true;
    } while (false);

    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    return ok;
}

/// Enumerate all private/local addresses on this machine (excluding loopback).
/// Covers RFC 1918 IPv4 + IPv6 link-local (fe80::/10) and ULA (fc00::/7).
/// Used to populate SANs in the self-signed certificate so local IP connections
/// do not trigger hostname mismatch warnings.
static QList<QHostAddress> getPrivateLanAddresses()
{
    QList<QHostAddress> result;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp)) continue;
        if (!iface.flags().testFlag(QNetworkInterface::IsRunning)) continue;
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;

        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            QHostAddress addr = entry.ip();
            if (addr.isLoopback()) continue;

            if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
                quint32 ip = addr.toIPv4Address();
                // RFC 1918: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16
                if ((ip & 0xFF000000) == 0x0A000000 || (ip & 0xFFF00000) == 0xAC100000 ||
                    (ip & 0xFFFF0000) == 0xC0A80000) {
                    result << addr;
                }
            } else if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
                // IPv6 link-local (fe80::/10) and unique local / ULA (fc00::/7)
                Q_IPV6ADDR ip6 = addr.toIPv6Address();
                if (ip6[0] == 0xFE && (ip6[1] & 0xC0) == 0x80) // fe80::/10 link-local
                    result << addr;
                else if ((ip6[0] & 0xFE) == 0xFC) // fc00::/7 ULA
                    result << addr;
            }
        }
    }
    return result;
}

// --- CertManager ---------------------------------------------------------------

QString CertManager::findCertDir()
{
    // Build list of root directories to scan
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QStringList candidates = {
        appData + "/cert/",
        QString(CERT_DIR),
        QCoreApplication::applicationDirPath() + "/cert/",
        QCoreApplication::applicationDirPath() + "/../cert/",
        QString(FRONTEND_DIR) + "/../backend/cert/",
    };

    // 2. Domain is set: find a cert whose CN matches the current domain
    if (!m_Domain.isEmpty()) {
        Logger::info("[CERT] Scanning for certificate matching domain: " + m_Domain);
        QString dir = findCertByDomain(m_Domain);
        if (!dir.isEmpty()) return dir;
        Logger::warning("[CERT] No certificate found for domain: " + m_Domain);
    }

    // 3. Fallback: return first valid cert dir (scanned)
    for (const auto& d : candidates) {
        if (!scanCertInDir(d).isEmpty() && !scanKeyInDir(d).isEmpty()) return d;
    }
    return {};
}

QString CertManager::findCertByDomain(const QString& domain)
{
    // Build root candidates (same as findCertDir)
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QStringList candidates = {
        appData + "/cert/",
        QString(CERT_DIR),
        QCoreApplication::applicationDirPath() + "/cert/",
        QCoreApplication::applicationDirPath() + "/../cert/",
        QString(FRONTEND_DIR) + "/../backend/cert/",
    };

    for (const auto& rootDir : candidates) {
        if (!QDir(rootDir).exists()) continue;

        // Recursively scan for PEM files
        QDirIterator it(rootDir, {"*.pem"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString filePath = it.next();
            QString certCn = extractCertCN(filePath);
            if (certCn.isEmpty()) continue;

            if (certCn.compare(domain, Qt::CaseInsensitive) == 0) {
                QDir certDir = QFileInfo(filePath).absoluteDir();
                QString keyPath = scanKeyInDir(certDir.absolutePath());
                if (!keyPath.isEmpty()) {
                    Logger::info(
                        QString("[CERT] Found matching certificate: CN=%1, file=%2, key=%3")
                            .arg(certCn, filePath, keyPath));
                    return certDir.absolutePath();
                }
                Logger::info(QString("[CERT] CN matches but no private key found in %1, skipping")
                                 .arg(certDir.absolutePath()));
            }
        }
    }
    return {};
}

QString CertManager::extractCertCN(const QString& pemPath)
{
    QFile f(pemPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QList<QSslCertificate> certs = QSslCertificate::fromDevice(&f, QSsl::Pem);
    f.close();
    if (certs.isEmpty()) return {};
    QStringList cns = certs.first().subjectInfo(QSslCertificate::CommonName);
    return cns.isEmpty() ? QString() : cns.first();
}

QString CertManager::scanKeyInDir(const QString& dir) const
{
    QDirIterator it(dir, {"*.pem"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString filePath = it.next();

        // Skip ACME account key — it's for ACME account auth, not TLS
        if (filePath.endsWith("account_key.pem")) continue;

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) continue;

        // Read content and check for private key marker before attempting QSslKey
        QByteArray content = file.readAll();
        file.close();
        if (!content.contains("PRIVATE KEY")) continue;

        // Re-open to reset cursor, try RSA private key
        if (!file.open(QIODevice::ReadOnly)) continue;
        QSslKey key(&file, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        file.close();

        if (key.isNull()) {
            // Try EC private key
            if (!file.open(QIODevice::ReadOnly)) continue;
            key = QSslKey(&file, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
            file.close();
        }

        if (!key.isNull()) {
            Logger::info("[CERT] Found private key: file=" + filePath);
            return filePath;
        }
    }
    return {};
}

QString CertManager::scanCertInDir(const QString& dir, const QString& domain) const
{
    QDirIterator it(dir, {"*.pem"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString filePath = it.next();
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) continue;

        QList<QSslCertificate> certs = QSslCertificate::fromDevice(&file, QSsl::Pem);
        file.close();

        if (certs.isEmpty()) continue;

        // If domain is specified, verify CN match
        if (!domain.isEmpty()) {
            QStringList cns = certs.first().subjectInfo(QSslCertificate::CommonName);
            if (cns.isEmpty() || cns.first().compare(domain, Qt::CaseInsensitive) != 0) continue;
        }

        QStringList cns = certs.first().subjectInfo(QSslCertificate::CommonName);
        QString cn = cns.isEmpty() ? "(no CN)" : cns.first();
        Logger::info(QString("[CERT] Found certificate: CN=%1, file=%2").arg(cn, filePath));
        return filePath;
    }
    return {};
}

QSslKey CertManager::loadKeyFromEnv() const
{
    // 1. Runtime env var (overrides everything)
    QByteArray data = qgetenv("MW_CERT_KEY");

    // 2. Build-time embedded key (from .env at project root, via DEFINE)
#ifdef MW_CERT_KEY
    if (data.isEmpty()) data = QByteArray(MW_CERT_KEY);
#endif

    if (data.isEmpty()) return {};

    // Try RSA first, then EC
    QSslKey key(data, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (!key.isNull()) return key;
    return QSslKey(data, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
}

QByteArray CertManager::resolvePemValue(const QString& value)
{
    if (value.isEmpty()) return {};

    // 1. Try env var — value IS the env var name (e.g. "MW_CERT_PEM")
    QByteArray data = qgetenv(value.toUtf8());
    if (!data.isEmpty()) return data;

    // 2. Try file path
    QFile f(value);
    if (f.open(QIODevice::ReadOnly)) return f.readAll();

    return {};
}

bool CertManager::loadCertFiles(const QString& certDir)
{
    // Check if a private key is provided via environment variable.
    // When set, we only need to find a matching certificate (the key
    // is already loaded from env, no file required).
    QSslKey envKey = loadKeyFromEnv();
    bool useEnvKey = !envKey.isNull();

    // Find the best certificate. Priority: root dir > subdirs, CN match > any cert.
    auto findCert = [this](const QString& dir, bool subdirs, const QString& domain) -> QString {
        auto flags = subdirs ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
        QDirIterator it(dir, {"*.pem"}, QDir::Files, flags);
        while (it.hasNext()) {
            QString path = it.next();
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) continue;
            QList<QSslCertificate> certs = QSslCertificate::fromDevice(&f, QSsl::Pem);
            f.close();
            if (certs.isEmpty()) continue;

            if (!domain.isEmpty()) {
                QStringList cns = certs.first().subjectInfo(QSslCertificate::CommonName);
                if (cns.isEmpty() || cns.first().compare(domain, Qt::CaseInsensitive) != 0)
                    continue;
            }

            QStringList cns = certs.first().subjectInfo(QSslCertificate::CommonName);
            QString cn = cns.isEmpty() ? "(no CN)" : cns.first();
            Logger::info(QString("[CERT] Found certificate: CN=%1, file=%2").arg(cn, path));
            return path;
        }
        return {};
    };

    // 1. Root dir, domain-filtered
    QString certPath = findCert(certDir, false, m_Domain);

    // 2. Root dir, any cert
    if (certPath.isEmpty()) certPath = findCert(certDir, false, QString());

    // 3. Subdirectories, domain-filtered
    if (certPath.isEmpty()) certPath = findCert(certDir, true, m_Domain);

    // 4. Subdirectories, any cert
    if (certPath.isEmpty()) certPath = findCert(certDir, true, QString());

    if (certPath.isEmpty()) {
        Logger::warning("No certificate found in " + certDir);
        return false;
    }

    // Load private key. Prefer the key file sitting next to the certificate —
    // it is authoritative for ACME-issued certs (cert + key are written together
    // in the same dir). The embedded env key (MW_CERT_KEY) belongs to ONE
    // specific embedded cert; pairing it with a different scanned cert produces
    // OpenSSL "key values mismatch" at handshake time (e.g. brunoocto key paired
    // with a brunchlee/damian cert). So the env key is only a last resort, used
    // when no file key is found alongside the cert (the embedded-cert deployment).
    QSslKey key;
    QString keySource;

    QFileInfo certFi(certPath);
    QString keyPath = scanKeyInDir(certFi.absolutePath());
    if (!keyPath.isEmpty()) {
        QFile keyFile(keyPath);
        if (keyFile.open(QIODevice::ReadOnly)) {
            key = QSslKey(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
            keyFile.close();
            if (key.isNull() && keyFile.open(QIODevice::ReadOnly)) {
                key = QSslKey(&keyFile, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
                keyFile.close();
            }
            if (!key.isNull()) keySource = keyPath;
        }
    }

    // Fall back to the embedded env key only when no file key was found.
    if (key.isNull() && useEnvKey) {
        key = envKey;
        keySource = "env:MW_CERT_KEY";
    }

    if (key.isNull()) {
        Logger::warning("No usable private key found for cert " + certPath);
        return false;
    }

    // Load cert chain
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open cert file: " + certFile.errorString());
        return false;
    }
    QList<QSslCertificate> chain = QSslCertificate::fromDevice(&certFile, QSsl::Pem);
    certFile.close();

    if (chain.isEmpty() || key.isNull()) {
        Logger::warning("SSL cert chain / key invalid");
        return false;
    }

    m_SslConfig = QSslConfiguration::defaultConfiguration();
    m_SslConfig.setLocalCertificateChain(chain);
    m_SslConfig.setPrivateKey(key);
    m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

    QString cn;
    if (!chain.isEmpty()) {
        QStringList cns = chain.first().subjectInfo(QSslCertificate::CommonName);
        cn = cns.isEmpty() ? "(no CN)" : cns.first();
    }
    Logger::info(
        QString("SSL certificate loaded: CN=%1, cert=%2, key=%3").arg(cn, certPath, keySource));
    return true;
}

bool CertManager::loadCertFilesExplicit(const QString& certFilePath)
{
    QFileInfo fi(certFilePath);

    // Load private key: env var first, then file scan
    QSslKey key = loadKeyFromEnv();
    QString keySource;

    if (!key.isNull()) {
        keySource = "env:MW_CERT_KEY";
    } else {
        QString keyPath = scanKeyInDir(fi.absolutePath());
        if (keyPath.isEmpty()) {
            Logger::warning("[CERT] No private key found in " + fi.absolutePath());
            return false;
        }
        QFile keyFile(keyPath);
        if (!keyFile.open(QIODevice::ReadOnly)) {
            Logger::warning("Failed to open key file: " + keyFile.errorString());
            return false;
        }
        key = QSslKey(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        keyFile.close();
        if (key.isNull()) {
            if (!keyFile.open(QIODevice::ReadOnly)) return false;
            key = QSslKey(&keyFile, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
            keyFile.close();
        }
        keySource = keyPath;
    }

    // Load cert chain
    QFile certFile(certFilePath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open cert file: " + certFile.errorString());
        return false;
    }
    QList<QSslCertificate> chain = QSslCertificate::fromDevice(&certFile, QSsl::Pem);
    certFile.close();

    if (chain.isEmpty() || key.isNull()) {
        Logger::warning("SSL cert chain / key invalid");
        return false;
    }

    m_SslConfig = QSslConfiguration::defaultConfiguration();
    m_SslConfig.setLocalCertificateChain(chain);
    m_SslConfig.setPrivateKey(key);
    m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

    QString cn;
    if (!chain.isEmpty()) {
        QStringList cns = chain.first().subjectInfo(QSslCertificate::CommonName);
        cn = cns.isEmpty() ? "(no CN)" : cns.first();
    }
    Logger::info(
        QString("SSL certificate loaded: CN=%1, cert=%2, key=%3").arg(cn, certFilePath, keySource));
    return true;
}

bool CertManager::loadCert()
{
    // Case 1: cert_pem / cert_key resolve to PEM data directly (env var or file)
    QByteArray certData = resolvePemValue(m_CertPem);
    QByteArray keyData = resolvePemValue(m_CertKey);

    // Build-time embedded fallback (GitHub Actions / .env at build time)
#ifdef MW_CERT_PEM
    if (certData.isEmpty()) certData = QByteArray(MW_CERT_PEM);
#endif
#ifdef MW_CERT_KEY
    if (keyData.isEmpty()) keyData = QByteArray(MW_CERT_KEY);
#endif

    if (!certData.isEmpty() && !keyData.isEmpty()) {
        QList<QSslCertificate> chain = QSslCertificate::fromData(certData, QSsl::Pem);

        // If domain is set, verify CN matches. An embedded cert for a
        // different domain (e.g. leftover from a previous unique_id) must
        // not be used — fall through to ACME file-based mode.
        if (!m_Domain.isEmpty() && !chain.isEmpty()) {
            QString cn = chain.first().subjectInfo(QSslCertificate::CommonName).value(0);
            if (cn.compare(m_Domain, Qt::CaseInsensitive) != 0) {
                Logger::warning(QString("[CERT] Embedded cert CN=%1 does not match domain=%2 — "
                                        "falling back to file scan")
                                    .arg(cn, m_Domain));
                certData.clear();
                keyData.clear();
            }
        }
    }

    if (!certData.isEmpty() && !keyData.isEmpty()) {
        QList<QSslCertificate> chain = QSslCertificate::fromData(certData, QSsl::Pem);
        QSslKey key(keyData, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (key.isNull()) key = QSslKey(keyData, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);

        if (!chain.isEmpty() && !key.isNull()) {
            m_SslConfig = QSslConfiguration::defaultConfiguration();
            m_SslConfig.setLocalCertificateChain(chain);
            m_SslConfig.setPrivateKey(key);
            m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

            QString cn = chain.first().subjectInfo(QSslCertificate::CommonName).value(0, "(no CN)");
            Logger::info(
                QString("SSL certificate loaded from source: CN=%1, cert_pem=%2, cert_key=%3")
                    .arg(cn, m_CertPem, m_CertKey));
            return true;
        }
        Logger::warning("Failed to load certificate from cert_pem/cert_key sources");
        // Fall through to scan
    }

    // Case 2: cert_pem is a file path (not an env var) → treat as explicit cert file
    if (!certData.isEmpty() && keyData.isEmpty() && !m_CertPem.isEmpty()) {
        // cert_pem resolved (file), cert_key didn't → look for key alongside the cert file
        if (!m_CertKey.isEmpty()) {
            // If cert_key is set but didn't resolve, the key file path may be wrong
            Logger::warning("[CERT] cert_key did not resolve: " + m_CertKey);
        }
    }

    // Case 3: scan by domain + fallback (only when cert_pem/cert_key are empty)
    QString certDir = findCertDir();

    if (!certDir.isEmpty()) {
        Logger::info("SSL certificate found in " + certDir);

        if (!loadCertFiles(certDir)) {
            Logger::warning("Failed to load certificate files");
            return generateSelfSignedCert();
        }

        QDateTime expiry = m_SslConfig.localCertificate().expiryDate();
        QDateTime renewThreshold = QDateTime::currentDateTimeUtc().addDays(14);

        if (expiry > renewThreshold) {
            Logger::info(QString("SSL certificate valid until %1, no renewal needed")
                             .arg(expiry.toString("yyyy-MM-dd")));
            return true;
        }

        Logger::warning(QString("SSL certificate expires %1, attempting lego renewal...")
                            .arg(expiry.toString("yyyy-MM-dd")));

        if (renewWithLego()) {
            Logger::info("Certificate renewed, reloading...");
            return loadCertFiles(certDir);
        }

        Logger::warning("Lego renewal failed, falling back to self-signed certificate");
    } else {
        Logger::warning("No SSL certificate files found, generating self-signed certificate");
    }

    return generateSelfSignedCert();
}

bool CertManager::renewWithLego()
{
    QProcess lego;
    lego.setProcessChannelMode(QProcess::MergedChannels);
    lego.start("lego", QStringList() << "renew");

    if (!lego.waitForStarted(5000)) {
        Logger::warning("lego not found in PATH, cannot auto-renew");
        return false;
    }

    if (!lego.waitForFinished(60000)) {
        lego.kill();
        lego.waitForFinished(5000);
        Logger::warning("lego renew timed out after 60s");
        return false;
    }

    QByteArray output = lego.readAll();

    if (lego.exitCode() != 0) {
        Logger::warning(QString("lego renew failed (exit %1): %2")
                            .arg(lego.exitCode())
                            .arg(QString::fromUtf8(output).trimmed()));
        return false;
    }

    Logger::info("lego renew completed successfully");
    return true;
}

bool CertManager::reloadTls()
{
    // Case 1: cert_pem / cert_key resolve to PEM data directly (env var or file)
    QByteArray certData = resolvePemValue(m_CertPem);
    QByteArray keyData = resolvePemValue(m_CertKey);

    // Build-time embedded fallback (GitHub Actions / .env at build time)
#ifdef MW_CERT_PEM
    if (certData.isEmpty()) certData = QByteArray(MW_CERT_PEM);
#endif
#ifdef MW_CERT_KEY
    if (keyData.isEmpty()) keyData = QByteArray(MW_CERT_KEY);
#endif

    if (!certData.isEmpty() && !keyData.isEmpty()) {
        QList<QSslCertificate> chain = QSslCertificate::fromData(certData, QSsl::Pem);

        if (!m_Domain.isEmpty() && !chain.isEmpty()) {
            QString cn = chain.first().subjectInfo(QSslCertificate::CommonName).value(0);
            if (cn.compare(m_Domain, Qt::CaseInsensitive) != 0) {
                Logger::warning(
                    QString("[CERT] Reload: embedded cert CN=%1 != domain=%2").arg(cn, m_Domain));
                certData.clear();
                keyData.clear();
            }
        }
    }

    if (!certData.isEmpty() && !keyData.isEmpty()) {
        QList<QSslCertificate> chain = QSslCertificate::fromData(certData, QSsl::Pem);
        QSslKey key(keyData, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (key.isNull()) key = QSslKey(keyData, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);

        if (!chain.isEmpty() && !key.isNull()) {
            m_SslConfig.setLocalCertificateChain(chain);
            m_SslConfig.setPrivateKey(key);
            Logger::info("TLS reloaded from cert_pem/cert_key sources");
            return true;
        }
    }

    // Scan by domain + fallback
    QString certDir = findCertDir();
    if (certDir.isEmpty()) {
        Logger::warning("[TLS] No certificate directory found, cannot reload");
        return false;
    }
    if (!loadCertFiles(certDir)) return false;
    return true;
}

bool CertManager::generateSelfSignedCert()
{
    // Use AppData for writability — Program Files is read-only for MSI installs.
    QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cert/";
    QDir().mkpath(certDir);

    // Build SAN list dynamically: localhost, loopback, and all detected LAN IPs.
    // This ensures the self-signed certificate matches all local access methods
    // (localhost, 127.0.0.1, ::1, 192.168.x.x, 10.x.x.x, etc.) and the browser
    // only shows a single "untrusted CA" warning (no hostname mismatch).
    QStringList sans;
    sans << "DNS:localhost"
         << "DNS:*.local"
         << "IP:127.0.0.1"
         << "IP:::1";

    for (const QHostAddress& addr : getPrivateLanAddresses()) {
        QHostAddress clean(addr);
        if (addr.protocol() == QAbstractSocket::IPv6Protocol) clean.setScopeId(QString());
        sans << "IP:" + clean.toString();
    }

    // Erase old cert/key before generating new ones.
    QFile::remove(certDir + "key.pem");
    QFile::remove(certDir + "cert.pem");

    if (!generateSelfSignedToFiles(certDir + "cert.pem", certDir + "key.pem", sans,
                                   "MoonlightWeb")) {
        Logger::error("Failed to generate self-signed certificate (libcrypto)");
        return false;
    }
    Logger::info(QString("Self-signed certificate generated with %1 SANs in %2")
                     .arg(sans.size())
                     .arg(certDir));

    // Restrict the private key to the owner (0600 on Unix, owner-only ACL on Win).
    QFile::setPermissions(certDir + "key.pem", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return loadCertFiles(certDir);
}

void CertManager::ensureLocalSslConfig()
{
    // The local self-signed cert lives in AppData/cert/, separate from any
    // public cert (ACME-issued certs live in the configured cert dir).
    // This cert is ALWAYS regenerated with SANs for localhost + all current
    // LAN IPs so that every local access method gets a hostname-matching
    // certificate (DHCP changes are reflected on restart).
    QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cert/";
    QDir().mkpath(certDir);

    // Dedicated filenames for the local self-signed cert. These MUST NOT collide
    // with the public cert key (cert/key.pem), which onAcmeFinished writes from
    // the ACME issuance. Sharing "key.pem" caused this regen to clobber the
    // ZeroSSL/LE private key, breaking the public cert on the next boot
    // (cert=fullchain.pem ZeroSSL + key=self-signed -> "key values mismatch").
    QString certPath = certDir + "local-cert.pem";
    QString keyPath = certDir + "local-key.pem";

    // Delete old files and regenerate with fresh SANs and current LAN IPs
    QFile::remove(certPath);
    QFile::remove(keyPath);

    // Build SAN list with current LAN IPs
    QStringList sans;
    sans << "DNS:localhost" << "DNS:*.local"
         << "IP:127.0.0.1" << "IP:::1";
    for (const QHostAddress& addr : getPrivateLanAddresses()) {
        // Strip scope ID from IPv6 addresses (e.g. "%ethernet_32770")
        // — OpenSSL SAN entries only accept bare IPv6 addresses.
        QHostAddress clean(addr);
        if (addr.protocol() == QAbstractSocket::IPv6Protocol) clean.setScopeId(QString());
        sans << "IP:" + clean.toString();
    }

    if (!generateSelfSignedToFiles(certPath, keyPath, sans, "MoonlightWeb")) {
        Logger::error("[CERT] Cannot generate local self-signed cert (libcrypto)");
        return;
    }

    // Restrict the private key to the owner (0600 on Unix, owner-only ACL on Win).
    QFile::setPermissions(keyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    // Load the local cert files into m_LocalSslConfig (separate from m_SslConfig)
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::error("[CERT] Cannot open local cert: " + certFile.errorString());
        return;
    }
    QList<QSslCertificate> chain = QSslCertificate::fromDevice(&certFile, QSsl::Pem);
    certFile.close();
    if (chain.isEmpty()) {
        Logger::error("[CERT] Local cert chain is empty");
        return;
    }

    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        Logger::error("[CERT] Cannot open local key: " + keyFile.errorString());
        return;
    }
    QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    keyFile.close();
    if (key.isNull()) {
        if (!keyFile.open(QIODevice::ReadOnly)) return;
        key = QSslKey(&keyFile, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
        keyFile.close();
    }
    if (key.isNull()) {
        Logger::error("[CERT] Cannot load local private key");
        return;
    }

    m_LocalSslConfig = QSslConfiguration::defaultConfiguration();
    m_LocalSslConfig.setLocalCertificateChain(chain);
    m_LocalSslConfig.setPrivateKey(key);
    m_LocalSslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    Logger::info("[CERT] Local self-signed config loaded: " + certPath);
}
