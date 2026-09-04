/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 *
 * Runs the engine's pure-logic units. No Qt, no GPU, no display: everything
 * here is verifiable on a headless CI runner, which is the point — the policy
 * that spares the user fifty settings (which GPU, which encoder, which codec,
 * what resolution) is exactly the part that must never silently drift.
 */
#include "native_test_framework.h"

#include "mw/native/NativeHost.h"

#include <cstdlib>
#include <string>

NativeTestStats g_nativeStats;

void run_selector_tests();
void run_capabilities_tests();
void run_capture_tests();
void run_vpl_params_tests();
void run_ds4_mapping_tests();
void run_stage_stats_tests();
void run_frame_cadence_tests();
void run_cadence_align_tests();
void run_restart_backoff_tests();
void run_rate_control_tests();
void run_audio_pacer_tests();

int main()
{
    // Route the engine's own logging to stderr for the whole run. The engine
    // explains itself in the log — which adapter refused a session, why a
    // driver was rejected — and a suite that hides that leaves a failure with
    // nothing to go on but a count.
    mw::native::NativeHost::setLogSink([](int level, const std::string& message) {
        static const char* kNames[] = {"debug", "info", "warn", "error"};
        const char* name = (level >= 0 && level <= 3) ? kNames[level] : "?";
        std::fprintf(stderr, "  [%s] %s\n", name, message.c_str());
    });

    run_capabilities_tests();
    run_selector_tests();
    run_vpl_params_tests();
    run_ds4_mapping_tests();
    run_stage_stats_tests();
    run_frame_cadence_tests();
    run_cadence_align_tests();
    run_restart_backoff_tests();
    run_rate_control_tests();
    run_audio_pacer_tests();
    run_capture_tests();

    const int total = g_nativeStats.passed + g_nativeStats.failed;
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "native-host: %d/%d checks passed, %d failed\n", g_nativeStats.passed,
                 total, g_nativeStats.failed);
    std::fprintf(stderr, "========================================\n");
    std::fflush(stderr);

    return g_nativeStats.failed == 0 ? 0 : 1;
}
