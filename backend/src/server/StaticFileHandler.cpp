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

#include "StaticFileHandler.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>

QMap<QString, QString> StaticFileHandler::s_MimeTypes = {
    {"html", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "application/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"webmanifest", "application/manifest+json"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"map", "application/json"},
};

StaticFileHandler::StaticFileHandler(const QString& rootDir, QObject* parent)
    : QObject(parent)
    , m_RootDir(QDir::cleanPath(rootDir))
{
    if (!m_RootDir.endsWith('/')) m_RootDir += '/';
}

HttpResponse StaticFileHandler::serveFile(const QString& requestPath) const
{
    // Prevent directory traversal
    QString safePath = requestPath;
    if (safePath.contains("..")) return HttpResponse::error(403, "Forbidden");

    // Default to index.html for root
    if (safePath == "/" || safePath.isEmpty()) safePath = "/index.html";

    // Remove leading slash
    if (safePath.startsWith('/')) safePath = safePath.mid(1);

    QString filePath = m_RootDir + safePath;
    QFileInfo fileInfo(filePath);

    if (!fileInfo.exists() || !fileInfo.isFile())
        return HttpResponse::error(404, "File not found: " + safePath);

    if (!fileInfo.canonicalFilePath().startsWith(m_RootDir))
        return HttpResponse::error(403, "Forbidden");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return HttpResponse::error(500, "Failed to read file");

    HttpResponse resp;
    resp.statusCode = 200;
    resp.contentType = mimeType(fileInfo.suffix());
    resp.body = file.readAll();

    // Text assets (html/css/js/manifest) must always be revalidated: iOS
    // WebKit otherwise caches them aggressively (even across PWA reinstalls),
    // so frontend edits never reach the device. Images/fonts may be cached.
    const QString ext = fileInfo.suffix().toLower();
    if (ext == "html" || ext == "css" || ext == "js" || ext == "webmanifest" || ext == "json")
        resp.headers["Cache-Control"] = "no-cache, must-revalidate";

    return resp;
}

QString StaticFileHandler::mimeType(const QString& path) const
{
    QString ext = path.toLower();
    if (s_MimeTypes.contains(ext)) return s_MimeTypes[ext];
    return "application/octet-stream";
}
