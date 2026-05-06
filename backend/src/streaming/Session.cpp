#include "Session.h"
#include "../backend/NvHTTP.h"
#include "../backend/NvComputer.h"
#include "../backend/IdentityManager.h"

#include <QJsonObject>
#include <QDebug>
#include <QUrl>

StreamSession::StreamSession(NvComputer* host, int appId,
                               NvHTTP* http, ResponseCallback respond,
                               QObject* parent)
    : QObject(parent)
    , m_Host(host)
    , m_AppId(appId)
    , m_Http(http)
    , m_Respond(std::move(respond))
{
}

StreamSession::~StreamSession()
{
    if (m_LaunchReply)
        m_LaunchReply->deleteLater();

    if (m_RtspClient) {
        m_RtspClient->stop();
        m_RtspClient->deleteLater();
    }
}

void StreamSession::start()
{
    if (!m_Host) {
        m_Respond(HttpResponse::error(404, "Host not found"));
        emit sessionFailed("Host not found");
        deleteLater();
        return;
    }

    if (m_Host->pairState != NvComputer::PS_PAIRED) {
        m_Respond(HttpResponse::error(403, "Host is not paired"));
        emit sessionFailed("Host is not paired");
        deleteLater();
        return;
    }

    // Generate per-session encryption keys
    m_Config.generateKeys();

    auto* identity = IdentityManager::get();
    QByteArray clientCert = identity->getCertificate();
    QByteArray clientKey = identity->getPrivateKey();

    qDebug() << "[Session] Launching app" << m_AppId << "on" << m_Host->name;

    m_LaunchReply = m_Http->launchAppAsync(
        m_Host->activeAddress, m_Host->activeHttpsPort,
        m_AppId, identity->getUniqueId(),
        m_Config.rikey, m_Config.rikeyid,
        StreamConfig::kWidth, StreamConfig::kHeight, StreamConfig::kFps,
        StreamConfig::kBitrateKbps,
        clientCert, clientKey);

    connect(m_LaunchReply, &QNetworkReply::finished,
            this, &StreamSession::onLaunchReplyFinished);
}

void StreamSession::quit()
{
    if (m_RtspClient) {
        m_RtspClient->stop();
        m_RtspClient->deleteLater();
        m_RtspClient = nullptr;
    }

    auto* identity = IdentityManager::get();
    QByteArray clientCert = identity->getCertificate();
    QByteArray clientKey = identity->getPrivateKey();

    QNetworkReply* reply = m_Http->quitAppAsync(
        m_Host->activeAddress, m_Host->activeHttpsPort,
        clientCert, clientKey);

    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void StreamSession::onLaunchReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    reply->deleteLater();
    m_LaunchReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "[Session] Launch failed:" << reply->errorString();
        m_Respond(HttpResponse::error(502, "Launch failed: " + reply->errorString()));
        emit sessionFailed(reply->errorString());
        deleteLater();
        return;
    }

    QByteArray data = reply->readAll();
    QString xml = QString::fromUtf8(data);

    try {
        NvHTTP::verifyResponseStatus(xml);
    } catch (const std::runtime_error& e) {
        m_Respond(HttpResponse::error(502, QString("Launch error: %1").arg(e.what())));
        emit sessionFailed(e.what());
        deleteLater();
        return;
    }

    QString sessionUrl = NvHTTP::parseSessionUrl(xml);
    if (sessionUrl.isEmpty()) {
        m_Respond(HttpResponse::error(502, "No session URL in launch response"));
        emit sessionFailed("No session URL");
        deleteLater();
        return;
    }

    QUrl rtspUrl(sessionUrl);
    if (!rtspUrl.isValid()) {
        m_Respond(HttpResponse::error(502, "Invalid RTSP URL: " + sessionUrl));
        emit sessionFailed("Invalid RTSP URL");
        deleteLater();
        return;
    }

    // Phase 2: RTSP handshake
    m_RtspClient = new RtspClient(this);
    connect(m_RtspClient, &RtspClient::handshakeComplete,
            this, &StreamSession::onRtspHandshakeComplete);
    connect(m_RtspClient, &RtspClient::handshakeFailed,
            this, &StreamSession::onRtspHandshakeFailed);

    m_RtspClient->start(rtspUrl, m_Host->uuid, m_Config);
}

void StreamSession::onRtspHandshakeComplete(const RtspClient::SessionInfo& info)
{
    qDebug() << "[Session] RTSP handshake complete — streaming on ports:"
             << "video=" << info.videoPort
             << "audio=" << info.audioPort
             << "control=" << info.controlPort;

    m_SessionInfo = info;

    QJsonObject result;
    result["status"] = "streaming";
    result["sessionUrl"] = QString("rtsp://%1:%2").arg(info.host).arg(info.rtspPort);
    result["videoPort"] = info.videoPort;
    result["audioPort"] = info.audioPort;
    result["controlPort"] = info.controlPort;
    result["sessionId"] = info.sessionId;

    m_Respond(HttpResponse::json(result));
    emit sessionStarted();

    // Session stays alive — streaming continues until quit() or disconnect
}

void StreamSession::onRtspHandshakeFailed(const QString& error)
{
    qWarning() << "[Session] RTSP handshake failed:" << error;

    // Try to quit the app on the host (best-effort)
    quit();

    QString friendly = QString("RTSP handshake failed — %1").arg(error);
    m_Respond(HttpResponse::error(502, friendly));
    emit sessionFailed(error);
    deleteLater();
}
