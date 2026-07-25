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
#include <QDateTime>
#include <QLocale>

// Baked in by CMake (target_compile_definitions). Fallback keeps non-CMake /
// tooling builds compiling; real builds always override it.
#ifndef MW_VERSION
#define MW_VERSION "0.0.0"
#endif

// Format a QDateTime as an HTTP-date (RFC 7231), e.g. "Sun, 06 Nov 1994
// 08:49:37 GMT". Uses the C locale so day/month names are always English.
static QString httpDate(const QDateTime& dt)
{
    return QLocale::c().toString(dt.toUTC(), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss")) +
           QStringLiteral(" GMT");
}

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

StaticFileHandler::StaticFileHandler(const QString& rootDir, const QString& version, QObject* parent)
    : QObject(parent)
    , m_RootDir(QDir::cleanPath(rootDir))
    , m_Version(version.isEmpty() ? QStringLiteral(MW_VERSION) : version)
{
    if (!m_RootDir.endsWith('/')) m_RootDir += '/';
}

HttpResponse StaticFileHandler::serveFile(const QString& requestPath, const QString& ifNoneMatch) const
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

    // Validators for conditional requests. The ETag folds in the app version so
    // a new build always busts caches even if the packager preserves mtimes;
    // mtime+size then distinguish per-file edits within a single version. It is
    // a strong validator: the served bytes are identical whenever it matches
    // (index.html's version stamp changes only when m_Version changes, which is
    // already part of the tag).
    const QDateTime lastModified = fileInfo.lastModified();
    const QString etag = QStringLiteral("\"%1-%2-%3\"")
                             .arg(m_Version)
                             .arg(lastModified.toUTC().toSecsSinceEpoch())
                             .arg(fileInfo.size());

    // Cheap 304 short-circuit: matching ETag ⇒ the browser's copy is current, so
    // skip reading and re-sending the body entirely.
    if (!ifNoneMatch.isEmpty() && ifNoneMatch.contains(etag)) {
        HttpResponse notModified;
        notModified.statusCode = 304;
        notModified.headers["ETag"] = etag;
        notModified.headers["Cache-Control"] = "no-cache, must-revalidate";
        return notModified;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return HttpResponse::error(500, "Failed to read file");

    const QString ext = fileInfo.suffix().toLower();

    HttpResponse resp;
    resp.statusCode = 200;
    resp.contentType = mimeType(ext);
    resp.body = file.readAll();

    // HTML is served with its cache-bust token resolved to the running version,
    // so <link>/<script> URLs like `css/base.css?v=__MW_VERSION__` become
    // `?v=1.2.3`. The query string changes with every release, guaranteeing a
    // fresh fetch of each stylesheet even where ETag revalidation is skipped.
    if (ext == "html")
        resp.body.replace("__MW_VERSION__", m_Version.toUtf8());

    resp.headers["ETag"] = etag;
    resp.headers["Last-Modified"] = httpDate(lastModified);

    // Text assets (html/css/js/manifest) must always be revalidated: iOS
    // WebKit otherwise caches them aggressively (even across PWA reinstalls),
    // so frontend edits never reach the device. Images/fonts may be cached.
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
