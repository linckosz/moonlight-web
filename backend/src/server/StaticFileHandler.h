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

#include <QObject>
#include <QMap>
#include "common/Types.h"

class StaticFileHandler : public QObject
{
    Q_OBJECT

public:
    explicit StaticFileHandler(const QString& rootDir, QObject* parent = nullptr);

    HttpResponse serveFile(const QString& requestPath) const;

private:
    QString mimeType(const QString& path) const;
    static QMap<QString, QString> s_MimeTypes;

    QString m_RootDir;
};
