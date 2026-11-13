/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The connection history (issue #42): one CSV line per stream, safe to open
 * in a spreadsheet, kept 90 days and 5 MB at most, read line by line, and
 * never in the logs archive.
 */

#include "test_framework.h"
#include "ViewerNotices.h"
#include "common/Logger.h"
#include "server/ConnectionHistory.h"
#include "server/ErrorReports.h"
#include "server/LogArchive.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

using CH = ConnectionHistory;

CH::Entry entry(const QDateTime& start, const QString& device, const QString& address,
                CH::EndedBy endedBy = CH::EndedBy::Client)
{
    CH::Entry e;
    e.start = start;
    e.end = start.addSecs(6223); // 1:43:43
    e.device = device;
    e.address = address;
    e.endedBy = endedBy;
    return e;
}

} // namespace

void run_connection_history_tests()
{
    const QDateTime t0(QDate(2026, 11, 13), QTime(21, 4, 12));

    SECTION("ConnectionHistory — a field a spreadsheet opens as text");

    CHECK_EQ(CH::field("Living room TV"), QStringLiteral("\"Living room TV\""));
    CHECK_EQ(CH::field("say \"hi\""), QStringLiteral("\"say \"\"hi\"\"\""));
    CHECK_EQ(CH::field(""), QStringLiteral("\"\""));
    // A formula is kept as text.
    CHECK_EQ(CH::field("=1+1"), QStringLiteral("\"'=1+1\""));
    CHECK_EQ(CH::field("+33 TV"), QStringLiteral("\"'+33 TV\""));
    CHECK_EQ(CH::field("-x"), QStringLiteral("\"'-x\""));
    CHECK_EQ(CH::field("@SUM(A1)"), QStringLiteral("\"'@SUM(A1)\""));
    CHECK_EQ(CH::field("\t=cmd"), QStringLiteral("\"' =cmd\""));
    CHECK_EQ(CH::field("\r=cmd"), QStringLiteral("\"' =cmd\""));
    // No line of its own, no control character.
    CHECK_EQ(CH::field("a\nb\x07"), QStringLiteral("\"a b \""));

    SECTION("ConnectionHistory — a line per stream");

    CHECK_EQ(CH::headerLine(),
             QByteArray("\"Start\",\"End\",\"Duration\",\"Device\",\"IP\",\"Connection\","
                        "\"Ended by\"\r\n"));
    {
        CH::Entry tv = entry(t0, "Living room TV", "192.168.1.50");
        tv.plainHttp = true;
        CHECK_EQ(CH::line(tv),
                 QByteArray("\"2026-11-13 21:04:12\",\"2026-11-13 22:47:55\",\"1:43:43\","
                            "\"Living room TV\",\"192.168.1.50\",\"Plain HTTP\",\"client\"\r\n"));
        CH::Entry far = entry(t0, "Bruno", "203.0.113.24", CH::EndedBy::Server);
        far.internet = true;
        CHECK(CH::line(far).contains("\"203.0.113.24\",\"HTTPS (Internet)\",\"server\""));
        CHECK(CH::line(entry(t0, "Leo", "192.168.1.66", CH::EndedBy::Tray))
                  .contains("\"HTTPS\",\"tray\""));
    }
    // The name a device gave itself, as the tray shows it (ViewerNotices::nameOf).
    {
        const QString turned = QStringLiteral("TV") + QChar(0x202E) + QStringLiteral("evil") +
                               QChar(0x2028) + QStringLiteral("x");
        const QString l = QString::fromUtf8(CH::line(entry(t0, turned, "192.168.1.50")));
        CHECK(!l.contains(QChar(0x202E)));
        CHECK(!l.contains(QChar(0x2028)));
        CHECK(l.contains(QStringLiteral("\"TVevilx\"")));
        CHECK(QString::fromUtf8(CH::line(entry(t0, "", "192.168.1.50")))
                  .contains(QStringLiteral(",\"Someone\",")));
        const QString cut =
            QString::fromUtf8(CH::line(entry(t0, QString(100, QLatin1Char('n')), "192.168.1.50")));
        CHECK(
            cut.contains(QString(ViewerNotices::kMaxNameLength, QLatin1Char('n')) + QChar(0x2026)));
        CHECK(!cut.contains(QString(ViewerNotices::kMaxNameLength + 1, QLatin1Char('n'))));
        // Still a formula's way in after the cleaning: kept as text.
        CHECK(QString::fromUtf8(CH::line(entry(t0, "=HYPERLINK(1)", "192.168.1.50")))
                  .contains(QStringLiteral(",\"'=HYPERLINK(1)\",")));
    }

    // A closed list, never free text.
    CHECK_EQ(CH::endedByName(CH::EndedBy::Client), QStringLiteral("client"));
    CHECK_EQ(CH::endedByName(CH::EndedBy::Tray), QStringLiteral("tray"));
    CHECK_EQ(CH::endedByName(CH::EndedBy::Admin), QStringLiteral("admin"));
    CHECK_EQ(CH::endedByName(CH::EndedBy::Server), QStringLiteral("server"));

    SECTION("ConnectionHistory — the administrator's alone");

    // isLocal, localChannel, lanHttp. The admin page, with its key: yes.
    CHECK(CH::mayDownload(true, false, false));
    // A device that is not the administrator, whatever it is.
    CHECK(!CH::mayDownload(false, false, false));
    CHECK(!CH::mayDownload(false, false, true));
    // The local channel holds the privilege without a key: a script could ask.
    CHECK(!CH::mayDownload(true, true, false));
    CHECK(!CH::mayDownload(false, true, false));
    // A TV on plain HTTP, even one that would somehow hold the privilege.
    CHECK(!CH::mayDownload(true, false, true));
    CHECK(!CH::mayDownload(true, true, true));

    SECTION("ConnectionHistory — the file, appended to and downloaded");

    {
        QTemporaryDir dir;
        const QString path = CH::pathIn(dir.path());
        CH history(path);
        // No file yet: the header alone.
        CHECK_EQ(history.download(t0), CH::headerLine());
        CHECK(history.append(entry(t0, "Living room TV", "192.168.1.50")));
        CHECK(history.append(entry(t0.addSecs(60), "Leo", "192.168.1.66")));
        const QByteArray all = history.download(t0.addSecs(120));
        CHECK(all.startsWith(CH::headerLine()));
        CHECK_EQ(all.count("\r\n"), qsizetype(3)); // the header once, two lines
        CHECK(all.contains("Living room TV"));
        // The owner's alone. Windows keeps that in the folder's access list,
        // which Qt's flags do not read: checked elsewhere.
        CHECK(QFile::permissions(path).testFlag(QFileDevice::ReadOwner));
#ifndef Q_OS_WIN
        CHECK(!QFile::permissions(path).testFlag(QFileDevice::ReadOther));
#endif

        // A line that cannot be read is left out, and the rest still comes.
        {
            QFile f(path);
            CHECK(f.open(QIODevice::Append));
            f.write("garbage,not a line\r\n\"2026-13-45 99:99:99\",\"x\"\r\n");
        }
        const QByteArray again = history.download(t0.addSecs(120));
        CHECK_EQ(again, all);
    }

    SECTION("ConnectionHistory — 90 days, 5 MB, the oldest first");

    {
        QTemporaryDir dir;
        CH history(CH::pathIn(dir.path()));
        history.append(entry(t0.addDays(-120), "Old", "192.168.1.10"));
        history.append(entry(t0.addDays(-91), "Older than 90 days", "192.168.1.11"));
        history.append(entry(t0.addDays(-89), "Kept", "192.168.1.12"));
        history.append(entry(t0, "Today", "192.168.1.13"));
        {
            QFile f(CH::pathIn(dir.path()));
            CHECK(f.open(QIODevice::Append));
            f.write("not a line at all\r\n");
        }
        // Read as it is: the old lines are not handed out even before a purge.
        CHECK(!history.download(t0).contains("Older than 90 days"));
        history.purge(t0);
        QFile f(CH::pathIn(dir.path()));
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray left = f.readAll();
        f.close();
        CHECK(left.startsWith(CH::headerLine()));
        CHECK(!left.contains("Old\""));
        CHECK(!left.contains("Older than 90 days"));
        CHECK(!left.contains("not a line at all"));
        CHECK(left.contains("Kept"));
        CHECK(left.contains("Today"));

        // Over the cap: the oldest of what is left goes first.
        for (int i = 0; i < 20; ++i)
            history.append(
                entry(t0.addSecs(i), QStringLiteral("Device %1").arg(i), "192.168.1.20"));
        const qint64 cap =
            CH::headerLine().size() + 5 * CH::line(entry(t0, "Device 10", "192.168.1.20")).size();
        history.purge(t0.addSecs(30), cap);
        QFile capped(CH::pathIn(dir.path()));
        CHECK(capped.open(QIODevice::ReadOnly));
        const QByteArray tail = capped.readAll();
        CHECK(tail.size() <= cap);
        CHECK(tail.contains("Device 19"));
        CHECK(!tail.contains("Kept"));
        CHECK(!tail.contains("Device 0\""));
    }

    SECTION("ConnectionHistory — never in the logs archive");

    {
        // The archive takes the logs folder whole; the history is at the data
        // folder's root, beside it.
        QTemporaryDir data;
        QDir(data.path()).mkpath(QStringLiteral("logs"));
        const QString logs = data.path() + QStringLiteral("/logs");
        {
            QFile log(logs + QStringLiteral("/moonlightweb.log"));
            CHECK(log.open(QIODevice::WriteOnly));
            log.write("a line\n");
        }
        CH history(CH::pathIn(data.path()));
        CHECK(history.append(entry(t0, "Living room TV", "192.168.1.50")));
        CHECK(!CH::pathIn(data.path()).startsWith(logs));
        const QList<QFileInfo> taken =
            LogArchive::latestPerKind(QDir(logs).entryInfoList(QDir::Files));
        CHECK_EQ(taken.size(), qsizetype(1));
        for (const QFileInfo& f : taken)
            CHECK(f.fileName() != QLatin1String(CH::kFileName));
    }

    SECTION("ConnectionHistory — never in the error reports");

    {
        // The reports carry the server's log lines and nothing else: the
        // history writes, purges, hands out and fails without a line of it.
        ErrorReports reports(QStringLiteral("0.4.4"), true, {});
        reports.setSenderForTests([](const QByteArray&) {});
        reports.attach(Logger::instance());
        const int before = reports.waitingHostLines();
        QTemporaryDir dir;
        CH history(CH::pathIn(dir.path()));
        CHECK(history.append(entry(t0, "Living room TV", "192.168.1.50")));
        history.purge(t0);
        CHECK(history.download(t0).contains("Living room TV"));
        CH nowhere(dir.path() + QStringLiteral("/no/such/folder/") + QLatin1String(CH::kFileName));
        CHECK(!nowhere.append(entry(t0, "Living room TV", "192.168.1.50")));
        CHECK_EQ(reports.waitingHostLines(), before);
    }
}
