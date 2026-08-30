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
            cfg, [](const EncodedFrame&) {}, nullptr, nullptr, nullptr, error);

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
        auto session = NativeHost::createSession(cfg, nullptr, nullptr, nullptr, nullptr, error);
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
