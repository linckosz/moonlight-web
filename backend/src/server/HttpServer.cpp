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

#include "HttpServer.h"
#include "HttpParser.h"
#include "RestRouter.h"
#include "StaticFileHandler.h"
#include "server/AuthManager.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QProcess>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslConfiguration>
#include <QRandomGenerator>
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
              SslReadyCallback onSslReady, ConnectionGuard* guard, QObject* parent = nullptr)
        : QTcpServer(parent)
        , m_PublicSslConfig(publicConfig)
        , m_LocalSslConfig(localConfig)
        , m_OnSslReady(std::move(onSslReady))
        , m_Guard(guard)
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

        // Drop banned / flooding peers before spending anything on the TLS
        // handshake. Exempt (loopback/LAN) addresses always pass.
        if (m_Guard && !m_Guard->allowConnection(ssl->peerAddress().toString())) {
            ssl->abort();
            ssl->deleteLater();
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
    ConnectionGuard* m_Guard = nullptr;
};

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
    // executable-relative path (deployment / MSI install), then to the macOS
    // app-bundle Resources dir. On macOS the frontend cannot live next to the
    // executable (Contents/MacOS): codesign treats everything there as code and
    // rejects the non-code assets, so the bundle ships it under
    // Contents/Resources/frontend (applicationDirPath is Contents/MacOS).
    QString frontendDir = QString(FRONTEND_DIR);
    if (!QDir(frontendDir).exists())
        frontendDir = QCoreApplication::applicationDirPath() + "/frontend/";
    if (!QDir(frontendDir).exists())
        frontendDir = QCoreApplication::applicationDirPath() + "/../Resources/frontend/";
    m_StaticFiles = new StaticFileHandler(frontendDir, this);
}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::reloadTls()
{
    // CertManager reloads the public config from its sources/dir; we then push
    // it onto the live listener so new connections use the fresh cert.
    if (!m_Certs.reloadTls()) return false;
    applyPublicSslConfig();
    return true;
}

// Push CertManager's public config onto the running SslServer as its public (SNI
// default) config. Without this, a reload only updates the config held by
// CertManager while the live server keeps serving the cert captured at
// construction time.
void HttpServer::applyPublicSslConfig()
{
    if (m_HttpsServer)
        static_cast<SslServer*>(m_HttpsServer)->setPublicSslConfig(m_Certs.publicConfig());
    if (m_SecondaryHttpsServer)
        static_cast<SslServer*>(m_SecondaryHttpsServer)->setPublicSslConfig(m_Certs.publicConfig());
}

bool HttpServer::start(quint16 preferredHttpsPort)
{
    Logger::info(QString("Qt SSL support=%1 build=%2 runtime=%3")
                     .arg(QSslSocket::supportsSsl() ? "yes" : "NO")
                     .arg(QSslSocket::sslLibraryBuildVersionString())
                     .arg(QSslSocket::sslLibraryVersionString()));

    m_HttpsPort = preferredHttpsPort;
    bool hasHttps = m_Certs.loadCert();

    // Generate the local self-signed cert with LAN SANs for SNI support.
    // When the public PositiveSSL/LE cert is loaded for public-domain clients,
    // the SslServer selects this local cert for localhost/LAN connections.
    // In the fallback case (no public cert, self-signed used as default),
    // ensureLocalSslConfig() regenerates the cert from scratch — the ~300ms
    // overhead is acceptable at startup and ensures SANs are always up-to-date.
    if (hasHttps) m_Certs.ensureLocalSslConfig();

    // If the default SSL config is a self-signed cert (no public PositiveSSL/LE
    // cert was found), sync the local config into the default config.
    // ensureLocalSslConfig() already generated the freshest cert with SANs
    // for current LAN IPs, so this gives the default config the same SANs.
    if (hasHttps && !m_Certs.publicConfig().localCertificate().isNull() &&
        m_Certs.publicConfig().localCertificate().isSelfSigned() &&
        !m_Certs.localConfig().localCertificate().isNull()) {
        m_Certs.setPublicConfig(m_Certs.localConfig());
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
        auto tryHttpsPort = [this](quint16 port) -> QTcpServer* { return createHttpsServer(port); };

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

    // Periodically purge idle ConnectionGuard entries so the per-IP table does
    // not grow unbounded under a churn of unique source addresses.
    if (!m_GuardPurgeTimer) {
        m_GuardPurgeTimer = new QTimer(this);
        connect(m_GuardPurgeTimer, &QTimer::timeout, this, [this]() { m_ConnGuard.purge(); });
        m_GuardPurgeTimer->start(60'000);
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
    removeSecondaryHttpsListener();
    m_ActiveHttpsPort = 0;
    for (QTcpSocket* socket : m_Buffers.keys()) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_Buffers.clear();
    m_PendingAsyncSockets.clear();
}

QTcpServer* HttpServer::createHttpsServer(quint16 port)
{
    auto* ssl = new SslServer(
        m_Certs.publicConfig(), m_Certs.localConfig(),
        [this](QSslSocket* socket) {
            m_Buffers[socket] = QByteArray();
            connect(socket, &QSslSocket::readyRead, this, &HttpServer::onReadyRead);
            connect(socket, &QSslSocket::disconnected, this, &HttpServer::onDisconnected);
            if (socket->bytesAvailable() > 0) onReadyReadSocket(socket);
        },
        &m_ConnGuard, this);
    if (ssl->listen(QHostAddress::Any, port)) return ssl;
    delete ssl;
    return nullptr;
}

bool HttpServer::addSecondaryHttpsListener(quint16 port)
{
    if (port == 0) return false;
    // Already reachable on this port through the primary listener — nothing to do.
    if (port == m_ActiveHttpsPort) return true;
    if (m_SecondaryHttpsServer && m_SecondaryHttpsPort == port) return true;

    // Replace any previous secondary bound to a different port.
    removeSecondaryHttpsListener();

    QTcpServer* srv = createHttpsServer(port);
    if (!srv) {
        Logger::error(QString("Secondary HTTPS listener failed to bind port %1").arg(port));
        return false;
    }
    m_SecondaryHttpsServer = srv;
    m_SecondaryHttpsPort = port;
    Logger::info(QString("Secondary HTTPS listener on port %1 (public domain parity)").arg(port));
    return true;
}

void HttpServer::removeSecondaryHttpsListener()
{
    if (m_SecondaryHttpsServer) {
        m_SecondaryHttpsServer->close();
        m_SecondaryHttpsServer->deleteLater();
        m_SecondaryHttpsServer = nullptr;
    }
    m_SecondaryHttpsPort = 0;
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
    if (addr == "127.0.0.1" || addr == "::1" || addr == "::ffff:127.0.0.1" ||
        QHostAddress(addr).isLoopback())
        return true;

    // Same-machine access via a non-loopback address (e.g. the host opening its
    // own LAN IP https://192.168.1.9:<port>): the connection's source IP is then
    // one of THIS machine's own interface addresses. A remote LAN/Internet peer
    // always presents its own distinct source IP, so matching one of our local
    // interface addresses proves the request originated on the host itself.
    QHostAddress peer(addr);
    if (peer.isNull()) return false;
    // Strip any IPv4-mapped IPv6 form (::ffff:192.168.1.9) before comparing.
    bool mappedOk = false;
    quint32 peer4 = peer.toIPv4Address(&mappedOk);
    for (const QHostAddress& local : QNetworkInterface::allAddresses()) {
        if (local.isLoopback()) continue;
        if (local == peer) return true;
        bool localOk = false;
        quint32 local4 = local.toIPv4Address(&localOk);
        if (mappedOk && localOk && local4 == peer4) return true;
    }
    return false;
}

QString HttpServer::sessionTokenFromRequest(const HttpRequest& req)
{
    QString cookie = req.headers.value("cookie");
    if (cookie.isEmpty()) return {};

    // Cookies are separated by "; " or ";"
    QStringList cookies = cookie.split(";");
    for (const QString& c : cookies) {
        QString trimmed = c.trimmed();
        if (trimmed.startsWith("mw_session=", Qt::CaseInsensitive))
            return trimmed.mid(QStringLiteral("mw_session=").length());
    }
    return {};
}

bool HttpServer::isAuthenticated(const HttpRequest& req) const
{
    if (!m_AuthManager) return true; // No auth manager = auth disabled

    QString token = sessionTokenFromRequest(req);
    return !token.isEmpty() && m_AuthManager->validateSession(token);
}

// --- Abuse mitigation -------------------------------------------------------

bool HttpServer::rejectIfAbusive(QTcpSocket* socket)
{
    if (m_ConnGuard.allowConnection(socket->peerAddress().toString())) return false;
    socket->abort();
    socket->deleteLater();
    return true;
}

// --- HTTP redirect ----------------------------------------------------------

void HttpServer::onHttpConnection()
{
    if (!m_HttpServer) return;
    while (QTcpSocket* socket = m_HttpServer->nextPendingConnection()) {
        // Drop banned / flooding peers immediately (cheap close, no parsing).
        if (rejectIfAbusive(socket)) continue;
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
    HttpRequest req = HttpParser::parse(requestData);
    req.clientAddress = socket->peerAddress().toString();
    // "Local" = full admin access: a loopback peer, or a host-key session — the
    // browser runs on the host machine but reaches us through the public domain
    // (its peer address is then the router/NAT-hairpin address, not loopback).
    req.isLocal = isLocalRequest(req.clientAddress) ||
                  (m_AuthManager && m_AuthManager->isHostSession(sessionTokenFromRequest(req)));

    // HTTP→HTTPS redirect for plain HTTP connections.
    //
    // Redirect HTTP requests to HTTPS so LAN/public access is always encrypted.
    //
    // Exception 1: skip redirect when the client is localhost AND the Host
    // header is a public domain — this indicates a TLS-terminating tunnel
    // (e.g. cloudflared, nport TLS mode) that forwards decrypted traffic
    // to our HTTP port. In that case the browser is already on HTTPS at
    // the tunnel edge, and redirecting would create a loop.
    //
    // Exception 2: skip redirect for a loopback Host (localhost / 127.0.0.1 /
    // ::1). Serving these over plain HTTP is safe (traffic never leaves the
    // machine) and lets the local entry points — the setup wizard and admin
    // page — open without a self-signed-cert warning (which some browsers,
    // notably Ubuntu's snap Firefox, render as a blank page). Streaming pages
    // still require HTTPS: the frontend gates them and offers a secure link.
    //
    // The redirect URL omits the port when it is the standard 443, so
    // http://domain → https://domain (clean URL without :443).
    if (!qobject_cast<QSslSocket*>(socket) && m_ActiveHttpsPort > 0) {
        QString host = req.headers.value("host");
        int portSep = host.lastIndexOf(':');
        QString hostname = (portSep >= 0) ? host.left(portSep) : host;

        bool isLocalClient = HttpServer::isLocalRequest(req.clientAddress);
        bool isPublicDomain = !isLanHost(hostname);
        bool isLoopbackHost = hostname.compare("localhost", Qt::CaseInsensitive) == 0 ||
                              hostname == "127.0.0.1" || hostname == "::1" || hostname == "[::1]";

        // Skip redirect behind a TLS-terminating tunnel (localhost client +
        // public Host header) or for a loopback Host (served as HTTP directly).
        if (!((isLocalClient && isPublicDomain) || isLoopbackHost)) {
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
        // Unauthenticated remote API hit = credential scanning; feed the guard
        // so repeated failures ban the source IP.
        m_ConnGuard.reportAuthFailure(req.clientAddress);
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
            m_ConnGuard.reportAuthFailure(clientSocket->peerAddress().toString());
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
