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
#include <QFile>
#include <QMutex>
#include <QTextStream>

class Logger : public QObject
{
    Q_OBJECT

public:
    enum Level { Debug, Info, Warning, Error };

    static Logger* instance();

    void setLogFile(const QString& path);
    void log(Level level, const QString& message);

    static void debug(const QString& msg)   { instance()->log(Debug, msg); }
    static void info(const QString& msg)    { instance()->log(Info, msg); }
    static void warning(const QString& msg) { instance()->log(Warning, msg); }
    static void error(const QString& msg)   { instance()->log(Error, msg); }

private:
    explicit Logger(QObject* parent = nullptr);
    QString levelString(Level level);

    QFile m_File;
    QTextStream m_Stream;
    QMutex m_Mutex;
    bool m_FileOpen;
};
