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

#include "server/LocalRequest.h"

#include "server/LocalChannel.h"
#include "server/RequestGuard.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace LocalRequest {

namespace {

/// The fields no answer is printed with (LocalRequest.h): the access PIN, this
/// machine's permanent identifier and its public address.
bool notPrinted(const QString& key)
{
    for (const char* name : {"pin", "unique_id", "public_ip"})
        if (key.compare(QLatin1String(name), Qt::CaseInsensitive) == 0) return true;
    return false;
}

QJsonValue withoutThem(const QJsonValue& value)
{
    if (value.isObject()) {
        QJsonObject out;
        const QJsonObject in = value.toObject();
        for (auto it = in.constBegin(); it != in.constEnd(); ++it) {
            if (notPrinted(it.key())) continue;
            out.insert(it.key(), withoutThem(it.value()));
        }
        return out;
    }
    if (value.isArray()) {
        QJsonArray out;
        const QJsonArray in = value.toArray();
        for (const QJsonValue& item : in)
            out.append(withoutThem(item));
        return out;
    }
    return value;
}

} // namespace

std::optional<Asked> parse(const QString& value)
{
    // \z, not $: the end of the text itself, never the place before a last
    // line break.
    static const QRegularExpression shape(
        QStringLiteral("\\A(GET|POST) (/[A-Za-z0-9_/-]{1,128})\\z"));
    const QRegularExpressionMatch m = shape.match(value);
    if (!m.hasMatch()) return std::nullopt;
    return Asked{m.captured(1), m.captured(2)};
}

bool allowed(const Asked& asked)
{
    if (!RequestGuard::localChannelRoute(asked.method, asked.path)) return false;
    struct Route
    {
        const char* method;
        const char* path;
    };
    static const Route kElsewhere[] = {
        {"GET", "/api/server/remote-link"},
        {"POST", "/api/admin/password"},
        {"POST", "/api/internet/enable"},
        // Not a credential: it talks to the router, which no script should
        // find itself doing by asking for a status.
        {"GET", "/api/internet/upnp-probe"},
    };
    for (const Route& route : kElsewhere)
        if (asked.method == QLatin1String(route.method) && asked.path == QLatin1String(route.path))
            return false;
    return true;
}

qsizetype maxBodyBytes()
{
    return static_cast<qsizetype>(mw::localchannel::kMaxRequestBytes);
}

QByteArray printable(const Asked& asked, const QByteArray& body)
{
    if (asked.method == QLatin1String("POST") &&
        asked.path == QLatin1String("/api/admin/pin/generate"))
        return body;
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError) return body;
    if (doc.isObject())
        return QJsonDocument(withoutThem(doc.object()).toObject()).toJson(QJsonDocument::Compact);
    if (doc.isArray())
        return QJsonDocument(withoutThem(doc.array()).toArray()).toJson(QJsonDocument::Compact);
    return body;
}

int exitCodeFor(int status)
{
    return status >= 200 && status < 300 ? 0 : 1;
}

} // namespace LocalRequest
