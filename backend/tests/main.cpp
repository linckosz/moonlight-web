/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
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
void run_static_files_tests();
void run_http_parser_tests();
void run_request_guard_tests();
void run_api_csrf_tests();
void run_session_pool_tests();
void run_port_fallback_tests();
void run_host_persistence_tests();
void run_rendezvous_id_tests();
void run_backend_probe_tests();
void run_host_os_probe_tests();
void run_tunnel_frame_tests();
void run_app_manifest_tests();
void run_wolf_coop_tests();
void run_multiseat_tests();
void run_session_metrics_tests();
void run_us_scancode_tests();
void run_input_watchdog_tests();

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Keep app data hermetic-ish for any path the units resolve internally.
    QCoreApplication::setApplicationName("mw-server-tests");
    QCoreApplication::setOrganizationName("moonlightweb");

    run_input_encoder_tests();
    run_stream_config_tests();
    run_input_crypto_tests();
    run_rest_router_tests();
    run_app_settings_tests();
    run_auth_manager_tests();
    run_connection_guard_tests();
    run_static_files_tests();
    run_http_parser_tests();
    run_request_guard_tests();
    run_api_csrf_tests();
    run_session_pool_tests();
    run_port_fallback_tests();
    run_host_persistence_tests();
    run_rendezvous_id_tests();
    run_backend_probe_tests();
    run_host_os_probe_tests();
    run_tunnel_frame_tests();
    run_app_manifest_tests();
    run_wolf_coop_tests();
    run_multiseat_tests();
    run_session_metrics_tests();
    run_us_scancode_tests();
    run_input_watchdog_tests();

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
