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
#include <QMap>
#include "common/Types.h"

class StaticFileHandler : public QObject
{
    Q_OBJECT

public:
    explicit StaticFileHandler(const QString& rootDir, const QString& version = QString(),
                               QObject* parent = nullptr);

    // ifNoneMatch: the request's If-None-Match header value; when it matches the
    // asset's current ETag, a bodyless 304 Not Modified is returned so the
    // browser reuses its cached copy without re-downloading.
    HttpResponse serveFile(const QString& requestPath,
                           const QString& ifNoneMatch = QString()) const;

    /// Every path this handler will serve, as request paths ("/js/app.js").
    ///
    /// It exists for the rendezvous tunnel. A browser reached that way has no
    /// network route to this machine, so it cannot discover the application by
    /// following links the way a browser normally does: it has to pull the whole
    /// set down through the tunnel in one pass and keep it. Guessing that set by
    /// parsing index.html would miss every module a script imports at runtime,
    /// which is most of them.
    QStringList listFiles() const;

private:
    QString mimeType(const QString& path) const;
    static QMap<QString, QString> s_MimeTypes;

    QString m_RootDir;
    QString m_Version; // app version, stamped into asset URLs and ETags
};
