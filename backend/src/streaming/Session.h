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
class StreamRelay;
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
                  bool upnpEnabled = true,
                  const QString& transport = "webrtc",
                  QObject* parent = nullptr);
    ~StreamSession();

    void start();
    void quit();

    /// Override the signaling WS URL returned to the browser.
    /// When nport tunnel is active, this is "wss://<subdomain>.nport.link".
    void setExplicitWsUrl(const QString& url) { m_ExplicitWsUrl = url; }

    /// Set the actual HTTPS port used by HttpServer (may differ from 443
    /// due to port fallback). Used to construct the correct wsUrl() for
    /// the browser when sharing the unified port.
    void setHttpsPort(quint16 port) { m_HttpsPort = port; }

signals:
    void relayCreated(DataChannelRelay* relay);
    void streamRelayCreated(StreamRelay* relay);
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
    bool m_UpnpEnabled = true;
    QString m_Transport = "webrtc";
    QNetworkReply* m_LaunchReply = nullptr;
    QString m_SessionUrl;

    /// If non-empty, overrides SignalingServer::wsUrl() in the /start response.
    QString m_ExplicitWsUrl;

    /// The HTTPS port HttpServer is actually listening on (may be != 443).
    quint16 m_HttpsPort = 443;

    MoonlightShim* m_Shim = nullptr;
    DataChannelRelay* m_Relay = nullptr;
    SignalingServer* m_Signaling = nullptr;
    StreamRelay* m_StreamRelay = nullptr;
};
