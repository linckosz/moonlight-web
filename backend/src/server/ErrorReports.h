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

#include "common/Logger.h"
#include "network/SessionMetrics.h"
#include "server/LogScrubber.h"

#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <functional>

class QNetworkAccessManager;
class QTimer;

/**
 * The error reports: what went wrong during a stream, sent to the project so it
 * can be fixed without anyone having to write in.
 *
 * What is kept, and only while it waits
 * -------------------------------------
 * The server's own ERROR and WARN lines, a stream worker's among them (they come
 * through its stderr relay), in a bounded ring; and the error lines the browser
 * pages send when a stream ends (frontend/js/util/ErrorTrail.js), the owner's and
 * the guests' alike. Nothing else: no INFO line, no whole log.
 *
 * When it is sent
 * ---------------
 * A stream that ends or fails arms a timer: kSettleMs later, so that the pages'
 * lines have arrived, one report goes, and only if at least one ERROR (here or in
 * a page) is waiting. At most one every kMinGapMs: a report that would come
 * sooner waits for that mark. Nothing is ever sent during a stream, and an error
 * met outside one waits for the next stream to end.
 *
 * What it carries
 * ---------------
 * The build (version, OS, architecture), the last stream's shape as the session
 * census sees it (SessionMetrics::Facts), how it ended, every ERROR line and the
 * kSentWarnings latest WARN, and the pages' lines with their browser's family and
 * major version, never its User-Agent. A line carries how long before the report
 * it was written, not a time of day. Every line first goes through the very
 * scrubber of the "Download logs" archive (LogScrubber), plus what people are
 * called there — the hosts' app titles, the paired devices' and the guests'
 * names — on a thread of its own; then the report is cut to kMaxReportChars.
 *
 * When it stays quiet
 * -------------------
 * Under the same switch as the statistics (AppSettings::errorReportsAllowed()),
 * with MW_NO_TELEMETRY, and in any build without MW_DOMAIN + MW_PDNS_TOKEN — the
 * rule SessionMetrics follows. Fire-and-forget: a failed post is dropped, never
 * retried, and nothing about a stream waits on it.
 *
 * The receiving half is deploy/powerdns/mw-proxy/errorlog.go.
 */
class ErrorReports : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxHostLines = 300;
    static constexpr qsizetype kMaxHostChars = 128 * 1024;
    static constexpr int kSentWarnings = 30;
    static constexpr int kMaxPages = 8;
    static constexpr int kMaxPageLines = 40;
    static constexpr int kMaxLineChars = 1024;
    static constexpr qsizetype kMaxReportChars = 48 * 1024;
    static constexpr int kSettleMs = 30 * 1000;
    static constexpr qint64 kMinGapMs = 15 * 60 * 1000;

    using NamesFn = std::function<LogScrubber::Names()>;

    /// @p enabled: AppSettings::errorReportsAllowed(). @p names: what the
    /// scrubber replaces, asked for on the main thread each time a report is
    /// built (hosts come and go), with the apps, devices and guests.
    ErrorReports(QString version, bool enabled, NamesFn names, QObject* parent = nullptr);
    ~ErrorReports() override;

    /// Whether a report would leave at all: switch on and a build that may.
    bool active() const;
    /// Apply the switch without a restart; switching off drops what waits.
    void setEnabled(bool enabled);

    /// Read @p log's lines from now on (the server's own log only).
    void attach(Logger* log);
    /// One line as the logger writes it. Any thread; quick; never logs.
    void tap(Logger::Level level, const QString& message);

    /// A page's error lines, from POST /api/errors/client or
    /// /api/share/player/errors. @p who: owner | host | guest. Body:
    /// {"sent": the page's clock when it sent, "dropped": n,
    ///  "lines": [{"t": the page's clock, "n": count, "m": text}]}.
    void addPage(const QString& who, const QString& userAgent, const QJsonObject& body);

    /// Next to SessionMetrics::reportEnd / reportFailure.
    void streamEnded(const SessionMetrics::Facts& facts, int seconds);
    void streamFailed(const SessionMetrics::Facts& facts, int httpCode);

    /// A browser's family (chrome | edge | firefox | safari | opera | samsung |
    /// webview | other) and major version, from its User-Agent.
    static QString browserFamily(const QString& userAgent, int* major);

    // ── Tests ───────────────────────────────────────────────────────────────
    /// The clock (ms since the epoch) the ring and the timer read.
    void setClockForTests(std::function<qint64()> clock);
    /// Send through @p send instead of the network; active() is then true
    /// whatever the build carries, unless the switch is off.
    void setSenderForTests(std::function<void(const QByteArray&)> send);
    /// Build and send now what the timer would, synchronously; false when there
    /// was nothing to send.
    bool flushForTests();
    /// When the armed timer fires (ms since the epoch), or 0 when none is armed.
    qint64 dueForTests() const { return m_DueMs; }
    int waitingHostLines() const;

private:
    struct Line
    {
        qint64 atMs = 0;
        bool error = true;
        int count = 1;
        QString text;
    };
    struct Page
    {
        QString who;
        QString browser;
        int major = 0;
        int dropped = 0;
        QList<Line> lines;
    };
    struct Snapshot
    {
        QJsonObject header; ///< build, stream, how it ended
        int dropped = 0;    ///< host lines the ring let go
        QList<Line> host;
        QList<Page> pages;
    };

    qint64 now() const;
    void arm(const QJsonObject& header);
    void fire();
    /// Take what waits, or false when no ERROR does. Caller holds nothing.
    bool take(Snapshot* out);
    void send(const QByteArray& body);
    static QByteArray build(const Snapshot& snap, const LogScrubber::Names& names, qint64 now);
    static QJsonObject headerFor(const QString& version, const SessionMetrics::Facts& facts);

    QString m_Version;
    NamesFn m_Names;
    std::atomic<bool> m_Enabled{false}; ///< read by the tap, on any thread
    QString m_Url;
    QString m_Token;
    QNetworkAccessManager* m_Nam = nullptr;
    QTimer* m_Timer = nullptr;
    qint64 m_DueMs = 0;
    qint64 m_LastSentMs = 0;
    QJsonObject m_Header;
    Logger* m_Attached = nullptr;
    std::function<qint64()> m_Clock;
    std::function<void(const QByteArray&)> m_SendForTests;
    QThreadPool m_Pool; ///< one thread: scrubbing never runs on the HTTP thread

    mutable QMutex m_Mutex; ///< what the tap and the pages fill
    QList<Line> m_Host;
    qsizetype m_HostChars = 0;
    int m_HostDropped = 0;
    QList<Page> m_Pages;
};
