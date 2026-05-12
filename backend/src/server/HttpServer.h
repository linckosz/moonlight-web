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
    explicit HttpServer(quint16 httpPort = 48000, quint16 httpsPort = 48443,
                        QObject* parent = nullptr);
    ~HttpServer();

    bool start();
    void stop();

    RestRouter* router() const { return m_Router; }
    QSslConfiguration sslConfiguration() const { return m_SslConfig; }

    /// Reload TLS configuration from cert/ directory.
    /// Called after Let's Encrypt certificate acquisition.
    bool reloadTls();

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
    HttpRequest parseRequest(const QByteArray& raw) const;
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

    QMap<QTcpSocket*, QByteArray> m_Buffers;
    QSet<QTcpSocket*> m_PendingAsyncSockets;
    static constexpr int ASYNC_TIMEOUT_MS = 30000;
};
