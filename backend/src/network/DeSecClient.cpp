#include "DeSecClient.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DeSecClient::DeSecClient(QObject* parent)
    : QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);
}

// ---------------------------------------------------------------------------
// Internal: send a synchronous authenticated request
// ---------------------------------------------------------------------------

QNetworkReply* DeSecClient::sendRequest(const QString& method, const QString& path,
                                        const QByteArray& body, int timeoutMs)
{
    QUrl url(apiBaseUrl() + path);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", authHeader().toUtf8());

    if (method == "POST" || method == "PUT" || method == "PATCH") {
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    }

    QNetworkReply* reply = nullptr;

    if (method == "GET") {
        reply = m_Nam->get(request);
    } else if (method == "POST") {
        reply = m_Nam->post(request, body);
    } else if (method == "PUT") {
        reply = m_Nam->put(request, body);
    } else if (method == "DELETE") {
        reply = m_Nam->deleteResource(request);
    } else {
        return nullptr;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        qWarning() << "[DeSecClient] Request timed out after" << timeoutMs << "ms:"
                   << method << url.toString();
        reply->abort();
        reply->deleteLater();
        return nullptr;
    }
    timer.stop();

    // Log HTTP 429 (Rate Limited) if present, before returning to caller
    logRateLimit(reply);

    return reply;
}

// ---------------------------------------------------------------------------
// Rate limit logging
// ---------------------------------------------------------------------------

void DeSecClient::logRateLimit(QNetworkReply* reply)
{
    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 429) {
        QByteArray retryAfter = reply->rawHeader("Retry-After");
        qWarning() << "[DeSecClient] HTTP 429 (Rate Limited) —"
                   << "Retry-After:" << QString::fromUtf8(retryAfter)
                   << "for" << reply->url().toString();
    }
}

// ---------------------------------------------------------------------------
// Check subdomain availability
// ---------------------------------------------------------------------------

bool DeSecClient::checkSubdomainAvailable(const QString& domain, const QString& subname,
                                          QString& errorMsg)
{
    // GET /api/v1/domains/{domain}/rrsets/{subname}/A/
    // 404 → available, 200 → already exists
    QString path = QStringLiteral("/domains/") + domain
                   + QStringLiteral("/rrsets/") + subname + QStringLiteral("/A/");

    qInfo() << "[DeSecClient] Checking subdomain:" << subname
            << "URL:" << (apiBaseUrl() + path);

    QNetworkReply* reply = sendRequest("GET", path);

    if (!reply) {
        errorMsg = QStringLiteral("deSEC request timed out (check network to desec.io)");
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    qInfo() << "[DeSecClient] checkSubdomain HTTP" << statusCode
            << "body:" << QString::fromUtf8(responseData).left(200);

    if (statusCode == 404) {
        return true;  // Not found → available
    } else if (statusCode == 200) {
        errorMsg = QStringLiteral("Subdomain %1.%2 already has an A record")
                   .arg(subname, domain);
        return false;
    } else if (statusCode == 401 || statusCode == 403) {
        errorMsg = QStringLiteral("deSEC authentication failed (HTTP %1) — check DESEC_TOKEN")
                   .arg(QString::number(statusCode));
        qWarning() << "[DeSecClient]" << errorMsg << "Response:" << QString::fromUtf8(responseData);
        emit error(errorMsg);
        return false;
    } else {
        errorMsg = QStringLiteral("deSEC check subdomain failed (HTTP %1): %2")
                   .arg(QString::number(statusCode), QString::fromUtf8(responseData));
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Create subdomain A record
// ---------------------------------------------------------------------------

bool DeSecClient::createSubdomain(const QString& domain, const QString& subname,
                                  const QString& ip, int ttl, QString& errorMsg)
{
    // POST /api/v1/domains/{domain}/rrsets/
    // Body: {"subname": "<subname>", "type": "A", "ttl": <ttl>, "records": ["<ip>"]}
    QJsonObject body;
    body[QStringLiteral("subname")] = subname;
    body[QStringLiteral("type")] = QStringLiteral("A");
    body[QStringLiteral("ttl")] = ttl;

    QJsonArray recordsArray;
    recordsArray.append(ip);
    body[QStringLiteral("records")] = recordsArray;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString path = QStringLiteral("/domains/") + domain + QStringLiteral("/rrsets/");

    qInfo() << "[DeSecClient] Creating A record:" << subname << "." << domain << "→" << ip;
    QNetworkReply* reply = sendRequest("POST", path, payload);

    if (!reply) {
        errorMsg = QStringLiteral("deSEC create A record request timed out");
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    qInfo() << "[DeSecClient] createSubdomain HTTP" << statusCode
            << "body:" << QString::fromUtf8(responseData).left(200);

    if (statusCode == 201) {
        qInfo() << "[DeSecClient] A record created:" << subname << "." << domain << "→" << ip;
        return true;
    } else {
        errorMsg = QStringLiteral("deSEC create A record failed (HTTP %1): %2")
                   .arg(QString::number(statusCode), QString::fromUtf8(responseData));
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Update subdomain A record (IP change)
// ---------------------------------------------------------------------------

bool DeSecClient::updateSubdomain(const QString& domain, const QString& subname,
                                  const QString& ip, int ttl, QString& errorMsg)
{
    // PUT /api/v1/domains/{domain}/rrsets/{subname}/A/
    // Body: {"ttl": <ttl>, "records": ["<ip>"]}
    QJsonObject body;
    body[QStringLiteral("ttl")] = ttl;

    QJsonArray recordsArray;
    recordsArray.append(ip);
    body[QStringLiteral("records")] = recordsArray;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString path = QStringLiteral("/domains/") + domain
                   + QStringLiteral("/rrsets/") + subname + QStringLiteral("/A/");

    qInfo() << "[DeSecClient] Updating A record:" << subname << "." << domain << "→" << ip;
    QNetworkReply* reply = sendRequest("PUT", path, payload);

    if (!reply) {
        errorMsg = QStringLiteral("deSEC update A record request timed out");
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    qInfo() << "[DeSecClient] updateSubdomain HTTP" << statusCode
            << "body:" << QString::fromUtf8(responseData).left(200);

    if (statusCode == 200) {
        qInfo() << "[DeSecClient] A record updated:" << subname << "." << domain << "→" << ip;
        return true;
    } else {
        errorMsg = QStringLiteral("deSEC update A record failed (HTTP %1): %2")
                   .arg(QString::number(statusCode), QString::fromUtf8(responseData));
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}

// ---------------------------------------------------------------------------
// List domains (diagnostics)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TXT record management (ACME DNS-01 challenge)
// ---------------------------------------------------------------------------

bool DeSecClient::createTxtRecord(const QString& domain, const QString& subname,
                                   const QString& value, int ttl, QString& errorMsg)
{
    // POST /api/v1/domains/{domain}/rrsets/
    // Body: {"subname": "<subname>", "type": "TXT", "ttl": <ttl>, "records": ["<value>"]}
    QJsonObject body;
    body[QStringLiteral("subname")] = subname;
    body[QStringLiteral("type")] = QStringLiteral("TXT");
    body[QStringLiteral("ttl")] = ttl;

    QJsonArray recordsArray;
    recordsArray.append(value);
    body[QStringLiteral("records")] = recordsArray;

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QString path = QStringLiteral("/domains/") + domain + QStringLiteral("/rrsets/");

    qInfo() << "[DeSecClient] Creating TXT record:" << subname << "." << domain;
    QNetworkReply* reply = sendRequest("POST", path, payload);

    if (!reply) {
        errorMsg = QStringLiteral("deSEC create TXT record request timed out");
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    qInfo() << "[DeSecClient] createTxtRecord HTTP" << statusCode;

    if (statusCode == 201) {
        qInfo() << "[DeSecClient] TXT record created:" << subname << "." << domain;
        return true;
    } else if (statusCode == 200) {
        // RRset already exists — update is fine
        qInfo() << "[DeSecClient] TXT record already exists, updated:" << subname << "." << domain;
        return true;
    } else {
        errorMsg = QStringLiteral("deSEC create TXT record failed (HTTP %1): %2")
                   .arg(QString::number(statusCode), QString::fromUtf8(responseData));
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}

bool DeSecClient::deleteTxtRecord(const QString& domain, const QString& subname,
                                   QString& errorMsg)
{
    // DELETE /api/v1/domains/{domain}/rrsets/{subname}/TXT/
    QString path = QStringLiteral("/domains/") + domain
                   + QStringLiteral("/rrsets/") + subname + QStringLiteral("/TXT/");

    qInfo() << "[DeSecClient] Deleting TXT record:" << subname << "." << domain;
    QNetworkReply* reply = sendRequest("DELETE", path);

    if (!reply) {
        errorMsg = QStringLiteral("deSEC delete TXT record request timed out");
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }

    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    qInfo() << "[DeSecClient] deleteTxtRecord HTTP" << statusCode;

    if (statusCode == 200 || statusCode == 204) {
        qInfo() << "[DeSecClient] TXT record deleted:" << subname << "." << domain;
        return true;
    } else if (statusCode == 404) {
        // Already gone — not an error
        qInfo() << "[DeSecClient] TXT record already deleted:" << subname << "." << domain;
        return true;
    } else {
        errorMsg = QStringLiteral("deSEC delete TXT record failed (HTTP %1): %2")
                   .arg(QString::number(statusCode), QString::fromUtf8(responseData));
        qWarning() << "[DeSecClient]" << errorMsg;
        emit error(errorMsg);
        return false;
    }
}

