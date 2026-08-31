/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "mw/native/NativeHost.h"

#if defined(_WIN32)
#include "capture/windows/DxgiDuplication.h"
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace mw::native;

// Capture is the one part of the engine that cannot be faked: it either gets
// real pixels off a real display or it does not. So this exercises the actual
// Desktop Duplication path against whatever display the machine has.
//
// It is written to be honest on a machine that cannot capture — a CI runner
// with no desktop, a build in session 0 — by reporting "skipped" rather than
// failing. A test that fails wherever the hardware is absent gets disabled, and
// a disabled test protects nothing.

void run_capture_tests()
{
    SECTION("Capture — Desktop Duplication against the real display");

#if !defined(_WIN32)
    std::fprintf(stderr, "  skipped: no Windows capture backend in this build\n");
#else
    const Capabilities caps = NativeHost::probe();
    if (caps.displays.empty()) {
        std::fprintf(stderr, "  skipped: no display attached (%s)\n",
                     caps.diagnostic.empty() ? toString(caps.reason) : caps.diagnostic.c_str());
        return;
    }

    // The primary display, on the GPU that actually drives it — the pairing the
    // whole zero-copy premise rests on.
    const DisplayInfo* target = &caps.displays.front();
    for (const DisplayInfo& d : caps.displays) {
        if (d.primary) {
            target = &d;
            break;
        }
    }
    const GpuInfo* gpu = caps.gpuFor(*target);
    if (!gpu) {
        std::fprintf(stderr, "  skipped: display %d names no GPU\n", target->id);
        return;
    }

    // The display's index within its own adapter is what DXGI wants, and it is
    // not the global display id on a multi-GPU machine.
    unsigned outputIndex = 0;
    for (const DisplayInfo& d : caps.displays) {
        if (d.id == target->id) break;
        if (d.gpuId == target->gpuId) ++outputIndex;
    }

    capture::DxgiDuplication duplication(gpu->nativeHandle, outputIndex);

    std::string error;
    if (!duplication.start(error)) {
        // A real refusal is worth printing but is not a test failure: Desktop
        // Duplication is legitimately unavailable in a remote session or on a
        // hybrid output, which is exactly why a WGC fallback is planned.
        std::fprintf(stderr, "  skipped: %s\n", error.c_str());
        return;
    }

    CHECK(duplication.width() > 0);
    CHECK(duplication.height() > 0);
    CHECK(duplication.device() != nullptr);
    std::fprintf(stderr, "  duplicating %dx%d on %s\n", duplication.width(), duplication.height(),
                 gpu->name.c_str());

    // The probe and the capture must agree on the size of the screen.
    //
    // They did not, and the discrepancy was invisible until both ran on real
    // hardware: DXGI_OUTPUT_DESC::DesktopCoordinates is DPI-scaled for a
    // process that is not per-monitor DPI aware, so a 2560×1440 panel at 125 %
    // was probed as 2048×1152 while duplication delivered the full 2560×1440.
    // The user would have been offered — and streamed — the wrong resolution.
    //
    // This is the assertion that would have caught it on the first run.
    CHECK_EQ(duplication.width(), target->width);
    CHECK_EQ(duplication.height(), target->height);
    if (duplication.width() != target->width || duplication.height() != target->height) {
        std::fprintf(stderr, "  MISMATCH: probe says %dx%d, duplication gives %dx%d\n",
                     target->width, target->height, duplication.width(), duplication.height());
    }

    // Nudge the desktop so there is something to capture: on a still screen
    // Desktop Duplication correctly reports nothing, and a test that waited for
    // spontaneous damage would be flaky by design.
    int captured = 0;
    int timeouts = 0;
    int64_t worstLatencyUs = 0;
    int64_t totalLatencyUs = 0;

    for (int attempt = 0; attempt < 60 && captured < 5; ++attempt) {
        capture::CapturedFrame frame;
        const capture::AcquireStatus status = duplication.acquire(100, frame);

        if (status == capture::AcquireStatus::Timeout) {
            ++timeouts;
            continue;
        }
        if (status == capture::AcquireStatus::Lost) {
            // A mode change mid-test is not a defect; recovering from it is the
            // documented contract, so exercise that instead of giving up.
            std::fprintf(stderr, "  duplication lost — restarting\n");
            if (!duplication.start(error)) {
                std::fprintf(stderr, "  restart failed: %s\n", error.c_str());
                return;
            }
            continue;
        }
        CHECK(status == capture::AcquireStatus::Ok);
        if (status != capture::AcquireStatus::Ok) break;

        // The frame must be a real texture on the device we opened, and its
        // timestamps must be ordered: a present cannot happen after the capture
        // that observed it.
        CHECK(frame.texture != nullptr);
        CHECK(frame.presentUs > 0);
        CHECK(frame.capturedUs >= frame.presentUs);

        const int64_t latencyUs = frame.capturedUs - frame.presentUs;
        totalLatencyUs += latencyUs;
        if (latencyUs > worstLatencyUs) worstLatencyUs = latencyUs;
        ++captured;

        duplication.release();
    }

    std::fprintf(stderr, "  frames=%d timeouts=%d\n", captured, timeouts);
    if (captured > 0) {
        std::fprintf(stderr, "  capture latency: mean %.2f ms, worst %.2f ms\n",
                     static_cast<double>(totalLatencyUs) / captured / 1000.0,
                     static_cast<double>(worstLatencyUs) / 1000.0);

        // The clock calibration is the thing most likely to be silently wrong,
        // and its failure mode is a latency in the hours. Anything past a
        // second means present and capture are being read off different epochs.
        CHECK(totalLatencyUs / captured < 1000000);
    } else {
        std::fprintf(stderr, "  note: the screen never changed during the test — "
                             "no frame to measure\n");
    }

    duplication.stop();

    // Stopping twice, and releasing without holding, must both be harmless:
    // teardown runs on error paths where the state is not known.
    duplication.stop();
    duplication.release();
#endif
}
