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

#include "Logger.h"
#include <QDateTime>
#include <QDebug>
#include <iostream>

Logger* Logger::instance()
{
    static Logger s_Instance;
    return &s_Instance;
}

Logger::Logger(QObject* parent)
    : QObject(parent)
    , m_FileOpen(false)
{
}

void Logger::setLogFile(const QString& path)
{
    QMutexLocker lock(&m_Mutex);
    if (m_File.isOpen())
        m_File.close();

    m_File.setFileName(path);
    m_FileOpen = m_File.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (m_FileOpen)
        m_Stream.setDevice(&m_File);
}

void Logger::log(Level level, const QString& message)
{
    QString line = QString("[%1] [%2] %3\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"))
        .arg(levelString(level))
        .arg(message);

    QMutexLocker lock(&m_Mutex);

    // Always print to console
    if (level >= Warning)
        std::cerr << line.toStdString();
    else
        std::cout << line.toStdString();

    // Write to file if configured
    if (m_FileOpen) {
        m_Stream << line;
        m_Stream.flush();
    }
}

QString Logger::levelString(Level level)
{
    switch (level) {
    case Debug:   return "DEBUG";
    case Info:    return "INFO";
    case Warning: return "WARN";
    case Error:   return "ERROR";
    }
    return "???";
}
