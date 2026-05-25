#include "HttpServer.h"
#include "RestRouter.h"
#include "StaticFileHandler.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslConfiguration>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

// --- SslServer: creates QSslSocket directly from native handle ----------------
// Avoids descriptor-transfer hack (get descriptor → setSocketDescriptor(-1) →
// recreate QSslSocket) which fails on Windows because QTcpSocket's
// setSocketDescriptor(-1) calls closesocket(), invalidating the handle.
class SslServer : public QTcpServer
{
public:
    using SslReadyCallback = std::function<void(QSslSocket*)>;

    SslServer(const QSslConfiguration& sslConfig,
              SslReadyCallback onSslReady,
              QObject* parent = nullptr)
        : QTcpServer(parent)
        , m_SslConfig(sslConfig)
        , m_OnSslReady(std::move(onSslReady))
    {}

protected:
    void incomingConnection(qintptr handle) override
    {
        QSslSocket* ssl = new QSslSocket(this);
        if (!ssl->setSocketDescriptor(handle)) {
            Logger::warning("[HTTPS] SslServer: failed to set socket descriptor");
            delete ssl;
            return;
        }

        ssl->setSslConfiguration(m_SslConfig);
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

        ssl->startServerEncryption();
    }

private:
    QSslConfiguration m_SslConfig;
    SslReadyCallback m_OnSslReady;
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
    // Writable path first (AppData): catches Let's Encrypt certs from lego
    // and self-signed certs generated at runtime — essential for MSI installs
    // where the application directory is read-only.
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QStringList candidates = {
        appData + "/cert/",
        QString(CERT_DIR),
        QCoreApplication::applicationDirPath() + "/cert/",
        QCoreApplication::applicationDirPath() + "/../cert/",
        QString(FRONTEND_DIR) + "/../backend/cert/",
    };

    for (const auto& d : candidates) {
        if ((QFile::exists(d + "fullchain.pem") || QFile::exists(d + "cert.pem")) && QFile::exists(d + "key.pem"))
            return d;
    }
    return {};
}

bool HttpServer::loadCertFiles(const QString& certDir)
{
    QString keyPath = certDir + "key.pem";

    // Try fullchain.pem first (includes intermediate CA chain), fallback to cert.pem
    QString certPath = certDir + "fullchain.pem";
    if (!QFile::exists(certPath))
        certPath = certDir + "cert.pem";

    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open cert file: " + certFile.errorString());
        return false;
    }
    QList<QSslCertificate> chain = QSslCertificate::fromDevice(&certFile, QSsl::Pem);
    certFile.close();

    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open key file: " + keyFile.errorString());
        return false;
    }
    QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    keyFile.close();

    if (chain.isEmpty() || key.isNull()) {
        Logger::warning("SSL cert chain / key invalid");
        return false;
    }

    m_SslConfig = QSslConfiguration::defaultConfiguration();
    m_SslConfig.setLocalCertificateChain(chain);
    m_SslConfig.setPrivateKey(key);
    m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

    Logger::info("SSL certificate (" + certPath.section('/', -1) + ") loaded from " + certDir);
    return true;
}

bool HttpServer::loadCert()
{
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
    QString certDir = findCertDir();
    if (certDir.isEmpty()) {
        Logger::warning("[TLS] No certificate directory found, cannot reload");
        return false;
    }
    return loadCertFiles(certDir);
}

bool HttpServer::generateSelfSignedCert()
{
    // Use AppData for writability — Program Files is read-only for MSI installs.
    QString certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cert/";
    QDir().mkpath(certDir);

    QProcess gen;
    gen.setProcessChannelMode(QProcess::MergedChannels);
    gen.start("openssl", QStringList()
        << "req" << "-x509" << "-newkey" << "rsa:2048"
        << "-keyout" << (certDir + "key.pem")
        << "-out" << (certDir + "cert.pem")
        << "-days" << "365" << "-nodes"
        << "-subj" << "/CN=Moonlight-Web");

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
        Logger::error(QString("openssl failed (exit %1): %2")
            .arg(gen.exitCode())
            .arg(QString::fromUtf8(output).trimmed()));
        return false;
    }

    Logger::info("Self-signed certificate generated in " + certDir);
    return loadCertFiles(certDir);
}

bool HttpServer::start(quint16 preferredHttpsPort)
{
    Logger::info(QString("Qt SSL support=%1 build=%2 runtime=%3")
        .arg(QSslSocket::supportsSsl() ? "yes" : "NO")
        .arg(QSslSocket::sslLibraryBuildVersionString())
        .arg(QSslSocket::sslLibraryVersionString()));

    m_HttpsPort = preferredHttpsPort;
    bool hasHttps = loadCert();

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
            Logger::warning("HTTP port " + QString::number(m_HttpPort)
                            + " unavailable (" + m_HttpServer->errorString()
                            + "), scanning fallback range...");
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
            connect(m_HttpServer, &QTcpServer::newConnection,
                    this, &HttpServer::onHttpConnection);
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
                m_SslConfig,
                [this](QSslSocket* socket) {
                    m_Buffers[socket] = QByteArray();
                    connect(socket, &QSslSocket::readyRead, this, &HttpServer::onReadyRead);
                    connect(socket, &QSslSocket::disconnected, this, &HttpServer::onDisconnected);
                    if (socket->bytesAvailable() > 0)
                        onReadyReadSocket(socket);
                },
                this
            );
            if (ssl->listen(QHostAddress::Any, port))
                return ssl;
            delete ssl;
            return nullptr;
        };

        // 1. Try the preferred port (default 443, or from settings.json)
        Logger::info("HTTPS attempting preferred port " + QString::number(preferredHttpsPort));
        m_HttpsServer = tryHttpsPort(preferredHttpsPort);
        m_ActiveHttpsPort = preferredHttpsPort;

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
    Logger::info(QString("Changing HTTPS port from %1 to %2...")
        .arg(oldPort)
        .arg(newPort));

    m_HttpsPort = newPort;
    stop();

    if (!start(newPort)) {
        Logger::error(QString("Failed to bind new HTTPS port %1, falling back to %2")
            .arg(newPort).arg(oldPort));
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
    if (h.isEmpty()) return true;  // Missing Host header → assume LAN

    // Localhost
    if (h == "localhost" || h == "127.0.0.1" || h == "::1")
        return true;

    QHostAddress addr(h);
    if (addr.isNull()) return false;  // Not an IP → public domain (e.g. tunnel endpoint)

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
    if (headerEnd == -1) return;

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
                       << "peer=" << socket->peerAddress().toString()
                       << ":" << socket->peerPort()
                       << "bytesToWrite=" << socket->bytesToWrite();
        }
        socket->deleteLater();
    }
}

void HttpServer::processRequest(QTcpSocket* socket, const QByteArray& requestData)
{
    HttpRequest req = parseRequest(requestData);

    // HTTP→HTTPS redirect for plain HTTP connections from LAN/local clients.
    // Only redirect when the user typed http:// in their browser (LAN/localhost).
    // Public tunnel connections are NOT redirected — the client is already on
    // HTTPS at the tunnel edge, and the tunnel forwards plain HTTP to us.
    if (!qobject_cast<QSslSocket*>(socket) && m_ActiveHttpsPort > 0) {
        QString host = req.headers.value("host");
        int portSep = host.lastIndexOf(':');
        QString hostname = (portSep >= 0) ? host.left(portSep) : host;

        if (isLanHost(hostname)) {
            QString location = QString("https://%1:%2%3")
                .arg(hostname)
                .arg(m_ActiveHttpsPort)
                .arg(req.path);
            HttpResponse resp;
            resp.statusCode = 307;
            resp.headers["Location"] = location;
            sendResponse(socket, resp);
            return;
        }
        // Public host (tunnel): fall through, serve directly
    }

    if (!req.path.startsWith("/api/")) {
        HttpResponse resp = m_StaticFiles->serveFile(req.path);
        // SPA fallback: for any non-API path that doesn't match a real file,
        // serve index.html so the frontend can handle its own routing via
        // the History API (e.g. /admin, /settings).
        if (resp.statusCode == 404)
            resp = m_StaticFiles->serveFile("/");
        sendResponse(socket, resp);
        return;
    }

    m_PendingAsyncSockets.insert(socket);
    qInfo() << "[HttpServer] processRequest — dispatching async, socket=" << socket
            << "method=" << req.method << "path=" << req.path;

    QTimer::singleShot(ASYNC_TIMEOUT_MS, socket, [this, socket]() {
        if (m_PendingAsyncSockets.contains(socket)) {
            qWarning() << "[HttpServer] Async timeout for" << socket
                       << "peer=" << socket->peerAddress().toString();
            m_PendingAsyncSockets.remove(socket);
            sendResponse(socket, HttpResponse::error(504, "Gateway Timeout"));
        }
    });

    m_Router->dispatchAsync(req, [this, socket](HttpResponse resp) {
        if (m_PendingAsyncSockets.contains(socket)) {
            m_PendingAsyncSockets.remove(socket);
            sendResponse(socket, resp);
        } else {
            qWarning() << "[HttpServer] Respond called but socket no longer pending — response discarded"
                       << "socket=" << socket << "status=" << resp.statusCode;
        }
    });
}

void HttpServer::handleWebSocketUpgrade(QTcpSocket* clientSocket, const QByteArray& requestData)
{
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
    QByteArray upgradeRequest = requestData;

    // Remove from our tracking — HttpServer should no longer manage this socket.
    m_Buffers.remove(clientSocket);
    m_PendingAsyncSockets.remove(clientSocket);

    // Disconnect HttpServer's handlers from this socket so they don't interfere
    // with the bidirectional proxy.
    QObject::disconnect(clientSocket, &QTcpSocket::readyRead,
                         this, &HttpServer::onReadyRead);
    QObject::disconnect(clientSocket, &QTcpSocket::disconnected,
                         this, &HttpServer::onDisconnected);

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
        if (target->state() == QAbstractSocket::ConnectedState)
            target->disconnectFromHost();
        target->deleteLater();
        clientSocket->deleteLater();
        delete guard;
    };

    // Pre-connect cleanup: if client disconnects before target connects,
    // this handler ensures the target socket is not left dangling.
    QObject::connect(clientSocket, &QTcpSocket::disconnected, cleanup);

    QObject::connect(target, &QTcpSocket::connected,
        [clientSocket, target, upgradeRequest, guard]() {
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
                [clientSocket, target]() {
                    target->write(clientSocket->readAll());
                });
            QObject::connect(target, &QTcpSocket::readyRead,
                [clientSocket, target]() {
                    clientSocket->write(target->readAll());
                });
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
    qInfo() << "[HttpServer] sendResponse, status=" << response.statusCode
            << "bodySize=" << response.body.size()
            << "socket=" << socket
            << "peer=" << (socket ? socket->peerAddress().toString() : "null")
            << "state=" << (socket ? socket->state() : -1);

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
    default:  statusText = "Unknown"; break;
    }

    respData.append("HTTP/1.1 " + QByteArray::number(response.statusCode) + " " + statusText.toUtf8() + "\r\n");
    respData.append("Content-Type: " + response.contentType.toUtf8() + "\r\n");
    respData.append("Content-Length: " + QByteArray::number(response.body.size()) + "\r\n");
    respData.append("Access-Control-Allow-Origin: *\r\n");
    respData.append("Connection: close\r\n");

    // Security headers
    respData.append("X-Content-Type-Options: nosniff\r\n");
    respData.append("X-Frame-Options: DENY\r\n");
    respData.append("Referrer-Policy: strict-origin-when-cross-origin\r\n");
    respData.append("Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self' wss:; worker-src 'self' blob:; frame-ancestors 'none'; base-uri 'self'; form-action 'self'\r\n");
    respData.append("Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n");

    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        respData.append(it.key().toUtf8() + ": " + it.value().toUtf8() + "\r\n");

    respData.append("\r\n");
    respData.append(response.body);

    socket->write(respData);
    socket->flush();
    socket->disconnectFromHost();
}
