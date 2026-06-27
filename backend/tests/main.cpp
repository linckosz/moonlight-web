/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Backend unit-test runner. Aggregates the per-unit suites and exits non-zero
 * if any check fails, so it can gate CI / PR validation. Run under coverage via
 * tests/run_coverage.bat (OpenCppCoverage).
 */
#include "test_framework.h"

#include <QCoreApplication>
#include <cstdlib>

TestStats g_stats;

void run_input_encoder_tests();
void run_stream_config_tests();
void run_input_crypto_tests();
void run_rest_router_tests();
void run_app_settings_tests();
void run_auth_manager_tests();
void run_connection_guard_tests();

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Keep app data hermetic-ish for any path the units resolve internally.
    QCoreApplication::setApplicationName("mw-server-tests");
    QCoreApplication::setOrganizationName("moonlight-web");

    run_input_encoder_tests();
    run_stream_config_tests();
    run_input_crypto_tests();
    run_rest_router_tests();
    run_app_settings_tests();
    run_auth_manager_tests();
    run_connection_guard_tests();

    const int total = g_stats.passed + g_stats.failed;
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "Backend TNR: %d/%d checks passed, %d failed\n", g_stats.passed, total,
                 g_stats.failed);
    std::fprintf(stderr, "========================================\n");

    // Exit via _Exit so the process return code is the test result and is not
    // clobbered by a crash in global/Qt/OpenSSL teardown after main() returns.
    const int rc = g_stats.failed == 0 ? 0 : 1;
    std::fflush(stderr);
    std::_Exit(rc);
}
