#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QUrl>

#include "StreamConfig.h"
#include "../common/Types.h"

class NvHTTP;
class NvComputer;
class StreamRelay;
class MoonlightShim;

class StreamSession : public QObject
{
    Q_OBJECT

public:
    StreamSession(NvComputer* host, int appId,
                  NvHTTP* http, ResponseCallback respond,
                  quint16 wsPort = 48001,
                  const QSslConfiguration& sslConfig = {},
                  const QString& serverHost = "localhost",
                  QObject* parent = nullptr);
    ~StreamSession();

    void start();
    void quit();

signals:
    void relayCreated(StreamRelay* relay);
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
    QSslConfiguration m_SslConfig;
    QString m_ServerHost;

    StreamConfig m_Config;
    QNetworkReply* m_LaunchReply = nullptr;
    QString m_SessionUrl;

    MoonlightShim* m_Shim = nullptr;
    StreamRelay* m_Relay = nullptr;
};
