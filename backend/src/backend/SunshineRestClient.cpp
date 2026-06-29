/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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

#include "SunshineRestClient.h"

#include "../common/Logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QUrl>

SunshineRestClient::SunshineRestClient(QObject* parent)
    : QObject(parent)
    , m_Nam(new QNetworkAccessManager(this))
{
    m_Nam->setProxy(QNetworkProxy::NoProxy);
}

void SunshineRestClient::sendPin(const QString& pin, const QString& user, const QString& pass,
                                 const QString& deviceName, quint16 port)
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(port);
    url.setPath(QStringLiteral("/api/pin"));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray creds = (user + ':' + pass).toUtf8().toBase64();
    req.setRawHeader("Authorization", "Basic " + creds);
    req.setTransferTimeout(10000);

    QJsonObject body;
    body[QStringLiteral("pin")] = pin;
    body[QStringLiteral("name")] = deviceName;
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_Nam->post(req, payload);
    // Sunshine serves a self-signed cert on the loopback management port.
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            Logger::warning(
                QStringLiteral("Sunshine /api/pin failed: %1").arg(reply->errorString()));
        } else {
            Logger::info(QStringLiteral("Sunshine /api/pin accepted: %1")
                             .arg(QString::fromUtf8(reply->readAll()).trimmed()));
        }
        reply->deleteLater();
    });
}
