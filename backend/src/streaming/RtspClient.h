#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUrl>
#include <QMap>
#include <QByteArray>
#include <cstdint>

#include "StreamConfig.h"

// Blocking RTSP client — OPTIONS → DESCRIBE → SETUP×3 → ANNOUNCE → PLAY
// Each command opens a fresh TCP connection because Sunshine closes the socket
// after every response (sock.shutdown in handle_msg).
class RtspClient : public QObject
{
    Q_OBJECT

public:
    struct SessionInfo {
        quint16 videoPort = 0;
        quint16 audioPort = 0;
        quint16 controlPort = 0;
        QByteArray avPingPayload;
        uint32_t controlConnectData = 0;
        QString sessionId;
        QString host;
        quint16 rtspPort = 0;
        QByteArray rikey;   // AES-128 key for input encryption (16 bytes)
        int rikeyid = 0;    // IV prefix (BE uint32 in first 4 bytes)
    };

    explicit RtspClient(QObject* parent = nullptr);
    ~RtspClient();

    void start(const QUrl& rtspUrl, const QString& uniqueId, const StreamConfig& config);
    void stop();

    const SessionInfo& sessionInfo() const { return m_SessionInfo; }

    // Release UDP socket ownership to the caller. Returns nullptr if already taken.
    QUdpSocket* takeVideoSocket();
    QUdpSocket* takeAudioSocket();
    QUdpSocket* takeControlSocket();

signals:
    void handshakeComplete(const RtspClient::SessionInfo& info);
    void handshakeFailed(const QString& error);
    void stateChanged(const QString& state, const QString& stage);

private:
    struct RtspResponse {
        int statusCode = 0;
        QMap<QString, QString> headers;
        QByteArray body;
    };

    // Blocking I/O helpers — each opens its own TCP connection
    bool sendAndWait(const QByteArray& method, const QString& pathSuffix,
                     const QMap<QString, QString>& extraHeaders = {},
                     const QByteArray& body = {});
    bool sendSetupAndWait(const QString& streamType);
    bool waitForResponse(QTcpSocket& sock, const QByteArray& method);

    // Helpers
    RtspResponse parseResponse(const QByteArray& data);
    QString buildAnnounceSdp() const;
    int bindUdpPort(QUdpSocket* socket);

    QUdpSocket* m_VideoSocket = nullptr;
    QUdpSocket* m_AudioSocket = nullptr;
    QUdpSocket* m_ControlSocket = nullptr;

    QString m_UniqueId;
    StreamConfig m_Config;
    SessionInfo m_SessionInfo;

    int m_CSeq = 1;
    int m_SetupCount = 0;
    QMap<QString, quint16> m_StreamPorts;

    enum State { Idle, Ready, Error };
    State m_State = Idle;
};
