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

#include <QByteArray>
#include <QDateTime>
#include <QFileInfo>
#include <QList>
#include <QMutex>
#include <QString>

#include "server/LogScrubber.h"

#include <atomic>

class QThread;

/**
 * The "Download logs" archive (admin page → Advanced): what someone attaches
 * to an email or an issue so the problem they just had can be read.
 *
 * Only the latest non-empty file of each kind — the server's log, the last
 * stream worker's, the browsers', the probe's, a bench's last traces... — never
 * the whole directory: a busy host keeps a week of them (a bench machine, 1.5 GB
 * in 6,000 files), and what explains a problem is what was written last. An
 * about.txt says which build and which system wrote them.
 * Every file, about.txt included, goes through one LogScrubber first: the
 * archive is made to be posted, and what is secret or personal in a log is
 * taken out before it goes in.
 *
 * Built on a thread of its own (the HTTP server is single-threaded, and a
 * large worker log takes a moment to deflate), polled for its progress, then
 * handed over whole. One archive at a time; the last one stays until the next.
 */
class LogArchive
{
public:
    struct Status
    {
        QString state = QStringLiteral("idle"); ///< idle | running | done | failed
        qint64 doneBytes = 0;                   ///< of the logs read and deflated so far
        qint64 totalBytes = 0;
        int files = 0;
        qint64 size = 0; ///< the archive's, once done
        QString fileName;
        QString error;
    };

    LogArchive() = default;
    ~LogArchive();
    LogArchive(const LogArchive&) = delete; // owns its build thread
    LogArchive& operator=(const LogArchive&) = delete;

    /// Start building from @p logDir, plus @p extraFile (the server's own log
    /// when --log put it elsewhere; may be empty), every file and @p about
    /// through @p scrubber. False while one is running.
    bool start(const QString& logDir, const QString& extraFile, const QByteArray& about,
               const QString& fileName, const LogScrubber& scrubber = LogScrubber());
    Status status() const;
    /// The finished archive; empty unless status().state is "done".
    QByteArray result() const;

    /// The kind a log file belongs to: its archives (`name.log.<stamp>`) are
    /// its own kind, and the files one run names after itself are one kind:
    /// every `…-worker-<pid>.log`, every `click-trace-<pid>-<ms>.csv`.
    static QString kindOf(const QString& fileName);
    /// The newest non-empty file of each kind among @p files, oldest first.
    /// Each file's size and date are asked again by its path: a directory
    /// listing's are stale for a file still open for writing (Windows).
    static QList<QFileInfo> latestPerKind(const QList<QFileInfo>& files);

private:
    mutable QMutex m_Mutex;
    QThread* m_Thread = nullptr;
    Status m_Status;
    QByteArray m_Result;
    std::atomic<qint64> m_Done{0};
};
