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

#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;

/**
 * @brief Minimal client for Sunshine's modern REST API (Basic Auth).
 *
 * Distinct from the GameStream/NvHTTP protocol: this talks to Sunshine's
 * management API on its HTTPS port (47990 by default). Used by the setup wizard
 * to check the credentials the user typed for an existing Sunshine, and by the
 * install-time provisioning to push the pairing PIN automatically
 * (`POST /api/pin`) instead of having the user type it in Sunshine's web UI.
 *
 * Sunshine serves a self-signed certificate, so TLS errors are ignored for the
 * loopback target only.
 */
class SunshineRestClient : public QObject
{
    Q_OBJECT

public:
    explicit SunshineRestClient(QObject* parent = nullptr);

    /// Outcome of checkCredentials(): the credentials were accepted, refused by
    /// Sunshine, or Sunshine never answered.
    struct CredentialCheck
    {
        enum Outcome
        {
            Accepted,
            Rejected,
            Unreachable,
        };
        Outcome outcome = Unreachable;
        QString error; ///< Transport error, for Unreachable only.
    };

    /// Blocking Basic-Auth probe of an already-installed Sunshine: GET
    /// /api/apps on https://127.0.0.1:<port>. Runs a nested event loop (the
    /// setup wizard's request handler is synchronous, like pairSunshine).
    ///
    /// Only a 401/403 means "wrong credentials": any other answer proves the
    /// Basic-Auth header was accepted, so an unknown endpoint on some other
    /// Sunshine version can never be mistaken for a bad password.
    CredentialCheck checkCredentials(const QString& user, const QString& pass, quint16 port = 47990,
                                     int timeoutMs = 8000);

    /// POST /api/pin {"pin","name"} with Basic Auth. Fire-and-forget: result is
    /// logged. Targets https://<host>:<port>/api/pin.
    ///
    /// `host` defaults to loopback, which is what the install-time provisioning
    /// uses. A MultiSeat seat needs a real address: its Apollo runs on the
    /// MultiSeat machine, one web UI per seat at PortBase + 1, and that is the
    /// only way to pair a seat at all — MultiSeat's own API can unpair clients
    /// but never pair one.
    void sendPin(const QString& pin, const QString& user, const QString& pass,
                 const QString& deviceName = QStringLiteral("moonlightweb"), quint16 port = 47990,
                 const QString& host = QStringLiteral("127.0.0.1"));

private:
    QNetworkAccessManager* m_Nam = nullptr;
};
