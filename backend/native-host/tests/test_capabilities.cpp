/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "mw/native/NativeHost.h"

#include <string>
#include <vector>

using namespace mw::native;

void run_capabilities_tests()
{
    SECTION("Capabilities — GPU lookup");

    {
        Capabilities caps;
        GpuInfo a;
        a.id = 0;
        a.name = "GPU A";
        GpuInfo b;
        b.id = 1;
        b.name = "GPU B";
        caps.gpus = {a, b};

        DisplayInfo onB;
        onB.id = 0;
        onB.gpuId = 1;
        CHECK(caps.gpuFor(onB) != nullptr);
        CHECK_EQ(caps.gpuFor(onB)->name, std::string("GPU B"));

        // A display whose GPU could not be identified must report "unknown"
        // rather than quietly resolving to the first adapter — the caller then
        // makes the cross-GPU decision deliberately.
        DisplayInfo orphan;
        orphan.id = 1;
        orphan.gpuId = -1;
        CHECK(caps.gpuFor(orphan) == nullptr);
    }

    SECTION("Capabilities — probe never throws and never lies");

    {
        // On a build with no platform backend this reports unavailable; on a
        // real one it may report either. What is asserted here is the
        // invariant that holds in both cases: an unavailable probe always
        // carries a reason, and an available one never does.
        const Capabilities caps = NativeHost::probe();
        if (caps.available) {
            CHECK_EQ(caps.reason, Unavailability::None);
            CHECK(!caps.displays.empty());
        } else {
            CHECK(caps.reason != Unavailability::None);
        }
    }

    SECTION("Capabilities — a session is refused with a message, never a crash");

    {
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::H264};

        std::string error;
        auto session = NativeHost::createSession(
            cfg, [](const EncodedFrame&) {}, nullptr, nullptr, nullptr, nullptr, error);

        // Either it built (a machine with a real backend) or it refused with an
        // explanation. A null session with an empty error would leave the
        // caller nothing to log or show.
        if (!session) CHECK(!error.empty());
    }

    {
        // A session with no video callback would encode into nothing. Caught up
        // front rather than after a pipeline has been built.
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::H264};

        std::string error;
        auto session =
            NativeHost::createSession(cfg, nullptr, nullptr, nullptr, nullptr, nullptr, error);
        CHECK(session == nullptr);
        CHECK(!error.empty());
    }

    SECTION("Capabilities — enum names are all populated");

    {
        // Every enumerator has a name: these strings reach the logs and the
        // stats overlay, where "unknown" would be a dead end for diagnosis.
        const std::vector<Codec> codecs = {Codec::Av1, Codec::Hevc, Codec::H264};
        for (Codec c : codecs)
            CHECK(std::string(toString(c)) != "unknown");

        const std::vector<EncoderApi> encoders = {EncoderApi::None,
                                                  EncoderApi::Nvenc,
                                                  EncoderApi::Amf,
                                                  EncoderApi::Vpl,
                                                  EncoderApi::VaApi,
                                                  EncoderApi::VideoToolbox,
                                                  EncoderApi::MediaFoundation,
                                                  EncoderApi::Software};
        for (EncoderApi e : encoders)
            CHECK(std::string(toString(e)) != "unknown");

        const std::vector<CaptureApi> captures = {
            CaptureApi::None, CaptureApi::DxgiDuplication, CaptureApi::WindowsGraphicsCapture,
            CaptureApi::PipeWire, CaptureApi::ScreenCaptureKit};
        for (CaptureApi c : captures)
            CHECK(std::string(toString(c)) != "unknown");

        const std::vector<Unavailability> reasons = {
            Unavailability::None,         Unavailability::NoDisplay,
            Unavailability::NoCaptureApi, Unavailability::CapturePermission,
            Unavailability::NoEncoder,    Unavailability::NoInteractiveSession,
            Unavailability::OsTooOld,     Unavailability::ArchNotSupported,
            Unavailability::ProbeFailed};
        for (Unavailability u : reasons)
            CHECK(std::string(toString(u)) != "unknown");
    }

    SECTION("Capabilities — what this machine actually reports");

    {
        // Not an assertion: an inventory. A suite that says "12 checks passed"
        // without saying what hardware it ran against is much harder to read
        // when a failure only happens on one bench, and this is the cheapest
        // possible way to have every run carry that context.
        const Capabilities caps = NativeHost::probe();
        std::fprintf(stderr, "  engine: %s (%s)\n", caps.available ? "available" : "unavailable",
                     caps.diagnostic.empty() ? toString(caps.reason) : caps.diagnostic.c_str());
        std::fprintf(stderr, "  capture: %s\n", toString(caps.capture));
        for (const GpuInfo& gpu : caps.gpus) {
            std::fprintf(stderr, "  gpu %d: %s [%04x:%04x] luid=%016llx encoders=%zu codecs=%zu\n",
                         gpu.id, gpu.name.c_str(), gpu.vendorId, gpu.deviceId,
                         static_cast<unsigned long long>(gpu.nativeHandle), gpu.encoders.size(),
                         gpu.codecs.size());
            for (EncoderApi e : gpu.encoders)
                std::fprintf(stderr, "        encoder: %s\n", toString(e));
        }
        for (const DisplayInfo& d : caps.displays) {
            std::fprintf(stderr, "  display %d: %s -> gpu %d%s%s\n", d.id, d.label.c_str(), d.gpuId,
                         d.hdrActive ? " [HDR]" : "", d.primary ? " [primary]" : "");
        }

        // Whatever the hardware, the association must hold: every display names
        // a GPU that exists. A display pointing at nothing would send the
        // Selector down the cross-GPU path on a machine that does not need it.
        for (const DisplayInfo& d : caps.displays)
            CHECK(caps.gpuFor(d) != nullptr);
    }

    SECTION("Capabilities — the log sink is optional");

    {
        // A library that writes somewhere of its own choosing cannot be
        // embedded. With no sink installed, logging must be a silent no-op.
        NativeHost::setLogSink(nullptr);
        (void)NativeHost::probe();

        int seen = 0;
        NativeHost::setLogSink([&seen](int, const std::string&) { seen++; });
        (void)NativeHost::probe();
        CHECK(seen > 0);
        NativeHost::setLogSink(nullptr);
    }
}
