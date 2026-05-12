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
    QString frontendDir = QString(FRONTEND_DIR);
    m_StaticFiles = new StaticFileHandler(frontendDir, this);
}

HttpServer::~HttpServer()
{
    stop();
}

QString HttpServer::findCertDir()
{
    QStringList candidates = {
        QString(CERT_DIR),
        QCoreApplication::applicationDirPath() + "/cert/",
        QCoreApplication::applicationDirPath() + "/../cert/",
        QString(FRONTEND_DIR) + "/../backend/cert/",
    };

    for (const auto& d : candidates) {
        if (QFile::exists(d + "cert.pem") && QFile::exists(d + "key.pem"))
            return d;
    }
    return {};
}

bool HttpServer::loadCertFiles(const QString& certDir)
{
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
        Logger::warning("SSL cert/key invalid");
        return false;
    }

    m_SslConfig = QSslConfiguration::defaultConfiguration();
    m_SslConfig.setLocalCertificate(cert);
    m_SslConfig.setPrivateKey(key);
    m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

    Logger::info("SSL certificate loaded from " + certDir);
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
    QString certDir = QCoreApplication::applicationDirPath() + "/cert/";
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
        auto* sslServer = new SslServer(
            m_SslConfig,
            [this](QSslSocket* ssl) {
                m_Buffers[ssl] = QByteArray();
                connect(ssl, &QSslSocket::readyRead, this, &HttpServer::onReadyRead);
                connect(ssl, &QSslSocket::disconnected, this, &HttpServer::onDisconnected);
                if (ssl->bytesAvailable() > 0)
                    onReadyReadSocket(ssl);
            },
            this
        );

        if (!sslServer->listen(QHostAddress::Any, m_HttpsPort)) {
            Logger::warning("HTTPS server failed to listen on port "
                            + QString::number(m_HttpsPort));
            delete sslServer;
        } else {
            Logger::info("HTTPS server on port " + QString::number(m_HttpsPort));
            m_HttpsServer = sslServer;
        }
    }

    emit started(m_HttpsPort);
    return true;
}

void HttpServer::stop()
{
    m_HttpServer->close();
    if (m_HttpsServer)
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

    if (!req.path.startsWith("/api/")) {
        HttpResponse resp = m_StaticFiles->serveFile(req.path);
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

    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        respData.append(it.key().toUtf8() + ": " + it.value().toUtf8() + "\r\n");

    respData.append("\r\n");
    respData.append(response.body);

    socket->write(respData);
    socket->flush();
    socket->disconnectFromHost();
}
