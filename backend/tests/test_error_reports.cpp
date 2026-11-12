/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The error reports leave this machine. What they keep, when they leave and
 * what they carry are privacy properties as much as features: a line that
 * should have been scrubbed, or a report sent during a stream, is invisible at
 * runtime. They are pinned here, without the network (setSenderForTests).
 */
#include "test_framework.h"
#include "server/ErrorReports.h"
#include "server/routes/ErrorRoutes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

const qint64 kNow = 1'800'000'000'000; // a fixed clock, ms since the epoch

struct Harness
{
    QList<QJsonObject> sent;
    qint64 clock = kNow;
    ErrorReports reports;

    explicit Harness(ErrorReports::NamesFn names = {})
        : reports(QStringLiteral("0.4.4"), true, std::move(names))
    {
        reports.setClockForTests([this]() { return clock; });
        reports.setSenderForTests(
            [this](const QByteArray& body) { sent << QJsonDocument::fromJson(body).object(); });
    }
};

QJsonObject pageBody(qint64 sent, const QList<QPair<qint64, QString>>& lines)
{
    QJsonArray a;
    for (const auto& l : lines)
        a.append(
            QJsonObject{{QStringLiteral("t"), double(l.first)}, {QStringLiteral("m"), l.second}});
    return QJsonObject{{QStringLiteral("sent"), double(sent)}, {QStringLiteral("lines"), a}};
}

const char* kChromeUa = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
                        "like Gecko) Chrome/131.0.0.0 Safari/537.36";

} // namespace

void run_error_reports_tests()
{
    SECTION("ErrorReports");

    // ── Silent unless everything lines up ───────────────────────────────────
    // The test environment has no MW_DOMAIN / MW_PDNS_TOKEN: a self-built
    // binary's situation, where nothing may leave whatever the switch says.
    {
        ErrorReports unbuilt(QStringLiteral("0.4.4"), true, {});
        CHECK(!unbuilt.active());
        Harness h;
        CHECK(h.reports.active());
        h.reports.setEnabled(false);
        CHECK(!h.reports.active());
        h.reports.tap(Logger::Error, QStringLiteral("[X] off"));
        CHECK_EQ(h.reports.waitingHostLines(), 0);
    }

    // ── What the tap keeps ──────────────────────────────────────────────────
    {
        Harness h;
        h.reports.tap(Logger::Debug, QStringLiteral("[X] debug"));
        h.reports.tap(Logger::Info, QStringLiteral("[X] info"));
        CHECK_EQ(h.reports.waitingHostLines(), 0);
        h.reports.tap(Logger::Warning, QStringLiteral("[X] warn"));
        h.reports.tap(Logger::Error, QStringLiteral("[X] error"));
        CHECK_EQ(h.reports.waitingHostLines(), 2);
        // A worker's lines arrive as INFO from its stderr relay, their level in
        // the text; its own INFO stays out.
        h.reports.tap(Logger::Info,
                      QStringLiteral("[StreamWorker/child] [2026-11-12 14:35:10.123] [ERROR] "
                                     "[Encoder] boom"));
        h.reports.tap(Logger::Info, QStringLiteral("[StreamWorker/child] [2026-11-12 "
                                                   "14:35:10.124] [WARN] [Encoder] late"));
        h.reports.tap(Logger::Info, QStringLiteral("[StreamWorker/child] [2026-11-12 "
                                                   "14:35:10.125] [INFO] [Encoder] fine"));
        CHECK_EQ(h.reports.waitingHostLines(), 4);
        // The same line again is one line, counted.
        for (int i = 0; i < 5; ++i)
            h.reports.tap(Logger::Error, QStringLiteral("[X] error"));
        CHECK_EQ(h.reports.waitingHostLines(), 4);

        CHECK(h.reports.flushForTests());
        CHECK_EQ(h.sent.size(), 1);
        const QJsonArray host = h.sent.first().value(QStringLiteral("host")).toArray();
        CHECK_EQ(host.size(), 4);
        bool workerError = false, counted = false;
        for (const QJsonValue& v : host) {
            const QJsonObject l = v.toObject();
            if (l.value(QStringLiteral("m")).toString() == QLatin1String("[Encoder] boom"))
                workerError = l.value(QStringLiteral("l")).toString() == QLatin1String("error");
            if (l.value(QStringLiteral("m")).toString() == QLatin1String("[X] error"))
                counted = l.value(QStringLiteral("n")).toInt() == 6;
        }
        CHECK(workerError);
        CHECK(counted);
        CHECK_EQ(h.reports.waitingHostLines(), 0);
    }

    // ── Bounded while it waits ──────────────────────────────────────────────
    {
        Harness h;
        for (int i = 0; i < ErrorReports::kMaxHostLines + 100; ++i)
            h.reports.tap(Logger::Warning, QStringLiteral("[X] warning %1").arg(i));
        CHECK_EQ(h.reports.waitingHostLines(), ErrorReports::kMaxHostLines);
        // Warnings alone are not an error: nothing leaves.
        CHECK(!h.reports.flushForTests());
        CHECK(h.sent.isEmpty());
        // One error: it leaves with only the latest warnings.
        h.reports.tap(Logger::Error, QStringLiteral("[X] the error"));
        CHECK(h.reports.flushForTests());
        const QJsonObject r = h.sent.first();
        CHECK_EQ(r.value(QStringLiteral("host")).toArray().size(), ErrorReports::kSentWarnings + 1);
        CHECK_EQ(r.value(QStringLiteral("dropped")).toInt(),
                 100 + 1 + ErrorReports::kMaxHostLines - ErrorReports::kSentWarnings - 1);
    }

    // ── Scrubbed as the archive is, apps included ───────────────────────────
    {
        Harness h([]() {
            LogScrubber::Names n;
            n.thisMachine.names << QStringLiteral("DUALRTX");
            LogScrubber::Machine kid;
            kid.names << QStringLiteral("Kids-PC");
            kid.addresses << QStringLiteral("192.168.1.50");
            n.hosts << kid;
            n.user = QStringLiteral("bruno");
            n.apps << QStringLiteral("Cyberpunk 2077");
            n.devices << QStringLiteral("iPhone de Bruno");
            n.guests << QStringLiteral("Léo Martin");
            n.cities << QStringLiteral("Clermont-Ferrand");
            return n;
        });
        h.reports.tap(Logger::Error,
                      QStringLiteral("[Launch] Cyberpunk 2077 failed on Kids-PC (192.168.1.50) "
                                     "from DUALRTX, C:\\Users\\bruno\\x, pin=1234"));
        QJsonObject page =
            pageBody(kNow, {{kNow - 2000, QStringLiteral("decoder error on Kids-PC")}});
        h.reports.addPage(QStringLiteral("owner"), QString::fromLatin1(kChromeUa), page);
        // What people are called, in a WARN line: the paired device, the guest.
        h.reports.tap(Logger::Warning,
                      QStringLiteral("[Share] Léo Martin joined from iPhone de Bruno"));
        h.reports.tap(Logger::Warning, QStringLiteral("[Auth] new session near Clermont-Ferrand"));
        CHECK(h.reports.flushForTests());
        const QByteArray all = QJsonDocument(h.sent.first()).toJson();
        const QString text = QString::fromUtf8(all);
        CHECK(!text.contains(QStringLiteral("Léo")));
        CHECK(!text.contains(QStringLiteral("iPhone")));
        CHECK(text.contains(QStringLiteral("[Share] guest joined from device")));
        CHECK(!text.contains(QStringLiteral("Clermont")));
        CHECK(text.contains(QStringLiteral("[Auth] new session near city")));
        CHECK(!all.contains("Cyberpunk"));
        CHECK(!all.contains("Kids-PC"));
        CHECK(!all.contains("192.168.1.50"));
        CHECK(!all.contains("DUALRTX"));
        CHECK(!all.contains("bruno"));
        CHECK(!all.contains("1234"));
        CHECK(all.contains("app failed on {B}"));
        // One scrubber for the whole report: the page names the same machine.
        CHECK(all.contains("decoder error on {B}"));
    }

    // ── The pages' lines ────────────────────────────────────────────────────
    {
        Harness h;
        // The page's clock is an hour off: only the gap to its send counts.
        const qint64 pageNow = kNow + 3600 * 1000;
        h.reports.addPage(QStringLiteral("guest"), QString::fromLatin1(kChromeUa),
                          pageBody(pageNow, {{pageNow - 5000, QStringLiteral("[Stream] lost")}}));
        h.reports.addPage(QStringLiteral("owner"), QString(), pageBody(kNow, {}));
        h.clock = kNow + 10 * 1000;
        CHECK(h.reports.flushForTests());
        const QJsonArray pages = h.sent.first().value(QStringLiteral("pages")).toArray();
        CHECK_EQ(pages.size(), 1); // the empty page is not one
        const QJsonObject p = pages.first().toObject();
        CHECK_EQ(p.value(QStringLiteral("who")).toString(), QStringLiteral("guest"));
        CHECK_EQ(p.value(QStringLiteral("browser")).toString(), QStringLiteral("chrome"));
        CHECK_EQ(p.value(QStringLiteral("major")).toInt(), 131);
        CHECK(!p.contains(QStringLiteral("ua")));
        const QJsonObject l = p.value(QStringLiteral("lines")).toArray().first().toObject();
        CHECK_EQ(l.value(QStringLiteral("ago")).toInt(), 15);

        // At most kMaxPages pages wait, the oldest going first.
        for (int i = 0; i < ErrorReports::kMaxPages + 3; ++i)
            h.reports.addPage(QStringLiteral("owner"), QString(),
                              pageBody(kNow, {{kNow, QStringLiteral("page %1").arg(i)}}));
        CHECK(h.reports.flushForTests());
        const QJsonArray capped = h.sent.last().value(QStringLiteral("pages")).toArray();
        CHECK_EQ(capped.size(), ErrorReports::kMaxPages);
        CHECK_EQ(capped.first()
                     .toObject()
                     .value(QStringLiteral("lines"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("m"))
                     .toString(),
                 QStringLiteral("page 3"));
    }

    // ── A report stays within its size ──────────────────────────────────────
    {
        Harness h;
        const QString longLine(4000, QLatin1Char('e'));
        for (int i = 0; i < 200; ++i)
            h.reports.tap(Logger::Error, QStringLiteral("%1 %2").arg(i).arg(longLine));
        for (int p = 0; p < ErrorReports::kMaxPages; ++p) {
            QList<QPair<qint64, QString>> lines;
            for (int i = 0; i < ErrorReports::kMaxPageLines; ++i)
                lines << qMakePair(kNow, QStringLiteral("p%1 %2 %3").arg(p).arg(i).arg(longLine));
            h.reports.addPage(QStringLiteral("owner"), QString(), pageBody(kNow, lines));
        }
        CHECK(h.reports.flushForTests());
        const QByteArray body = QJsonDocument(h.sent.first()).toJson(QJsonDocument::Compact);
        CHECK(body.size() < ErrorReports::kMaxReportChars + 16 * 1024);
        const QJsonArray host = h.sent.first().value(QStringLiteral("host")).toArray();
        CHECK(!host.isEmpty());
        // Each line cut to its own cap, the newest kept.
        CHECK(host.last().toObject().value(QStringLiteral("m")).toString().size() <=
              ErrorReports::kMaxLineChars + 4);
        CHECK(host.last()
                  .toObject()
                  .value(QStringLiteral("m"))
                  .toString()
                  .startsWith(QStringLiteral("199 ")));
        CHECK(h.sent.first().value(QStringLiteral("dropped")).toInt() > 0);
    }

    // ── When it leaves ──────────────────────────────────────────────────────
    {
        Harness h;
        SessionMetrics::Facts f;
        f.height = 1080;
        f.fps = 60;
        f.codec = QStringLiteral("hevc");
        f.kind = QStringLiteral("owner");
        h.reports.streamFailed(f, 503);
        CHECK_EQ(h.reports.dueForTests(), kNow + ErrorReports::kSettleMs);
        h.reports.tap(Logger::Error, QStringLiteral("[X] refused"));
        CHECK(h.reports.flushForTests());
        const QJsonObject r = h.sent.first();
        CHECK_EQ(r.value(QStringLiteral("event")).toString(), QStringLiteral("failed"));
        CHECK_EQ(r.value(QStringLiteral("code")).toInt(), 503);
        CHECK_EQ(r.value(QStringLiteral("h")).toInt(), 1080);
        CHECK_EQ(r.value(QStringLiteral("codec")).toString(), QStringLiteral("hevc"));
        CHECK_EQ(r.value(QStringLiteral("v")).toString(), QStringLiteral("0.4.4"));
        CHECK(r.contains(QStringLiteral("os")));
        // The next one waits kMinGapMs after the last report.
        h.clock = kNow + 60 * 1000;
        h.reports.streamEnded(f, 120);
        CHECK_EQ(h.reports.dueForTests(), kNow + ErrorReports::kMinGapMs);
        // Switching off disarms it and drops what waits.
        h.reports.tap(Logger::Error, QStringLiteral("[X] again"));
        h.reports.setEnabled(false);
        CHECK_EQ(h.reports.dueForTests(), 0);
        CHECK_EQ(h.reports.waitingHostLines(), 0);
    }

    // ── Through the logger ──────────────────────────────────────────────────
    {
        Harness h;
        h.reports.attach(Logger::instance());
        Logger::error(QStringLiteral("[ErrorReportsTest] through the logger"));
        CHECK_EQ(h.reports.waitingHostLines(), 1);
    } // the destructor takes the tap off: the next line must not crash
    Logger::error(QStringLiteral("[ErrorReportsTest] after the tap is gone"));

    // ── The route ───────────────────────────────────────────────────────────
    {
        Harness h;
        HttpRequest big;
        big.body = QByteArray(60 * 1024, ' ');
        CHECK_EQ(takeErrorLines(big, h.reports, QStringLiteral("owner")).statusCode, 413);
        QJsonArray many;
        for (int i = 0; i < 300; ++i)
            many.append(QJsonObject{{QStringLiteral("m"), QStringLiteral("x")}});
        HttpRequest manyLines;
        manyLines.body = QJsonDocument(QJsonObject{{QStringLiteral("lines"), many}}).toJson();
        CHECK_EQ(takeErrorLines(manyLines, h.reports, QStringLiteral("owner")).statusCode, 413);
        HttpRequest ok;
        ok.headers.insert(QStringLiteral("user-agent"), QString::fromLatin1(kChromeUa));
        ok.body = QJsonDocument(pageBody(kNow, {{kNow, QStringLiteral("[MW] oops")}})).toJson();
        CHECK_EQ(takeErrorLines(ok, h.reports, QStringLiteral("host")).statusCode, 200);
        CHECK(h.reports.flushForTests());
        const QJsonObject p =
            h.sent.first().value(QStringLiteral("pages")).toArray().first().toObject();
        CHECK_EQ(p.value(QStringLiteral("who")).toString(), QStringLiteral("host"));
    }

    // ── Browser families ────────────────────────────────────────────────────
    {
        struct Case
        {
            const char* ua;
            const char* family;
            int major;
        };
        const Case cases[] = {
            {kChromeUa, "chrome", 131},
            {"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
             "Chrome/131.0.0.0 Safari/537.36 Edg/131.0.2903.86",
             "edge", 131},
            {"Mozilla/5.0 (X11; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0", "firefox",
             133},
            {"Mozilla/5.0 (iPhone; CPU iPhone OS 18_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, "
             "like Gecko) Version/18.1 Mobile/15E148 Safari/604.1",
             "safari", 18},
            {"Mozilla/5.0 (iPhone; CPU iPhone OS 18_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, "
             "like Gecko) CriOS/131.0.6778.73 Mobile/15E148 Safari/604.1",
             "chrome", 131},
            {"Mozilla/5.0 (Linux; Android 14; SM-S918B) AppleWebKit/537.36 (KHTML, like Gecko) "
             "SamsungBrowser/26.0 Chrome/122.0.0.0 Mobile Safari/537.36",
             "samsung", 26},
            {"Mozilla/5.0 (Linux; Android 14; Pixel 8; wv) AppleWebKit/537.36 (KHTML, like Gecko) "
             "Version/4.0 Chrome/131.0.6778.39 Mobile Safari/537.36",
             "webview", 131},
            {"curl/8.4.0", "other", 0},
        };
        for (const Case& c : cases) {
            int major = -1;
            CHECK_EQ(ErrorReports::browserFamily(QString::fromLatin1(c.ua), &major),
                     QString::fromLatin1(c.family));
            CHECK_EQ(major, c.major);
        }
    }
}
