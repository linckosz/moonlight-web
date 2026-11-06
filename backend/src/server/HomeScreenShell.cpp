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

#include "server/HomeScreenShell.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

namespace mw::homescreen {

namespace {

// The rendezvous identifier's shape (bootstrap/v1/tunnel.js ID_SHAPE). Anything
// else is not an address the entry page can open, so it is never written in.
const QRegularExpression kHostId(QStringLiteral("^[0-9a-z]{26}$"));

QString dataUrl(const QByteArray& png)
{
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

QString htmlAttribute(const QString& value)
{
    return value.toHtmlEscaped();
}

// Splices @p value over the first capture of the first match. Not
// QString::replace with "\1": the value is a name somebody typed, and a
// backslash in it would be read as a back-reference.
void replaceCaptured(QString& text, const QRegularExpression& pattern, const QString& value)
{
    const QRegularExpressionMatch m = pattern.match(text);
    if (m.hasMatch()) text.replace(m.capturedStart(1), m.capturedLength(1), value);
}

} // namespace

QString title(const QString& editionName, const QString& machineName)
{
    const QString machine = machineName.trimmed();
    return machine.isEmpty() ? editionName : QStringLiteral("%1 [%2]").arg(editionName, machine);
}

QString shownMachineName(const QString& machineName, bool signedIn)
{
    return signedIn ? machineName : QString();
}

QByteArray manifest(const QByteArray& json, const QString& title, const QString& hostId,
                    const QString& handoffKey, const QByteArray& icon192, const QByteArray& icon512)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return json;

    QJsonObject obj = doc.object();
    if (!title.isEmpty()) {
        obj[QStringLiteral("name")] = title;
        obj[QStringLiteral("short_name")] = title;
    }
    if (kHostId.match(hostId).hasMatch()) {
        // The path form, the one people share: the entry page reads the machine
        // from it and needs nothing from storage. `id` follows, so two machines
        // installed side by side are two applications rather than one replaced.
        //
        // The key goes in the fragment, which no browser sends to the server it
        // fetched from: the entry page forwards #k= to the application, and the
        // introduction server never sees it. `id` stays bare, so a new key is
        // the same application and not a second one.
        const QString address = QLatin1Char('/') + hostId;
        obj[QStringLiteral("start_url")] =
            handoffKey.isEmpty() ? address
                                 : address + QStringLiteral("#k=") +
                                       QString::fromLatin1(QUrl::toPercentEncoding(handoffKey));
        obj[QStringLiteral("id")] = address;
    }

    QJsonArray icons = obj.value(QStringLiteral("icons")).toArray();
    for (qsizetype i = 0; i < icons.size(); ++i) {
        QJsonObject icon = icons.at(i).toObject();
        const QString src = icon.value(QStringLiteral("src")).toString();
        const QByteArray& bytes = src.endsWith(QLatin1String("icon-192.png"))   ? icon192
                                  : src.endsWith(QLatin1String("icon-512.png")) ? icon512
                                                                                : QByteArray();
        if (bytes.isEmpty()) continue;
        icon[QStringLiteral("src")] = dataUrl(bytes);
        icons[i] = icon;
    }
    if (!icons.isEmpty()) obj[QStringLiteral("icons")] = icons;

    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

QByteArray shell(const QByteArray& html, const QString& title, const QByteArray& icon180)
{
    QString page = QString::fromUtf8(html);

    if (!title.isEmpty()) {
        static const QRegularExpression appTitle(QString::fromLatin1(
            R"re(<meta\s+name="apple-mobile-web-app-title"\s+content="([^"]*)")re"));
        replaceCaptured(page, appTitle, htmlAttribute(title));
    }

    if (!icon180.isEmpty()) {
        static const QRegularExpression touchIcon(
            QString::fromLatin1(R"re(<link\s+rel="apple-touch-icon"[^>]*\shref="([^"]*)")re"));
        replaceCaptured(page, touchIcon, dataUrl(icon180));
    }

    return page.toUtf8();
}

} // namespace mw::homescreen
