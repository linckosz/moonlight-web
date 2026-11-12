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

#include "Logger.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cstdio>
#include <iostream>

Logger* Logger::instance()
{
    static Logger s_Instance;
    return &s_Instance;
}

Logger* Logger::client()
{
    static Logger* s_Client = [] {
        static Logger logger;
        logger.setConsoleEcho(false);
        return &logger;
    }();
    return s_Client;
}

Logger::Logger(QObject* parent)
    : QObject(parent)
    , m_FileOpen(false)
{}

bool Logger::hasLogFile() const
{
    QMutexLocker lock(&m_Mutex);
    return m_FileOpen;
}

QString Logger::logFilePath() const
{
    QMutexLocker lock(&m_Mutex);
    return m_File.fileName();
}

void Logger::setLogFile(const QString& path, bool rotating)
{
    QMutexLocker lock(&m_Mutex);
    if (m_File.isOpen()) m_File.close();

    m_File.setFileName(path);
    m_Rotating = rotating;
    m_BytesSinceCheck = 0;

    // Roll BEFORE opening, not after the first oversized write: a host that is
    // started and stopped often would otherwise keep appending to a file that
    // was already over the ceiling and never cross it again mid-run.
    if (m_Rotating && QFileInfo(path).size() >= m_MaxFileBytes) rollOver();

    m_FileOpen = m_File.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (m_FileOpen) {
        m_Stream.setDevice(&m_File);
    } else {
        // stderr is the only place left (launchd captures it in agent.log).
        // Seen in the wild: app-data dir created root-owned by the pkg
        // postinstall → every log write silently vanished.
        fprintf(stderr, "[Logger] Cannot open log file '%s': %s\n", qPrintable(path),
                qPrintable(m_File.errorString()));
    }

    // This is what reclaims the log of a host upgrading from the unbounded era:
    // the old file was just rolled into .1 whole, so the very first check after
    // opening sees hundreds of megabytes and gives the disk back at once,
    // without anyone having to ask for it.
    if (m_Rotating) pruneArchives();
}

void Logger::log(Level level, const QString& message)
{
    if (level < m_MinLevel) return;
    QString line = QString("[%1] [%2] %3\n")
                       .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"))
                       .arg(levelString(level))
                       .arg(message);

    QMutexLocker lock(&m_Mutex);

    if (m_Tap) m_Tap(level, message);

    // Print to console (the client log excepted). In --stream-worker mode stdout
    // carries the JSON event protocol, so every level is routed to stderr instead.
    if (m_ConsoleEcho) {
        if (level >= Warning || m_ForceStderr)
            std::cerr << line.toStdString();
        else
            std::cout << line.toStdString();
    }

    // Write to file if configured
    if (m_FileOpen) {
        m_Stream << line;
        m_Stream.flush();

        if (m_Rotating) {
            // Ask the filesystem for the real size rarely: stat on every line
            // would put a syscall on a path that runs several times a second.
            m_BytesSinceCheck += line.size();
            // Never let the interval exceed a quarter of the ceiling, or a small
            // ceiling (the tests') would never be checked before it is passed.
            if (m_BytesSinceCheck >= qMin(kSizeCheckInterval, m_MaxFileBytes / 4)) {
                m_BytesSinceCheck = 0;
                rotateIfNeeded();
            }
        }
    }
}

void Logger::setTap(Tap tap)
{
    QMutexLocker lock(&m_Mutex);
    m_Tap = std::move(tap);
}

void Logger::setBudgetForTests(qint64 maxFileBytes, qint64 maxTotalBytes, qint64 retentionSeconds)
{
    QMutexLocker lock(&m_Mutex);
    m_MaxFileBytes = maxFileBytes > 0 ? maxFileBytes : kMaxFileBytes;
    m_MaxTotalBytes = maxTotalBytes > 0 ? maxTotalBytes : kMaxTotalBytes;
    m_RetentionSecs =
        retentionSeconds > 0 ? retentionSeconds : qint64(kRetentionDays) * 24 * 60 * 60;
}

void Logger::rotateIfNeeded()
{
    if (!m_FileOpen || m_File.size() < m_MaxFileBytes) return;

    const QString path = m_File.fileName();
    m_Stream.flush();
    m_File.close();
    m_FileOpen = false;

    rollOver();

    m_File.setFileName(path);
    m_FileOpen = m_File.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (m_FileOpen) {
        m_Stream.setDevice(&m_File);
    } else {
        // The rollover renamed the live file away and we cannot make a new one.
        // Say so on stderr; losing the log must never take the server with it.
        m_Stream.setDevice(nullptr);
        fprintf(stderr, "[Logger] Cannot reopen log file '%s' after rotation: %s\n",
                qPrintable(path), qPrintable(m_File.errorString()));
    }

    // After reopening, so the line explaining the trim lands in the new
    // generation rather than into a closed handle.
    pruneArchives();
}

void Logger::rollOver()
{
    const QString path = m_File.fileName();
    if (path.isEmpty()) return;

    // The archive's name is when it was closed, and its modification time is
    // when its last line was written — which is what retention is measured on.
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    QString target = path + QLatin1Char('.') + stamp;
    // Two rollovers in the same second (a host logging very fast, or a tiny
    // roll point under test) must not collide: Windows' rename refuses an
    // existing target, and a failed rename would pin the live file at its roll
    // point forever, checking and failing on every line from then on.
    for (int n = 1; QFile::exists(target) && n < 1000; ++n)
        target = path + QLatin1Char('.') + stamp + QStringLiteral("-%1").arg(n);

    QFile::rename(path, target);
}

QStringList Logger::archivePathsOldestFirst() const
{
    const QString path = m_File.fileName();
    if (path.isEmpty()) return {};

    const QFileInfo live(path);
    // Only this log's own archives: `moonlightweb.log.*` never matches a
    // worker's `moonlightweb-worker-<pid>.log` or `moonlightweb-probe.log`,
    // which belong to other processes and are none of our business.
    const QStringList names =
        live.dir().entryList({live.fileName() + QStringLiteral(".*")}, QDir::Files);

    QVector<QPair<QDateTime, QString>> dated;
    dated.reserve(names.size());
    for (const QString& name : names) {
        const QString full = live.dir().filePath(name);
        dated.append({QFileInfo(full).lastModified(), full});
    }
    std::sort(dated.begin(), dated.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    QStringList out;
    out.reserve(dated.size());
    for (const auto& d : dated)
        out.append(d.second);
    return out;
}

void Logger::pruneArchives()
{
    if (m_File.fileName().isEmpty()) return;

    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-m_RetentionSecs);
    qint64 expired = 0;
    qint64 braked = 0;

    // Age first: this is the actual promise. An archive whose last line predates
    // the window is gone whatever the disk looks like.
    for (const QString& archive : archivePathsOldestFirst()) {
        const QFileInfo info(archive);
        if (info.lastModified() >= cutoff) break; // sorted: the rest are younger
        const qint64 size = info.size();
        if (QFile::remove(archive)) expired += size;
    }

    // Then the brake, and only if it is actually hit. Oldest first again, so
    // what survives is always the most recent history. The live file is never
    // touched: it is the one still being written.
    if (footprintLocked() > m_MaxTotalBytes) {
        for (const QString& archive : archivePathsOldestFirst()) {
            if (footprintLocked() <= m_MaxTotalBytes) break;
            const qint64 size = QFileInfo(archive).size();
            if (QFile::remove(archive)) braked += size;
        }
    }

    if (expired == 0 && braked == 0) return;

    // Said out loud, not silent: this deletes the user's history, and a host
    // that simply lost its past would look broken. The brake in particular is
    // the interesting one — it means the machine is logging fast enough that
    // seven days no longer fit, which is worth seeing in the log itself.
    const QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString line;
    if (braked > 0) {
        line = QStringLiteral("[%1] [WARN] [Logger] Log is over its %2 MB ceiling — dropped %3 MB, "
                              "so history now goes back less than %4 days\n")
                   .arg(now)
                   .arg(m_MaxTotalBytes / (1024 * 1024))
                   .arg((expired + braked) / (1024 * 1024))
                   .arg(m_RetentionSecs / (24 * 60 * 60));
    } else {
        line = QStringLiteral("[%1] [INFO] [Logger] Dropped %2 MB of entries older than %3 days\n")
                   .arg(now)
                   .arg(expired / (1024 * 1024))
                   .arg(m_RetentionSecs / (24 * 60 * 60));
    }
    // Written straight to the stream: log() would re-enter the lock we hold.
    if (m_FileOpen) {
        m_Stream << line;
        m_Stream.flush();
    }
}

qint64 Logger::footprintLocked() const
{
    const QString path = m_File.fileName();
    if (path.isEmpty()) return 0;

    qint64 total = QFileInfo(path).size();
    for (const QString& archive : archivePathsOldestFirst())
        total += QFileInfo(archive).size();
    return total;
}

qint64 Logger::logFootprintBytes() const
{
    QMutexLocker lock(&m_Mutex);
    return footprintLocked();
}

QString Logger::levelString(Level level)
{
    switch (level) {
    case Debug: return "DEBUG";
    case Info: return "INFO";
    case Warning: return "WARN";
    case Error: return "ERROR";
    }
    return "???";
}
