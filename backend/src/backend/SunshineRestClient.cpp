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

void SunshineRestClient::ensureMinChannels(int minChannels, const QString& user,
                                           const QString& pass, quint16 port)
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(port);
    url.setPath(QStringLiteral("/api/config"));

    QNetworkRequest req(url);
    const QByteArray creds = (user + ':' + pass).toUtf8().toBase64();
    req.setRawHeader("Authorization", "Basic " + creds);
    req.setTransferTimeout(10000);

    QNetworkReply* reply = m_Nam->get(req);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });

    QObject::connect(
        reply, &QNetworkReply::finished, this, [this, reply, minChannels, creds, url]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                Logger::warning(QStringLiteral("Sunshine GET /api/config failed: %1")
                                    .arg(reply->errorString()));
                return;
            }
            QJsonObject cfg = QJsonDocument::fromJson(reply->readAll()).object();
            // `channels` is stored as a string; missing = default 1.
            const int current =
                cfg.value(QStringLiteral("channels")).toVariant().toString().toInt();
            if (current >= minChannels) {
                Logger::info(QStringLiteral("Sunshine channels already %1 (>= %2) — "
                                            "leaving as is")
                                 .arg(current)
                                 .arg(minChannels));
                return;
            }
            // GET /api/config returns metadata keys alongside the
            // config — strip them before saving the config back.
            cfg.remove(QStringLiteral("platform"));
            cfg.remove(QStringLiteral("version"));
            cfg[QStringLiteral("channels")] = QString::number(minChannels);

            QNetworkRequest post(url);
            post.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            post.setRawHeader("Authorization", "Basic " + creds);
            post.setTransferTimeout(10000);
            QNetworkReply* save =
                m_Nam->post(post, QJsonDocument(cfg).toJson(QJsonDocument::Compact));
            QObject::connect(save, &QNetworkReply::sslErrors, save,
                             [save](const QList<QSslError>&) { save->ignoreSslErrors(); });
            QObject::connect(
                save, &QNetworkReply::finished, this, [this, save, minChannels, creds, url]() {
                    save->deleteLater();
                    if (save->error() != QNetworkReply::NoError) {
                        Logger::warning(QStringLiteral("Sunshine POST /api/config failed: %1")
                                            .arg(save->errorString()));
                        return;
                    }
                    Logger::info(QStringLiteral("Sunshine channels raised to %1 — restarting "
                                                "Sunshine to apply")
                                     .arg(minChannels));
                    // Apply the new limit: Sunshine only reads the
                    // config at startup.
                    QUrl restartUrl(url);
                    restartUrl.setPath(QStringLiteral("/api/restart"));
                    QNetworkRequest r(restartUrl);
                    r.setHeader(QNetworkRequest::ContentTypeHeader,
                                QStringLiteral("application/json"));
                    r.setRawHeader("Authorization", "Basic " + creds);
                    r.setTransferTimeout(10000);
                    QNetworkReply* rr = m_Nam->post(r, QByteArray("{}"));
                    QObject::connect(rr, &QNetworkReply::sslErrors, rr,
                                     [rr](const QList<QSslError>&) { rr->ignoreSslErrors(); });
                    QObject::connect(rr, &QNetworkReply::finished, rr, &QNetworkReply::deleteLater);
                });
        });
}
