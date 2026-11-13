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

#include "HttpParser.h"

#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace HttpParser {

namespace {

/// C0 and DEL; C1 (NEL among them) and the line and paragraph separators,
/// which some readers of a log take for a line break.
bool isControl(char16_t u)
{
    return u < 0x20 || (u >= 0x7F && u <= 0x9F) || u == 0x2028 || u == 0x2029;
}

/// True when @p s carries a control character (isControl).
///
/// QUrl::path() percent-decodes, so a request target of "/x%0d%0aEvil:1" yields
/// a path with real CR/LF in it. Anything that later echoes the path into a
/// response header — the HTTP→HTTPS redirect's Location does — would emit those
/// bytes verbatim and let the caller append headers of their own (CWE-113).
/// Detected here, at the only place that decodes, so every consumer is covered.
bool hasControlChars(const QString& s)
{
    for (const QChar c : s)
        if (isControl(c.unicode())) return true;
    return false;
}

/// The same, a tab apart: what a header's value may hold.
bool hasControlCharsButTab(const QString& s)
{
    for (const QChar c : s)
        if (c != QLatin1Char('\t') && isControl(c.unicode())) return true;
    return false;
}

} // namespace

bool isMethod(const QString& method)
{
    if (method.isEmpty() || method.size() > 16) return false;
    for (const QChar c : method)
        if (c < QLatin1Char('A') || c > QLatin1Char('Z')) return false;
    return true;
}

bool isHeaderLine(const QString& name, const QString& value)
{
    return !hasControlChars(name) && !hasControlCharsButTab(value);
}

bool mayBeServed(const HttpRequest& req)
{
    if (req.malformed || hasControlChars(req.path) || !isMethod(req.method)) return false;
    for (auto it = req.headers.cbegin(); it != req.headers.cend(); ++it)
        if (!isHeaderLine(it.key(), it.value())) return false;
    return true;
}

HttpRequest parse(const QByteArray& raw)
{
    HttpRequest req;
    QString data = QString::fromUtf8(raw);
    QStringList lines = data.split("\r\n");

    if (!lines.isEmpty()) {
        // Lines end with CR LF alone: a request line that holds any other
        // control character (a lone LF among them) is not one.
        if (hasControlChars(lines[0])) req.malformed = true;
        QStringList parts = lines[0].split(' ');
        if (parts.size() >= 2) {
            req.method = parts[0].toUpper();
            if (!isMethod(req.method)) req.malformed = true;
            QUrl url(parts[1]);
            req.path = url.path();
            if (req.path.isEmpty()) req.path = "/";
            if (hasControlChars(req.path)) req.malformed = true;
            QUrlQuery query(url);
            for (const auto& item : query.queryItems()) {
                if (hasControlChars(item.first) || hasControlChars(item.second))
                    req.malformed = true;
                req.queryParams[item.first] = item.second;
            }
        }
    }

    int i = 1;
    for (; i < lines.size(); i++) {
        if (lines[i].isEmpty()) break;
        int colon = lines[i].indexOf(':');
        if (colon > 0) {
            QString key = lines[i].left(colon).trimmed();
            QString value = lines[i].mid(colon + 1).trimmed();
            if (!isHeaderLine(key, value)) req.malformed = true;
            req.headers[key.toLower()] = value;
        }
    }

    if (i < lines.size()) {
        QStringList bodyLines = lines.mid(i + 1);
        req.body = bodyLines.join("\r\n").toUtf8();
    }
    return req;
}

} // namespace HttpParser
