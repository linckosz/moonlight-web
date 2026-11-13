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
#include <QCryptographicHash>
#include <QDirIterator>
#include <algorithm>
#include <QDateTime>
#include <QLocale>

#include "common/Edition.h"

// Baked in by CMake (target_compile_definitions). Fallback keeps non-CMake /
// tooling builds compiling; real builds always override it.
#ifndef MW_VERSION
#define MW_VERSION "0.0.0-dev"
#endif

// Format a QDateTime as an HTTP-date (RFC 7231), e.g. "Sun, 06 Nov 1994
// 08:49:37 GMT". Uses the C locale so day/month names are always English.
static QString httpDate(const QDateTime& dt)
{
    return QLocale::c().toString(dt.toUTC(), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss")) +
           QStringLiteral(" GMT");
}

// The icons a browser shows for this application — the tab, the home-screen
// shortcut — are the ones a DEV identity has to wear in blue. They are swapped
// here, under their usual names, rather than in index.html: the page and the
// manifest stay byte for byte what production serves, and a browser reaching
// this host through the introduction server gets the same answer as one on
// loopback (there, the tab's document is this host's index.html, served back by
// the service worker). The blue copies live in assets/dev/, which is kept out of
// the file list: nothing ever asks for them by that name.
static const QStringList kSwappedIcons = {
    QStringLiteral("assets/favicon.ico"),
    QStringLiteral("assets/icon-180.png"),
    QStringLiteral("assets/icon-192.png"),
    QStringLiteral("assets/icon-512.png"),
};

// Relative path of the file that answers `relative` (no leading slash).
static QString servedPath(const QString& rootDir, const QString& relative)
{
    if (!mw::edition::isDev() || !kSwappedIcons.contains(relative)) return relative;
    const QString swapped = mw::edition::iconAsset(relative.mid(int(qstrlen("assets/"))));
    return QFileInfo::exists(rootDir + swapped) ? swapped : relative;
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
    {"webp", "image/webp"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"map", "application/json"},
};

StaticFileHandler::StaticFileHandler(const QString& rootDir, const QString& version,
                                     QObject* parent)
    : QObject(parent)
    , m_RootDir(QDir::cleanPath(rootDir))
    , m_Version(version.isEmpty() ? QStringLiteral(MW_VERSION) : version)
{
    if (!m_RootDir.endsWith('/')) m_RootDir += '/';
}

// What exists for the developer and never for a user: the same set the install()
// rules in CMakeLists drop from a packaged build. A developer run points this
// handler straight at the source tree, so without this it would serve them, and
// list them for the tunnel's bootstrap, which pulls every listed file across a
// data channel before the page can start: a stray coverage report is not
// untidiness there, it is a minute of waiting. `relative` has no leading slash.
static bool isDevOnly(const QString& relative)
{
    static const QStringList kDevOnlyDirs = {
        QStringLiteral("node_modules"), QStringLiteral("test"),     QStringLiteral("scripts"),
        QStringLiteral("build"),        QStringLiteral("coverage"), QStringLiteral(".claude"),
    };
    // Tool configuration and notes, by name at any depth, as install() matches them.
    static const QStringList kDevOnlyFiles = {
        QStringLiteral("eslint.config.js"),  QStringLiteral("package.json"),
        QStringLiteral("package-lock.json"), QStringLiteral("prompt-frontend.md"),
        QStringLiteral("tsconfig.json"),     QStringLiteral("vitest.config.js"),
    };

    const QStringList segments = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.isEmpty()) return false;
    // Dot files are configuration for tools, not content, and the leading dot is
    // what a scanner probes for.
    if (std::any_of(segments.cbegin(), segments.cend(),
                    [](const QString& s) { return s.startsWith(QLatin1Char('.')); }))
        return true;
    // Case-blind: Windows opens PACKAGE.JSON as package.json.
    if (kDevOnlyDirs.contains(segments.first(), Qt::CaseInsensitive) && segments.size() > 1)
        return true;
    return kDevOnlyFiles.contains(segments.last(), Qt::CaseInsensitive);
}

QStringList StaticFileHandler::listFiles() const
{
    QStringList paths;
    const QDir root(m_RootDir);
    if (!root.exists()) return paths;

    QDirIterator it(m_RootDir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString relative = root.relativeFilePath(it.filePath());
        const QStringList segments = relative.split(QLatin1Char('/'));

        if (isDevOnly(relative)) continue;
        // The DEV icons are served under the production names (servedPath).
        if (segments.size() > 1 && segments[0] == QLatin1String("assets") &&
            segments[1] == QLatin1String("dev"))
            continue;

        // A source map is served on request, for whoever opens the debugger. It
        // has no business in the set every visitor downloads first.
        if (relative.endsWith(QLatin1String(".map"))) continue;

        paths << QLatin1Char('/') + relative;
    }
    paths.sort();
    return paths;
}

qint64 StaticFileHandler::servedSize(const QString& requestPath) const
{
    const QFileInfo info(m_RootDir + servedPath(m_RootDir, requestPath.mid(1)));
    return info.isFile() ? info.size() : -1;
}

QString StaticFileHandler::contentTag() const
{
    // Same set as listFiles(), by construction — it IS listFiles(), so the
    // fingerprint can never come to describe a different set than the manifest
    // it is stamped on.
    QCryptographicHash hash(QCryptographicHash::Md5);
    for (const QString& path : listFiles()) {
        // The bytes actually served under that name — a swapped icon included.
        const QFileInfo info(m_RootDir + servedPath(m_RootDir, path.mid(1)));
        hash.addData(path.toUtf8());
        hash.addData(QByteArray::number(info.lastModified().toUTC().toSecsSinceEpoch()));
        hash.addData(QByteArray::number(info.size()));
    }
    return QString::fromLatin1(hash.result().toHex().left(12));
}

HttpResponse StaticFileHandler::serveFile(const QString& requestPath,
                                          const QString& ifNoneMatch) const
{
    // Prevent directory traversal
    QString safePath = requestPath;
    if (safePath.contains("..")) return HttpResponse::error(403, "Forbidden");

    // Default to index.html for root
    if (safePath == "/" || safePath.isEmpty()) safePath = "/index.html";

    // Remove leading slash
    if (safePath.startsWith('/')) safePath = safePath.mid(1);

    QString filePath = m_RootDir + servedPath(m_RootDir, safePath);
    QFileInfo fileInfo(filePath);

    if (!fileInfo.exists() || !fileInfo.isFile())
        return HttpResponse::error(404, "File not found: " + safePath);

    if (!fileInfo.canonicalFilePath().startsWith(m_RootDir))
        return HttpResponse::error(403, "Forbidden");

    // Judged on the file actually reached, not on the spelling of the request:
    // "/js/../package.json" is refused above, but "\package.json" or a symlink
    // would get past a check of the request path. Answered like any missing file.
    if (isDevOnly(fileInfo.canonicalFilePath().mid(m_RootDir.size())))
        return HttpResponse::error(404, "File not found: " + safePath);

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
    if (ext == "html") resp.body.replace("__MW_VERSION__", m_Version.toUtf8());

    // The name a home-screen shortcut takes: a DEV identity's must say so, like
    // its desktop and tray entries. The page itself keeps its title.
    if (ext == "webmanifest" && mw::edition::isDev())
        resp.body.replace("\"MoonlightWeb\"", "\"MoonlightWebDev\"");

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
