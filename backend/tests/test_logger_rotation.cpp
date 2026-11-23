/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What the host keeps of its own log. It was a pure append with no ceiling
 * until 16/09/2026: 472 MB over 70 days on the machine running longest, a
 * median of 7.4 MB a day, of which 69% was the per-host `serverinfo` poll that
 * runs every ~7 s whether or not anyone streams. Nothing there is
 * developer-specific — a one-host install writes that same poll — so an
 * ordinary user was on course for roughly a gigabyte a year on their system
 * disk.
 *
 * The promise is a DURATION: seven days of history. Size is the consequence and
 * follows the number of hosts actually polled, because a host that stops
 * answering stops writing lines. kMaxTotalBytes is not the budget but a brake,
 * for the one case a duration cannot defend against — a host logging abnormally
 * would fill a disk long before seven days were up.
 *
 * What these tests hold: the live file stays bounded, archives expire on AGE
 * (not on a count, and not on size while the disk is fine), the brake trims
 * oldest-first when it is hit, both say so in the log, a host upgrading from
 * the unbounded era gets its disk back on the first open, the archive glob
 * never touches a neighbouring worker or probe log, and the two callers whose
 * file is NOT a long-lived server log — an operator's --log path, a
 * per-session worker — are left completely alone. And since 15/11/2026, a log
 * that stops taking lines (an open or a write refused, a rollover whose new
 * file cannot be made) comes back by itself and says how much it lost.
 */
#include "test_framework.h"

#include "common/Logger.h"
#include "common/PrivilegedPlace.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <iostream>
#include <sstream>

namespace {

qint64 sizeOf(const QString& path)
{
    return QFileInfo(path).size();
}

void writeLines(int count)
{
    const QString filler(200, QLatin1Char('x'));
    for (int i = 0; i < count; ++i)
        Logger::info(filler);
}

/// Write until the live file is seen to shrink — exactly one rollover, however
/// the roll point and the size-check interval happen to line up.
bool fillUntilRollover(const QString& path)
{
    const QString filler(200, QLatin1Char('x'));
    qint64 prev = sizeOf(path);
    for (int i = 0; i < 20000; ++i) {
        Logger::info(filler);
        const qint64 now = sizeOf(path);
        if (now < prev) return true;
        prev = now;
    }
    return false;
}

bool fileContains(const QString& path, const QString& needle)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QByteArray all = f.readAll();
    f.close();
    return all.contains(needle.toUtf8());
}

/// Archives beside `log`, i.e. everything named `log.*`.
QStringList archivesOf(const QString& log)
{
    const QFileInfo info(log);
    return info.dir().entryList({info.fileName() + QStringLiteral(".*")}, QDir::Files);
}

/// Age a file by moving its modification time back. Retention is measured on
/// that timestamp, so this buys days of history without waiting for them —
/// sleeping through a real window would make the suite unrunnable.
bool backdate(const QString& path, int secs)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    const bool ok = f.setFileTime(QDateTime::currentDateTime().addSecs(-secs),
                                  QFileDevice::FileModificationTime);
    f.close();
    return ok;
}

/// Every line of `path` that carries `tag`, and whether each one is whole:
/// "<tag> <n> " followed by the filler and nothing else.
bool wholeLines(const QString& path, const QString& tag, int expected, const QString& filler)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QList<QByteArray> lines = f.readAll().split('\n');
    f.close();
    const QByteArray t = tag.toUtf8();
    const QByteArray tail = filler.toUtf8();
    QList<bool> seen(expected, false);
    for (const QByteArray& line : lines) {
        const int at = line.indexOf(t + ' ');
        if (at < 0) continue;
        const QByteArray rest = line.mid(at + t.size() + 1);
        const int space = rest.indexOf(' ');
        if (space <= 0) return false;
        bool ok = false;
        const int n = rest.left(space).toInt(&ok);
        if (!ok || n < 0 || n >= expected || seen[n]) return false;
        if (rest.mid(space + 1) != tail) return false;
        seen[n] = true;
    }
    return !seen.contains(false);
}

/// Logger echoes every INFO to stdout. Filling a log several times over means
/// thousands of lines of filler, which would bury the suite's own output — so
/// stdout is parked on a sink for the duration. The framework writes to stderr
/// and is unaffected.
class MutedStdout
{
public:
    MutedStdout()
        : m_Saved(std::cout.rdbuf(m_Sink.rdbuf()))
    {}
    ~MutedStdout() { std::cout.rdbuf(m_Saved); }

private:
    std::ostringstream m_Sink;
    std::streambuf* m_Saved;
};

} // namespace

void run_logger_rotation_tests()
{
    SECTION("Logger rotation");
    MutedStdout quiet;

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString log = dir.filePath(QStringLiteral("moonlightweb.log"));

    // Production is 5 MB / 128 MB / 7 days. Scaled down: pushing 128 MB through
    // a path that flushes every line, or waiting a week, would make this suite
    // unrunnable. Retention is an hour here, and files are aged by hand.
    const qint64 kFile = 8 * 1024;
    const qint64 kTotal = 64 * 1024;
    const qint64 kRetentionSecs = 3600;
    Logger::instance()->setBudgetForTests(kFile, kTotal, kRetentionSecs);

    // ── The live file stays bounded, and archives appear ────────────────────
    Logger::instance()->setLogFile(log);
    CHECK(fillUntilRollover(log));
    CHECK(archivesOf(log).size() >= 1);
    // The overshoot is one size-check interval plus one line, never unbounded.
    CHECK(sizeOf(log) < kFile * 2);

    // ── Archives are kept on AGE, not on a count ────────────────────────────
    // A young archive survives however many there are; the disk is fine, so
    // nothing else has any business deleting it.
    {
        CHECK(fillUntilRollover(log));
        CHECK(fillUntilRollover(log));
        const int before = archivesOf(log).size();
        CHECK(before >= 3);

        // Nothing has aged out and the brake is not hit: all of them stay.
        writeLines(5);
        CHECK(archivesOf(log).size() == before);
    }

    // ── An archive past the window is dropped, and the log says so ──────────
    {
        const QStringList names = archivesOf(log);
        CHECK(!names.isEmpty());
        const QString oldest = QFileInfo(log).dir().filePath(names.first());
        CHECK(backdate(oldest, kRetentionSecs * 2)); // older than the window

        const int before = archivesOf(log).size();
        CHECK(fillUntilRollover(log)); // any rollover runs the prune
        const QStringList after = archivesOf(log);

        CHECK(!after.contains(QFileInfo(oldest).fileName())); // the aged one left
        CHECK(after.size() == before);                        // and one fresh archive replaced it
        CHECK(fileContains(log, QStringLiteral("older than")));
    }

    // ── The brake trims oldest-first when the disk is the problem ───────────
    // Retention alone cannot defend against a host logging abnormally fast:
    // nothing is old yet, and the disk fills anyway.
    {
        writeLines(4000); // far more than kTotal, all of it minutes old
        CHECK(Logger::instance()->logFootprintBytes() <= kTotal);
        CHECK(fileContains(log, QStringLiteral("over its")));
    }

    // ── Neighbouring logs are never touched ─────────────────────────────────
    // `moonlightweb.log.*` must not reach a worker's per-PID file or the
    // probe's, which belong to other processes.
    {
        const QString worker = dir.filePath(QStringLiteral("moonlightweb-worker-123.log"));
        const QString probe = dir.filePath(QStringLiteral("moonlightweb-probe.log"));
        for (const QString& p : {worker, probe}) {
            QFile f(p);
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("not ours\n");
            f.close();
            CHECK(backdate(p, kRetentionSecs * 100)); // ancient, and irrelevant
        }

        writeLines(2000); // plenty of rollovers and prunes
        CHECK(QFile::exists(worker));
        CHECK(QFile::exists(probe));
    }

    // ── A host upgrading from the unbounded era gets its disk back at once ──
    {
        const QString legacy = dir.filePath(QStringLiteral("legacy.log"));
        QFile seed(legacy);
        CHECK(seed.open(QIODevice::WriteOnly));
        seed.write(QByteArray(int(kTotal) * 4, 'y')); // far past the brake
        seed.close();
        CHECK(sizeOf(legacy) > kTotal);

        Logger::instance()->setLogFile(legacy);
        CHECK(Logger::instance()->logFootprintBytes() <= kTotal);
        CHECK(sizeOf(legacy) < kFile); // the live file restarted small
    }

    // ── A non-rotating file is left completely alone ────────────────────────
    // An operator's --log path, and a per-session worker's own file: rolling or
    // pruning either would destroy the very capture that was asked for.
    {
        const QString fixed = dir.filePath(QStringLiteral("operator.log"));
        Logger::instance()->setLogFile(fixed, /*rotating=*/false);
        writeLines(600);
        CHECK(sizeOf(fixed) > kTotal); // grew straight past the brake
        CHECK(archivesOf(fixed).isEmpty());
    }

    // ── A log that stops taking lines comes back by itself ──────────────────
    // 15/11/2026: a host's log went silent at a rotation and stayed silent for
    // hours, the server still running, until a restart. A refused open or
    // write must close the file and try again, bounded in time, and the file
    // must say how much was lost.
    {
        // A real path that cannot be opened: a directory under the log's name.
        const QString blocked = dir.filePath(QStringLiteral("blocked.log"));
        CHECK(QDir().mkdir(blocked));
        Logger::instance()->setReopenRetryForTests(3600 * 1000);
        Logger::instance()->setLogFile(blocked, /*rotating=*/false);
        CHECK(!Logger::instance()->hasLogFile());
        writeLines(3);

        // The path is free again, but the delay has not passed: no attempt on
        // every line, a broken log costs one open per delay at most.
        CHECK(QDir().rmdir(blocked));
        writeLines(2);
        CHECK(!QFile::exists(blocked));

        Logger::instance()->setReopenRetryForTests(0);
        Logger::info(QStringLiteral("blocked log is back"));
        CHECK(Logger::instance()->hasLogFile());
        CHECK(fileContains(blocked, QStringLiteral("Log resumed")));
        CHECK(fileContains(blocked, QStringLiteral("5 lines lost")));
        CHECK(fileContains(blocked, QStringLiteral("blocked log is back")));
    }

    // A write refused on an open file, then one refused reopen: the two lines
    // are counted, and the first line that gets through follows the notice.
    {
        const QString live = dir.filePath(QStringLiteral("refused.log"));
        Logger::instance()->setLogFile(live, /*rotating=*/false);
        Logger::info(QStringLiteral("refused-before"));
        Logger::instance()->failNextForTests(/*opens=*/1, /*writes=*/1);
        Logger::info(QStringLiteral("refused-first"));  // the write is refused
        Logger::info(QStringLiteral("refused-second")); // and so is the reopen
        Logger::info(QStringLiteral("refused-after"));
        CHECK(Logger::instance()->hasLogFile());
        CHECK(fileContains(live, QStringLiteral("refused-before")));
        CHECK(!fileContains(live, QStringLiteral("refused-first")));
        CHECK(!fileContains(live, QStringLiteral("refused-second")));
        CHECK(fileContains(live, QStringLiteral("2 lines lost")));
        CHECK(fileContains(live, QStringLiteral("refused-after")));
    }

    // A rollover whose new file cannot be opened: the archive goes back under
    // the live name and keeps the lines, and the next size check rolls it.
    {
        const QString kept = dir.filePath(QStringLiteral("kept.log"));
        Logger::instance()->setReopenRetryForTests(-1); // the real delay
        Logger::instance()->setLogFile(kept);
        Logger::info(QStringLiteral("kept-marker"));
        Logger::instance()->failNextForTests(/*opens=*/1, /*writes=*/0);
        CHECK(fillUntilRollover(kept));
        CHECK(Logger::instance()->hasLogFile());
        const QStringList archives = archivesOf(kept);
        CHECK(archives.size() == 1); // the refused roll left no archive of its own
        if (archives.size() == 1) {
            const QString archive = QFileInfo(kept).dir().filePath(archives.first());
            CHECK(fileContains(archive, QStringLiteral("kept-marker")));
            CHECK(!fileContains(archive, QStringLiteral("lines lost")));
        }
        CHECK(!fileContains(kept, QStringLiteral("lines lost")));
    }
    Logger::instance()->failNextForTests(0, 0);
    Logger::instance()->setReopenRetryForTests(-1);

    // ── Several writers on one file ─────────────────────────────────────────
    // Every write goes to the end of the file, even with several writers: a
    // server and the one that replaces it, a worker. Two descriptors opened
    // the way the Logger opens its file, written in turn: every line whole.
    // As a process with its user's rights, even where the suite runs elevated
    // (the CI's runner): a privileged one opens only in a place of its own.
    mw::place::setForTests(QString(), {}, /*privileged=*/false);
    {
        const QString filler(120, QLatin1Char('y'));
        const QString shared = dir.filePath(QStringLiteral("shared.log"));
        QFile one(shared), two(shared);
        QString why;
        CHECK(mw::place::openForAppend(one, &why));
        CHECK(mw::place::openForAppend(two, &why));
        for (int i = 0; i < 200; ++i) {
            one.write(QStringLiteral("one %1 %2\n").arg(i).arg(filler).toUtf8());
            one.flush();
            two.write(QStringLiteral("two %1 %2\n").arg(i).arg(filler).toUtf8());
            two.flush();
        }
        one.close();
        two.close();
        CHECK(wholeLines(shared, QStringLiteral("one"), 200, filler));
        CHECK(wholeLines(shared, QStringLiteral("two"), 200, filler));

        // After a rollover the Logger opens its file again: a second writer on
        // the new file, and still every line whole.
        const QString rolled = dir.filePath(QStringLiteral("rolled.log"));
        Logger::instance()->setLogFile(rolled);
        CHECK(fillUntilRollover(rolled));
        QFile other(rolled);
        CHECK(mw::place::openForAppend(other, &why));
        for (int i = 0; i < 15; ++i) {
            Logger::info(QStringLiteral("mine %1 %2").arg(i).arg(filler));
            other.write(QStringLiteral("other %1 %2\n").arg(i).arg(filler).toUtf8());
            other.flush();
        }
        other.close();
        CHECK(wholeLines(rolled, QStringLiteral("mine"), 15, filler));
        CHECK(wholeLines(rolled, QStringLiteral("other"), 15, filler));
    }
    mw::place::resetForTests();

    // Leave the singleton pointing somewhere harmless: QTemporaryDir cannot
    // remove a file this process still holds open.
    Logger::instance()->setBudgetForTests(0, 0, 0); // back to the real budget
    Logger::instance()->setLogFile(QDir::tempPath() + QStringLiteral("/mw-backend-tnr.log"),
                                   /*rotating=*/false);
}
