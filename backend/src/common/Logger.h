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
#include <QFile>
#include <QMutex>
#include <QStringList>
#include <QTextStream>

#include <atomic>
#include <functional>

class Logger : public QObject
{
    Q_OBJECT

public:
    enum Level
    {
        Debug,
        Info,
        Warning,
        Error
    };

    static Logger* instance();
    /// The browsers' own console, sent over while debug mode is on (client log
    /// routes): its own file, moonlightweb-client.log, with the same rotation
    /// and retention, and no console echo — those lines were printed where
    /// they were written, in the browser.
    static Logger* client();

    /**
     * What the host keeps, kept entirely by the host itself — there is no button
     * anywhere that asks the user to tidy their own log.
     *
     * The log was pure append with no ceiling until 16/09/2026, and on the
     * machine that had been running longest it had reached 472 MB over 70 days
     * — a median of 7.4 MB a day, 69% of it the per-host `serverinfo` poll that
     * runs every ~7 s whether or not anyone is streaming. That is not a
     * developer-only shape: a one-host install still writes that poll at the
     * same cadence, so an ordinary user was on course for about a gigabyte a
     * year of a file nothing ever truncated, on the system disk.
     *
     * The promise is a DURATION, not a size: seven days of history. Size is the
     * consequence, and it follows the number of hosts actually being polled all
     * by itself — a host that stops answering stops writing lines, so it stops
     * costing disk, with no activity tracking to build and no bytes-per-host
     * constant to keep up to date as log lines are added or removed. Measured
     * 16/09: ~1.1 MB per host per day, so seven days is ~10 MB for a one-host
     * install and ~135 MB for the eighteen-host bench.
     *
     * kMaxTotalBytes is NOT the budget — it is a brake. It exists for the case
     * the duration cannot defend against: a host logging abnormally (an error
     * loop, a reconnection that never succeeds) would fill a system disk long
     * before seven days were up. In ordinary use it never bites; when it does,
     * retention drops below seven days and the log says so.
     */
    static constexpr int kRetentionDays = 7;
    static constexpr qint64 kMaxTotalBytes = 128 * 1024 * 1024;
    /// Roll point for the live file. Small enough that expiry is granular (a
    /// whole archive leaves at once, so this is the resolution of the seven-day
    /// edge), large enough that a busy host is not renaming files all day.
    static constexpr qint64 kMaxFileBytes = 5 * 1024 * 1024;

    /**
     * Open `path` for appending, rolling it over first if it is already at the
     * ceiling. Rotation is also checked as lines are written.
     *
     * `rotating` false keeps the old unbounded behaviour, for the callers whose
     * file is not a long-lived server log: a `--log` path the operator chose for
     * one run, and a per-session stream worker's own file, which dies with the
     * session. Rolling those would cut a capture in half for no benefit.
     */
    void setLogFile(const QString& path, bool rotating = true);
    void log(Level level, const QString& message);

    /// Bytes the live log and its archives occupy together.
    qint64 logFootprintBytes() const;

    /// Shrink the budget so the TNR can watch it work: seven real days cannot be
    /// waited for, and pushing 128 MB through a path that flushes every line
    /// would dominate the run. Retention is given in SECONDS here.
    void setBudgetForTests(qint64 maxFileBytes, qint64 maxTotalBytes, qint64 retentionSeconds);

    /// Route ALL console echo to stderr (worker mode: stdout is the JSON
    /// event protocol and must stay clean).
    void setConsoleToStderr(bool enable) { m_ForceStderr = enable; }

    /// Lines below @p level are dropped, from the file and the console alike.
    /// Info by default: DEBUG is what --verbose (or its box on the admin
    /// page) brings back. Dropped at the source, so a worker's DEBUG never
    /// reaches the server's log through the stderr relay either.
    void setMinLevel(Level level) { m_MinLevel = level; }
    Level minLevel() const { return Level(m_MinLevel.load()); }
    /// Whether log() also prints to the console. On, except for client().
    void setConsoleEcho(bool enable) { m_ConsoleEcho = enable; }
    /// Handed every line this logger keeps (past the level check), with its
    /// level and text, as it is written: how the error reports (ErrorReports.h)
    /// read the server's own log. Runs under this logger's lock, on whichever
    /// thread logged: it must be quick and must never log. Empty to remove.
    using Tap = std::function<void(Level, const QString&)>;
    void setTap(Tap tap);

    /// Whether setLogFile() has opened a file.
    bool hasLogFile() const;
    /// The file set by setLogFile(), empty when none was.
    QString logFilePath() const;

    static void debug(const QString& msg) { instance()->log(Debug, msg); }
    static void info(const QString& msg) { instance()->log(Info, msg); }
    static void warning(const QString& msg) { instance()->log(Warning, msg); }
    static void error(const QString& msg) { instance()->log(Error, msg); }

private:
    explicit Logger(QObject* parent = nullptr);
    QString levelString(Level level);

    /// Characters written between two size checks. Small enough that the file
    /// never overshoots the ceiling by more than this, large enough that the
    /// stat costs nothing against the poll traffic that fills the log.
    static constexpr qint64 kSizeCheckInterval = 64 * 1024;

    /// Rename the live file to `name.<yyyyMMdd-hhmmss>`. Timestamped rather than
    /// a shifting .1/.2/.3 chain: retention is by age, so the count of archives
    /// is whatever seven days happen to need, not a number fixed in advance.
    /// Caller holds m_Mutex and the file must be closed.
    void rollOver();
    /// Close, roll and reopen when the live file has reached the roll point.
    /// Caller holds m_Mutex.
    void rotateIfNeeded();
    /// Drop archives past the retention window, then — only if the brake is hit
    /// — the oldest survivors until the total fits. Caller holds m_Mutex.
    void pruneArchives();
    /// Every `name.*` beside the live file, oldest first by modification time,
    /// which is when its last line was written. Caller holds m_Mutex.
    QStringList archivePathsOldestFirst() const;
    /// logFootprintBytes without taking the lock. Caller holds m_Mutex.
    qint64 footprintLocked() const;

    QFile m_File;
    QTextStream m_Stream;
    mutable QMutex m_Mutex;
    bool m_FileOpen;
    bool m_ForceStderr = false;
    std::atomic<int> m_MinLevel{Info};
    std::atomic<bool> m_ConsoleEcho{true};
    bool m_Rotating = false;
    Tap m_Tap;
    qint64 m_MaxFileBytes = kMaxFileBytes;
    qint64 m_MaxTotalBytes = kMaxTotalBytes;
    qint64 m_RetentionSecs = qint64(kRetentionDays) * 24 * 60 * 60;
    /// Written since the last size check, so the common path costs no syscall.
    qint64 m_BytesSinceCheck = 0;
};
