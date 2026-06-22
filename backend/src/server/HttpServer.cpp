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

#include "HttpServer.h"
#include "RestRouter.h"
#include "StaticFileHandler.h"
#include "server/AuthManager.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslConfiguration>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <functional>
#include <memory>

// Request hardening caps (anti-DoS): bound how much we buffer before a complete
// request is available, so a client cannot grow our memory without limit by
// sending headers/body that never complete.
static constexpr int MAX_HEADER_BYTES = 32 * 1024;     // 32 KB of headers
static constexpr int MAX_BODY_BYTES = 8 * 1024 * 1024; // 8 MB body

// --- SslServer: creates QSslSocket directly from native handle ----------------
// Avoids descriptor-transfer hack (get descriptor → setSocketDescriptor(-1) →
// recreate QSslSocket) which fails on Windows because QTcpSocket's
// setSocketDescriptor(-1) calls closesocket(), invalidating the handle.
//
// Supports SNI (Server Name Indication): the TLS ClientHello is peeked before
// starting encryption, the SNI hostname is extracted, and the matching SSL
// configuration is selected (public PositiveSSL/LE cert vs self-signed LAN cert).
class SslServer : public QTcpServer
{
public:
    using SslReadyCallback = std::function<void(QSslSocket*)>;

    SslServer(const QSslConfiguration& publicConfig, const QSslConfiguration& localConfig,
              SslReadyCallback onSslReady, QObject* parent = nullptr)
        : QTcpServer(parent)
        , m_PublicSslConfig(publicConfig)
        , m_LocalSslConfig(localConfig)
        , m_OnSslReady(std::move(onSslReady))
    {}

    // Update the public (SNI default) config on a running server. Needed after
    // ACME issuance so new connections get the freshly issued cert without a
    // full server restart.
    void setPublicSslConfig(const QSslConfiguration& cfg) { m_PublicSslConfig = cfg; }

protected:
    void incomingConnection(qintptr handle) override
    {
        QSslSocket* ssl = new QSslSocket(this);
        if (!ssl->setSocketDescriptor(handle)) {
            Logger::warning("[HTTPS] SslServer: failed to set socket descriptor");
            delete ssl;
            return;
        }

        ssl->setPeerVerifyMode(QSslSocket::VerifyNone);

        connect(ssl, &QSslSocket::encrypted, this, [this, ssl]() {
            Logger::info("[HTTPS] TLS connection established");
            m_OnSslReady(ssl);
        });

        connect(ssl, &QSslSocket::sslErrors, this, [ssl](const QList<QSslError>& errors) {
            for (const auto& e : errors)
                Logger::warning("[HTTPS] SSL error: " + e.errorString());
            ssl->ignoreSslErrors();
        });

        connect(ssl, &QAbstractSocket::errorOccurred, this, [ssl](QAbstractSocket::SocketError) {
            Logger::warning("[HTTPS] Socket error: " + ssl->errorString());
            ssl->deleteLater();
        });

        // Non-blocking SNI selection: peek the ClientHello once it arrives (via
        // readyRead) instead of blocking the accept thread with waitForReadyRead.
        // A client that connects but never sends data no longer stalls the server
        // (slowloris). A 3s timeout falls back to the public config.
        // peek() is non-destructive — the bytes remain for OpenSSL.
        auto done = std::make_shared<bool>(false);
        auto conn = std::make_shared<QMetaObject::Connection>();

        auto begin = [this, ssl, done, conn]() {
            if (*done) return;
            *done = true;
            QObject::disconnect(*conn);

            QByteArray data = ssl->peek(4096);
            QString sni = parseSniHostname(data);
            // Default to the public cert; only use the local self-signed cert when
            // SNI explicitly names a LAN hostname.
            bool isLanSni = !sni.isEmpty() && isLanHostname(sni);
            ssl->setSslConfiguration(isLanSni ? m_LocalSslConfig : m_PublicSslConfig);
            ssl->startServerEncryption();
        };

        *conn = connect(ssl, &QSslSocket::readyRead, this, begin);
        QTimer::singleShot(3000, ssl, [begin]() { begin(); });
    }

private:
    /// Extract the SNI hostname from a raw TLS ClientHello handshake record.
    /// Returns empty string if the data is not a ClientHello or has no SNI extension.
    static QString parseSniHostname(const QByteArray& data)
    {
        // Minimum size for a ClientHello with SNI: ~50 bytes
        if (data.size() < 50) return {};

        const uchar* d = reinterpret_cast<const uchar*>(data.constData());
        int pos = 0;

        // TLS Record: ContentType (1) + Version (2) + Length (2)
        if (pos >= data.size() || d[pos++] != 0x16) // Not a Handshake record
            return {};
        pos += 4; // skip version + length
        if (pos >= data.size()) return {};

        // Handshake: Type (1) + Length (3)
        if (d[pos] != 0x01) return {}; // Not ClientHello
        pos += 4;                      // skip type + length
        if (pos >= data.size()) return {};

        // ClientHello: Version (2) + Random (32) + SessionID (1 + var)
        pos += 34; // skip version + random
        if (pos >= data.size()) return {};
        int sidLen = d[pos++];
        pos += sidLen;
        if (pos >= data.size()) return {};

        // Cipher Suites (2 + var)
        if (pos + 2 > data.size()) return {};
        int csLen = (d[pos] << 8) | d[pos + 1];
        pos += 2 + csLen;
        if (pos >= data.size()) return {};

        // Compression Methods (1 + var)
        int compLen = d[pos++];
        pos += compLen;
        if (pos >= data.size()) return {};

        // Extensions (2 + var)
        if (pos + 2 > data.size()) return {};
        int extLen = (d[pos] << 8) | d[pos + 1];
        pos += 2;
        int extEnd = pos + extLen;
        if (extEnd > data.size()) return {};

        while (pos + 4 <= extEnd) {
            int extType = (d[pos] << 8) | d[pos + 1];
            pos += 2;
            int extLen = (d[pos] << 8) | d[pos + 1];
            pos += 2;
            int extDataEnd = pos + extLen;
            if (extDataEnd > extEnd) break;

            if (extType == 0x0000) { // SNI extension
                // ServerNameList: length (2) + ServerName entries
                if (pos + 2 > extDataEnd) break;
                int listLen = (d[pos] << 8) | d[pos + 1];
                int sniEnd = pos + 2 + listLen;
                if (sniEnd > extDataEnd) break;
                pos += 2;

                // First entry: NameType (1) + NameLength (2) + Hostname
                if (pos + 3 > sniEnd) break;
                int nameType = d[pos++];
                if (nameType != 0x00) break; // Not host_name
                int nameLen = (d[pos] << 8) | d[pos + 1];
                pos += 2;
                if (pos + nameLen > sniEnd) break;

                return QString::fromUtf8(data.constData() + pos, nameLen);
            }

            pos = extDataEnd;
        }

        return {};
    }

    /// Check whether a hostname is a LAN/localhost address.
    /// Used instead of HttpServer::isLanHost because this is a static method.
    static bool isLanHostname(const QString& host)
    {
        if (host.isEmpty()) return true;
        QString h = host.toLower().trimmed();

        // Strip IPv6 brackets: "[fe80::1]" → "fe80::1"
        if (h.startsWith('[') && h.endsWith(']')) h = h.mid(1, h.length() - 2);

        if (h == "localhost" || h == "127.0.0.1" || h == "::1") return true;

        QHostAddress addr(h);
        if (addr.isNull()) return false;
        if (addr.isLoopback()) return true;

        if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
            quint32 ip = addr.toIPv4Address();
            if ((ip & 0xFF000000) == 0x0A000000) return true; // 10.0.0.0/8
            if ((ip & 0xFFF00000) == 0xAC100000) return true; // 172.16.0.0/12
            if ((ip & 0xFFFF0000) == 0xC0A80000) return true; // 192.168.0.0/16
        } else if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
            Q_IPV6ADDR ip6 = addr.toIPv6Address();
            if (ip6[0] == 0xFE && (ip6[1] & 0xC0) == 0x80) return true; // fe80::/10 link-local
            if ((ip6[0] & 0xFE) == 0xFC) return true;                   // fc00::/7 ULA
        }
        return false;
    }

    QSslConfiguration m_PublicSslConfig;
    QSslConfiguration m_LocalSslConfig;
    SslReadyCallback m_OnSslReady;
};

// --- Helpers -------------------------------------------------------------------

/// Locate a native (non-MSYS) openssl executable.
/// On Windows, the Git Bash openssl appears in PATH and its MSYS2 runtime
/// converts POSIX-looking arguments (e.g. "/CN=Moonlight-Web") into Windows
/// paths, breaking req -subj. Prefer the Windows-native install paths.
static QString findOpenssl()
{
#ifdef Q_OS_WIN
    // Force MSYS2_ARG_CONV_EXCL so that even if the MSYS openssl is picked,
    // it won't mangle POSIX-looking arguments.
    qputenv("MSYS2_ARG_CONV_EXCL", "*");

    QStringList candidates = {
        QDir::fromNativeSeparators("C:\\Program Files\\OpenSSL-Win64\\bin\\openssl.exe"),
        QDir::fromNativeSeparators("C:\\Program Files\\OpenSSL\\bin\\openssl.exe"),
    };
    for (const QString& path : candidates) {
        if (QFile::exists(path)) {
            Logger::info(QString("[CERT] Using native OpenSSL: %1").arg(path));
            return path;
        }
    }
    Logger::warning(
        "[CERT] Native OpenSSL not found, falling back to PATH (MSYS2 may break -subj)");
#endif
    return "openssl";
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

// --- HttpServer --------------------------------------------------------------

HttpServer::HttpServer(quint16 httpPort, quint16 httpsPort, QObject* parent)
    : QObject(parent)
    , m_HttpServer(new QTcpServer(this))
    , m_HttpsServer(nullptr)
    , m_Router(new RestRouter(this))
    , m_HttpPort(httpPort)
    , m_HttpsPort(httpsPort)
{
    // Try compile-time frontend path first (development), fall back to
    // executable-relative path (deployment / MSI install).
    QString frontendDir = QString(FRONTEND_DIR);
    if (!QDir(frontendDir).exists())
        frontendDir = QCoreApplication::applicationDirPath() + "/frontend/";
    m_StaticFiles = new StaticFileHandler(frontendDir, this);
}

HttpServer::~HttpServer()
{
    stop();
}

QString HttpServer::findCertDir()
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

QString HttpServer::findCertByDomain(const QString& domain)
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

QString HttpServer::extractCertCN(const QString& pemPath)
{
    QFile f(pemPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QList<QSslCertificate> certs = QSslCertificate::fromDevice(&f, QSsl::Pem);
    f.close();
    if (certs.isEmpty()) return {};
    QStringList cns = certs.first().subjectInfo(QSslCertificate::CommonName);
    return cns.isEmpty() ? QString() : cns.first();
}

QString HttpServer::scanKeyInDir(const QString& dir) const
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

QString HttpServer::scanCertInDir(const QString& dir, const QString& domain) const
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

QSslKey HttpServer::loadKeyFromEnv() const
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

QByteArray HttpServer::resolvePemValue(const QString& value)
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

bool HttpServer::loadCertFiles(const QString& certDir)
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

bool HttpServer::loadCertFilesExplicit(const QString& certFilePath)
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

bool HttpServer::loadCert()
{
    // Case 1: cert_pem / cert_key resolve to PEM data directly (env var or file)
    QByteArray certData = resolvePemValue(m_CertPem);
    QByteArray keyData = resolvePemValue(m_CertKey);

    // Build-time embedded fallback (GitHub Actions / .env at qmake time)
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

bool HttpServer::renewWithLego()
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

bool HttpServer::reloadTls()
{
    // Case 1: cert_pem / cert_key resolve to PEM data directly (env var or file)
    QByteArray certData = resolvePemValue(m_CertPem);
    QByteArray keyData = resolvePemValue(m_CertKey);

    // Build-time embedded fallback (GitHub Actions / .env at qmake time)
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
            applyPublicSslConfig();
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
    applyPublicSslConfig();
    return true;
}

// Push the current m_SslConfig onto the running SslServer as its public (SNI
// default) config. Without this, a reload only updates m_SslConfig while the
// live server keeps serving the cert captured at construction time.
void HttpServer::applyPublicSslConfig()
{
    if (m_HttpsServer) static_cast<SslServer*>(m_HttpsServer)->setPublicSslConfig(m_SslConfig);
}

bool HttpServer::generateSelfSignedCert()
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

    // Erase old cert/key/config before generating new ones
    QFile::remove(certDir + "key.pem");
    QFile::remove(certDir + "cert.pem");
    QFile::remove(certDir + "openssl-san.cnf");

    // Write a minimal OpenSSL config file for SANs (avoids -addext validation
    // errors when the system openssl.cnf is not accessible).
    QFile configFile(certDir + "openssl-san.cnf");
    if (configFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray cnf = "[req]\n";
        cnf += "distinguished_name = req_distinguished_name\n";
        cnf += "x509_extensions = v3_req\n";
        cnf += "prompt = no\n";
        cnf += "[req_distinguished_name]\n";
        cnf += "[v3_req]\n";
        cnf += "subjectAltName = " + sans.join(",").toUtf8() + "\n";
        configFile.write(cnf);
        configFile.close();
    }

    QProcess gen;
    gen.setProcessChannelMode(QProcess::MergedChannels);
    gen.start(findOpenssl(), QStringList() << "req" << "-x509" << "-newkey" << "rsa:2048"
                                           << "-keyout" << (certDir + "key.pem") << "-out"
                                           << (certDir + "cert.pem") << "-days" << "365" << "-nodes"
                                           << "-subj" << "/CN=Moonlight-Web"
                                           << "-config" << (certDir + "openssl-san.cnf")
                                           << "-extensions" << "v3_req");

    if (!gen.waitForStarted(5000)) {
        Logger::error("openssl not found in PATH, cannot generate self-signed certificate");
        return false;
    }

    if (!gen.waitForFinished(30000)) {
        gen.kill();
        gen.waitForFinished(5000);
        Logger::error("openssl timed out generating self-signed certificate");
        return false;
    }

    QByteArray output = gen.readAll();

    if (gen.exitCode() != 0) {
        // Try without config file (older OpenSSL < 1.1.1 fallback)
        Logger::warning(QString("openssl -config SAN failed (exit %1): %2")
                            .arg(gen.exitCode())
                            .arg(QString::fromUtf8(output).trimmed()));

        QFile::remove(certDir + "key.pem");
        QFile::remove(certDir + "cert.pem");

        gen.start(findOpenssl(), QStringList()
                                     << "req" << "-x509" << "-newkey" << "rsa:2048"
                                     << "-keyout" << (certDir + "key.pem") << "-out"
                                     << (certDir + "cert.pem") << "-days" << "365" << "-nodes"
                                     << "-subj" << "/CN=Moonlight-Web");

        if (!gen.waitForStarted(5000) || !gen.waitForFinished(30000)) {
            Logger::error("openssl fallback failed");
            return false;
        }
        if (gen.exitCode() != 0) {
            Logger::error(QString("openssl fallback failed (exit %1): %2")
                              .arg(gen.exitCode())
                              .arg(QString::fromUtf8(gen.readAll()).trimmed()));
            return false;
        }

        Logger::info("Self-signed certificate generated WITHOUT SANs (OpenSSL too old)");
    } else {
        Logger::info(QString("Self-signed certificate generated with %1 SANs in %2")
                         .arg(sans.size())
                         .arg(certDir));
    }

    // Restrict the private key to the owner (0600 on Unix, owner-only ACL on Win).
    QFile::setPermissions(certDir + "key.pem", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return loadCertFiles(certDir);
}

void HttpServer::ensureLocalSslConfig()
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
    QString configPath = certDir + "local-san.cnf";

    // Delete old files and regenerate with fresh SANs and current LAN IPs
    QFile::remove(certPath);
    QFile::remove(keyPath);
    QFile::remove(configPath);

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

    // Write a minimal OpenSSL config file to avoid the "Error checking x509
    // extensions defined via -addext" validation error that occurs when the
    // system openssl.cnf is not accessible or doesn't define the extensions.
    QFile configFile(configPath);
    if (configFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray cnf = "[req]\n";
        cnf += "distinguished_name = req_distinguished_name\n";
        cnf += "x509_extensions = v3_req\n";
        cnf += "prompt = no\n";
        cnf += "[req_distinguished_name]\n";
        cnf += "[v3_req]\n";
        cnf += "subjectAltName = " + sans.join(",").toUtf8() + "\n";
        configFile.write(cnf);
        configFile.close();
    }

    QProcess gen;
    gen.setProcessChannelMode(QProcess::MergedChannels);
    gen.start(findOpenssl(), QStringList() << "req" << "-x509" << "-newkey" << "rsa:2048"
                                           << "-keyout" << keyPath << "-out" << certPath << "-days"
                                           << "365" << "-nodes"
                                           << "-subj" << "/CN=Moonlight-Web"
                                           << "-config" << configPath << "-extensions" << "v3_req");

    if (!gen.waitForStarted(5000) || !gen.waitForFinished(30000) || gen.exitCode() != 0) {
        QByteArray errOut = gen.readAll().trimmed();
        Logger::warning(QString("[CERT] Local cert with SANs failed (exit=%1): %2")
                            .arg(gen.exitCode())
                            .arg(QString::fromUtf8(errOut)));
        Logger::warning("[CERT] Retrying without SANs...");
        QFile::remove(certPath);
        QFile::remove(keyPath);
        gen.start(findOpenssl(), QStringList() << "req" << "-x509" << "-newkey" << "rsa:2048"
                                               << "-keyout" << keyPath << "-out" << certPath
                                               << "-days" << "365" << "-nodes"
                                               << "-subj" << "/CN=Moonlight-Web");
        if (!gen.waitForStarted(5000) || !gen.waitForFinished(30000) || gen.exitCode() != 0) {
            QByteArray errOut2 = gen.readAll().trimmed();
            Logger::error(QString("[CERT] Cannot generate local self-signed cert (exit=%1): %2")
                              .arg(gen.exitCode())
                              .arg(QString::fromUtf8(errOut2)));
            return;
        }
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

bool HttpServer::start(quint16 preferredHttpsPort)
{
    Logger::info(QString("Qt SSL support=%1 build=%2 runtime=%3")
                     .arg(QSslSocket::supportsSsl() ? "yes" : "NO")
                     .arg(QSslSocket::sslLibraryBuildVersionString())
                     .arg(QSslSocket::sslLibraryVersionString()));

    m_HttpsPort = preferredHttpsPort;
    bool hasHttps = loadCert();

    // Generate the local self-signed cert with LAN SANs for SNI support.
    // When the public PositiveSSL/LE cert is loaded for public-domain clients,
    // the SslServer selects this local cert for localhost/LAN connections.
    // In the fallback case (no public cert, self-signed used as default),
    // ensureLocalSslConfig() regenerates the cert from scratch — the ~300ms
    // overhead is acceptable at startup and ensures SANs are always up-to-date.
    if (hasHttps) ensureLocalSslConfig();

    // If the default SSL config is a self-signed cert (no public PositiveSSL/LE
    // cert was found), sync the local config into the default config.
    // ensureLocalSslConfig() already generated the freshest cert with SANs
    // for current LAN IPs, so this gives the default config the same SANs.
    if (hasHttps && !m_SslConfig.localCertificate().isNull() &&
        m_SslConfig.localCertificate().isSelfSigned() &&
        !m_LocalSslConfig.localCertificate().isNull()) {
        m_SslConfig = m_LocalSslConfig;
        Logger::info("[CERT] Default config synced to local self-signed cert with SANs");
    }

    // Start HTTP server with port fallback (required for tunnels).
    // Try the preferred port first, then scan from 49080 upward.
    {
        auto tryHttpPort = [this](quint16 port) -> bool {
            if (m_HttpServer->listen(QHostAddress::Any, port)) {
                m_HttpPort = port;
                return true;
            }
            return false;
        };

        bool httpOk = false;

        // 1. Try the preferred port
        if (tryHttpPort(m_HttpPort)) {
            httpOk = true;
        } else {
            Logger::warning("HTTP port " + QString::number(m_HttpPort) + " unavailable (" +
                            m_HttpServer->errorString() + "), scanning fallback range...");
        }

        // 2. Fallback: scan from 49080 upward
        if (!httpOk) {
            for (quint16 p = 49080; p <= 65535; ++p) {
                if (tryHttpPort(p)) {
                    httpOk = true;
                    break;
                }
            }
        }

        if (httpOk) {
            connect(m_HttpServer, &QTcpServer::newConnection, this, &HttpServer::onHttpConnection);
            Logger::info("HTTP server on port " + QString::number(m_HttpPort));
        } else {
            Logger::error("HTTP server failed: no available port in any range");
            m_HttpServer->deleteLater();
            m_HttpServer = nullptr;
        }
    }

    // Start HTTPS with port fallback
    if (hasHttps) {
        // Lambda to create and test an SslServer on a given port
        auto tryHttpsPort = [this](quint16 port) -> SslServer* {
            auto* ssl = new SslServer(
                m_SslConfig, m_LocalSslConfig,
                [this](QSslSocket* socket) {
                    m_Buffers[socket] = QByteArray();
                    connect(socket, &QSslSocket::readyRead, this, &HttpServer::onReadyRead);
                    connect(socket, &QSslSocket::disconnected, this, &HttpServer::onDisconnected);
                    if (socket->bytesAvailable() > 0) onReadyReadSocket(socket);
                },
                this);
            if (ssl->listen(QHostAddress::Any, port)) return ssl;
            delete ssl;
            return nullptr;
        };

        // 1. Try the preferred port (default 443, or from settings.json)
        Logger::info("HTTPS attempting preferred port " + QString::number(preferredHttpsPort));
        m_HttpsServer = tryHttpsPort(preferredHttpsPort);
        if (m_HttpsServer) m_ActiveHttpsPort = m_HttpsServer->serverPort();

        // 2. Fallback range 1: 49443 to 65443, step 1000
        if (!m_HttpsServer) {
            for (quint16 p = 49443; p <= 65443; p += 1000) {
                m_HttpsServer = tryHttpsPort(p);
                if (m_HttpsServer) {
                    m_ActiveHttpsPort = p;
                    break;
                }
            }
        }

        // 3. Fallback range 2: 49152 to 65535, step 1
        if (!m_HttpsServer) {
            for (quint16 p = 49152; p <= 65535; ++p) {
                if ((p - 49152) % 1000 == 0)
                    Logger::info("HTTPS scanning ports starting at " + QString::number(p));
                m_HttpsServer = tryHttpsPort(p);
                if (m_HttpsServer) {
                    m_ActiveHttpsPort = p;
                    break;
                }
            }
        }

        if (m_HttpsServer) {
            Logger::info("HTTPS server started on port " + QString::number(m_ActiveHttpsPort));
        } else {
            Logger::error("HTTPS server failed: no available port in any fallback range");
        }
    }

    emit started(m_ActiveHttpsPort);
    return true;
}

void HttpServer::stop()
{
    if (m_HttpServer) {
        m_HttpServer->close();
    }
    if (m_HttpsServer) {
        m_HttpsServer->close();
        m_HttpsServer->deleteLater();
        m_HttpsServer = nullptr;
    }
    m_ActiveHttpsPort = 0;
    for (QTcpSocket* socket : m_Buffers.keys()) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_Buffers.clear();
    m_PendingAsyncSockets.clear();
}

bool HttpServer::changeHttpsPort(quint16 newPort)
{
    quint16 oldPort = m_ActiveHttpsPort;
    Logger::info(QString("Changing HTTPS port from %1 to %2...").arg(oldPort).arg(newPort));

    m_HttpsPort = newPort;
    stop();

    if (!start(newPort)) {
        Logger::error(QString("Failed to bind new HTTPS port %1, falling back to %2")
                          .arg(newPort)
                          .arg(oldPort));
        if (!start(oldPort)) {
            Logger::error("Could not restart HTTPS server on any port");
            return false;
        }
    }

    Logger::info(QString("HTTPS port changed to %1").arg(m_ActiveHttpsPort));
    return true;
}

bool HttpServer::isLanHost(const QString& host) const
{
    QString h = host.toLower().trimmed();
    if (h.isEmpty()) return true; // Missing Host header → assume LAN

    // Localhost
    if (h == "localhost" || h == "127.0.0.1" || h == "::1") return true;

    QHostAddress addr(h);
    if (addr.isNull()) return false; // Not an IP → public domain (e.g. tunnel endpoint)

    if (addr.isLoopback()) return true;

    // Private IPv4 ranges
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        quint32 ip = addr.toIPv4Address();
        // 10.0.0.0/8
        if ((ip & 0xFF000000) == 0x0A000000) return true;
        // 172.16.0.0/12
        if ((ip & 0xFFF00000) == 0xAC100000) return true;
        // 192.168.0.0/16
        if ((ip & 0xFFFF0000) == 0xC0A80000) return true;
    }

    return false;
}

bool HttpServer::isLocalRequest(const QString& addr)
{
    if (addr.isEmpty()) return false;
    return addr == "127.0.0.1" || addr == "::1" || addr == "::ffff:127.0.0.1" ||
           QHostAddress(addr).isLoopback();
}

bool HttpServer::isAuthenticated(const HttpRequest& req) const
{
    if (!m_AuthManager) return true; // No auth manager = auth disabled

    // Parse Cookie header for mw_session token
    QString cookie = req.headers.value("cookie");
    if (cookie.isEmpty()) return false;

    // Cookies are separated by "; " or ";"
    QStringList cookies = cookie.split(";");
    for (const QString& c : cookies) {
        QString trimmed = c.trimmed();
        if (trimmed.startsWith("mw_session=", Qt::CaseInsensitive)) {
            QString token = trimmed.mid(QStringLiteral("mw_session=").length());
            if (m_AuthManager->validateSession(token)) return true;
        }
    }
    return false;
}

// --- HTTP redirect ----------------------------------------------------------

void HttpServer::onHttpConnection()
{
    if (!m_HttpServer) return;
    while (QTcpSocket* socket = m_HttpServer->nextPendingConnection()) {
        // Non-encrypted HTTP server: process requests directly (no redirect to HTTPS).
        // This allows external tunnels (cloudflared etc.) to connect via HTTP
        // (they use http://localhost:<port> as the origin).
        // External TLS access goes through the separate HTTPS listener.
        m_Buffers[socket] = QByteArray();
        connect(socket, &QTcpSocket::readyRead, this, &HttpServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &HttpServer::onDisconnected);
    }
}

// --- Shared request handling ------------------------------------------------

void HttpServer::onReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;
    onReadyReadSocket(socket);
}

void HttpServer::onReadyReadSocket(QTcpSocket* socket)
{
    m_Buffers[socket].append(socket->readAll());

    QByteArray& buffer = m_Buffers[socket];
    int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd == -1) {
        // Headers still incomplete: bail until more data — but cap the wait so a
        // client dripping bytes without ever ending the headers can't grow memory.
        if (buffer.size() > MAX_HEADER_BYTES) {
            sendResponse(socket, HttpResponse::error(431, "Request Header Fields Too Large"));
        }
        return;
    }

    QString headerPart = QString::fromUtf8(buffer.left(headerEnd));

    // WebSocket upgrade: proxy the connection to the local signaling server.
    // This allows both HTTPS and WebSocket signaling to share the same port 443,
    // which is required for the tunnel to expose the full UI.
    if (headerPart.contains("Upgrade: websocket", Qt::CaseInsensitive)) {
        handleWebSocketUpgrade(socket, buffer);
        return;
    }

    int contentLength = 0;
    for (const QString& line : headerPart.split("\r\n")) {
        if (line.startsWith("Content-Length:", Qt::CaseInsensitive)) {
            contentLength = line.mid(15).trimmed().toInt();
            break;
        }
    }

    // Reject oversized or malformed bodies before buffering them.
    if (contentLength < 0 || contentLength > MAX_BODY_BYTES) {
        sendResponse(socket, HttpResponse::error(413, "Payload Too Large"));
        return;
    }

    int totalSize = headerEnd + 4 + contentLength;
    if (buffer.size() < totalSize) return;

    QByteArray requestData = buffer.left(totalSize);
    buffer.remove(0, totalSize);
    processRequest(socket, requestData);
}

void HttpServer::onDisconnected()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        bool wasPending = m_PendingAsyncSockets.contains(socket);
        m_Buffers.remove(socket);
        m_PendingAsyncSockets.remove(socket);
        if (wasPending) {
            qWarning() << "[HttpServer] onDisconnected — socket had pending async request!"
                       << "peer=" << socket->peerAddress().toString() << ":" << socket->peerPort()
                       << "bytesToWrite=" << socket->bytesToWrite();
        }
        socket->deleteLater();
    }
}

void HttpServer::processRequest(QTcpSocket* socket, const QByteArray& requestData)
{
    HttpRequest req = parseRequest(requestData);
    req.clientAddress = socket->peerAddress().toString();

    // HTTP→HTTPS redirect for plain HTTP connections.
    //
    // Redirect ALL HTTP requests to HTTPS, regardless of whether the
    // hostname is LAN (localhost, 192.168.x.x) or public domain.
    //
    // Exception: skip redirect when the client is localhost AND the Host
    // header is a public domain — this indicates a TLS-terminating tunnel
    // (e.g. cloudflared, nport TLS mode) that forwards decrypted traffic
    // to our HTTP port. In that case the browser is already on HTTPS at
    // the tunnel edge, and redirecting would create a loop.
    //
    // The redirect URL omits the port when it is the standard 443, so
    // http://domain → https://domain (clean URL without :443).
    if (!qobject_cast<QSslSocket*>(socket) && m_ActiveHttpsPort > 0) {
        QString host = req.headers.value("host");
        int portSep = host.lastIndexOf(':');
        QString hostname = (portSep >= 0) ? host.left(portSep) : host;

        bool isLocalClient = HttpServer::isLocalRequest(req.clientAddress);
        bool isPublicDomain = !isLanHost(hostname);

        // Skip redirect when behind a TLS-terminating tunnel (localhost
        // client + public Host header = tunnel already handled TLS).
        if (!(isLocalClient && isPublicDomain)) {
            QString portPart;
            if (m_ActiveHttpsPort != 443) portPart = QString(":%1").arg(m_ActiveHttpsPort);

            QString location = QString("https://%1%2%3").arg(hostname).arg(portPart).arg(req.path);
            HttpResponse resp;
            resp.statusCode = 307;
            resp.headers["Location"] = location;
            sendResponse(socket, resp);
            return;
        }
    }

    if (!req.path.startsWith("/api/")) {
        HttpResponse resp = m_StaticFiles->serveFile(req.path);
        // SPA fallback: for any non-API path that doesn't match a real file,
        // serve index.html so the frontend can handle its own routing via
        // the History API (e.g. /admin, /settings).
        if (resp.statusCode == 404) resp = m_StaticFiles->serveFile("/");
        sendResponse(socket, resp);
        return;
    }

    // ── Auth check for API routes ──────────────────────────────────────────
    // Exemptions: localhost, /api/auth/*, /api/health, /api/server/hostname.
    // Only /api/server/hostname is public (the login screen displays the PC name
    // before authentication); /api/server/status (ports) now requires a session.
    if (m_AuthManager && !HttpServer::isLocalRequest(req.clientAddress) &&
        req.path != "/api/health" && req.path != "/api/server/hostname" &&
        !req.path.startsWith("/api/auth/") && !isAuthenticated(req)) {
        QJsonObject obj;
        obj["error"] = "authentication_required";
        HttpResponse resp = HttpResponse::json(obj, 401);
        sendResponse(socket, resp);
        return;
    }

    m_PendingAsyncSockets.insert(socket);

    QTimer::singleShot(ASYNC_TIMEOUT_MS, socket, [this, socket]() {
        if (m_PendingAsyncSockets.contains(socket)) {
            qWarning() << "[HttpServer] Async timeout for" << socket
                       << "peer=" << socket->peerAddress().toString();
            m_PendingAsyncSockets.remove(socket);
            sendResponse(socket, HttpResponse::error(504, "Gateway Timeout"));
        }
    });

    m_Router->dispatchAsync(req, [this, socket](const HttpResponse& resp) {
        if (m_PendingAsyncSockets.contains(socket)) {
            m_PendingAsyncSockets.remove(socket);
            sendResponse(socket, resp);
        } else {
            qWarning()
                << "[HttpServer] Respond called but socket no longer pending — response discarded"
                << "socket=" << socket << "status=" << resp.statusCode;
        }
    });
}

void HttpServer::handleWebSocketUpgrade(QTcpSocket* clientSocket, const QByteArray& requestData)
{
    // ── Auth check: validate session cookie before proxying WS upgrade ────
    if (m_AuthManager && !HttpServer::isLocalRequest(clientSocket->peerAddress().toString())) {
        QString headerPart = QString::fromUtf8(requestData.left(requestData.indexOf("\r\n\r\n")));
        bool authenticated = false;
        for (const QString& line : headerPart.split("\r\n")) {
            if (line.startsWith("Cookie:", Qt::CaseInsensitive)) {
                QString cookie = line.mid(7).trimmed();
                QStringList cookies = cookie.split(";");
                for (const QString& c : cookies) {
                    QString trimmed = c.trimmed();
                    if (trimmed.startsWith("mw_session=", Qt::CaseInsensitive)) {
                        QString token = trimmed.mid(QStringLiteral("mw_session=").length());
                        if (m_AuthManager->validateSession(token)) {
                            authenticated = true;
                            break;
                        }
                    }
                }
            }
            if (authenticated) break;
        }
        if (!authenticated) {
            QJsonObject obj;
            obj["error"] = "authentication_required";
            HttpResponse resp = HttpResponse::json(obj, 401);
            sendResponse(clientSocket, resp);
            return;
        }
    }

    // Parse the WebSocket path from the upgrade request to determine the target.
    //   GET /ws          → proxy to m_SignalingPort (WebRTC signaling)
    //   GET /ws/stream   → proxy to m_StreamRelayPort (legacy WSS StreamRelay)
    QString firstLine = QString::fromUtf8(requestData.left(requestData.indexOf("\r\n")));
    QString path = firstLine.section(' ', 1, 1);
    quint16 targetPort = (path == "/ws/stream") ? m_StreamRelayPort : m_SignalingPort;

    qInfo() << "[HttpServer] WebSocket upgrade detected, path=" << path
            << "targetPort=" << targetPort;

    // Copy the upgrade request BEFORE removing from m_Buffers.  requestData is a
    // const reference to the QByteArray inside m_Buffers — remove() destroys it.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) — copy is required.
    QByteArray upgradeRequest = requestData;

    // Remove from our tracking — HttpServer should no longer manage this socket.
    m_Buffers.remove(clientSocket);
    m_PendingAsyncSockets.remove(clientSocket);

    // Disconnect HttpServer's handlers from this socket so they don't interfere
    // with the bidirectional proxy.
    QObject::disconnect(clientSocket, &QTcpSocket::readyRead, this, &HttpServer::onReadyRead);
    QObject::disconnect(clientSocket, &QTcpSocket::disconnected, this, &HttpServer::onDisconnected);

    // Target socket: connects to the local WebSocket server (signaling or stream relay).
    QTcpSocket* target = new QTcpSocket(this);

    // Guard flags: cleanup is called at most once, regardless of which signal
    // fires first (client disconnect, target disconnect, target error).
    bool* guard = new bool(false);

    auto cleanup = [clientSocket, target, guard]() {
        if (*guard) return;
        *guard = true;
        if (clientSocket->state() == QAbstractSocket::ConnectedState)
            clientSocket->disconnectFromHost();
        if (target->state() == QAbstractSocket::ConnectedState) target->disconnectFromHost();
        target->deleteLater();
        clientSocket->deleteLater();
        delete guard;
    };

    // Pre-connect cleanup: if client disconnects before target connects,
    // this handler ensures the target socket is not left dangling.
    QObject::connect(clientSocket, &QTcpSocket::disconnected, cleanup);

    QObject::connect(
        target, &QTcpSocket::connected, [clientSocket, target, upgradeRequest, guard]() {
            // Late connection after cleanup: tear down and return.
            if (*guard) {
                target->disconnectFromHost();
                return;
            }

            // Forward the initial HTTP upgrade request to the signaling server.
            // This includes all headers (Upgrade, Sec-WebSocket-Key, etc.).
            target->write(upgradeRequest);

            // Bidirectional forwarding: client <-> signaling server.
            QObject::connect(clientSocket, &QTcpSocket::readyRead,
                             [clientSocket, target]() { target->write(clientSocket->readAll()); });
            QObject::connect(target, &QTcpSocket::readyRead,
                             [clientSocket, target]() { clientSocket->write(target->readAll()); });
        });

    // Post-connect cleanup: when either side disconnects or errors out.
    QObject::connect(target, &QTcpSocket::disconnected, cleanup);
    QObject::connect(target, &QAbstractSocket::errorOccurred,
                     [target, cleanup](QAbstractSocket::SocketError) {
                         qWarning() << "[HttpServer] WebSocket proxy: connection error:"
                                    << target->errorString();
                         cleanup();
                     });

    target->connectToHost(QHostAddress::LocalHost, targetPort);
}

HttpRequest HttpServer::parseRequest(const QByteArray& raw) const
{
    HttpRequest req;
    QString data = QString::fromUtf8(raw);
    QStringList lines = data.split("\r\n");

    if (!lines.isEmpty()) {
        QStringList parts = lines[0].split(' ');
        if (parts.size() >= 2) {
            req.method = parts[0].toUpper();
            QUrl url(parts[1]);
            req.path = url.path();
            if (req.path.isEmpty()) req.path = "/";
            QUrlQuery query(url);
            for (const auto& item : query.queryItems())
                req.queryParams[item.first] = item.second;
        }
    }

    int i = 1;
    for (; i < lines.size(); i++) {
        if (lines[i].isEmpty()) break;
        int colon = lines[i].indexOf(':');
        if (colon > 0) {
            QString key = lines[i].left(colon).trimmed();
            QString value = lines[i].mid(colon + 1).trimmed();
            req.headers[key.toLower()] = value;
        }
    }

    if (i < lines.size()) {
        QStringList bodyLines = lines.mid(i + 1);
        req.body = bodyLines.join("\r\n").toUtf8();
    }
    return req;
}

void HttpServer::sendResponse(QTcpSocket* socket, const HttpResponse& response)
{
    // Only log failures — per-request logging floods the console with the
    // periodic /api/hosts polling.
    if (response.statusCode >= 400) {
        qInfo() << "[HttpServer] sendResponse, status=" << response.statusCode
                << "bodySize=" << response.body.size() << "socket=" << socket
                << "peer=" << (socket ? socket->peerAddress().toString() : "null")
                << "state=" << (socket ? socket->state() : -1);
    }

    QByteArray respData;
    QString statusText;
    switch (response.statusCode) {
    case 200: statusText = "OK"; break;
    case 201: statusText = "Created"; break;
    case 204: statusText = "No Content"; break;
    case 400: statusText = "Bad Request"; break;
    case 403: statusText = "Forbidden"; break;
    case 404: statusText = "Not Found"; break;
    case 500: statusText = "Internal Server Error"; break;
    default: statusText = "Unknown"; break;
    }

    respData.append("HTTP/1.1 " + QByteArray::number(response.statusCode) + " " +
                    statusText.toUtf8() + "\r\n");
    respData.append("Content-Type: " + response.contentType.toUtf8() + "\r\n");
    respData.append("Content-Length: " + QByteArray::number(response.body.size()) + "\r\n");
    // No Access-Control-Allow-Origin: the frontend is served same-origin by this
    // server, so CORS is never needed. Omitting it prevents any cross-origin page
    // from reading API responses.
    respData.append("Connection: close\r\n");

    // Security headers
    respData.append("X-Content-Type-Options: nosniff\r\n");
    respData.append("X-Frame-Options: DENY\r\n");
    respData.append("Referrer-Policy: strict-origin-when-cross-origin\r\n");
    // 'wasm-unsafe-eval' allows WebAssembly compilation only (not JS eval) —
    // required by the WASM Opus decoder fallback used on iOS/WebKit.
    // Google Fonts: stylesheet from fonts.googleapis.com, font files from
    // fonts.gstatic.com (graceful fallback to system fonts if offline).
    respData.append(
        "Content-Security-Policy: default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; "
        "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' "
        "https://fonts.gstatic.com; img-src 'self' data: blob:; connect-src 'self' wss:; "
        "worker-src 'self' blob:; frame-ancestors 'none'; base-uri 'self'; form-action 'self'\r\n");
    respData.append("Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n");

    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        respData.append(it.key().toUtf8() + ": " + it.value().toUtf8() + "\r\n");

    respData.append("\r\n");
    respData.append(response.body);

    socket->write(respData);
    socket->flush();
    socket->disconnectFromHost();
}
