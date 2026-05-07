#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QWebSocketServer>
#include <QWebSocket>
#include <memory>
#include "RtspClient.h"

class EnetControlStream;
class InputCrypto;

// Relays UDP RTP streams to the browser via WebSocket and forwards
// browser input events to Sunshine via ENet reliable UDP control channel.
// Single-client: only one WebSocket connection at a time.
class StreamRelay : public QObject
{
    Q_OBJECT

public:
    // Takes ownership of the UDP sockets (reparents them).
    StreamRelay(QUdpSocket* videoSocket, QUdpSocket* audioSocket,
                QUdpSocket* controlUdpSocket,
                const RtspClient::SessionInfo& info,
                quint16 wsPort, QObject* parent = nullptr);
    ~StreamRelay();

    bool start();
    void stop();

    quint16 wsPort() const { return m_WsPort; }
    QString wsUrl() const;

signals:
    void sessionEnded();
    void clientConnected();
    void clientDisconnected();

private slots:
    void onVideoReadyRead();
    void onAudioReadyRead();
    void onNewWsConnection();
    void onWsTextMessage(const QString& message);
    void onWsDisconnected();
    void onEnetConnected();
    void onEnetFailed(const QString& error);
    void onEnetDisconnected();

private:
    void forwardUdp(QUdpSocket* udp, quint8 channel);
    void sendToControl(const QByteArray& packet);

    QUdpSocket* m_VideoSocket = nullptr;
    QUdpSocket* m_AudioSocket = nullptr;
    QUdpSocket* m_ControlUdpSocket = nullptr;
    QWebSocketServer* m_WsServer = nullptr;
    QWebSocket* m_WsClient = nullptr;
    EnetControlStream* m_EnetControl = nullptr;
    std::unique_ptr<InputCrypto> m_Crypto;
    RtspClient::SessionInfo m_SessionInfo;
    quint16 m_WsPort = 48001;
    bool m_Running = false;
};
