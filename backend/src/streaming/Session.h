#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QUrl>

#include "StreamConfig.h"
#include "RtspClient.h"
#include "../common/Types.h"

class NvHTTP;
class NvComputer;
class StreamRelay;

// Orchestrates launchApp → RTSP handshake → streaming session.
// Created per-stream, self-deletes when done.
class StreamSession : public QObject
{
    Q_OBJECT

public:
    // Takes ownership of the response callback — will call it exactly once.
    StreamSession(NvComputer* host, int appId,
                  NvHTTP* http, ResponseCallback respond,
                  quint16 wsPort = 48001,
                  QObject* parent = nullptr);
    ~StreamSession();

    void start();
    void quit();

    const RtspClient::SessionInfo& sessionInfo() const { return m_SessionInfo; }

signals:
    void relayCreated(StreamRelay* relay);
    void sessionStarted();
    void sessionFailed(const QString& error);

private slots:
    void onLaunchReplyFinished();
    void onRtspHandshakeComplete(const RtspClient::SessionInfo& info);
    void onRtspHandshakeFailed(const QString& error);

private:
    NvComputer* m_Host;
    int m_AppId;
    NvHTTP* m_Http;
    ResponseCallback m_Respond;
    quint16 m_WsPort = 48001;

    StreamConfig m_Config;
    RtspClient* m_RtspClient = nullptr;
    QNetworkReply* m_LaunchReply = nullptr;
    RtspClient::SessionInfo m_SessionInfo;
};
