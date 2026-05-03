#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include "common/Types.h"

class RestRouter;
class StaticFileHandler;

class HttpServer : public QObject
{
    Q_OBJECT

public:
    explicit HttpServer(quint16 port = 48000, QObject* parent = nullptr);
    ~HttpServer();

    bool start();
    void stop();

    RestRouter* router() const { return m_Router; }

signals:
    void started(quint16 port);
    void serverError(const QString& message);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    void processRequest(QTcpSocket* socket, const QByteArray& requestData);
    HttpRequest parseRequest(const QByteArray& raw) const;
    void sendResponse(QTcpSocket* socket, const HttpResponse& response);

    QTcpServer* m_TcpServer;
    RestRouter* m_Router;
    StaticFileHandler* m_StaticFiles;
    quint16 m_Port;

    // Per-connection read buffers
    QMap<QTcpSocket*, QByteArray> m_Buffers;
};
