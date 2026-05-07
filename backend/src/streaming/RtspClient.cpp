#include "RtspClient.h"

#include <QStringList>
#include <QDebug>
#include <QRegularExpression>

RtspClient::RtspClient(QObject* parent)
    : QObject(parent)
{
}

RtspClient::~RtspClient()
{
    stop();
}

void RtspClient::start(const QUrl& rtspUrl, const QString& uniqueId, const StreamConfig& config)
{
    if (m_State != Idle) {
        emit handshakeFailed("RTSP client already started");
        return;
    }

    m_UniqueId = uniqueId;
    m_Config = config;
    m_CSeq = 1;
    m_SessionInfo.host = rtspUrl.host();
    m_SessionInfo.rtspPort = rtspUrl.port(48010);
    m_SessionInfo.rikey = config.rikey;
    m_SessionInfo.rikeyid = config.rikeyid;

    // --- OPTIONS ---
    emit stateChanged("connecting", "OPTIONS");
    if (!sendAndWait("OPTIONS", ""))
        return;

    // --- DESCRIBE ---
    emit stateChanged("connecting", "DESCRIBE");
    QMap<QString, QString> describeHeaders;
    describeHeaders["Accept"] = "application/sdp";
    describeHeaders["If-Modified-Since"] = "Thu, 01 Jan 1970 00:00:00 GMT";
    if (!sendAndWait("DESCRIBE", "", describeHeaders))
        return;

    // --- SETUP video ---
    emit stateChanged("connecting", "SETUP video");
    m_SetupCount = 0;
    if (!sendSetupAndWait("video"))
        return;

    // --- SETUP audio ---
    emit stateChanged("connecting", "SETUP audio");
    if (!sendSetupAndWait("audio"))
        return;

    // --- SETUP control ---
    emit stateChanged("connecting", "SETUP control");
    if (!sendSetupAndWait("control"))
        return;

    // --- ANNOUNCE ---
    emit stateChanged("connecting", "ANNOUNCE");
    QString sdp = buildAnnounceSdp();
    QMap<QString, QString> announceHeaders;
    announceHeaders["Content-Type"] = "application/sdp";
    announceHeaders["Content-Length"] = QString::number(sdp.toUtf8().size());
    if (!sendAndWait("ANNOUNCE", "", announceHeaders, sdp.toUtf8()))
        return;

    // --- PLAY ---
    emit stateChanged("streaming", "PLAY");
    if (!sendAndWait("PLAY", ""))
        return;

    // --- Ready ---
    m_State = Ready;
    emit stateChanged("streaming", "ready");
    emit handshakeComplete(m_SessionInfo);
}

QUdpSocket* RtspClient::takeVideoSocket()
{
    QUdpSocket* s = m_VideoSocket;
    m_VideoSocket = nullptr;
    return s;
}

QUdpSocket* RtspClient::takeAudioSocket()
{
    QUdpSocket* s = m_AudioSocket;
    m_AudioSocket = nullptr;
    return s;
}

QUdpSocket* RtspClient::takeControlSocket()
{
    QUdpSocket* s = m_ControlSocket;
    m_ControlSocket = nullptr;
    return s;
}

void RtspClient::stop()
{
    m_State = Idle;

    delete m_VideoSocket;   m_VideoSocket = nullptr;
    delete m_AudioSocket;   m_AudioSocket = nullptr;
    delete m_ControlSocket; m_ControlSocket = nullptr;
}

// --- Blocking helpers -------------------------------------------------------

bool RtspClient::sendAndWait(const QByteArray& method, const QString& pathSuffix,
                              const QMap<QString, QString>& extraHeaders,
                              const QByteArray& body)
{
    // Build request
    QString path = "rtsp://" + m_SessionInfo.host + ":" +
                   QString::number(m_SessionInfo.rtspPort) + pathSuffix;

    QByteArray req;
    req += method + " " + path.toUtf8() + " RTSP/1.0\r\n";
    req += "CSeq: " + QByteArray::number(m_CSeq++) + "\r\n";
    req += "X-GS-ClientVersion: 14\r\n";
    req += "Host: " + m_SessionInfo.host.toUtf8() + "\r\n";

    for (auto it = extraHeaders.begin(); it != extraHeaders.end(); ++it)
        req += it.key().toUtf8() + ": " + it.value().toUtf8() + "\r\n";

    if (!m_SessionInfo.sessionId.isEmpty())
        req += "Session: " + m_SessionInfo.sessionId.toUtf8() + "\r\n";

    req += "\r\n";
    if (!body.isEmpty())
        req += body;

    // Fresh TCP connection per command — Sunshine closes the socket after each response
    QTcpSocket sock;
    sock.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    sock.connectToHost(m_SessionInfo.host, m_SessionInfo.rtspPort);

    if (!sock.waitForConnected(5000)) {
        emit handshakeFailed(method + ": connect failed — " + sock.errorString());
        return false;
    }

    qint64 written = sock.write(req);
    if (written < req.size()) {
        emit handshakeFailed(method + ": write failed — " + sock.errorString());
        return false;
    }

    return waitForResponse(sock, method);
}

bool RtspClient::sendSetupAndWait(const QString& streamType)
{
    // Bind UDP socket if needed
    quint16 port = m_StreamPorts.value(streamType, 0);
    if (port == 0) {
        QUdpSocket** sockPtr;
        if (streamType == "video")
            sockPtr = &m_VideoSocket;
        else if (streamType == "audio")
            sockPtr = &m_AudioSocket;
        else
            sockPtr = &m_ControlSocket;

        *sockPtr = new QUdpSocket(this);
        port = bindUdpPort(*sockPtr);
        if (port == 0) {
            emit handshakeFailed("Failed to bind UDP socket for " + streamType);
            return false;
        }
        m_StreamPorts[streamType] = port;
    }

    QMap<QString, QString> headers;
    headers["Transport"] = QString("RTP/AVP;unicast;client_port=%1-%2")
                               .arg(port).arg(port + 1);

    if (!sendAndWait("SETUP", "/streamid=" + streamType + "/0", headers))
        return false;

    return true;
}

bool RtspClient::waitForResponse(QTcpSocket& sock, const QByteArray& method)
{
    QByteArray buffer;

    while (true) {
        if (!sock.waitForReadyRead(5000)) {
            emit handshakeFailed(QString("%1: no response — %2")
                                     .arg(QString::fromUtf8(method))
                                     .arg(sock.errorString()));
            return false;
        }

        QByteArray chunk = sock.readAll();
        buffer.append(chunk);

        // Check if we have a complete RTSP message (headers terminated by \r\n\r\n)
        int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            continue;

        // Check Content-Length
        QByteArray headerPart = buffer.left(headerEnd);
        int contentLength = 0;
        for (const QByteArray& line : headerPart.split('\n')) {
            if (line.trimmed().startsWith("Content-Length:")) {
                contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
            }
        }

        int totalLen = headerEnd + 4 + contentLength;
        if (buffer.size() < totalLen)
            continue;

        RtspResponse resp = parseResponse(buffer);

        if (resp.statusCode != 200) {
            qWarning() << "[RTSP]" << method << "returned" << resp.statusCode;
            emit handshakeFailed(QString("%1 returned %2")
                                     .arg(QString::fromUtf8(method))
                                     .arg(resp.statusCode));
            return false;
        }

        // Process response: extract session info from SETUP responses
        if (method == "SETUP") {
            QString transport = resp.headers.value("Transport", "");
            QRegularExpression portRe("server_port=(\\d+)");
            auto match = portRe.match(transport);
            quint16 serverPort = match.hasMatch() ? match.captured(1).toUShort() : 0;

            if (m_SetupCount == 0) {
                m_SessionInfo.videoPort = serverPort;
                m_SessionInfo.avPingPayload = resp.headers.value("X-SS-Ping-Payload", "").toUtf8();
            } else if (m_SetupCount == 1) {
                m_SessionInfo.audioPort = serverPort;
            } else {
                m_SessionInfo.controlPort = serverPort;
                m_SessionInfo.controlConnectData =
                    resp.headers.value("X-SS-Connect-Data", "0").toUInt();
                m_SessionInfo.sessionId = resp.headers.value("Session", "")
                                              .split(';').value(0).trimmed();
            }
            m_SetupCount++;
        }

        return true;
    }
}

RtspClient::RtspResponse RtspClient::parseResponse(const QByteArray& data)
{
    RtspResponse resp;
    QString text = QString::fromUtf8(data);
    QStringList lines = text.split("\r\n");

    if (!lines.isEmpty()) {
        QStringList parts = lines[0].split(' ');
        if (parts.size() >= 2)
            resp.statusCode = parts[1].toInt();
    }

    int i = 1;
    for (; i < lines.size(); ++i) {
        if (lines[i].isEmpty())
            break;

        int colon = lines[i].indexOf(':');
        if (colon > 0) {
            QString key = lines[i].left(colon).trimmed();
            QString value = lines[i].mid(colon + 1).trimmed();
            resp.headers[key] = value;
        }
    }

    int bodyStart = data.indexOf("\r\n\r\n");
    if (bodyStart >= 0)
        resp.body = data.mid(bodyStart + 4);

    return resp;
}

// --- SDP construction -------------------------------------------------------

QString RtspClient::buildAnnounceSdp() const
{
    QString sdp;
    sdp += "s=" + m_UniqueId + "\r\n";
    sdp += QString("a=x-nv-video[0].clientViewportWd:%1\r\n").arg(m_Config.kWidth);
    sdp += QString("a=x-nv-video[0].clientViewportHt:%1\r\n").arg(m_Config.kHeight);
    sdp += QString("a=x-nv-video[0].maxFPS:%1\r\n").arg(m_Config.kFps);
    sdp += QString("a=x-nv-video[0].packetSize:%1\r\n").arg(m_Config.kPacketSize);
    sdp += "a=x-nv-video[0].maxNumReferenceFrames:1\r\n";
    sdp += "a=x-nv-video[0].encoderCscMode:0\r\n";
    sdp += "a=x-nv-video[0].dynamicRangeMode:0\r\n";
    sdp += QString("a=x-nv-video[0].clientRefreshRateX100:%1\r\n").arg(m_Config.kClientRefreshRateX100);
    sdp += QString("a=x-nv-video[0].videoEncoderSlicesPerFrame:%1\r\n").arg(m_Config.kVideoEncoderSlicesPerFrame);
    sdp += QString("a=x-nv-vqos[0].bw.maximumBitrateKbps:%1\r\n").arg(m_Config.kBitrateKbps);
    sdp += QString("a=x-nv-vqos[0].bitStreamFormat:%1\r\n").arg(m_Config.kVideoFormat);
    sdp += "a=x-nv-vqos[0].fec.minRequiredFecPackets:0\r\n";
    sdp += "a=x-nv-vqos[0].qosTrafficType:5\r\n";
    sdp += QString("a=x-nv-audio.surround.numChannels:%1\r\n").arg(m_Config.kAudioChannels);
    sdp += QString("a=x-nv-audio.surround.channelMask:%1\r\n").arg(m_Config.kAudioChannelMask);
    sdp += QString("a=x-nv-audio.surround.AudioQuality:%1\r\n").arg(m_Config.kAudioQuality);
    sdp += QString("a=x-nv-aqos.packetDuration:%1\r\n").arg(m_Config.kAudioPacketDuration);
    sdp += "a=x-nv-aqos.qosTrafficType:5\r\n";
    sdp += "a=x-nv-general.useReliableUdp:1\r\n";
    sdp += "a=x-nv-general.featureFlags:0\r\n";
    sdp += "a=x-ml-general.featureFlags:0\r\n";
    sdp += QString("a=x-ml-video.configuredBitrateKbps:%1\r\n").arg(m_Config.kBitrateKbps);
    sdp += "a=x-ss-general.encryptionEnabled:0\r\n";
    sdp += "a=x-ss-video[0].chromaSamplingType:0\r\n";
    sdp += "a=x-ss-video[0].intraRefresh:0\r\n";
    return sdp;
}

int RtspClient::bindUdpPort(QUdpSocket* socket)
{
    if (!socket->bind(QHostAddress::AnyIPv4, 0)) {
        qWarning() << "[RTSP] Failed to bind UDP socket:" << socket->errorString();
        return 0;
    }
    quint16 port = socket->localPort();
    return port;
}
