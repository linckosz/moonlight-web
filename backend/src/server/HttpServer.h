#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSslSocket>
#include <QSslConfiguration>
#include "common/Types.h"

class RestRouter;
class StaticFileHandler;

class HttpServer : public QObject
{
    Q_OBJECT

public:
    explicit HttpServer(quint16 httpPort = 80, quint16 httpsPort = 443,
                        QObject* parent = nullptr);
    ~HttpServer();

    /// Start servers. preferredHttpsPort is tried first, then fallback ranges.
    bool start(quint16 preferredHttpsPort = 443);
    void stop();

    RestRouter* router() const { return m_Router; }
    QSslConfiguration sslConfiguration() const { return m_SslConfig; }

    /// Port of the local signaling WebSocket server (for WS→proxy routing).
    void setSignalingPort(quint16 port) { m_SignalingPort = port; }

    /// Port of the local stream relay WebSocket server (legacy WSS mode).
    /// The HttpServer proxies wss://host/ws/stream to this local port.
    void setStreamRelayPort(quint16 port) { m_StreamRelayPort = port; }

    /// The port the HTTPS server actually bound to (0 if not started).
    quint16 activeHttpsPort() const { return m_ActiveHttpsPort; }

    /// The HTTP redirect server port.
    quint16 httpPort() const { return m_HttpPort; }

    /// Reload TLS configuration from cert/ directory.
    /// Called after Let's Encrypt certificate acquisition.
    bool reloadTls();

    /// Change the HTTPS port at runtime. Stops current listeners and
    /// re-binds to the new port. Returns true if the new port binds OK.
    /// All existing connections are closed during the transition.
    bool changeHttpsPort(quint16 newPort);

signals:
    void started(quint16 port);
    void serverError(const QString& message);

private slots:
    void onHttpConnection();
    void onReadyRead();
    void onDisconnected();

private:
    void processRequest(QTcpSocket* socket, const QByteArray& requestData);
    void onReadyReadSocket(QTcpSocket* socket);
    void sendResponse(QTcpSocket* socket, const HttpResponse& response);
    void handleWebSocketUpgrade(QTcpSocket* clientSocket, const QByteArray& requestData);
    HttpRequest parseRequest(const QByteArray& raw) const;
    bool isLanHost(const QString& host) const;
    bool loadCert();
    QString findCertDir();
    bool loadCertFiles(const QString& certDir);
    bool renewWithLego();
    bool generateSelfSignedCert();

    QTcpServer* m_HttpServer;
    QTcpServer* m_HttpsServer;
    QSslConfiguration m_SslConfig;
    RestRouter* m_Router;
    StaticFileHandler* m_StaticFiles;
    quint16 m_HttpPort;
    quint16 m_HttpsPort;
    quint16 m_ActiveHttpsPort = 0;

    quint16 m_SignalingPort = 48001;
    quint16 m_StreamRelayPort = 48002;

    QMap<QTcpSocket*, QByteArray> m_Buffers;
    QSet<QTcpSocket*> m_PendingAsyncSockets;
    static constexpr int ASYNC_TIMEOUT_MS = 30000;
};
