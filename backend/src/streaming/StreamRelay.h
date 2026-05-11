#pragma once

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSslConfiguration>

class MoonlightShim;

class StreamRelay : public QObject
{
    Q_OBJECT

public:
    StreamRelay(MoonlightShim* shim,
                quint16 wsPort,
                const QSslConfiguration& sslConfig = {},
                QObject* parent = nullptr);
    ~StreamRelay();

    bool start();
    void stop();

    void setServerHost(const QString& host) { m_ServerHost = host; }
    quint16 wsPort() const { return m_WsPort; }
    QString wsUrl() const;

signals:
    void sessionEnded();
    void clientConnected();
    void clientDisconnected();

private slots:
    void onVideoFrame(const QByteArray& data, int frameType, int frameNumber);
    void onAudioSample(const QByteArray& data);
    void onNewWsConnection();
    void onWsTextMessage(const QString& message);
    void onWsDisconnected();

    void onShimConnectionStarted();
    void onShimConnectionFailed(const QString& error);
    void onShimConnectionTerminated(int errorCode);

private:
    MoonlightShim* m_Shim;
    QWebSocketServer* m_WsServer = nullptr;
    QWebSocket* m_WsClient = nullptr;
    quint16 m_WsPort = 48001;
    QString m_ServerHost = "localhost";
    bool m_Running = false;
    bool m_Stopping = false;
    bool m_StreamStarted = false;
    QList<QByteArray> m_PendingVideoFrames;
    QList<QByteArray> m_PendingAudioFrames;
    int m_FrameCount = 0;
};
