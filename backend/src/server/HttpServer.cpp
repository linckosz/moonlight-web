#include "HttpServer.h"
#include "RestRouter.h"
#include "StaticFileHandler.h"
#include "common/Logger.h"

#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

HttpServer::HttpServer(quint16 port, QObject* parent)
    : QObject(parent)
    , m_TcpServer(new QTcpServer(this))
    , m_Router(new RestRouter(this))
    , m_Port(port)
{
    QString frontendDir = QString(FRONTEND_DIR);
    m_StaticFiles = new StaticFileHandler(frontendDir, this);
}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start()
{
    if (!m_TcpServer->listen(QHostAddress::Any, m_Port)) {
        emit serverError("Failed to listen on port " + QString::number(m_Port)
                          + ": " + m_TcpServer->errorString());
        return false;
    }

    connect(m_TcpServer, &QTcpServer::newConnection,
            this, &HttpServer::onNewConnection);

    Logger::info("HTTP server listening on port " + QString::number(m_Port));
    emit started(m_Port);
    return true;
}

void HttpServer::stop()
{
    m_TcpServer->close();
    for (QTcpSocket* socket : m_Buffers.keys())
        socket->disconnectFromHost();
}

void HttpServer::onNewConnection()
{
    while (QTcpSocket* socket = m_TcpServer->nextPendingConnection()) {
        m_Buffers[socket] = QByteArray();
        connect(socket, &QTcpSocket::readyRead, this, &HttpServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &HttpServer::onDisconnected);
    }
}

void HttpServer::onReadyRead()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;

    m_Buffers[socket].append(socket->readAll());

    // Check for complete HTTP request (headers end with \r\n\r\n)
    QByteArray& buffer = m_Buffers[socket];
    int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd == -1)
        return;

    // Parse Content-Length to determine if body is complete
    QString headerPart = QString::fromUtf8(buffer.left(headerEnd));
    int contentLength = 0;
    for (const QString& line : headerPart.split("\r\n")) {
        if (line.startsWith("Content-Length:", Qt::CaseInsensitive)) {
            contentLength = line.mid(15).trimmed().toInt();
            break;
        }
    }

    int totalSize = headerEnd + 4 + contentLength;
    if (buffer.size() < totalSize)
        return; // Body not fully received yet

    // Process complete request
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

    // Static files are always served synchronously
    if (!req.path.startsWith("/api/")) {
        HttpResponse resp = m_StaticFiles->serveFile(req.path);
        sendResponse(socket, resp);
        return;
    }

    // Track socket for async response
    m_PendingAsyncSockets.insert(socket);

    // Auto-timeout: abort if no response within ASYNC_TIMEOUT_MS
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

    // Parse request line: METHOD /path?query HTTP/1.1
    if (!lines.isEmpty()) {
        QStringList parts = lines[0].split(' ');
        if (parts.size() >= 2) {
            req.method = parts[0].toUpper();

            QUrl url(parts[1]);
            req.path = url.path();
            if (req.path.isEmpty())
                req.path = "/";

            // Parse query parameters
            QUrlQuery query(url);
            for (const auto& item : query.queryItems())
                req.queryParams[item.first] = item.second;
        }
    }

    // Parse headers
    int i = 1;
    for (; i < lines.size(); i++) {
        if (lines[i].isEmpty())
            break; // End of headers
        int colon = lines[i].indexOf(':');
        if (colon > 0) {
            QString key = lines[i].left(colon).trimmed();
            QString value = lines[i].mid(colon + 1).trimmed();
            req.headers[key.toLower()] = value;
        }
    }

    // Body is everything after the blank line
    if (i < lines.size()) {
        QStringList bodyLines = lines.mid(i + 1);
        req.body = bodyLines.join("\r\n").toUtf8();
    }

    return req;
}

void HttpServer::sendResponse(QTcpSocket* socket, const HttpResponse& response)
{
    QByteArray respData;

    // Status line
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
