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

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <functional>

struct HttpRequest
{
    QString method;
    QString path;
    QMap<QString, QString> headers;
    QMap<QString, QString> queryParams;
    QMap<QString, QString> pathParams;
    QByteArray body;
    QString clientAddress;      // Populated by HttpServer from socket peer
    bool isLocal = false;       // Populated by HttpServer: loopback peer OR a valid
                                // host-key session (browser on the host machine
                                // reaching us through the public domain) OR a LAN
                                // session that unlocked the remote admin password.
                                // Gates all "localhost only" admin functionality.
    bool isHostMachine = false; // Same, minus the password unlock: the caller
                                // really is the host machine. Only for actions
                                // that need the local desktop (setup wizard,
                                // Sunshine install), not for admin rights.
    bool hostTrusted = false;   // The Host header names this machine (loopback,
                                // a LAN address, an mDNS name, or our domain) —
                                // false means we were reached under someone
                                // else's name, e.g. a third-party tunnel.
    bool malformed = false;     // The request line/headers could not be trusted —
                                // currently set when the percent-decoded path
                                // carries control characters (CR/LF smuggled in
                                // as %0d/%0a). Answered with 400 and never routed.
};

struct HttpResponse
{
    int statusCode = 200;
    QString contentType;
    QMap<QString, QString> headers;
    QByteArray body;

    static HttpResponse json(const QJsonObject& obj, int status = 200);
    static HttpResponse text(const QString& text, int status = 200);
    static HttpResponse error(int status, const QString& message);
};

// Async route callback — handler calls this when response is ready
using ResponseCallback = std::function<void(HttpResponse)>;
