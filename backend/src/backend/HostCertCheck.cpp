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

#include "backend/HostCertCheck.h"

#include <QNetworkProxy>
#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTimer>

#include <memory>

namespace HostCertCheck {

bool sameCertificate(const QByteArray& expectedPem, const QByteArray& candidatePem)
{
    const QSslCertificate expected(expectedPem, QSsl::Pem);
    const QSslCertificate candidate(candidatePem, QSsl::Pem);
    return !expected.isNull() && expected == candidate;
}

void look(const QString& address, quint16 httpsPort, const QByteArray& expectedPem,
          const QByteArray& clientCertPem, const QByteArray& clientKeyPem, QObject* context,
          int timeoutMs, std::function<void(Shown)> done)
{
    auto* socket = new QSslSocket(context);
    socket->setProxy(QNetworkProxy::NoProxy);

    // Hosts sign their own certificates: comparing it is the whole check.
    QSslConfiguration config = socket->sslConfiguration();
    config.setLocalCertificate(QSslCertificate(clientCertPem, QSsl::Pem));
    config.setPrivateKey(QSslKey(clientKeyPem, QSsl::Rsa, QSsl::Pem));
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    socket->setSslConfiguration(config);

    auto* timer = new QTimer(socket);
    timer->setSingleShot(true);

    // Every way out comes through here, and the first one wins.
    auto report = [socket, done = std::move(done),
                   reported = std::make_shared<bool>(false)](Shown shown) {
        if (*reported) return;
        *reported = true;
        socket->disconnect();
        socket->abort();
        socket->deleteLater();
        done(shown);
    };

    QObject::connect(socket, &QSslSocket::encrypted, socket, [socket, expectedPem, report]() {
        report(sameCertificate(expectedPem, socket->peerCertificate().toPem()) ? Shown::Expected
                                                                               : Shown::Another);
    });
    QObject::connect(socket, &QAbstractSocket::errorOccurred, socket,
                     [report](QAbstractSocket::SocketError) { report(Shown::Nothing); });
    QObject::connect(timer, &QTimer::timeout, socket, [report]() { report(Shown::Nothing); });

    timer->start(timeoutMs);
    socket->connectToHostEncrypted(address, httpsPort);
}

void verify(const QString& address, quint16 httpsPort, const QByteArray& expectedPem,
            const QByteArray& clientCertPem, const QByteArray& clientKeyPem, QObject* context,
            int timeoutMs, std::function<void(bool)> done)
{
    look(address, httpsPort, expectedPem, clientCertPem, clientKeyPem, context, timeoutMs,
         [done = std::move(done)](Shown shown) { done(shown == Shown::Expected); });
}

} // namespace HostCertCheck
