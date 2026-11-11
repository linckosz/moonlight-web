/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * A browser's report of a tunnel that could not be opened (TunnelDiag.h,
 * issue #48). What matters: the log line is made of this file's words and
 * clamped numbers only, nothing the browser wrote reaches it as written, and
 * the lines an hour are bounded across all sessions, the rest counted and said
 * once.
 */

#include "../src/server/TunnelDiag.h"

#include "test_framework.h"

#include <QJsonArray>
#include <QJsonObject>

void run_tunnel_diag_tests()
{
    using namespace TunnelDiag;

    SECTION("TunnelDiag — a car's report, as issue #48 would have had it");
    {
        const QJsonObject msg{
            {QStringLiteral("type"), QStringLiteral("diag")},
            {QStringLiteral("why"), QStringLiteral("failed")},
            {QStringLiteral("ice"), QStringLiteral("failed")},
            {QStringLiteral("connection"), QStringLiteral("failed")},
            {QStringLiteral("gathering"), QStringLiteral("complete")},
            {QStringLiteral("ms"), 9812.6},
            {QStringLiteral("cand"), QJsonObject{{QStringLiteral("host/udp/v4"), 1}}},
            {QStringLiteral("recv"), 6},
            {QStringLiteral("checks"), QJsonObject{{QStringLiteral("sent"), 14},
                                                   {QStringLiteral("answered"), 0},
                                                   {QStringLiteral("received"), 0}}},
        };
        const QString line = describe(msg);
        CHECK(line ==
              QStringLiteral("the browser found no route (failed) — ice=failed, connection=failed, "
                             "gathering=complete, 9812 ms after its answer; sent host/udp/v4 x1; "
                             "had 6 from this host; checks sent 14, answered 0, received 0"));
    }

    SECTION("TunnelDiag — a state outside the list is written as other, never as sent");
    {
        const QJsonObject msg{
            {QStringLiteral("why"), QStringLiteral("evil\n[Auth] admin signed in")},
            {QStringLiteral("ice"), QStringLiteral("failed; rm -rf")},
            {QStringLiteral("connection"), 3},
            {QStringLiteral("gathering"), QJsonArray{QStringLiteral("complete")}},
        };
        const QString line = describe(msg);
        CHECK(line.startsWith(QStringLiteral("the browser found no route (other) — ice=other, "
                                             "connection=other, gathering=other, 0 ms")));
        CHECK(!line.contains(QStringLiteral("evil")));
        CHECK(!line.contains(QStringLiteral("rm -rf")));
        CHECK(!line.contains(QLatin1Char('\n')));
    }

    SECTION("TunnelDiag — only the known candidate kinds are read, in this file's order");
    {
        const QJsonObject msg{
            {QStringLiteral("cand"),
             QJsonObject{
                 {QStringLiteral("srflx/udp/v4"), 2},
                 {QStringLiteral("host/udp/mdns"), 1},
                 {QStringLiteral("203.0.113.9:4000"), 5},
                 {QStringLiteral("host/sctp/v4"), 7},
                 {QStringLiteral("relay/tcp/v6"), 0},
             }},
        };
        const QString line = describe(msg);
        CHECK(line.contains(QStringLiteral("; sent host/udp/mdns x1, srflx/udp/v4 x2;")));
        CHECK(!line.contains(QStringLiteral("203.0.113.9")));
        CHECK(!line.contains(QStringLiteral("sctp")));
        CHECK(!line.contains(QStringLiteral("relay")));
    }

    SECTION("TunnelDiag — numbers are clamped, and what is not a number counts as none");
    {
        const QJsonObject msg{
            {QStringLiteral("ms"), 1e12},
            {QStringLiteral("recv"), -4},
            {QStringLiteral("cand"),
             QJsonObject{{QStringLiteral("host/tcp/v6"), 1e9},
                         {QStringLiteral("host/udp/v4"), QStringLiteral("12")}}},
            {QStringLiteral("checks"),
             QJsonObject{{QStringLiteral("sent"), 123456},
                         {QStringLiteral("answered"), QStringLiteral("7")}}},
        };
        const QString line = describe(msg);
        CHECK(line.contains(QStringLiteral("600000 ms after its answer")));
        CHECK(line.contains(QStringLiteral("; sent host/tcp/v6 x999; had 0 from this host")));
        CHECK(line.contains(QStringLiteral("checks sent 999, answered 0, received 0")));
    }

    SECTION("TunnelDiag — an empty report still makes a line that says so");
    {
        const QString line = describe(QJsonObject{});
        CHECK(line.contains(QStringLiteral("sent no candidate")));
        CHECK(line.endsWith(QStringLiteral("; no check counters")));
    }

    SECTION("TunnelDiag — the hour's lines are bounded, the rest counted and said once");
    {
        Limiter lim;
        const qint64 t0 = 1000;
        for (int i = 0; i < kMaxLinesPerWindow; ++i)
            CHECK(lim.admit(t0 + i));
        CHECK(!lim.admit(t0 + 100));
        CHECK(!lim.admit(t0 + 200));
        CHECK_EQ(lim.ignored(), 2);
        // Still within the hour: nothing to say yet.
        CHECK_EQ(lim.closeWindow(t0 + kWindowMs - 1), 0);
        CHECK(lim.msLeft(t0 + 1000) == kWindowMs - 1000);
        // The hour is over: said once, then never again.
        CHECK_EQ(lim.closeWindow(t0 + kWindowMs), 2);
        CHECK_EQ(lim.closeWindow(t0 + kWindowMs + 5), 0);
        CHECK(lim.msLeft(t0 + kWindowMs + 5) == 0);
        // A new hour starts with the next report.
        CHECK(lim.admit(t0 + kWindowMs + 10));
        CHECK_EQ(lim.ignored(), 0);
    }

    SECTION("TunnelDiag — an hour with nothing ignored closes without a word");
    {
        Limiter lim;
        CHECK(lim.admit(0));
        CHECK_EQ(lim.closeWindow(kWindowMs * 2), 0);
    }
}
