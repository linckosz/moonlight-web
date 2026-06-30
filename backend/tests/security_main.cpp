/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Standalone entry for the cross-platform security TNR (ctest). Runs only the
 * pure-logic security units that depend on Qt Core/Network alone (no OpenSSL,
 * no Windows APIs), so the same anti-abuse logic is verified on Linux, macOS
 * and Windows — guaranteeing security parity across platforms. The full
 * Windows-only suite (with coverage) is driven separately by tests/main.cpp.
 */
#include "test_framework.h"

#include <QCoreApplication>
#include <cstdlib>

TestStats g_stats;

void run_connection_guard_tests();

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    run_connection_guard_tests();

    const int total = g_stats.passed + g_stats.failed;
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "Security TNR: %d/%d checks passed, %d failed\n", g_stats.passed, total,
                 g_stats.failed);
    std::fprintf(stderr, "========================================\n");

    const int rc = g_stats.failed == 0 ? 0 : 1;
    std::fflush(stderr);
    std::_Exit(rc);
}
