/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "PdnsClient.h"

#include <QBuffer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PdnsClient::PdnsClient(QObject* parent)
    : QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);
}

// ---------------------------------------------------------------------------
// Internal: synchronous helpers
// ---------------------------------------------------------------------------

QNetworkReply* PdnsClient::sendGet(const QString& url, int timeoutMs)
{
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("X-API-Key", m_Token.toUtf8());
    request.setRawHeader("Accept", "application/json");

    QNetworkReply* reply = m_Nam->get(request);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        qWarning() << "[PdnsClient] Request timed out after" << timeoutMs << "ms:"
                   << "GET" << url;
        reply->abort();
        reply->deleteLater();
        return nullptr;
    }
    timer.stop();

    return reply;
}

QNetworkReply* PdnsClient::sendPatch(const QString& url, const QByteArray& body, int timeoutMs)
{
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("X-API-Key", m_Token.toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QBuffer* buf = new QBuffer;
    buf->setData(body);
    buf->open(QIODevice::ReadOnly);
    QNetworkReply* reply = m_Nam->sendCustomRequest(request, "PATCH", buf);
    buf->setParent(reply);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        qWarning() << "[PdnsClient] Request timed out after" << timeoutMs << "ms:"
                   << "PATCH" << url;
        reply->abort();
        reply->deleteLater();
        return nullptr;
    }
    timer.stop();

    return reply;
}

// ---------------------------------------------------------------------------
// Endpoint / zone configuration (env-driven, single source of truth)
// ---------------------------------------------------------------------------

QString PdnsClient::baseDomain()
{
    QString env = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    return env.isEmpty() ? QStringLiteral("moonlightweb.top") : env;
}

QString PdnsClient::apiBaseUrl()
{
    // Full PowerDNS API base URL. The API host is independent of MW_DOMAIN
    // (it need not live under the application's domain), so it is configured
    // explicitly. Fallback derives "https://api.{MW_DOMAIN}/..." when unset.
    QString env = QString::fromUtf8(qgetenv("MW_PDNS_URL"));
    if (!env.isEmpty()) return env;
    return QStringLiteral("https://api.") + baseDomain() +
           QStringLiteral("/api/v1/servers/localhost");
}

QString PdnsClient::zoneName()
{
    return baseDomain() + QStringLiteral(".");
}

// ---------------------------------------------------------------------------
// Helper: build FQDN with trailing dot for the PowerDNS API
// ---------------------------------------------------------------------------

QString PdnsClient::fqdn(const QString& subname)
{
    // zoneName() already carries the trailing dot.
    return subname + QLatin1Char('.') + zoneName();
}

// ---------------------------------------------------------------------------
// Check subdomain availability
// ---------------------------------------------------------------------------

bool PdnsClient::checkSubdomainAvailable(const QString& subname, QString& errorMsg)
{
    // GET /zones/moonlightweb.top.?rrset_name={subname}.moonlightweb.top.&rrset_type=A
    QString zone = zoneName();
    QString rrsetName = fqdn(subname);

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("rrset_name"), rrsetName);
    query.addQueryItem(QStringLiteral("rrset_type"), QStringLiteral("A"));

    QUrl url(apiBaseUrl() + QStringLiteral("/zones/") + zone);
    // Append trailing '?' so PowerDNS sees the query (zone URL already has trailing dot)
    url.setQuery(query);

    QString urlStr = url.toString();
    qInfo() << "[PdnsClient] Checking subdomain:" << subname << "URL:" << urlStr;

    QNetworkReply* reply = sendGet(urlStr);

    if (!reply) {
        errorMsg =
            QStringLiteral("PowerDNS request timed out (check network to %1)").arg(apiBaseUrl());
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    qInfo() << "[PdnsClient] checkSubdomain HTTP" << statusCode
            << "body:" << QString::fromUtf8(responseData).left(300);

    if (statusCode != 200) {
        errorMsg = QStringLiteral("PowerDNS check subdomain failed (HTTP %1): %2")
                       .arg(QString::number(statusCode), QString::fromUtf8(responseData));
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    // Parse zone object: look for a matching A record in "rrsets"
    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    if (!doc.isObject()) {
        errorMsg = QStringLiteral("PowerDNS check subdomain: invalid JSON response");
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    QJsonArray rrsets = doc.object().value(QStringLiteral("rrsets")).toArray();
    for (const QJsonValue& v : rrsets) {
        QJsonObject rr = v.toObject();
        if (rr.value(QStringLiteral("name")).toString() == rrsetName &&
            rr.value(QStringLiteral("type")).toString() == QStringLiteral("A")) {
            // Found an existing A record
            errorMsg = QStringLiteral("Subdomain %1 already has an A record")
                           .arg(subname + QStringLiteral(".moonlightweb.top"));
            return false;
        }
    }

    // No matching A record found — available
    qInfo() << "[PdnsClient] Subdomain" << subname << "is available";
    return true;
}

// ---------------------------------------------------------------------------
// Create or update A record (unified PATCH REPLACE)
// ---------------------------------------------------------------------------

bool PdnsClient::createOrUpdateSubdomain(const QString& subname, const QString& ip, int ttl,
                                         QString& errorMsg)
{
    QString zone = zoneName();
    QString rrsetName = fqdn(subname);

    // Build PATCH body
    QJsonObject record;
    record[QStringLiteral("content")] = ip;
    record[QStringLiteral("disabled")] = false;

    QJsonArray records;
    records.append(record);

    QJsonObject rrset;
    rrset[QStringLiteral("name")] = rrsetName;
    rrset[QStringLiteral("type")] = QStringLiteral("A");
    rrset[QStringLiteral("ttl")] = ttl;
    rrset[QStringLiteral("changetype")] = QStringLiteral("REPLACE");
    rrset[QStringLiteral("records")] = records;

    QJsonArray rrsets;
    rrsets.append(rrset);

    QJsonObject body;
    body[QStringLiteral("rrsets")] = rrsets;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString url = apiBaseUrl() + QStringLiteral("/zones/") + zone;

    qInfo() << "[PdnsClient] Creating/updating A record:" << subname << ".moonlightweb.top ->"
            << ip;
    QNetworkReply* reply = sendPatch(url, payload);

    if (!reply) {
        errorMsg = QStringLiteral("PowerDNS patch request timed out");
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    qInfo() << "[PdnsClient] createOrUpdateSubdomain HTTP" << statusCode;

    if (statusCode == 204) {
        qInfo() << "[PdnsClient] A record created/updated:" << subname << ".moonlightweb.top ->"
                << ip;
        return true;
    } else {
        errorMsg = QStringLiteral("PowerDNS create/update A record failed (HTTP %1): %2")
                       .arg(QString::number(statusCode), QString::fromUtf8(responseData));
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Delete A record (PATCH DELETE)
// ---------------------------------------------------------------------------

bool PdnsClient::deleteSubdomain(const QString& subname, QString& errorMsg)
{
    QString zone = zoneName();

    QJsonObject rrset;
    rrset[QStringLiteral("name")] = fqdn(subname);
    rrset[QStringLiteral("type")] = QStringLiteral("A");
    rrset[QStringLiteral("changetype")] = QStringLiteral("DELETE");
    rrset[QStringLiteral("records")] = QJsonArray();

    QJsonArray rrsets;
    rrsets.append(rrset);

    QJsonObject body;
    body[QStringLiteral("rrsets")] = rrsets;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString url = apiBaseUrl() + QStringLiteral("/zones/") + zone;

    qInfo() << "[PdnsClient] Deleting A record:" << subname;
    QNetworkReply* reply = sendPatch(url, payload);

    if (!reply) {
        errorMsg = QStringLiteral("PowerDNS delete A record request timed out");
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    qInfo() << "[PdnsClient] deleteSubdomain HTTP" << statusCode;

    if (statusCode == 204) {
        qInfo() << "[PdnsClient] A record deleted:" << subname;
        return true;
    } else if (statusCode == 404 || statusCode == 422) {
        qInfo() << "[PdnsClient] A record already deleted:" << subname << "(HTTP" << statusCode
                << ")";
        return true;
    } else {
        errorMsg = QStringLiteral("PowerDNS delete A record failed (HTTP %1)")
                       .arg(QString::number(statusCode));
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}

// ---------------------------------------------------------------------------
// TXT record management (ACME DNS-01 challenge)
// ---------------------------------------------------------------------------

bool PdnsClient::createTxtRecord(const QString& fqdnSubname, const QString& value, int ttl,
                                 QString& errorMsg)
{
    QString zone = zoneName();
    // fqdnSubname is passed as the full FQDN with trailing dot, e.g.
    // "_acme-challenge.92b8d127.moonlightweb.top."

    QJsonObject record;
    record[QStringLiteral("content")] = QChar('"') + value + QChar('"');
    record[QStringLiteral("disabled")] = false;

    QJsonArray records;
    records.append(record);

    QJsonObject rrset;
    rrset[QStringLiteral("name")] = fqdnSubname;
    rrset[QStringLiteral("type")] = QStringLiteral("TXT");
    rrset[QStringLiteral("ttl")] = ttl;
    rrset[QStringLiteral("changetype")] = QStringLiteral("REPLACE");
    rrset[QStringLiteral("records")] = records;

    QJsonArray rrsets;
    rrsets.append(rrset);

    QJsonObject body;
    body[QStringLiteral("rrsets")] = rrsets;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString url = apiBaseUrl() + QStringLiteral("/zones/") + zone;

    qInfo() << "[PdnsClient] Creating TXT record:" << fqdnSubname;
    QNetworkReply* reply = sendPatch(url, payload);

    if (!reply) {
        errorMsg = QStringLiteral("PowerDNS create TXT record request timed out");
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    qInfo() << "[PdnsClient] createTxtRecord HTTP" << statusCode;

    if (statusCode == 204) {
        qInfo() << "[PdnsClient] TXT record created:" << fqdnSubname;
        return true;
    } else {
        errorMsg = QStringLiteral("PowerDNS create TXT record failed (HTTP %1)")
                       .arg(QString::number(statusCode));
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}

bool PdnsClient::getTxtRecord(const QString& fqdnSubname, QString& valueOut, QString& errorMsg)
{
    valueOut.clear();

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("rrset_name"), fqdnSubname);
    query.addQueryItem(QStringLiteral("rrset_type"), QStringLiteral("TXT"));

    QUrl url(apiBaseUrl() + QStringLiteral("/zones/") + zoneName());
    url.setQuery(query);

    QNetworkReply* reply = sendGet(url.toString());
    if (!reply) {
        errorMsg = QStringLiteral("PowerDNS getTxtRecord timed out");
        return false;
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    if (statusCode != 200) {
        errorMsg = QStringLiteral("PowerDNS getTxtRecord failed (HTTP %1)").arg(statusCode);
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    if (!doc.isObject()) {
        errorMsg = QStringLiteral("PowerDNS getTxtRecord: invalid JSON");
        return false;
    }

    const QJsonArray rrsets = doc.object().value(QStringLiteral("rrsets")).toArray();
    for (const QJsonValue& v : rrsets) {
        QJsonObject rr = v.toObject();
        if (rr.value(QStringLiteral("name")).toString() == fqdnSubname &&
            rr.value(QStringLiteral("type")).toString() == QStringLiteral("TXT")) {
            const QJsonArray records = rr.value(QStringLiteral("records")).toArray();
            if (!records.isEmpty()) {
                QString content =
                    records.first().toObject().value(QStringLiteral("content")).toString();
                // Strip the surrounding quotes PowerDNS stores for TXT content.
                if (content.size() >= 2 && content.startsWith('"') && content.endsWith('"'))
                    content = content.mid(1, content.size() - 2);
                valueOut = content;
            }
            return true;
        }
    }
    return true; // query OK, record absent
}

bool PdnsClient::deleteTxtRecord(const QString& fqdnSubname, QString& errorMsg)
{
    QString zone = zoneName();

    QJsonObject rrset;
    rrset[QStringLiteral("name")] = fqdnSubname;
    rrset[QStringLiteral("type")] = QStringLiteral("TXT");
    rrset[QStringLiteral("changetype")] = QStringLiteral("DELETE");
    rrset[QStringLiteral("records")] = QJsonArray();

    QJsonArray rrsets;
    rrsets.append(rrset);

    QJsonObject body;
    body[QStringLiteral("rrsets")] = rrsets;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString url = apiBaseUrl() + QStringLiteral("/zones/") + zone;

    qInfo() << "[PdnsClient] Deleting TXT record:" << fqdnSubname;
    QNetworkReply* reply = sendPatch(url, payload);

    if (!reply) {
        errorMsg = QStringLiteral("PowerDNS delete TXT record request timed out");
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    qInfo() << "[PdnsClient] deleteTxtRecord HTTP" << statusCode;

    if (statusCode == 204) {
        qInfo() << "[PdnsClient] TXT record deleted:" << fqdnSubname;
        return true;
    } else if (statusCode == 404 || statusCode == 422) {
        // Already gone or no such RRset — not an error
        qInfo() << "[PdnsClient] TXT record already deleted:" << fqdnSubname << "(HTTP"
                << statusCode << ")";
        return true;
    } else {
        errorMsg = QStringLiteral("PowerDNS delete TXT record failed (HTTP %1)")
                       .arg(QString::number(statusCode));
        qWarning() << "[PdnsClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}
