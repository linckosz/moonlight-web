#include "HttpServer.h"
#include "RestRouter.h"
#include "StaticFileHandler.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

// --- HttpServer --------------------------------------------------------------

HttpServer::HttpServer(quint16 httpPort, quint16 httpsPort, QObject* parent)
    : QObject(parent)
    , m_HttpServer(new QTcpServer(this))
    , m_HttpsServer(new QSslServer(this))
    , m_Router(new RestRouter(this))
    , m_HttpPort(httpPort)
    , m_HttpsPort(httpsPort)
{
    QString frontendDir = QString(FRONTEND_DIR);
    m_StaticFiles = new StaticFileHandler(frontendDir, this);
}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::loadCert()
{
    QStringList candidates = {
        QString(CERT_DIR),
        QCoreApplication::applicationDirPath() + "/cert/",
        QCoreApplication::applicationDirPath() + "/../cert/",
        QString(FRONTEND_DIR) + "/../backend/cert/",
    };

    QString certDir;
    for (const auto& d : candidates) {
        if (QFile::exists(d + "cert.pem") && QFile::exists(d + "key.pem")) {
            certDir = d;
            break;
        }
    }

    if (certDir.isEmpty()) {
        Logger::warning("SSL cert not found — HTTPS disabled");
        return false;
    }

    QString certPath = certDir + "cert.pem";
    QString keyPath = certDir + "key.pem";

    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open cert file: " + certFile.errorString());
        return false;
    }
    QSslCertificate cert(&certFile, QSsl::Pem);
    certFile.close();

    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open key file: " + keyFile.errorString());
        return false;
    }
    QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    keyFile.close();

    if (cert.isNull() || key.isNull()) {
        Logger::warning("SSL cert/key invalid — HTTPS disabled");
        return false;
    }

    m_SslConfig = QSslConfiguration::defaultConfiguration();
    m_SslConfig.setLocalCertificate(cert);
    m_SslConfig.setPrivateKey(key);
    m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

    m_HttpsServer->setSslConfiguration(m_SslConfig);

    Logger::info("SSL certificate loaded from " + certDir);
    return true;
}

bool HttpServer::start()
{
    Logger::info(QString("Qt SSL support=%1 build=%2 runtime=%3")
        .arg(QSslSocket::supportsSsl() ? "yes" : "NO")
        .arg(QSslSocket::sslLibraryBuildVersionString())
        .arg(QSslSocket::sslLibraryVersionString()));

    bool hasHttps = loadCert();

    if (!m_HttpServer->listen(QHostAddress::Any, m_HttpPort)) {
        emit serverError("Failed to listen on port " + QString::number(m_HttpPort)
                          + ": " + m_HttpServer->errorString());
        return false;
    }
    connect(m_HttpServer, &QTcpServer::newConnection,
            this, &HttpServer::onHttpConnection);
    Logger::info("HTTP redirect server on port " + QString::number(m_HttpPort));

    if (hasHttps) {
        if (!m_HttpsServer->listen(QHostAddress::Any, m_HttpsPort)) {
            Logger::warning("HTTPS server failed to listen on port "
                            + QString::number(m_HttpsPort));
        } else {
            Logger::info("HTTPS server on port " + QString::number(m_HttpsPort));
            // QSslServer emits startedEncryptionHandshake for each new connection.
            // We then wait for the socket's encrypted signal before processing requests.
            connect(m_HttpsServer, &QSslServer::startedEncryptionHandshake,
                    this, [this](QSslSocket* ssl) {
                Logger::info("[HTTPS] handshake starting");
                connect(ssl, &QSslSocket::encrypted, this, [this, ssl]() {
                    Logger::info("[HTTPS] TLS connection established");
                    m_Buffers[ssl] = QByteArray();
                    connect(ssl, &QSslSocket::readyRead, this, &HttpServer::onReadyRead);
                    connect(ssl, &QSslSocket::disconnected, this, &HttpServer::onDisconnected);
                    if (ssl->bytesAvailable() > 0)
                        onReadyReadSocket(ssl);
                });
                connect(ssl, &QAbstractSocket::errorOccurred,
                        [ssl](QAbstractSocket::SocketError err) {
                    Logger::warning("[HTTPS] Socket error: " + QString::number(err)
                                    + " " + ssl->errorString());
                });
            });
            connect(m_HttpsServer, &QSslServer::sslErrors,
                    [](QSslSocket* socket, const QList<QSslError>& errors) {
                for (const auto& e : errors)
                    Logger::warning("[HTTPS] SSL error: " + e.errorString());
                socket->ignoreSslErrors();
            });
        }
    }

    emit started(m_HttpsPort);
    return true;
}

void HttpServer::stop()
{
    m_HttpServer->close();
    m_HttpsServer->close();
    for (QTcpSocket* socket : m_Buffers.keys())
        socket->disconnectFromHost();
}

// --- HTTP redirect ----------------------------------------------------------

void HttpServer::onHttpConnection()
{
    while (QTcpSocket* socket = m_HttpServer->nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            QByteArray data = socket->readAll();
            QString host = "localhost";
            QString req = QString::fromUtf8(data);
            for (const QString& line : req.split("\r\n")) {
                if (line.startsWith("Host:", Qt::CaseInsensitive)) {
                    host = line.mid(5).trimmed();
                    int colon = host.indexOf(':');
                    if (colon >= 0) host = host.left(colon);
                    break;
                }
            }
            QString redirectUrl = QString("https://%1:%2/").arg(host).arg(m_HttpsPort);
            QByteArray resp;
            resp.append("HTTP/1.1 301 Moved Permanently\r\n");
            resp.append("Location: " + redirectUrl.toUtf8() + "\r\n");
            resp.append("Content-Length: 0\r\n");
            resp.append("Connection: close\r\n");
            resp.append("\r\n");
            socket->write(resp);
            socket->flush();
            socket->disconnectFromHost();
        });
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
        m_Buffers.remove(socket);
        m_PendingAsyncSockets.remove(socket);
        socket->deleteLater();
    }
}

void HttpServer::processRequest(QTcpSocket* socket, const QByteArray& requestData)
{
    HttpRequest req = parseRequest(requestData);

    if (!req.path.startsWith("/api/")) {
        HttpResponse resp = m_StaticFiles->serveFile(req.path);
        sendResponse(socket, resp);
        return;
    }

    m_PendingAsyncSockets.insert(socket);

    QTimer::singleShot(ASYNC_TIMEOUT_MS, socket, [this, socket]() {
        if (m_PendingAsyncSockets.contains(socket)) {
            m_PendingAsyncSockets.remove(socket);
            sendResponse(socket, HttpResponse::error(504, "Gateway Timeout"));
        }
    });

    m_Router->dispatchAsync(req, [this, socket](HttpResponse resp) {
        if (m_PendingAsyncSockets.contains(socket)) {
            m_PendingAsyncSockets.remove(socket);
            sendResponse(socket, resp);
        }
    });
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

    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        respData.append(it.key().toUtf8() + ": " + it.value().toUtf8() + "\r\n");

    respData.append("\r\n");
    respData.append(response.body);

    socket->write(respData);
    socket->flush();
    socket->disconnectFromHost();
}
