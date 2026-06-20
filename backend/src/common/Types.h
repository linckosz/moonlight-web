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

#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <functional>

struct HttpRequest {
    QString method;
    QString path;
    QMap<QString, QString> headers;
    QMap<QString, QString> queryParams;
    QMap<QString, QString> pathParams;
    QByteArray body;
    QString clientAddress;   // Populated by HttpServer from socket peer
};

struct HttpResponse {
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
