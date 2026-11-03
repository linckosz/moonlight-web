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

#include "LogArchive.h"
#include "common/ZipWriter.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <memory>

LogArchive::~LogArchive()
{
    if (m_Thread) {
        m_Thread->wait();
        delete m_Thread;
    }
}

QString LogArchive::kindOf(const QString& fileName)
{
    // Archives first: `moonlightweb.log.20260930-101010` is moonlightweb.log.
    QString kind = fileName;
    const int archive = kind.indexOf(QLatin1String(".log."));
    if (archive >= 0) kind.truncate(archive + 4);
    // A name ending in numbers is one run's: `…-worker-<pid>.log`, and the bench
    // traces `click-trace-<pid>-<ms>.csv`, `relay-frames-…`, `relay-audio-…`.
    // Each of them counted as a kind of its own would put every run in the zip.
    static const QRegularExpression perRun(QStringLiteral("^(.*?)(?:-\\d+)+(\\.[^.]+)$"));
    const QRegularExpressionMatch m = perRun.match(kind);
    if (m.hasMatch()) kind = m.captured(1) + QStringLiteral("-*") + m.captured(2);
    return kind;
}

QList<QFileInfo> LogArchive::latestPerKind(const QList<QFileInfo>& files)
{
    QHash<QString, QFileInfo> latest;
    for (const QFileInfo& listed : files) {
        // Asked again by its path, not taken from the directory listing. On
        // Windows the listing says what the file was when its writer last
        // closed it: a log held open is 0 bytes there however much it holds.
        // The client log is opened at the first batch and never closed, so it
        // was never in an archive (issue #48, 01/11/2026); a server log born
        // in this run (a first install, a rotation) is the same.
        const QFileInfo f(listed.absoluteFilePath());
        if (!f.isFile() || f.size() == 0) continue;
        const QString kind = kindOf(f.fileName());
        auto it = latest.find(kind);
        if (it == latest.end() || f.lastModified() > it->lastModified()) latest[kind] = f;
    }
    QList<QFileInfo> out = latest.values();
    std::sort(out.begin(), out.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.lastModified() < b.lastModified();
    });
    return out;
}

bool LogArchive::start(const QString& logDir, const QString& extraFile, const QByteArray& about,
                       const QString& fileName, const LogScrubber& scrubber)
{
    QMutexLocker lock(&m_Mutex);
    if (m_Status.state == QLatin1String("running")) return false;
    if (m_Thread) {
        m_Thread->wait();
        delete m_Thread;
        m_Thread = nullptr;
    }

    QList<QFileInfo> files =
        latestPerKind(QDir(logDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot));
    // A server logging elsewhere (--log) has its own log in the archive, in
    // place of the directory's, which an earlier run wrote.
    const QFileInfo extra(extraFile);
    if (!extraFile.isEmpty() && extra.isFile() && extra.size() > 0 &&
        extra.absolutePath() != QDir(logDir).absolutePath()) {
        files.removeIf([](const QFileInfo& f) {
            return LogArchive::kindOf(f.fileName()) == QLatin1String("moonlightweb.log");
        });
        files.append(extra);
    }

    qint64 total = 0;
    for (const QFileInfo& f : files)
        total += f.size();

    m_Status = Status();
    m_Status.state = QStringLiteral("running");
    m_Status.totalBytes = total;
    m_Status.files = int(files.size());
    m_Status.fileName = fileName;
    m_Result.clear();
    m_Done = 0;

    // Shared, because the thread's callable is const and scrubbing keeps state.
    auto scrub = std::make_shared<LogScrubber>(scrubber);
    m_Thread = QThread::create([this, files, about, scrub]() {
        ZipWriter zip;
        QString error;
        // One scrubber for the whole archive: an address or a name gets the
        // same stand-in in every file.
        zip.addFile(QStringLiteral("about.txt"), scrub->scrub(about), QDateTime::currentDateTime());
        for (const QFileInfo& f : files) {
            QFile in(f.absoluteFilePath());
            // Shared read: the live logs are open for writing, by this process
            // and by any worker still running.
            if (!in.open(QIODevice::ReadOnly)) {
                error = QStringLiteral("cannot read %1: %2").arg(f.fileName(), in.errorString());
                break;
            }
            const QByteArray data = scrub->scrub(in.readAll());
            if (!zip.addFile(f.fileName(), data, f.lastModified())) {
                error = QStringLiteral("%1 is too large for the archive").arg(f.fileName());
                break;
            }
            m_Done += f.size();
        }
        QByteArray out = error.isEmpty() ? zip.finish() : QByteArray();

        QMutexLocker done(&m_Mutex);
        m_Status.doneBytes = m_Done;
        if (error.isEmpty()) {
            m_Status.state = QStringLiteral("done");
            m_Status.size = out.size();
            m_Result = std::move(out);
        } else {
            m_Status.state = QStringLiteral("failed");
            m_Status.error = error;
        }
    });
    m_Thread->start();
    return true;
}

LogArchive::Status LogArchive::status() const
{
    QMutexLocker lock(&m_Mutex);
    Status s = m_Status;
    if (s.state == QLatin1String("running")) s.doneBytes = m_Done;
    return s;
}

QByteArray LogArchive::result() const
{
    QMutexLocker lock(&m_Mutex);
    return m_Status.state == QLatin1String("done") ? m_Result : QByteArray();
}
