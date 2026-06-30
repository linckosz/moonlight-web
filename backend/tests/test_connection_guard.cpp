/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/ConnectionGuard.h"

void run_connection_guard_tests()
{
    SECTION("ConnectionGuard");

    using CG = ConnectionGuard;
    const qint64 t0 = 1'000'000; // arbitrary fixed epoch (ms)

    // ── Exemptions: loopback / RFC1918 / IPv6 ULA / link-local never limited ──
    CHECK(CG::isExempt("127.0.0.1"));
    CHECK(CG::isExempt("::1"));
    CHECK(CG::isExempt("::ffff:127.0.0.1"));
    CHECK(CG::isExempt("10.0.0.5"));
    CHECK(CG::isExempt("172.16.4.1"));
    CHECK(CG::isExempt("192.168.1.50"));
    CHECK(CG::isExempt("fe80::1%eth0"));
    CHECK(CG::isExempt("fc00::1234"));
    CHECK(!CG::isExempt("8.8.8.8"));
    CHECK(!CG::isExempt("2001:db8::1"));
    CHECK(!CG::isExempt(""));

    // ── A normal public client well under the threshold is always allowed ────
    {
        CG g;
        const QString ip = "203.0.113.10";
        bool allAllowed = true;
        for (int i = 0; i < CG::CONN_MAX_PER_WINDOW; ++i)
            if (!g.allowConnection(ip, t0 + i)) allAllowed = false;
        CHECK(allAllowed);
        CHECK(!g.isBanned(ip, t0 + CG::CONN_MAX_PER_WINDOW));
    }

    // ── Connection flood: crossing the cap within the window bans the IP ─────
    {
        CG g;
        const QString ip = "203.0.113.20";
        bool banned = false;
        // All timestamps inside one window so they accumulate.
        for (int i = 0; i <= CG::CONN_MAX_PER_WINDOW; ++i)
            if (!g.allowConnection(ip, t0 + i)) banned = true;
        CHECK(banned);
        CHECK(g.isBanned(ip, t0 + CG::CONN_MAX_PER_WINDOW + 1));
        CHECK(g.banSecondsRemaining(ip, t0 + CG::CONN_MAX_PER_WINDOW + 1) > 0);
        // Still banned just before expiry, free once the ban elapses.
        CHECK(g.isBanned(ip, t0 + CG::BAN_MS - 1));
        CHECK(!g.isBanned(ip, t0 + CG::BAN_MS + CG::CONN_MAX_PER_WINDOW + 10));
    }

    // ── Sliding window: connections spread beyond the window never ban ───────
    {
        CG g;
        const QString ip = "203.0.113.30";
        bool everBanned = false;
        // One connection every (window/2) ms over many iterations: at most ~2
        // ever coexist in a window, far below the cap.
        for (int i = 0; i < CG::CONN_MAX_PER_WINDOW * 4; ++i) {
            qint64 now = t0 + static_cast<qint64>(i) * (CG::CONN_WINDOW_MS / 2 + 1);
            if (!g.allowConnection(ip, now)) everBanned = true;
        }
        CHECK(!everBanned);
    }

    // ── Auth-failure flood (brute force) bans the IP ─────────────────────────
    {
        CG g;
        const QString ip = "203.0.113.40";
        for (int i = 0; i < CG::AUTHFAIL_MAX; ++i)
            g.reportAuthFailure(ip, t0 + i);
        CHECK(g.isBanned(ip, t0 + CG::AUTHFAIL_MAX));
        // A banned IP is refused at connection time.
        CHECK(!g.allowConnection(ip, t0 + CG::AUTHFAIL_MAX + 1));
    }

    // ── Exempt IPs never get banned, even when flooded ───────────────────────
    {
        CG g;
        const QString lan = "192.168.0.42";
        bool everRefused = false;
        for (int i = 0; i <= CG::CONN_MAX_PER_WINDOW * 2; ++i)
            if (!g.allowConnection(lan, t0 + i)) everRefused = true;
        for (int i = 0; i < CG::AUTHFAIL_MAX * 2; ++i)
            g.reportAuthFailure(lan, t0 + i);
        CHECK(!everRefused);
        CHECK(!g.isBanned(lan, t0));
        CHECK_EQ(g.trackedIpCount(), 0); // exempt IPs are never tracked
    }

    // ── Purge drops idle, unbanned entries but keeps active bans ─────────────
    {
        CG g;
        g.allowConnection("203.0.113.50", t0);                 // idle, not banned
        for (int i = 0; i <= CG::CONN_MAX_PER_WINDOW; ++i)     // banned
            g.allowConnection("203.0.113.51", t0 + i);
        CHECK_EQ(g.trackedIpCount(), 2);
        g.purge(t0 + CG::IDLE_PURGE_MS + 1);
        CHECK_EQ(g.trackedIpCount(), 1); // idle one dropped, banned one kept
        CHECK(g.isBanned("203.0.113.51", t0 + CG::IDLE_PURGE_MS + 1));
    }
}
