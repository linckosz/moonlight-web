#include "NvHTTP.h"

#include <QDebug>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QNetworkProxy>

#include <QUuid>

NvHTTP::NvHTTP(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), m_Nam(nam)
{
    // Disable system proxy for LAN connections (Sunshine is always local)
    m_Nam->setProxy(QNetworkProxy::NoProxy);
}

QUrl NvHTTP::buildUrl(const NvAddress& address, const QString& command,
                       const QString& uniqueId, const QString& arguments) const
{
    QUrl url;
    url.setScheme("http");
    url.setHost(address.address());
    url.setPort(address.port());
    url.setPath("/" + command);

    QString query = "uniqueid=" + uniqueId
                    + "&uuid=" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!arguments.isEmpty())
        query += "&" + arguments;
    url.setQuery(query);

    return url;
}

QNetworkReply* NvHTTP::getServerInfoAsync(const NvAddress& address, const QString& uniqueId)
{
    QUrl url = buildUrl(address, "serverinfo", uniqueId);

    QNetworkRequest req(url);
    req.setTransferTimeout(FAST_FAIL_TIMEOUT_MS);
    req.setRawHeader("User-Agent", "Moonlight-Web/0.1");

    return m_Nam->get(req);
}

QNetworkReply* NvHTTP::getServerInfoAsyncHttps(const NvAddress& address, const QString& uniqueId,
                                                  const QByteArray& clientCertPem,
                                                  const QByteArray& clientKeyPem)
{
    // Build URL as string to avoid any QUrl encoding quirks
    QString urlStr = QString("https://%1:%2/serverinfo?uniqueid=%3&uuid=%4")
                         .arg(address.address())
                         .arg(MW_HTTPS_PORT)
                         .arg(uniqueId,
                              QUuid::createUuid().toString(QUuid::WithoutBraces));
    QUrl url(urlStr);

    QNetworkRequest req(url);
    req.setTransferTimeout(REQUEST_TIMEOUT_MS);
    req.setRawHeader("User-Agent", "Moonlight-Web/0.1");

    QSslConfiguration sslConfig = req.sslConfiguration();
    sslConfig.setLocalCertificate(QSslCertificate(clientCertPem, QSsl::Pem));
    sslConfig.setPrivateKey(QSslKey(clientKeyPem, QSsl::Rsa, QSsl::Pem));
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(sslConfig);

    return m_Nam->get(req);
}

// --- Static XML helpers ----------------------------------------------------

void NvHTTP::verifyResponseStatus(const QString& xml)
{
    QXmlStreamReader reader(xml);

    if (reader.readNextStartElement()) {
        int statusCode = reader.attributes().value("status_code").toInt();

        if (statusCode == 200)
            return;

        if (reader.attributes().value("status_message").toString() == "Invalid"
            && statusCode == -1)
            statusCode = 418;

        throw std::runtime_error(
            QString("HTTP %1: %2")
                .arg(statusCode)
                .arg(reader.attributes().value("status_message").toString())
                .toStdString());
    }
}

QString NvHTTP::getXmlString(const QString& xml, const QString& tagName)
{
    QXmlStreamReader reader(xml);

    while (!reader.atEnd()) {
        if (reader.readNextStartElement()) {
            if (reader.name().toString() == tagName)
                return reader.readElementText();
        }
    }

    return QString();
}

QByteArray NvHTTP::getXmlStringFromHex(const QString& xml, const QString& tagName)
{
    QString hex = getXmlString(xml, tagName);
    if (hex.isEmpty())
        return QByteArray();
    return QByteArray::fromHex(hex.toUtf8());
}

QVector<NvDisplayMode> NvHTTP::getDisplayModeList(const QString& serverInfo)
{
    QVector<NvDisplayMode> modes;
    QXmlStreamReader reader(serverInfo);

    while (!reader.atEnd()) {
        if (reader.readNextStartElement()) {
            if (reader.name().toString() == "DisplayMode") {
                NvDisplayMode mode;
                while (!(reader.isEndElement() && reader.name().toString() == "DisplayMode")) {
                    reader.readNext();
                    if (reader.isStartElement()) {
                        if (reader.name().toString() == "Width")
                            mode.width = reader.readElementText().toInt();
                        else if (reader.name().toString() == "Height")
                            mode.height = reader.readElementText().toInt();
                        else if (reader.name().toString() == "RefreshRate")
                            mode.refreshRate = reader.readElementText().toInt();
                    }
                }
                if (mode.width > 0)
                    modes.append(mode);
            }
        }
    }

    // Sort by resolution * refresh rate (descending)
    std::sort(modes.begin(), modes.end(), [](const NvDisplayMode& a, const NvDisplayMode& b) {
        int pixelsA = a.width * a.height * a.refreshRate;
        int pixelsB = b.width * b.height * b.refreshRate;
        return pixelsA > pixelsB;
    });

    return modes;
}

int NvHTTP::getCurrentGame(const QString& serverInfo)
{
    QString state = getXmlString(serverInfo, "state");
    if (!state.isEmpty() && state.endsWith("_SERVER_BUSY"))
        return getXmlString(serverInfo, "currentgame").toInt();
    return 0;
}

QNetworkReply* NvHTTP::getAppListAsync(const NvAddress& address, quint16 httpsPort,
                                           const QByteArray& clientCertPem,
                                           const QByteArray& clientKeyPem)
{
    QUrl url(QString("https://%1:%2/applist")
                 .arg(address.address())
                 .arg(httpsPort));

    QNetworkRequest req(url);
    req.setTransferTimeout(REQUEST_TIMEOUT_MS);
    req.setRawHeader("User-Agent", "Moonlight-Web/0.1");

    QSslConfiguration sslConfig = req.sslConfiguration();
    sslConfig.setLocalCertificate(QSslCertificate(clientCertPem, QSsl::Pem));
    sslConfig.setPrivateKey(QSslKey(clientKeyPem, QSsl::Rsa, QSsl::Pem));
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(sslConfig);

    return m_Nam->get(req);
}

QVector<NvApp> NvHTTP::parseAppList(const QString& xml)
{
    QVector<NvApp> apps;
    QXmlStreamReader reader(xml);

    while (!reader.atEnd()) {
        if (reader.readNextStartElement()) {
            if (reader.name().toString() == "App") {
                int id = 0;
                QString name;
                bool hdrSupported = false;

                while (!(reader.isEndElement() && reader.name().toString() == "App")) {
                    reader.readNext();
                    if (reader.isStartElement()) {
                        if (reader.name().toString() == "ID")
                            id = reader.readElementText().toInt();
                        else if (reader.name().toString() == "AppTitle")
                            name = reader.readElementText();
                        else if (reader.name().toString() == "IsHdrSupported")
                            hdrSupported = reader.readElementText() == "1";
                    }
                }

                if (id != 0 && !name.isEmpty())
                    apps.append(NvApp(id, name, hdrSupported));
            }
        }
    }

    return apps;
}

QNetworkReply* NvHTTP::launchAppAsync(const NvAddress& address, quint16 httpsPort,
                                               int appId, const QString& uniqueId,
                                               const QByteArray& rikey, int rikeyid,
                                               int width, int height, int fps, int bitrate,
                                               const QByteArray& clientCertPem,
                                               const QByteArray& clientKeyPem)
{
    Q_UNUSED(bitrate)  // sent via RTSP ANNOUNCE, not /launch
    QString mode = QString("%1x%2x%3").arg(width).arg(height).arg(fps);
    QString query = QString("appid=%1&uniqueid=%2&mode=%3&rikey=%4&rikeyid=%5"
                            "&localAudioPlayMode=0&sops=1&surroundAudioInfo=196610"
                            "&gcmap=0&hdrMode=0&corever=0")
                        .arg(appId)
                        .arg(uniqueId)
                        .arg(mode)
                        .arg(QString::fromLatin1(rikey.toHex()))
                        .arg(rikeyid);

    QUrl url(QString("https://%1:%2/launch?%3")
                 .arg(address.address())
                 .arg(httpsPort)
                 .arg(query));

    qDebug() << "[NvHTTP] launchApp URL:" << url.toString();
    qDebug() << "[NvHTTP]   client cert size:" << clientCertPem.size()
             << "key size:" << clientKeyPem.size();

    QNetworkRequest req(url);
    req.setTransferTimeout(30000);  // launch can take a few seconds
    req.setRawHeader("User-Agent", "Moonlight-Web/0.1");

    QSslConfiguration sslConfig = req.sslConfiguration();
    QSslCertificate clientCert(clientCertPem, QSsl::Pem);
    QSslKey clientKey(clientKeyPem, QSsl::Rsa, QSsl::Pem);

    qDebug() << "[NvHTTP]   client cert valid:" << !clientCert.isNull()
             << "key valid:" << !clientKey.isNull();

    sslConfig.setLocalCertificate(clientCert);
    sslConfig.setPrivateKey(clientKey);
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(sslConfig);

    // Log proxy settings
    QNetworkProxy proxy = m_Nam->proxy();
    qDebug() << "[NvHTTP]   proxy type:" << proxy.type()
             << "host:" << proxy.hostName() << "port:" << proxy.port();

    QNetworkReply* reply = m_Nam->get(req);

    // Log SSL errors as they happen
    QObject::connect(reply, &QNetworkReply::sslErrors,
                     [url](const QList<QSslError>& errors) {
        qWarning() << "[NvHTTP] SSL errors for" << url.toString();
        for (const auto& e : errors) {
            qWarning() << "[NvHTTP]   -" << e.errorString();
        }
    });

    return reply;
}

QNetworkReply* NvHTTP::quitAppAsync(const NvAddress& address, quint16 httpsPort,
                                     const QByteArray& clientCertPem,
                                     const QByteArray& clientKeyPem)
{
    QUrl url(QString("https://%1:%2/cancel")
                 .arg(address.address())
                 .arg(httpsPort));

    QNetworkRequest req(url);
    req.setTransferTimeout(REQUEST_TIMEOUT_MS);
    req.setRawHeader("User-Agent", "Moonlight-Web/0.1");

    QSslConfiguration sslConfig = req.sslConfiguration();
    sslConfig.setLocalCertificate(QSslCertificate(clientCertPem, QSsl::Pem));
    sslConfig.setPrivateKey(QSslKey(clientKeyPem, QSsl::Rsa, QSsl::Pem));
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(sslConfig);

    return m_Nam->get(req);
}

QString NvHTTP::parseSessionUrl(const QString& launchXml)
{
    return getXmlString(launchXml, "sessionUrl0");
}

QVector<int> NvHTTP::parseQuad(const QString& quad)
{
    QVector<int> ret;
    if (quad.isEmpty())
        return ret;

    QStringList parts = quad.split('.');
    ret.reserve(parts.size());
    for (const auto& part : parts)
        ret.append(part.toInt());
    return ret;
}
