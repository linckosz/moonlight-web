#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QUrl>

#include "StreamConfig.h"
#include "../common/Types.h"

class NvHTTP;
class NvComputer;
class DataChannelRelay;
class SignalingServer;
class MoonlightShim;

class StreamSession : public QObject
{
    Q_OBJECT

public:
    StreamSession(NvComputer* host, int appId,
                  NvHTTP* http, ResponseCallback respond,
                  quint16 wsPort = 48001,
                  const QString& serverHost = "localhost",
                  VideoCodec videoCodec = VideoCodec::Auto,
                  bool gamingMode = true,
                  QObject* parent = nullptr);
    ~StreamSession();

    void start();
    void quit();

    /// Override the signaling WS URL returned to the browser.
    /// When zrok is active, this should be "wss://<zrok-public-url>".
    void setExplicitWsUrl(const QString& url) { m_ExplicitWsUrl = url; }

signals:
    void relayCreated(DataChannelRelay* relay);
    void sessionStarted();
    void sessionFailed(const QString& error);

private slots:
    void onLaunchReplyFinished();
    void onShimConnectionStarted();
    void onShimConnectionFailed(const QString& error);

private:
    void doLaunchApp(const QByteArray& clientCert, const QByteArray& clientKey);

    NvComputer* m_Host;
    int m_AppId;
    NvHTTP* m_Http;
    ResponseCallback m_Respond;
    quint16 m_WsPort = 48001;
    QString m_ServerHost;

    StreamConfig m_Config;
    bool m_GamingMode = true;
    QNetworkReply* m_LaunchReply = nullptr;
    QString m_SessionUrl;

    /// If non-empty, overrides SignalingServer::wsUrl() in the /start response.
    QString m_ExplicitWsUrl;

    MoonlightShim* m_Shim = nullptr;
    DataChannelRelay* m_Relay = nullptr;
    SignalingServer* m_Signaling = nullptr;
};
