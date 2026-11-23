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
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QStandardPaths>
#include <functional>

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

// --- Helpers -------------------------------------------------------------------

/// Read back a private key written by generateSelfSignedToFiles(). Returns
/// nullptr when the file is missing, unreadable, encrypted, or not an RSA key of
/// at least 2048 bits: the caller then generates a fresh one.
static EVP_PKEY* readPrivateKeyFile(const QString& keyPath)
{
    QFile f(keyPath);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    QByteArray pem = f.readAll();
    f.close();

    EVP_PKEY* pkey = nullptr;
    if (BIO* bio = BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size()))) {
        // No passphrase: never let OpenSSL prompt on a console for one.
        pkey = PEM_read_bio_PrivateKey(
            bio, nullptr, [](char*, int, int, void*) -> int { return 0; }, nullptr);
        BIO_free(bio);
    }
    pem.fill('\0');

    if (pkey && (EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA || EVP_PKEY_bits(pkey) < 2048)) {
        EVP_PKEY_free(pkey);
        pkey = nullptr;
    }
    return pkey;
}

/// A certificate or key file given by a path is read only when it is a regular
/// file of this size or less.
static constexpr qint64 kMaxPemFileBytes = 1024 * 1024;

static bool looksLikePem(const QByteArray& data)
{
    return data.trimmed().startsWith("-----BEGIN");
}

/// The bytes of @p path when it is an existing regular file of at most
/// kMaxPemFileBytes, empty otherwise.
static QByteArray readSmallRegularFile(const QString& path)
{
    if (path.isEmpty()) return {};
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile() || fi.size() > kMaxPemFileBytes) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.read(kMaxPemFileBytes);
}

/// A PEM private key, RSA or EC; null when it is neither.
static QSslKey keyFromPem(const QByteArray& pem)
{
    QSslKey key(pem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull()) key = QSslKey(pem, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    return key;
}

/// The value of the environment variable @p name as PEM: its content when it
/// is PEM, else the content of the file it names when that is a regular file
/// of at most kMaxPemFileBytes holding PEM. @p filePath gets that file's path.
/// A variable that is set but is neither is reported by its name only.
static QByteArray pemFromEnv(const QString& name, QString* filePath = nullptr)
{
    if (name.isEmpty()) return {};
    const QByteArray value = qgetenv(name.toUtf8().constData());
    if (value.isEmpty()) return {};
    if (looksLikePem(value)) return value;
    const QString path = QString::fromLocal8Bit(value).trimmed();
    QByteArray data = readSmallRegularFile(path);
    if (looksLikePem(data)) {
        if (filePath) *filePath = path;
        return data;
    }
    data.fill('\0');
    Logger::warning(QString("[CERT] %1 is set in this server's environment but is neither PEM "
                            "nor a readable file: ignored")
                        .arg(name));
    return {};
}

/// Whether @p key is the private key of @p cert (what a TLS handshake needs;
/// a mismatch fails there as "key values mismatch").
static bool keyMatchesCert(const QSslCertificate& cert, const QSslKey& key)
{
    if (cert.isNull() || key.isNull()) return false;
    const QByteArray der = cert.toDer();
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.constData());
    X509* x509 = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    QByteArray keyPem = key.toPem();
    BIO* bio = BIO_new_mem_buf(keyPem.constData(), static_cast<int>(keyPem.size()));
    EVP_PKEY* pkey =
        bio ? PEM_read_bio_PrivateKey(
                  bio, nullptr, [](char*, int, int, void*) -> int { return 0; }, nullptr)
            : nullptr;
    const bool match = x509 && pkey && X509_check_private_key(x509, pkey) == 1;
    if (pkey) EVP_PKEY_free(pkey);
    if (bio) BIO_free(bio);
    if (x509) X509_free(x509);
    keyPem.fill('\0');
    ERR_clear_error(); // a mismatch leaves its reason queued for the next TLS call
    return match;
}

/// Generate a self-signed RSA-2048 certificate + private key entirely via
/// libcrypto — no external openssl.exe dependency (CI artifacts run on clean
/// machines without OpenSSL on PATH). `sans` entries use the OpenSSL v3 conf
/// syntax ("DNS:localhost", "IP:127.0.0.1", ...). Writes PEM files.
///
/// With @p reuseKey, a usable key already at @p keyPath signs the new
/// certificate and its file is left untouched (@p keptKey reports it); a key
/// that cannot be read is replaced by a fresh one.
static bool generateSelfSignedToFiles(const QString& certPath, const QString& keyPath,
                                      const QStringList& sans, const QString& commonName,
                                      bool reuseKey = false, bool* keptKey = nullptr)
{
    bool ok = false;
    EVP_PKEY* pkey = reuseKey ? readPrivateKeyFile(keyPath) : nullptr;
    const bool kept = pkey != nullptr;
    if (keptKey) *keptKey = kept;
    if (!pkey) pkey = EVP_RSA_gen(2048);
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

        if (!kept && !writePem(keyPath, [pkey](BIO* b) {
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

/// Whether the local certificate already on disk can be served again as is: it
/// was signed for the key beside it, it still names every address in @p sans,
/// and it has more than @p minDaysLeft days to run.
///
/// Keeping it is what lets a browser's "proceed anyway" survive a restart. That
/// exception is held for one certificate, not for the host, so a new one at
/// every start sent the page back to the warning after each reboot and update —
/// and a page already open saw its requests fail without a word.
static bool localCertReusable(const QString& certPath, const QString& keyPath,
                              const QStringList& sans, int minDaysLeft)
{
    QFile f(certPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray pem = f.readAll();
    f.close();

    X509* x509 = nullptr;
    if (BIO* bio = BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size()))) {
        x509 = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
    }
    if (!x509) return false;
    EVP_PKEY* pkey = readPrivateKeyFile(keyPath);
    const bool keyMatches = pkey && X509_check_private_key(x509, pkey) == 1;
    if (pkey) EVP_PKEY_free(pkey);
    X509_free(x509);
    if (!keyMatches) return false;

    const QList<QSslCertificate> chain = QSslCertificate::fromData(pem, QSsl::Pem);
    if (chain.isEmpty()) return false;
    const QSslCertificate& cert = chain.first();
    if (cert.expiryDate() <= QDateTime::currentDateTimeUtc().addDays(minDaysLeft)) return false;

    const auto names = cert.subjectAlternativeNames();
    const QStringList dns = names.values(QSsl::DnsEntry);
    QList<QHostAddress> ips;
    for (const QString& ip : names.values(QSsl::IpAddressEntry))
        ips << QHostAddress(ip);
    for (const QString& san : sans) {
        if (san.startsWith(QLatin1String("DNS:"))) {
            if (!dns.contains(san.mid(4), Qt::CaseInsensitive)) return false;
        } else if (san.startsWith(QLatin1String("IP:"))) {
            if (!ips.contains(QHostAddress(san.mid(3)))) return false;
        }
    }
    return true;
}

/// Enumerate all private/local addresses on this machine (excluding loopback).
/// Covers RFC 1918 IPv4, the 100.64.0.0/10 range a mesh VPN hands out
/// (Tailscale: a guest link can carry it), and IPv6 link-local (fe80::/10) and
/// ULA (fc00::/7, Tailscale's fd7a:115c:a1e0::/48 among them).
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
                // RFC 1918: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, then
                // 100.64.0.0/10 (CGNAT, what Tailscale hands out)
                if ((ip & 0xFF000000) == 0x0A000000 || (ip & 0xFFF00000) == 0xAC100000 ||
                    (ip & 0xFFFF0000) == 0xC0A80000 || (ip & 0xFFC00000) == 0x64400000) {
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
            if (!certFileMatchesDomain(filePath, domain)) continue;

            QDir certDir = QFileInfo(filePath).absoluteDir();
            QString keyPath = scanKeyInDir(certDir.absolutePath());
            if (!keyPath.isEmpty()) {
                Logger::info(QString("[CERT] Found matching certificate: CN=%1, file=%2, key=%3")
                                 .arg(extractCertCN(filePath), filePath, keyPath));
                return certDir.absolutePath();
            }
            Logger::info(QString("[CERT] Domain matches but no private key found in %1, skipping")
                             .arg(certDir.absolutePath()));
        }
    }
    return {};
}

bool CertManager::certMatchesDomain(const QSslCertificate& cert, const QString& domain)
{
    if (domain.isEmpty()) return true;

    // "*.example.com" covers "host.example.com" but not "example.com" nor a
    // deeper "a.b.example.com" (RFC 6125: one label, leftmost only).
    auto matches = [&domain](const QString& name) {
        if (name.isEmpty()) return false;
        if (name.startsWith(QLatin1String("*."))) {
            const QString suffix = name.mid(1); // ".example.com"
            const int dot = domain.indexOf(QLatin1Char('.'));
            return dot > 0 && domain.mid(dot).compare(suffix, Qt::CaseInsensitive) == 0;
        }
        return name.compare(domain, Qt::CaseInsensitive) == 0;
    };

    const QStringList cns = cert.subjectInfo(QSslCertificate::CommonName);
    for (const QString& cn : cns)
        if (matches(cn)) return true;

    const QList<QString> sans = cert.subjectAlternativeNames().values(QSsl::DnsEntry);
    for (const QString& san : sans)
        if (matches(san)) return true;

    return false;
}

bool CertManager::certFileMatchesDomain(const QString& pemPath, const QString& domain)
{
    QFile f(pemPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QList<QSslCertificate> certs = QSslCertificate::fromDevice(&f, QSsl::Pem);
    f.close();
    if (certs.isEmpty()) return false;
    return certMatchesDomain(certs.first(), domain);
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

QStringList CertManager::scanKeysInDir(const QString& dir) const
{
    QStringList found;
    QDirIterator it(dir, {"*.pem"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString filePath = it.next();

        // Skip the ACME account key a pre-0.3 install left behind: it
        // authenticated to the certificate authority, it is not a TLS key.
        if (filePath.endsWith("account_key.pem")) continue;

        // Check for the private key marker before attempting QSslKey
        QByteArray content = readSmallRegularFile(filePath);
        const bool isKey = content.contains("PRIVATE KEY") && !keyFromPem(content).isNull();
        content.fill('\0');
        if (!isKey) continue;
        Logger::info("[CERT] Found private key: file=" + filePath);
        found << filePath;
    }
    return found;
}

QString CertManager::scanKeyInDir(const QString& dir) const
{
    return scanKeysInDir(dir).value(0);
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

        // If a domain is specified, the cert must cover it (CN or SAN)
        if (!certMatchesDomain(certs.first(), domain)) continue;

        QStringList cns = certs.first().subjectInfo(QSslCertificate::CommonName);
        QString cn = cns.isEmpty() ? "(no CN)" : cns.first();
        Logger::info(QString("[CERT] Found certificate: CN=%1, file=%2").arg(cn, filePath));
        return filePath;
    }
    return {};
}

QSslKey CertManager::loadKeyFromEnv() const
{
    // 1. Runtime env var (overrides everything): PEM, or the path of a PEM file
    QByteArray data = pemFromEnv(QStringLiteral("MW_CERT_KEY"));

    // 2. Build-time embedded key (from .env at project root, via DEFINE)
#ifdef MW_CERT_KEY
    if (data.isEmpty()) data = QByteArray(MW_CERT_KEY);
#endif

    if (data.isEmpty()) return {};
    const QSslKey key = keyFromPem(data);
    data.fill('\0');
    return key;
}

QByteArray CertManager::resolvePemValue(const QString& value, QString* filePath)
{
    if (value.isEmpty()) return {};

    // 1. An environment variable of that name: PEM, or the path of a PEM file
    if (!qgetenv(value.toUtf8().constData()).isEmpty()) return pemFromEnv(value, filePath);

    // 2. A file path: a regular file of at most kMaxPemFileBytes
    QByteArray data = readSmallRegularFile(value);
    if (!data.isEmpty() && filePath) *filePath = value;
    return data;
}

CertManager::BuiltInPair CertManager::builtInPair()
{
    BuiltInPair pair;
#ifdef MW_CERT_PEM
    pair.certPem = QByteArray(MW_CERT_PEM);
#endif
#ifdef MW_CERT_KEY
    pair.keyPem = QByteArray(MW_CERT_KEY);
#endif
    return pair;
}

QString CertManager::keyBesideCert(const QString& certFile, const QSslCertificate& cert,
                                   QSslKey* key)
{
    const QFileInfo fi(certFile);
    const QDir dir = fi.absoluteDir();
    const QStringList names = {QStringLiteral("privkey.pem"), QStringLiteral("key.pem"),
                               fi.completeBaseName() + QStringLiteral(".key")};
    for (const QString& name : names) {
        const QString path = dir.filePath(name);
        QByteArray data = readSmallRegularFile(path);
        const QSslKey candidate = looksLikePem(data) ? keyFromPem(data) : QSslKey();
        data.fill('\0');
        if (candidate.isNull()) continue;
        if (!keyMatchesCert(cert, candidate)) {
            Logger::warning(QString("[CERT] %1 does not match the certificate %2: skipped")
                                .arg(path, certFile));
            continue;
        }
        if (key) *key = candidate;
        return path;
    }
    return {};
}

bool CertManager::pairFromSources(const QString& certPem, const QString& certKey,
                                  const QString& domain, QList<QSslCertificate>* chainOut,
                                  QSslKey* keyOut, QString* described, const BuiltInPair& builtIn)
{
    QString certFile;
    QString keyFile;
    QByteArray certData = resolvePemValue(certPem, &certFile);
    QByteArray keyData = resolvePemValue(certKey, &keyFile);
    QString certSource = certFile.isEmpty() ? certPem : certFile;
    QString keySource = keyFile.isEmpty() ? certKey : keyFile;

    // The build's own pair (embedded at build time) counts only as a pair:
    // its certificate stands in for an empty cert_pem, and then only its own
    // key goes with it. A certificate from cert_pem never takes the build's key.
    if (certData.isEmpty() && !builtIn.certPem.isEmpty()) {
        if (!keyData.isEmpty())
            Logger::warning(QString("[CERT] cert_key (%1) is not used: cert_pem (%2) gave no "
                                    "certificate, and the build's certificate goes only with "
                                    "the build's key")
                                .arg(keySource, certPem));
        keyData.fill('\0');
        certData = builtIn.certPem;
        keyData = builtIn.keyPem;
        certSource = QStringLiteral("the build's certificate");
        keySource = QStringLiteral("the build's key");
        certFile.clear();
    }

    if (certData.isEmpty()) {
        // The defaults name variables that are usually unset: say nothing then.
        if (certPem != QLatin1String("MW_CERT_PEM") || !keyData.isEmpty())
            Logger::warning(QString("[CERT] cert_pem (%1) gave no certificate: it is neither set "
                                    "in this server's environment nor a readable file")
                                .arg(certPem));
        keyData.fill('\0');
        return false;
    }

    const QList<QSslCertificate> chain = QSslCertificate::fromData(certData, QSsl::Pem);
    if (chain.isEmpty()) {
        Logger::warning(QString("[CERT] cert_pem (%1) holds no PEM certificate").arg(certSource));
        keyData.fill('\0');
        return false;
    }

    // If domain is set, the cert must cover it (CN or SAN). One for a
    // different domain (e.g. leftover from a previous unique_id, or the
    // build-time embedded shared-domain cert on a bring-your-own-domain
    // install) must not be used — fall through to the file scan.
    if (!domain.isEmpty() && !certMatchesDomain(chain.first(), domain)) {
        const QString cn = chain.first().subjectInfo(QSslCertificate::CommonName).value(0);
        Logger::warning(QString("[CERT] The certificate from %1 (CN=%2) does not cover domain=%3 "
                                "— falling back to file scan")
                            .arg(certSource, cn, domain));
        keyData.fill('\0');
        return false;
    }

    QSslKey key;
    if (!keyData.isEmpty()) {
        key = keyFromPem(keyData);
        keyData.fill('\0');
        if (key.isNull()) {
            Logger::warning(QString("[CERT] cert_key (%1) holds no RSA or EC private key in PEM, "
                                    "unencrypted")
                                .arg(keySource));
            return false;
        }
    } else if (!certFile.isEmpty()) {
        // No key from cert_key: one beside a certificate file, under fixed names.
        keySource = keyBesideCert(certFile, chain.first(), &key);
        if (keySource.isEmpty()) {
            Logger::warning(
                QString("[CERT] cert_pem (%1) gave a certificate but cert_key (%2) gave no private "
                        "key: it is neither set in this server's environment nor a readable "
                        "file, and no privkey.pem, key.pem or %3.key beside the certificate "
                        "matches it. Set cert_key in settings.json to the key file's path, "
                        "readable by the account the server runs as.")
                    .arg(certFile, certKey, QFileInfo(certFile).completeBaseName()));
            return false;
        }
    } else {
        Logger::warning(
            QString("[CERT] cert_pem (%1) gave a certificate but cert_key (%2) gave no private "
                    "key: it is neither set in this server's environment nor a readable file. "
                    "Set cert_key in settings.json to the key file's path, readable by the "
                    "account the server runs as, or %2 in the server's own environment.")
                .arg(certSource, certKey));
        return false;
    }

    if (!keyMatchesCert(chain.first(), key)) {
        Logger::warning(QString("[CERT] The private key from %1 does not match the certificate "
                                "from %2: neither is used")
                            .arg(keySource, certSource));
        return false;
    }

    if (chainOut) *chainOut = chain;
    if (keyOut) *keyOut = key;
    if (described) *described = QString("cert=%1, key=%2").arg(certSource, keySource);
    return true;
}

bool CertManager::loadCertFiles(const QString& certDir)
{
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

            if (!certMatchesDomain(certs.first(), domain)) continue;

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

    // Load cert chain
    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open cert file: " + certFile.errorString());
        return false;
    }
    QList<QSslCertificate> chain = QSslCertificate::fromDevice(&certFile, QSsl::Pem);
    certFile.close();
    if (chain.isEmpty()) {
        Logger::warning("SSL cert chain invalid: " + certPath);
        return false;
    }

    // Load private key. Prefer a key file sitting next to the certificate —
    // a cert and its key are dropped in together — and of those, the one that
    // is this certificate's key: a folder can hold several pairs. The embedded
    // env key (MW_CERT_KEY) belongs to ONE specific embedded cert, so it is only
    // a last resort, and it too must match.
    QSslKey key;
    QString keySource;
    for (const QString& keyPath : scanKeysInDir(QFileInfo(certPath).absolutePath())) {
        QByteArray data = readSmallRegularFile(keyPath);
        const QSslKey candidate = keyFromPem(data);
        data.fill('\0');
        if (!candidate.isNull() && keyMatchesCert(chain.first(), candidate)) {
            key = candidate;
            keySource = keyPath;
            break;
        }
    }

    if (key.isNull()) {
        const QSslKey envKey = loadKeyFromEnv();
        if (!envKey.isNull()) {
            if (keyMatchesCert(chain.first(), envKey)) {
                key = envKey;
                keySource = "env:MW_CERT_KEY";
            } else {
                Logger::warning("[CERT] The key from MW_CERT_KEY does not match the certificate " +
                                certPath + ": not used");
            }
        }
    }

    if (key.isNull()) {
        Logger::warning("[CERT] No private key that matches the certificate " + certPath);
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
    if (!keyMatchesCert(chain.first(), key)) {
        Logger::warning(QString("[CERT] The private key from %1 does not match the certificate %2")
                            .arg(keySource, certFilePath));
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
    // Case 1: cert_pem / cert_key (environment variable, or file path), or the
    // build's own pair; a key beside a certificate file when cert_key gives none.
    QList<QSslCertificate> chain;
    QSslKey key;
    QString described;
    if (pairFromSources(m_CertPem, m_CertKey, m_Domain, &chain, &key, &described)) {
        m_SslConfig = QSslConfiguration::defaultConfiguration();
        m_SslConfig.setLocalCertificateChain(chain);
        m_SslConfig.setPrivateKey(key);
        m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
        const QString cn =
            chain.first().subjectInfo(QSslCertificate::CommonName).value(0, "(no CN)");
        Logger::info(QString("SSL certificate loaded from source: CN=%1, %2").arg(cn, described));
        return true;
    }

    // Case 3: scan by domain + fallback
    QString certDir = findCertDir();

    if (!certDir.isEmpty()) {
        Logger::info("SSL certificate found in " + certDir);

        if (!loadCertFiles(certDir)) {
            Logger::warning("Failed to load certificate files");
            return generateSelfSignedCert();
        }

        // A loaded certificate is always kept, even close to expiry: replacing a
        // real (if ageing) certificate with a self-signed one turns a warning
        // nobody sees yet into a browser error right now. Renewal belongs to
        // whoever owns the domain — the user, for a domain they configured
        // themselves (they drop new PEM files in and restart).
        QDateTime expiry = m_SslConfig.localCertificate().expiryDate();
        if (expiry <= QDateTime::currentDateTimeUtc().addDays(14)) {
            Logger::warning(QString("SSL certificate expires %1 — renew it soon")
                                .arg(expiry.toString("yyyy-MM-dd")));
        } else {
            Logger::info(QString("SSL certificate valid until %1, no renewal needed")
                             .arg(expiry.toString("yyyy-MM-dd")));
        }
        return true;
    }

    Logger::warning("No SSL certificate files found, generating self-signed certificate");
    return generateSelfSignedCert();
}

bool CertManager::reloadTls()
{
    QList<QSslCertificate> chain;
    QSslKey key;
    QString described;
    if (pairFromSources(m_CertPem, m_CertKey, m_Domain, &chain, &key, &described)) {
        m_SslConfig.setLocalCertificateChain(chain);
        m_SslConfig.setPrivateKey(key);
        Logger::info("TLS reloaded from cert_pem/cert_key sources: " + described);
        return true;
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
    // (localhost, 127.0.0.1, ::1, 192.168.x.x, 10.x.x.x, Tailscale's 100.x, etc.) and the browser
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
    // public cert (which lives in the configured cert dir).
    // This cert names localhost + all current LAN IPs so that every local
    // access method gets a hostname-matching certificate. It is kept across
    // restarts while it still names them all and has more than 30 days left
    // (localCertReusable), and regenerated otherwise — a DHCP change is still
    // reflected on restart.
    // Its KEY is kept across restarts in every case: a client that pins the server's public
    // key on first contact (MoonlightWeb TV pins the SPKI) would otherwise see
    // a "server key changed" after every restart. Deleting local-key.pem
    // rotates it.
    QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cert/";
    QDir().mkpath(certDir);

    // Dedicated filenames for the local self-signed cert. These MUST NOT
    // collide with the public cert key (cert/key.pem). Sharing "key.pem" made
    // this regeneration clobber the public private key, breaking the public
    // cert on the next boot (cert=fullchain.pem + key=self-signed ->
    // "key values mismatch").
    QString certPath = certDir + "local-cert.pem";
    QString keyPath = certDir + "local-key.pem";

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

    if (localCertReusable(certPath, keyPath, sans, kLocalCertMinDaysLeft)) {
        Logger::info("[CERT] Local cert kept: it still covers this machine's addresses");
    } else {
        // Replace the old cert with one naming the current addresses.
        QFile::remove(certPath);
        bool keptKey = false;
        if (!generateSelfSignedToFiles(certPath, keyPath, sans, "MoonlightWeb", true, &keptKey)) {
            Logger::error("[CERT] Cannot generate local self-signed cert (libcrypto)");
            return;
        }
        Logger::info(keptKey ? "[CERT] Local cert renewed with its existing key"
                             : "[CERT] Local cert generated with a new key");
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
