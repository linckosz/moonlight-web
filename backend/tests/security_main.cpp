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
#include <QStandardPaths>
#include <cstdlib>

TestStats g_stats;

void run_connection_guard_tests();
void run_request_guard_tests();
void run_api_csrf_tests();
void run_share_manager_tests();
void run_pairing_binding_tests();
void run_gamepad_driver_tests();

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // ShareManager persists its state under AppDataLocation. Test mode moves
    // that to a throwaway directory so a test run never touches (or wipes) the
    // share state of a real install on the developer's machine.
    QStandardPaths::setTestModeEnabled(true);

    run_connection_guard_tests();
    run_request_guard_tests();
    run_api_csrf_tests();
    run_share_manager_tests();
    run_pairing_binding_tests();
    run_gamepad_driver_tests();

    const int total = g_stats.passed + g_stats.failed;
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "Security TNR: %d/%d checks passed, %d failed\n", g_stats.passed, total,
                 g_stats.failed);
    std::fprintf(stderr, "========================================\n");

    const int rc = g_stats.failed == 0 ? 0 : 1;
    std::fflush(stderr);
    std::_Exit(rc);
}
