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
#include "PortFallback.h"
#include "RequestGuard.h"
#include "RestRouter.h"
#include "StaticFileHandler.h"
#include "server/AuthManager.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QJsonArray>
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

// Drop anything that could end a header line early (CR, LF, NUL). Applied to
// every header we emit so no value can ever inject a header of its own.
static QString sanitizeHeaderValue(const QString& value)
{
    QString out;
    out.reserve(value.size());
    for (const QChar c : value)
        if (c.unicode() >= 0x20 && c.unicode() != 0x7F) out.append(c);
    return out;
}

// True when @p authority holds nothing but what a host[:port] is made of.
// Guards the one place a request-supplied value reaches a policy header: a Host
// of "example.com; connect-src *" would otherwise close the CSP's connect-src
// directive and open a new one.
static bool isPlainAuthority(const QString& authority)
{
    if (authority.isEmpty()) return false;
    for (const QChar c : authority) {
        const bool ok = c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('-') ||
                        c == QLatin1Char(':') || c == QLatin1Char('[') || c == QLatin1Char(']');
        if (!ok) return false;
    }
    return true;
}

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
    m_StaticFiles =
        new StaticFileHandler(frontendDir, QCoreApplication::applicationVersion(), this);

    // Admin key: a per-run secret the frontend fetches from /api/admin/token and
    // echoes on every admin write (see adminKeyMatches). Deliberately NOT
    // persisted — it is a CSRF barrier, not a credential, and a fresh one each
    // run means a copy that leaked into a log or a screenshot dies with the
    // process. The frontend re-fetches it transparently after a restart.
    QByteArray key(32, '\0');
    for (int i = 0; i < key.size(); ++i)
        key[i] = static_cast<char>(QRandomGenerator::securelySeeded().bounded(256));
    m_AdminKey = QString::fromLatin1(
        key.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
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
    // Preferred port, then the stable ladder (PortFallback), then a scan.
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

        // 2. Stable ladder: fixed low ports no outgoing connection can steal, so
        // the port persisted here is still the port bound on the next boot.
        if (!httpOk) {
            for (quint16 p : PortFallback::kHttp) {
                if (tryHttpPort(p)) {
                    httpOk = true;
                    break;
                }
            }
        }

        // 3. Last resort: scan from 49080 upward. These ports are inside the OS
        // ephemeral range, so one that is free today may be taken tomorrow —
        // good enough to answer at all when the ladder above is fully occupied.
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

        // 2. Stable ladder: fixed low ports (8443, 18443, 28443) the OS never
        // hands out as ephemeral. main() persists whatever we bind here and the
        // next boot prefers it, so a port an outgoing connection can be holding
        // makes the local address drift on every restart — hence this rung
        // before the high ranges below.
        if (!m_HttpsServer) {
            for (quint16 p : PortFallback::kHttps) {
                m_HttpsServer = tryHttpsPort(p);
                if (m_HttpsServer) {
                    m_ActiveHttpsPort = p;
                    break;
                }
            }
        }

        // 3. Fallback range 1: 49443 to 65443, step 1000
        if (!m_HttpsServer) {
            for (quint16 p = 49443; p <= 65443; p += 1000) {
                m_HttpsServer = tryHttpsPort(p);
                if (m_HttpsServer) {
                    m_ActiveHttpsPort = p;
                    break;
                }
            }
        }

        // 4. Fallback range 2: 49152 to 65535, step 1
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

quint16 HttpServer::authorizeTunnelWebSocket(const HttpRequest& req, QString* outError)
{
    auto refuse = [outError](const QString& why) -> quint16 {
        if (outError) *outError = why;
        return 0;
    };

    // Named before the shape test below, which would answer "not a signalling
    // path" — true, but it hides the actual reason. This channel exists so a
    // second launch on the host machine surfaces admin in the tab already open;
    // there is no such thing to surface for a browser somewhere else.
    if (req.path == QLatin1String("/ws/control"))
        return refuse(QStringLiteral("the control channel is host-local"));

    bool wsIsRelay = false;
    const int slot = slotFromWsPath(req.path, &wsIsRelay);
    if (slot < 0) return refuse(QStringLiteral("not a signalling path"));

    // The legacy WebSocket video relay is deliberately unreachable here. It
    // exists so a browser whose UDP is blocked can still receive frames over the
    // same TLS connection that serves the page — and there is no such connection
    // through a tunnel. Its replacement on this path is ICE-TCP, which the
    // stream's own peer connection negotiates. Carrying video through the
    // control channel instead would put every frame through one SCTP stream that
    // is also answering API calls.
    if (wsIsRelay) return refuse(QStringLiteral("the legacy video relay is not carried"));

    quint16 targetPort = m_SignalingPort;
    if (slot > 0) {
        targetPort = m_SlotPorts.value(slot).signaling;
        if (targetPort == 0) return refuse(QStringLiteral("no such slot"));
    }

    const bool playerSlot = slot >= m_FirstPlayerSlot && slot < m_FirstExtraSlot;
    if (playerSlot) {
        if (!m_PlayerSlotAuth || !m_PlayerSlotAuth(req, slot)) {
            m_ConnGuard.reportAuthFailure(req.clientAddress);
            return refuse(QStringLiteral("not authorised for this player slot"));
        }
        return targetPort;
    }

    // No local exemption on this path, ever — see Arrival. A session is the only
    // thing that opens it.
    if (m_AuthManager && !m_AuthManager->validateSession(sessionTokenFromRequest(req))) {
        m_ConnGuard.reportAuthFailure(req.clientAddress);
        return refuse(QStringLiteral("authentication required"));
    }
    return targetPort;
}

QMap<QString, QString> HttpServer::securityHeaders(const QString& hostHeader)
{
    // The page may only open a WebSocket back to the origin it was served from.
    // Spelled out as wss://<host> rather than relying on 'self' to cover ws/wss:
    // CSP3 says it does, but WebKit has historically not honoured that, and this
    // header gates the streaming signaling channel on iOS.
    //
    // Over the rendezvous tunnel the origin is the introduction server's, and
    // that is deliberate rather than an oversight: the page really does hold one
    // WebSocket there — the signalling line — and it is the ONLY thing it may
    // open. Everything else it needs arrives through the tunnel this policy
    // travels on.
    const QString wsOrigin = RequestGuard::normalizeAuthority(hostHeader);
    const QString connectSrc = isPlainAuthority(wsOrigin)
                                   ? QStringLiteral("'self' wss://%1").arg(wsOrigin)
                                   : QStringLiteral("'self'");

    QMap<QString, QString> headers;
    // No Access-Control-Allow-Origin: the frontend is served same-origin by this
    // server, so CORS is never needed. Omitting it prevents any cross-origin page
    // from reading API responses.
    headers["X-Content-Type-Options"] = "nosniff";
    headers["X-Frame-Options"] = "DENY";
    headers["Referrer-Policy"] = "strict-origin-when-cross-origin";
    // 'wasm-unsafe-eval' allows WebAssembly compilation only (not JS eval) —
    // required by the WASM Opus decoder fallback used on iOS/WebKit.
    // Google Fonts: stylesheet from fonts.googleapis.com, font files from
    // fonts.gstatic.com (graceful fallback to system fonts if offline).
    headers["Content-Security-Policy"] =
        QStringLiteral(
            "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' "
            "https://fonts.gstatic.com; img-src 'self' data: blob:; connect-src %1; "
            "worker-src 'self' blob:; frame-ancestors 'none'; base-uri 'self'; form-action 'self'; "
            "object-src 'none'")
            .arg(connectSrc);
    headers["Strict-Transport-Security"] = "max-age=31536000; includeSubDomains; preload";
    return headers;
}

QString HttpServer::cookieFromRequest(const HttpRequest& req, const QString& name)
{
    QString cookie = req.headers.value("cookie");
    if (cookie.isEmpty()) return {};

    const QString prefix = name + QLatin1Char('=');
    // Cookies are separated by "; " or ";"
    QStringList cookies = cookie.split(";");
    for (const QString& c : cookies) {
        QString trimmed = c.trimmed();
        if (trimmed.startsWith(prefix, Qt::CaseInsensitive)) return trimmed.mid(prefix.length());
    }
    return {};
}

QString HttpServer::sessionTokenFromRequest(const HttpRequest& req)
{
    return cookieFromRequest(req, QStringLiteral("mw_session"));
}

void HttpServer::reportAuthFailure(const QString& ip)
{
    m_ConnGuard.reportAuthFailure(ip);
}

int HttpServer::slotFromWsPath(const QString& path, bool* outIsRelay)
{
    // "/ws" and "/ws/stream" are slot 0; "/ws3" and "/ws3/stream" are slot 3.
    // Anything else (including "/ws/control") is not a stream socket.
    QString rest = path;
    if (!rest.startsWith(QLatin1String("/ws"))) return -1;
    rest = rest.mid(3);

    bool isRelay = false;
    if (rest.endsWith(QLatin1String("/stream"))) {
        isRelay = true;
        rest.chop(7);
    }
    if (outIsRelay) *outIsRelay = isRelay;

    if (rest.isEmpty()) return 0;
    bool ok = false;
    const int slot = rest.toInt(&ok);
    return (ok && slot > 0) ? slot : -1;
}

bool HttpServer::isAuthenticated(const HttpRequest& req) const
{
    if (!m_AuthManager) return true; // No auth manager = auth disabled

    QString token = sessionTokenFromRequest(req);
    return !token.isEmpty() && m_AuthManager->validateSession(token);
}

bool HttpServer::adminKeyMatches(const HttpRequest& req) const
{
    const QByteArray given = req.headers.value(QStringLiteral("x-mw-admin-key")).toUtf8();
    const QByteArray expected = m_AdminKey.toUtf8();
    if (expected.isEmpty()) return false; // fail closed

    // Constant-time: fold the length difference into the accumulator so the
    // running time never depends on where the first mismatch is.
    const int n = qMax(given.size(), expected.size());
    quint8 diff = static_cast<quint8>(given.size() ^ expected.size());
    for (int i = 0; i < n; ++i) {
        const quint8 a = i < given.size() ? static_cast<quint8>(given[i]) : 0;
        const quint8 b = i < expected.size() ? static_cast<quint8>(expected[i]) : 0;
        diff |= static_cast<quint8>(a ^ b);
    }
    return diff == 0;
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
    const QStringList headerLines = headerPart.split("\r\n");

    bool haveContentLength = false;
    bool haveTransferEncoding = false;
    for (const QString& line : headerLines) {
        if (!haveContentLength && line.startsWith("Content-Length:", Qt::CaseInsensitive)) {
            contentLength = line.mid(15).trimmed().toInt();
            haveContentLength = true;
        } else if (line.startsWith("Transfer-Encoding:", Qt::CaseInsensitive)) {
            const QString coding = line.section(QLatin1Char(':'), 1).trimmed();
            if (coding.compare(QLatin1String("identity"), Qt::CaseInsensitive) != 0)
                haveTransferEncoding = true;
        }
    }

    // Bodies are framed by Content-Length alone — chunked coding is not
    // implemented. Two cases must not be waved through:
    //
    //   Both headers present: the framing is ambiguous, and a front-end proxy
    //   that honours Transfer-Encoding while we honour Content-Length is exactly
    //   the disagreement request smuggling is built on. Nothing legitimate sends
    //   both.
    //
    //   Transfer-Encoding alone on a request that carries a body: we would read
    //   a zero-length body and hand the handler a truncated request. It has
    //   never worked; say so rather than misparse it.
    //
    // A bodyless GET/HEAD carrying a stray Transfer-Encoding is left alone: some
    // tunnels add it blanket-style, and there is no body for us to get wrong.
    if (haveTransferEncoding) {
        const QString method = headerPart.section(QLatin1Char(' '), 0, 0).trimmed().toUpper();
        const bool bodyExpected = method != QLatin1String("GET") && method != QLatin1String("HEAD");
        if (haveContentLength) {
            sendResponse(socket, HttpResponse::error(400, "Ambiguous message framing"));
            return;
        }
        if (bodyExpected) {
            sendResponse(socket, HttpResponse::error(501, "Transfer coding not supported"));
            return;
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

    // A request target carrying control characters is refused before anything
    // reads it: the redirect below puts the path in a Location header, and a
    // smuggled CR/LF would let the caller append headers of their own.
    if (req.malformed) {
        Logger::warning(QString("[HttpServer] Malformed request target refused from %1")
                            .arg(req.clientAddress));
        m_ConnGuard.reportAuthFailure(req.clientAddress);
        sendResponse(socket, HttpResponse::error(400, "Bad Request"));
        return;
    }

    const bool peerLocal = isLocalRequest(req.clientAddress);

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
    //
    // It lives here rather than in serveRequest() because it is a property of
    // the socket the request arrived on, not of the request: a tunnel carries no
    // scheme of its own, and there is nowhere for it to redirect to.
    if (!qobject_cast<QSslSocket*>(socket) && m_ActiveHttpsPort > 0) {
        QString host = req.headers.value("host");
        int portSep = host.lastIndexOf(':');
        QString hostname = (portSep >= 0) ? host.left(portSep) : host;

        bool isLocalClient = peerLocal;
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

    const QString hostHeader = req.headers.value("host");
    m_PendingAsyncSockets.insert(socket);
    QTimer::singleShot(ASYNC_TIMEOUT_MS, socket, [this, socket]() {
        if (m_PendingAsyncSockets.contains(socket)) {
            qWarning() << "[HttpServer] Async timeout for" << socket
                       << "peer=" << socket->peerAddress().toString();
            m_PendingAsyncSockets.remove(socket);
            sendResponse(socket, HttpResponse::error(504, "Gateway Timeout"));
        }
    });

    serveRequest(std::move(req), Arrival::Socket,
                 [this, socket, hostHeader](const HttpResponse& resp) {
                     if (m_PendingAsyncSockets.contains(socket)) {
                         m_PendingAsyncSockets.remove(socket);
                         sendResponse(socket, resp, hostHeader);
                     } else {
                         // The socket is no longer pending because it disconnected
                         // mid-request: onDisconnected() already did
                         // socket->deleteLater(), so by the time this async response
                         // arrives the QTcpSocket is a FREED QObject. Stream its
                         // ADDRESS as a plain void* — never the QObject* itself,
                         // because QDebug::operator<<(const QObject*) dereferences it
                         // (className/objectName) and would crash on the dangling
                         // object (the UAF that took the whole server down on
                         // browser-close double /quit).
                         qWarning() << "[HttpServer] Respond called but socket no longer pending "
                                       "— response discarded"
                                    << "socket=" << static_cast<const void*>(socket)
                                    << "status=" << resp.statusCode;
                     }
                 });
}

void HttpServer::serveRequest(HttpRequest req, Arrival arrival, ResponseCallback respond)
{
    // ── Access decision ───────────────────────────────────────────────────────
    // The local privilege below is granted on the source IP alone, and a browser
    // running on the host lends that IP to every page it has open: without these
    // checks any website could drive the whole admin API through the victim's
    // browser (no CORS header of ours is involved — the response being
    // unreadable does not stop the request from taking effect). The rules live
    // in RequestGuard::evaluate so they can be tested as a unit.
    //
    // A tunnel request is handed to us by code running on this machine, so every
    // signal that says "local" would say so for a visitor on the other side of
    // the world. All three of them are therefore forced shut here — §4.3 of the
    // architecture, and the reason it is a rule and not a heuristic:
    //
    //   peerLocal   the address is ours, never the browser's
    //   hostSession the host key is only readable from the host machine, so a
    //               tunnel caller cannot have obtained it — but "cannot" is a
    //               property of another route's guard, and privileges should not
    //               rest on a second file's correctness
    //   adminKeyOk  the per-run admin key is only ever served to local callers
    //
    // What remains reachable from the tunnel is what remote access has always
    // been: a session established with the PIN. Admin still needs the machine
    // itself or the LAN unlock, exactly as it does under the public domain today.
    const bool viaTunnel = (arrival == Arrival::Tunnel);
    req.viaTunnel = viaTunnel;

    // A target carrying control characters is refused before anything reads it.
    // The socket path already catches this at parse time; the tunnel builds its
    // requests itself, so the check belongs here too rather than resting on the
    // caller having done it.
    if (req.malformed || req.path.contains(QLatin1Char('\r')) ||
        req.path.contains(QLatin1Char('\n'))) {
        Logger::warning(QString("[HttpServer] Malformed request target refused from %1")
                            .arg(req.clientAddress));
        m_ConnGuard.reportAuthFailure(req.clientAddress);
        respond(HttpResponse::error(400, "Bad Request"));
        return;
    }

    const bool peerLocal = !viaTunnel && isLocalRequest(req.clientAddress);
    const QString sessionToken = sessionTokenFromRequest(req);
    RequestGuard::Context ctx;
    ctx.peerLocal = peerLocal;
    ctx.hostSession = !viaTunnel && m_AuthManager && m_AuthManager->isHostSession(sessionToken);
    ctx.adminSession = m_AuthManager && m_AuthManager->isAdminSession(sessionToken);
    ctx.adminKeyOk = !viaTunnel && adminKeyMatches(req);
    ctx.publicDomain = m_Certs.domain();

    const RequestGuard::Decision decision =
        RequestGuard::evaluate(RequestGuard::describe(req), ctx);

    if (decision.outcome != RequestGuard::Outcome::Allow) {
        if (decision.outcome == RequestGuard::Outcome::BlockCrossSite) {
            Logger::warning(QString("[HttpServer] Cross-site request refused: %1 %2 (origin='%3')")
                                .arg(req.method, req.path, req.headers.value("origin")));
            m_ConnGuard.reportAuthFailure(req.clientAddress);
        }
        QJsonObject obj;
        obj["error"] = RequestGuard::blockError(decision.outcome);
        respond(HttpResponse::json(obj, RequestGuard::blockStatus(decision.outcome)));
        return;
    }
    if (decision.hostUntrusted) {
        Logger::warning(QString("[HttpServer] Local privilege denied for untrusted Host '%1' "
                                "(possible DNS rebinding) on %2")
                            .arg(req.headers.value("host"), req.path));
    }

    const bool localPrivilege = decision.localPrivilege;
    req.isLocal = decision.adminPrivilege;
    req.isHostMachine = decision.hostMachine;
    req.hostTrusted = decision.hostTrusted;

    // The frontend fetches the admin key here. Handled inline rather than as a
    // route so it can never be reached without the checks above.
    if (req.path == QLatin1String("/api/admin/token")) {
        QJsonObject obj;
        HttpResponse resp;
        switch (RequestGuard::adminTokenReply(decision, ctx, isAuthenticated(req))) {
        case RequestGuard::AdminTokenReply::Grant:
            obj["token"] = m_AdminKey;
            resp = HttpResponse::json(obj);
            break;
        case RequestGuard::AdminTokenReply::Empty:
            // A valid session that simply has no admin rights (a remote client
            // that has not unlocked with the admin password). Answer plainly
            // with no key — and do NOT count it as an auth failure, or
            // ConnectionGuard would ban a legitimate remote user from their own
            // server after AUTHFAIL_MAX page loads.
            resp = HttpResponse::json(obj);
            break;
        case RequestGuard::AdminTokenReply::Deny:
            m_ConnGuard.reportAuthFailure(req.clientAddress);
            obj["error"] = "forbidden";
            resp = HttpResponse::json(obj, 403);
            break;
        }
        resp.headers["Cache-Control"] = "no-store";
        respond(resp);
        return;
    }

    // The set of files that make up the application, for a browser that has to
    // pull all of them through a data channel before it can start. Answered
    // inline, before the session gate, because every file it names is already
    // served to anyone who asks — the list adds no reach, only the ability to
    // fetch them in one pass instead of discovering them one redirect at a time.
    if (req.path == QLatin1String("/api/app/manifest")) {
        QJsonArray files;
        const QStringList paths = m_StaticFiles->listFiles();
        for (const QString& p : paths) files.append(p);

        HttpResponse manifest = HttpResponse::json(
            QJsonObject{{QStringLiteral("version"), QCoreApplication::applicationVersion()},
                        {QStringLiteral("files"), files}});
        manifest.headers["Cache-Control"] = "no-store";
        respond(manifest);
        return;
    }

    if (!req.path.startsWith("/api/")) {
        // Static assets answer reads and nothing else. Every write verb on them
        // was already a no-op that returned the app shell with 200; saying 405
        // states the contract instead of implying the request did something.
        if (req.method != QLatin1String("GET") && req.method != QLatin1String("HEAD")) {
            HttpResponse notAllowed = HttpResponse::error(405, "Method Not Allowed");
            notAllowed.headers["Allow"] = "GET, HEAD";
            respond(notAllowed);
            return;
        }

        // /version.json is synthesised from the running app version (single
        // source of truth: MW_VERSION, baked in at build from the git tag) so
        // it never needs hand-bumping. The frontend's VersionGuard polls it and
        // reloads when it changes, pulling a fresh app after every update.
        if (req.path == "/version.json") {
            HttpResponse resp;
            resp.statusCode = 200;
            resp.contentType = "application/json";
            resp.body =
                "{\"version\":\"" + QCoreApplication::applicationVersion().toUtf8() + "\"}\n";
            resp.headers["Cache-Control"] = "no-cache, must-revalidate";
            respond(resp);
            return;
        }

        const QString ifNoneMatch = req.headers.value("if-none-match");
        HttpResponse resp = m_StaticFiles->serveFile(req.path, ifNoneMatch);
        // SPA fallback: a path with no file extension is a frontend route
        // (/admin, /settings) and gets index.html so the History API can pick it
        // up on reload. A path that names a file is a genuine miss and must say
        // so — answering 200 with the app shell for /settings.json or /.git/config
        // makes every probe look like a hit, both to a scanner and to a reader
        // trying to tell a real leak from the fallback.
        const QString lastSegment = req.path.section(QLatin1Char('/'), -1);
        const bool looksLikeFile = lastSegment.contains(QLatin1Char('.'));
        if (resp.statusCode == 404 && !looksLikeFile)
            resp = m_StaticFiles->serveFile("/", ifNoneMatch);
        // HEAD is the same response without the representation.
        if (req.method == QLatin1String("HEAD")) resp.body.clear();
        respond(resp);
        return;
    }

    // ── Auth check for API routes ──────────────────────────────────────────
    // Exemptions: localhost, /api/auth/*, /api/health, /api/server/hostname.
    // Only /api/server/hostname is public (the login screen displays the PC name
    // before authentication); /api/server/status (ports) now requires a session.
    // /api/share/player/* is the one other public surface: a player has no
    // MoonlightWeb session and never gets one — those routes authenticate the
    // device from its own mw_player cookie and report their own failures.
    if (m_AuthManager && !localPrivilege && req.path != "/api/health" &&
        req.path != "/api/server/hostname" && !req.path.startsWith("/api/auth/") &&
        !req.path.startsWith("/api/share/player/") && !isAuthenticated(req)) {
        // Unauthenticated remote API hit = credential scanning; feed the guard
        // so repeated failures ban the source IP.
        m_ConnGuard.reportAuthFailure(req.clientAddress);
        QJsonObject obj;
        obj["error"] = "authentication_required";
        respond(HttpResponse::json(obj, 401));
        return;
    }

    m_Router->dispatchAsync(req, [respond](const HttpResponse& resp) {
        // API answers are per-session state (host list, sessions, settings)
        // and several are privilege-dependent — the very same URL returns
        // less to a remote client than to the host. Nothing may keep a copy:
        // no-store also keeps them out of the browser's on-disk cache, where
        // they would outlive the session that was allowed to see them.
        // Handlers that set their own policy keep it.
        HttpResponse out = resp;
        if (!out.headers.contains("Cache-Control")) out.headers["Cache-Control"] = "no-store";
        respond(out);
    });
}

void HttpServer::handleWebSocketUpgrade(QTcpSocket* clientSocket, const QByteArray& requestData)
{
    const HttpRequest up = HttpParser::parse(requestData);
    const QString peerAddr = clientSocket->peerAddress().toString();

    // Same refusal as the REST path: a control character in the target means the
    // handshake was crafted, and it is forwarded verbatim to the WS server below.
    if (up.malformed) {
        Logger::warning(QString("[HttpServer] Malformed WebSocket upgrade target refused from %1")
                            .arg(peerAddr));
        m_ConnGuard.reportAuthFailure(peerAddr);
        sendResponse(clientSocket, HttpResponse::error(400, "Bad Request"));
        return;
    }

    // ── Origin check ──────────────────────────────────────────────────────────
    // WebSockets are not subject to the same-origin policy: any page may open
    // one to any host, and no CORS negotiation stands in the way. Since a local
    // peer skips the session check below, a page open in the host's browser
    // would otherwise reach the signaling channel directly — which relays
    // keyboard and mouse events to the streamed desktop. Browsers always send
    // Origin on the handshake and a page cannot alter it.
    if (!RequestGuard::isWebSocketOriginAllowed(up.headers.value("origin"),
                                                up.headers.value("host"))) {
        Logger::warning(QString("[HttpServer] WebSocket upgrade refused: origin '%1' does not "
                                "match host '%2'")
                            .arg(up.headers.value("origin"), up.headers.value("host")));
        m_ConnGuard.reportAuthFailure(peerAddr);
        QJsonObject obj;
        obj["error"] = "cross_site_request_blocked";
        sendResponse(clientSocket, HttpResponse::json(obj, 403));
        return;
    }

    // Parse the WebSocket path from the upgrade request to determine the target.
    //   GET /ws          → proxy to m_SignalingPort (WebRTC signaling, slot 0)
    //   GET /ws/stream   → proxy to m_StreamRelayPort (legacy WSS, slot 0)
    //   GET /ws/control  → proxy to m_ControlPort (single-tab dedup channel)
    //   GET /wsN         → proxy to slot N's signaling (1 = owner standby,
    //   GET /wsN/stream  → proxy to slot N's legacy relay   2.. = players)
    QString firstLine = QString::fromUtf8(requestData.left(requestData.indexOf("\r\n")));
    QString path = firstLine.section(' ', 1, 1);

    bool wsIsRelay = false;
    const int wsSlot = slotFromWsPath(path, &wsIsRelay);
    // Player slots are the bounded range [firstPlayerSlot, firstExtraSlot): those
    // callers hold an mw_player cookie and nothing else. An "extra" slot at or
    // above firstExtraSlot is the OWNER streaming a second host, so it must be
    // authenticated as the owner (session / local privilege), NOT gated behind a
    // player cookie it will never have — which otherwise 401s its /wsN upgrade.
    const bool playerSlot = wsSlot >= m_FirstPlayerSlot && wsSlot < m_FirstExtraSlot;

    quint16 targetPort = m_SignalingPort;
    if (path == QLatin1String("/ws/control")) {
        targetPort = m_ControlPort;
    } else if (wsSlot == 0) {
        targetPort = wsIsRelay ? m_StreamRelayPort : m_SignalingPort;
    } else if (wsSlot > 0) {
        const SlotPorts ports = m_SlotPorts.value(wsSlot);
        targetPort = wsIsRelay ? ports.relay : ports.signaling;
        if (targetPort == 0) {
            Logger::warning(
                QString("[HttpServer] WebSocket upgrade for unconfigured slot %1").arg(wsSlot));
            sendResponse(clientSocket, HttpResponse::error(404, "Not Found"));
            return;
        }
    }

    // ── Auth check: validate the caller before proxying the WS upgrade ────
    // Same Host requirement as the REST path: a rebound name must not inherit
    // the local peer's exemption, and neither must the public domain — behind a
    // TLS-terminating tunnel every visitor arrives from loopback under it, and
    // this channel carries keyboard and mouse. The host machine reaching itself
    // through the domain still passes: it holds a host-key session.
    //
    // A player's slot is the exception: those callers have no MoonlightWeb
    // session and never will, so the mw_player cookie bound to that slot's
    // activation is what opens it — and nothing else, not even a local peer.
    const bool localPrivilege = HttpServer::isLocalRequest(peerAddr) &&
                                RequestGuard::isLocalHostName(up.headers.value("host"));
    if (playerSlot) {
        if (!m_PlayerSlotAuth || !m_PlayerSlotAuth(up, wsSlot)) {
            m_ConnGuard.reportAuthFailure(peerAddr);
            QJsonObject obj;
            obj["error"] = "authentication_required";
            sendResponse(clientSocket, HttpResponse::json(obj, 401));
            return;
        }
    } else if (m_AuthManager && !localPrivilege) {
        if (!m_AuthManager->validateSession(sessionTokenFromRequest(up))) {
            m_ConnGuard.reportAuthFailure(peerAddr);
            QJsonObject obj;
            obj["error"] = "authentication_required";
            HttpResponse resp = HttpResponse::json(obj, 401);
            sendResponse(clientSocket, resp);
            return;
        }
    }

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

    // Guard flag: cleanup is called at most once, regardless of which signal
    // fires first (client disconnect, target disconnect, target error). A
    // shared_ptr (not a raw new/delete) is required because the socket
    // destructors triggered by deleteLater() below emit disconnected() again
    // *from inside ~QAbstractSocket* — that re-entrant emission still reaches
    // this lambda (connections are only severed later, in ~QObject). With a raw
    // guard that had already been deleted, the re-entrant call read freed memory
    // and then dereferenced the half-destroyed socket via state() → SIGSEGV
    // (observed on macOS during WSS transport churn). The shared_ptr keeps the
    // flag alive for the lifetime of every connection that captured it, so the
    // re-entrant call sees *guard == true and returns immediately.
    auto guard = std::make_shared<bool>(false);

    auto cleanup = [clientSocket, target, guard]() {
        if (*guard) return;
        *guard = true;
        if (clientSocket->state() == QAbstractSocket::ConnectedState)
            clientSocket->disconnectFromHost();
        if (target->state() == QAbstractSocket::ConnectedState) target->disconnectFromHost();
        target->deleteLater();
        clientSocket->deleteLater();
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

void HttpServer::sendResponse(QTcpSocket* socket, const HttpResponse& response,
                              const QString& hostHeader)
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
    case 304: statusText = "Not Modified"; break;
    case 307: statusText = "Temporary Redirect"; break;
    case 400: statusText = "Bad Request"; break;
    case 401: statusText = "Unauthorized"; break;
    case 403: statusText = "Forbidden"; break;
    case 404: statusText = "Not Found"; break;
    case 405: statusText = "Method Not Allowed"; break;
    case 413: statusText = "Payload Too Large"; break;
    case 415: statusText = "Unsupported Media Type"; break;
    case 429: statusText = "Too Many Requests"; break;
    case 431: statusText = "Request Header Fields Too Large"; break;
    case 500: statusText = "Internal Server Error"; break;
    case 501: statusText = "Not Implemented"; break;
    case 504: statusText = "Gateway Timeout"; break;
    default: statusText = "Unknown"; break;
    }

    // Defaults first, then the handler's own headers on top: a handler that sets
    // Cache-Control or a stricter policy wins, and neither can be emitted twice.
    QMap<QString, QString> headers = securityHeaders(hostHeader);
    headers["Connection"] = "close";

    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        headers[it.key()] = it.value();

    respData.append("HTTP/1.1 " + QByteArray::number(response.statusCode) + " " +
                    statusText.toUtf8() + "\r\n");
    // 304 Not Modified carries no body and no content type (the browser reuses
    // its cached representation); other responses always describe their body.
    if (!response.contentType.isEmpty())
        respData.append("Content-Type: " + sanitizeHeaderValue(response.contentType).toUtf8() +
                        "\r\n");
    respData.append("Content-Length: " + QByteArray::number(response.body.size()) + "\r\n");

    // Last barrier against header injection: whatever a handler put in a header
    // value, it cannot end the line and start one of its own. The request path is
    // already refused at parse time, so this only ever fires on a bug.
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        respData.append(sanitizeHeaderValue(it.key()).toUtf8() + ": " +
                        sanitizeHeaderValue(it.value()).toUtf8() + "\r\n");

    respData.append("\r\n");
    respData.append(response.body);

    socket->write(respData);
    socket->flush();
    socket->disconnectFromHost();
}
