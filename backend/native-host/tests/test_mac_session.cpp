/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "mw/native/NativeHost.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using namespace mw::native;

// A whole macOS session, the way the backend would run one: probe, select,
// create, stream for a moment, stop. Once per codec the Mac encodes, with the
// frames written to a file so a decoder can look at them afterwards — the only
// proof of orientation and colour this side of a browser.
//
// Needs the console session (a binary started over SSH is refused by the probe
// with NoInteractiveSession — run it through launchd's gui domain) and the
// Screen Recording permission, which the first run asks the OS to prompt for.

namespace {

#if defined(MW_NATIVE_MACOS)
void runOne(const Capabilities& caps, const DisplayInfo& display, Codec codec, const char* path)
{
    SessionConfig config;
    config.displayId = display.id;
    config.fps = 60;
    config.bitrateKbps = 20000;
    config.clientCodecs = {codec};
    config.intraRefresh = true; // asked; the encoder has none, and SessionInfo says

    std::atomic<int> frames{0};
    std::atomic<int> keyframes{0};
    std::atomic<bool> firstWasKeyframe{false};
    std::atomic<bool> orderOk{true};
    std::atomic<uint32_t> lastNumber{0};
    std::atomic<int64_t> worstProcessingUs{0};
    std::atomic<size_t> bytes{0};
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::string ended;

    std::string error;
    std::unique_ptr<Session> session = NativeHost::createSession(
        config,
        [&](const EncodedFrame& f) {
            const int n = frames.fetch_add(1);
            if (f.keyframe) keyframes.fetch_add(1);
            if (n == 0) firstWasKeyframe.store(f.keyframe);
            if (n > 0 && f.frameNumber != lastNumber.load() + 1) orderOk.store(false);
            lastNumber.store(f.frameNumber);
            if (f.processingUs() > worstProcessingUs.load())
                worstProcessingUs.store(f.processingUs());
            bytes.fetch_add(f.size);
            out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size));
        },
        nullptr, nullptr, nullptr, [&](const std::string& reason) { ended = reason; }, error);
    if (!session) {
        std::fprintf(stderr, "  createSession failed: %s\n", error.c_str());
        CHECK(false);
        return;
    }
    CHECK(session->start(error));
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());

    const SessionInfo& info = session->info();
    std::fprintf(stderr, "  session: %dx%d %s via %s on %s, intra-refresh %s, capture %s\n",
                 info.width, info.height, toString(info.codec), toString(info.encoder),
                 info.gpuName.c_str(), info.intraRefresh ? "on" : "off", toString(info.capture));
    CHECK_EQ(info.width, display.width);
    CHECK_EQ(info.height, display.height);
    CHECK_EQ(static_cast<int>(info.codec), static_cast<int>(codec));
    CHECK_EQ(static_cast<int>(info.encoder), static_cast<int>(EncoderApi::VideoToolbox));
    CHECK_EQ(static_cast<int>(info.capture), static_cast<int>(CaptureApi::ScreenCaptureKit));
    CHECK(!info.intraRefresh);
    CHECK(!info.referenceInvalidation);
    (void)caps;

    // Two seconds. A still desktop yields the first frame plus the floor at
    // 2 fps, plus whatever the refinement burst adds; anything moving yields
    // more. Either way there are frames, and the first is a keyframe.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    session->requestKeyframe();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    session->stop();
    out.close();

    std::fprintf(stderr, "  %d frame(s), %d keyframe(s), %zu bytes, worst host latency %.2f ms%s\n",
                 frames.load(), keyframes.load(), bytes.load(), worstProcessingUs.load() / 1000.0,
                 ended.empty() ? "" : (", ended: " + ended).c_str());
    CHECK(ended.empty());
    CHECK(frames.load() >= 3);
    CHECK(keyframes.load() >= 2); // the first, and the one asked for
    CHECK(firstWasKeyframe.load());
    CHECK(orderOk.load());
    std::fprintf(stderr, "  wrote %s — decode it to look at the picture\n", path);
}
#endif

} // namespace

void run_mac_session_tests()
{
    SECTION("macOS — a whole session through NativeHost, frames to a file");

#if !defined(MW_NATIVE_MACOS)
    std::fprintf(stderr, "  skipped: macOS backend not built\n");
#else
    const Capabilities caps = NativeHost::probe();
    if (!caps.available) {
        std::fprintf(stderr, "  skipped: %s (%s)\n", toString(caps.reason),
                     caps.diagnostic.c_str());
        return;
    }
    CHECK_EQ(static_cast<int>(caps.capture), static_cast<int>(CaptureApi::ScreenCaptureKit));
    CHECK(!caps.displays.empty());
    CHECK(!caps.gpus.empty());

    const DisplayInfo* display = nullptr;
    for (const DisplayInfo& d : caps.displays)
        if (d.primary) display = &d;
    if (!display) display = &caps.displays.front();
    const GpuInfo* gpu = caps.gpuFor(*display);
    CHECK(gpu != nullptr);
    if (!gpu || gpu->encoders.empty()) {
        std::fprintf(stderr, "  skipped: the display's GPU has no encoder\n");
        return;
    }
    std::fprintf(stderr, "  %s — %s on %s\n", display->label.c_str(), display->detail.c_str(),
                 gpu->name.c_str());

    for (Codec codec : gpu->codecs) {
        if (codec == Codec::H264) runOne(caps, *display, codec, "/tmp/bench-mac-session.h264");
        if (codec == Codec::Hevc) runOne(caps, *display, codec, "/tmp/bench-mac-session.h265");
    }
#endif
}
