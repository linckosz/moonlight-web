/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "audio/windows/HostMute.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#endif

#include <cstdio>
#include <string>

// Silencing the host's speakers while the loopback keeps hearing the mix
// (plan D5). Two things are worth a test here, and neither is the mute
// itself — that was measured once with a probe, and depends on the driver:
//
//   * the survey must always have a verdict AND a sentence for it, because
//     "the host keeps hearing itself" is a state the user has to be told about;
//   * engage()/release() must leave the machine exactly as they found it. A
//     stream that ends with the speakers still muted is a support ticket.
//
// A runner with no playback device (CI) answers None with a reason, and the
// round trip below is skipped rather than pretended.

#if defined(_WIN32)
namespace {

// The endpoint mute as Windows reports it right now, or -1 without a device.
int defaultOutputMuted()
{
    using Microsoft::WRL::ComPtr;
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result = -1;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume))) {
        BOOL muted = FALSE;
        if (SUCCEEDED(volume->GetMute(&muted))) result = muted ? 1 : 0;
    }
    if (SUCCEEDED(coInit)) CoUninitialize();
    return result;
}

} // namespace
#endif

void run_host_mute_tests()
{
    SECTION("Host mute — the speakers, not the capture");

#if !defined(_WIN32)
    std::fprintf(stderr, "  skipped: Windows only\n");
#else
    using mw::native::audio::HostMute;

    // The survey speaks in every case.
    std::string why;
    const HostMute::Strategy planned = HostMute::available(why);
    CHECK(!why.empty());
    std::fprintf(stderr, "  strategy: %s — %s\n", mw::native::audio::toString(planned),
                 why.c_str());

    const int mutedBefore = defaultOutputMuted();
    if (mutedBefore < 0) {
        std::fprintf(stderr, "  skipped: no playback device on this machine\n");
        CHECK(planned == HostMute::Strategy::None);
        return;
    }

    // Round trip: what engage() does, release() undoes.
    {
        HostMute mute;
        std::string how;
        const HostMute::Strategy done = mute.engage(how);
        CHECK(!how.empty());
        CHECK(done == planned);
        CHECK(mute.strategy() == done);
        std::fprintf(stderr, "  engage: %s\n", how.c_str());

        if (done == HostMute::Strategy::HardwareMute) {
            // The one strategy this process can observe from outside: the
            // endpoint reports muted while engaged...
            CHECK_EQ(defaultOutputMuted(), 1);
        }
        mute.release();
        CHECK(mute.strategy() == HostMute::Strategy::None);
        // ...and exactly as before once released — whichever way it was.
        CHECK_EQ(defaultOutputMuted(), mutedBefore);

        // Idempotent.
        mute.release();
        CHECK_EQ(defaultOutputMuted(), mutedBefore);
    }

    // The destructor releases too: a session torn down by an exception path
    // must not leave the room silent.
    {
        HostMute mute;
        std::string how;
        mute.engage(how);
    }
    CHECK_EQ(defaultOutputMuted(), mutedBefore);
#endif
}
