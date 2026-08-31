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

#include <cstdlib>

NativeTestStats g_nativeStats;

void run_selector_tests();
void run_capabilities_tests();
void run_capture_tests();

int main()
{
    run_capabilities_tests();
    run_selector_tests();
    run_capture_tests();

    const int total = g_nativeStats.passed + g_nativeStats.failed;
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "native-host: %d/%d checks passed, %d failed\n", g_nativeStats.passed,
                 total, g_nativeStats.failed);
    std::fprintf(stderr, "========================================\n");
    std::fflush(stderr);

    return g_nativeStats.failed == 0 ? 0 : 1;
}
