#include "NvHTTP.h"

#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>

#include <QUuid>

NvHTTP::NvHTTP(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), m_Nam(nam)
{
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

    qInfo() << "[MW] HTTPS URL:" << url.toString();

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
