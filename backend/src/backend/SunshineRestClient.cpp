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

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QTimer>
#include <QUrl>

SunshineRestClient::SunshineRestClient(QObject* parent)
    : QObject(parent)
    , m_Nam(new QNetworkAccessManager(this))
{
    m_Nam->setProxy(QNetworkProxy::NoProxy);
}

SunshineRestClient::CredentialCheck SunshineRestClient::checkCredentials(const QString& user,
                                                                         const QString& pass,
                                                                         quint16 port,
                                                                         int timeoutMs)
{
    CredentialCheck check;

    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(port);
    url.setPath(QStringLiteral("/api/apps"));

    QNetworkRequest req(url);
    // Sent up front rather than waiting for the 401 challenge: this IS the test.
    const QByteArray creds = (user + ':' + pass).toUtf8().toBase64();
    req.setRawHeader("Authorization", "Basic " + creds);
    req.setTransferTimeout(timeoutMs);

    QNetworkReply* reply = m_Nam->get(req);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });

    // Second line of defence behind setTransferTimeout: the wizard's request
    // handler is blocked in here, so this loop must always be left.
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs + 2000, &loop, &QEventLoop::quit);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        check.error = QStringLiteral("Sunshine did not answer within %1 ms").arg(timeoutMs);
        Logger::warning(QStringLiteral("Sunshine credential check: %1").arg(check.error));
        return check;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString transportError = reply->errorString();
    reply->deleteLater();

    if (status == 401 || status == 403) {
        check.outcome = CredentialCheck::Rejected;
        Logger::info(QStringLiteral("Sunshine credential check: refused (HTTP %1)").arg(status));
    } else if (status > 0) {
        // Answered at all → the Basic-Auth header got past the guard.
        check.outcome = CredentialCheck::Accepted;
        Logger::info(QStringLiteral("Sunshine credential check: accepted (HTTP %1)").arg(status));
    } else {
        check.error = transportError;
        Logger::warning(
            QStringLiteral("Sunshine credential check: unreachable (%1)").arg(transportError));
    }
    return check;
}

// One POST /api/pin, carrying whatever authorisation the caller worked out.
// `authHeader` and `cookie` are alternatives: Sunshine wants the first, Apollo
// the second. `done` gets the HTTP status, or 0 when nothing answered.
void SunshineRestClient::postPin(const QString& pin, const QString& deviceName, quint16 port,
                                 const QString& host, const QByteArray& authHeader,
                                 const QByteArray& cookie, std::function<void(int)> done)
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(host);
    url.setPort(port);
    url.setPath(QStringLiteral("/api/pin"));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!authHeader.isEmpty()) req.setRawHeader("Authorization", authHeader);
    if (!cookie.isEmpty()) req.setRawHeader("Cookie", cookie);
    req.setTransferTimeout(10000);

    QJsonObject body;
    body[QStringLiteral("pin")] = pin;
    body[QStringLiteral("name")] = deviceName;
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_Nam->post(req, payload);
    // Sunshine and Apollo both serve a self-signed cert on their management port.
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done = std::move(done)]() {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            Logger::warning(
                QStringLiteral("Sunshine /api/pin failed: %1").arg(reply->errorString()));
        } else if (status >= 200 && status < 300) {
            Logger::info(QStringLiteral("Sunshine /api/pin accepted: %1")
                             .arg(QString::fromUtf8(reply->readAll()).trimmed()));
        }
        reply->deleteLater();
        if (done) done(status);
    });
}

void SunshineRestClient::sendPin(const QString& pin, const QString& user, const QString& pass,
                                 const QString& deviceName, quint16 port, const QString& host)
{
    const QByteArray basic = "Basic " + (user + ':' + pass).toUtf8().toBase64();

    // Basic first: that is what Sunshine proper speaks, and the install-time
    // path against a local Sunshine must keep behaving exactly as it did.
    postPin(pin, deviceName, port, host, basic, QByteArray(),
            [this, pin, user, pass, deviceName, port, host](int status) {
                if (status != 401 && status != 403) return;

                // Apollo — the Sunshine fork MultiSeat puts in front of every
                // seat — dropped Basic for its management API. It refuses it
                // whatever the password (a wrong one and the right one both come
                // back 401), and expects a login that hands out an `auth` cookie
                // instead. Only fall through to that once Basic has been
                // refused, so nothing changes for a real Sunshine.
                Logger::info(QStringLiteral(
                    "Sunshine /api/pin refused Basic auth — trying the Apollo login instead"));
                loginForCookie(user, pass, port, host,
                               [this, pin, deviceName, port, host](const QByteArray& cookie) {
                                   if (cookie.isEmpty()) {
                                       Logger::warning(QStringLiteral(
                                           "Apollo login gave no session — the PIN cannot be "
                                           "pushed, so this seat stays unpaired"));
                                       return;
                                   }
                                   postPin(pin, deviceName, port, host, QByteArray(), cookie,
                                           nullptr);
                               });
            });
}

// POST /api/login {"username","password"} → Set-Cookie: auth=… . Hands back an
// empty cookie on any failure; the caller decides what that means.
void SunshineRestClient::loginForCookie(const QString& user, const QString& pass, quint16 port,
                                        const QString& host,
                                        std::function<void(const QByteArray&)> done)
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(host);
    url.setPort(port);
    url.setPath(QStringLiteral("/api/login"));

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(10000);

    QJsonObject body;
    body[QStringLiteral("username")] = user;
    body[QStringLiteral("password")] = pass;

    QNetworkReply* reply = m_Nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done = std::move(done)]() {
        QByteArray cookie;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 200 && status < 300) {
            // Take the cookie's name=value and drop the attributes: sending
            // Secure/SameSite back would make the header invalid.
            const QByteArray setCookie = reply->rawHeader("Set-Cookie");
            const int end = setCookie.indexOf(';');
            cookie = end < 0 ? setCookie : setCookie.left(end);
        } else {
            Logger::warning(
                QStringLiteral("Apollo /api/login refused the seat credentials (HTTP %1)")
                    .arg(status));
        }
        reply->deleteLater();
        if (done) done(cookie);
    });
}
