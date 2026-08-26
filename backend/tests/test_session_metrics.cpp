/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The census must be silent by default and honest about the device it saw.
 * Both are privacy properties rather than features, so they are pinned here:
 * a regression on either is invisible at runtime — the stream still works, the
 * dashboard just quietly starts saying something untrue.
 */
#include "test_framework.h"
#include "network/SessionMetrics.h"

void run_session_metrics_tests()
{
    SECTION("SessionMetrics");

    // ── Reporting is off unless everything lines up ─────────────────────────
    // The test environment has no MW_DOMAIN / MW_PDNS_TOKEN, which is exactly
    // the situation of a self-built binary: nothing may be sent, whatever the
    // instance's own setting says.
    {
        SessionMetrics off(QStringLiteral("0.3.0"), false);
        CHECK(!off.active());
        SessionMetrics onButUnbuilt(QStringLiteral("0.3.0"), true);
        CHECK(!onButUnbuilt.active());
        // A no-op report must not crash or block, and there is nothing to
        // observe: the point is that it does nothing at all.
        SessionMetrics::Facts f;
        f.height = 1080;
        f.fps = 60;
        onButUnbuilt.reportStart(f);
        onButUnbuilt.reportEnd(f, 42);
        onButUnbuilt.reportFailure(f, 503);
    }

    // The opt-out must win even on a build that carries the credentials.
    {
        qputenv("MW_DOMAIN", "example.invalid");
        qputenv("MW_PDNS_TOKEN", "test-token");
        SessionMetrics allowed(QStringLiteral("0.3.0"), true);
        CHECK(allowed.active());

        SessionMetrics refused(QStringLiteral("0.3.0"), false);
        CHECK(!refused.active());

        // MW_NO_TELEMETRY overrides the instance setting, not the other way
        // round: an environment that says no is final.
        qputenv("MW_NO_TELEMETRY", "1");
        SessionMetrics vetoed(QStringLiteral("0.3.0"), true);
        CHECK(!vetoed.active());
        qunsetenv("MW_NO_TELEMETRY");

        qunsetenv("MW_DOMAIN");
        qunsetenv("MW_PDNS_TOKEN");
    }

    // ── Device class ───────────────────────────────────────────────────────
    // A closed vocabulary: whatever the browser claims, exactly one of four
    // words leaves the machine.
    CHECK_EQ(SessionMetrics::clientClass(QString()), QString("other"));
    CHECK_EQ(SessionMetrics::clientClass(
                 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/140.0 Safari/537.36"),
             QString("desktop"));
    CHECK_EQ(SessionMetrics::clientClass(
                 "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Safari/605.1.15"),
             QString("desktop"));
    CHECK_EQ(SessionMetrics::clientClass(
                 "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) Mobile/15E148"),
             QString("mobile"));
    // Android WITH "Mobile" is a phone; the same string without it is a tablet —
    // the one distinction the UA actually encodes.
    CHECK_EQ(SessionMetrics::clientClass("Mozilla/5.0 (Linux; Android 14; Pixel 8) Mobile Safari"),
             QString("mobile"));
    CHECK_EQ(SessionMetrics::clientClass("Mozilla/5.0 (Linux; Android 14; SM-X200) Safari"),
             QString("tablet"));
    CHECK_EQ(SessionMetrics::clientClass(
                 "Mozilla/5.0 (iPad; CPU OS 18_0 like Mac OS X) Version/18.0 Safari"),
             QString("tablet"));
    // A TV stick also says Android, and is tested before it.
    CHECK_EQ(SessionMetrics::clientClass("Mozilla/5.0 (Linux; Android 12; BRAVIA 4K) Safari"),
             QString("tv"));
    CHECK_EQ(SessionMetrics::clientClass("Mozilla/5.0 (SMART-TV; Linux; Tizen 7.0) Safari"),
             QString("tv"));
    CHECK_EQ(SessionMetrics::clientClass("Mozilla/5.0 (Web0S; Linux/SmartTV) Safari"),
             QString("tv"));
}
