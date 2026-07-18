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
 * management API on its HTTPS port (47990 by default). Currently only used by
 * the install-time provisioning to push the pairing PIN automatically
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

    /// POST /api/pin {"pin","name"} with Basic Auth. Fire-and-forget: result is
    /// logged. Targets https://127.0.0.1:<port>/api/pin (port = Sunshine HTTPS).
    void sendPin(const QString& pin, const QString& user, const QString& pass,
                 const QString& deviceName = QStringLiteral("moonlightweb"), quint16 port = 47990);

    /// Ensure Sunshine's "Maximum Connected Clients" (config key `channels`)
    /// is at least `minChannels` — required for the dual-stream seamless
    /// switching (two concurrent sessions on the shared app). Reads
    /// GET /api/config; if the current value is already >= min, does nothing.
    /// Otherwise saves the updated config (POST /api/config) and asks Sunshine
    /// to restart (POST /api/restart) so the new limit applies. Fire-and-forget.
    void ensureMinChannels(int minChannels, const QString& user, const QString& pass,
                           quint16 port = 47990);

private:
    QNetworkAccessManager* m_Nam = nullptr;
};
