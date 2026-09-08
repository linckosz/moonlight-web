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
#elif defined(__APPLE__)
#include "audio/macos/HostMute.h"

#include <CoreAudio/CoreAudio.h>
#endif

#include <cstdio>
#include <string>

// Silencing the host's speakers while the capture keeps hearing the mix
// (plan D5). Two things are worth a test here, and neither is the mute
// itself — that was measured once per platform with a probe, and depends on
// where the OS puts its tap:
//
//   * the survey must always have a verdict AND a sentence for it, because
//     "the host keeps hearing itself" is a state the user has to be told about;
//   * engage()/release() must leave the machine exactly as they found it. A
//     stream that ends with the speakers still muted is a support ticket.
//
// Windows and macOS both have a HostMute, with strategies of their own; the
// round trip below is the same for both, and asserts on whichever value the
// chosen strategy makes observable from outside. A runner with no playback
// device (CI) answers None with a reason, and the round trip is skipped rather
// than pretended.

#if defined(_WIN32) || defined(__APPLE__)
#define MW_HAS_HOST_MUTE 1
namespace {

#if defined(_WIN32)
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
// Windows never takes the volume route (it would silence the capture too), so
// there is nothing to read back for it.
float defaultOutputVolume()
{
    return -1.0f;
}
#else
AudioDeviceID defaultOutputDevice()
{
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &device) !=
        noErr)
        return kAudioObjectUnknown;
    return device;
}

// The master mute of the default output, or -1 when this Mac's output has
// none — an HDMI display, say, where HostMute uses the volume instead.
int defaultOutputMuted()
{
    const AudioDeviceID device = defaultOutputDevice();
    if (device == kAudioObjectUnknown) return -1;
    AudioObjectPropertyAddress a{kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput,
                                 kAudioObjectPropertyElementMain};
    UInt32 value = 0, size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &value) != noErr) return -1;
    return value ? 1 : 0;
}

// The master volume, or -1 when the output has none.
float defaultOutputVolume()
{
    const AudioDeviceID device = defaultOutputDevice();
    if (device == kAudioObjectUnknown) return -1.0f;
    AudioObjectPropertyAddress a{kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput,
                                 kAudioObjectPropertyElementMain};
    Float32 value = 0.0f;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &value) != noErr) return -1.0f;
    return value;
}
#endif

// Arranging the precondition, not exercising the class: a machine whose
// speakers are ALREADY muted sends engage() down its "left as it is" path, and
// the branch that matters — write, then put back — would never run. The test
// unmutes for the length of the round trip and restores whatever it found, on
// every exit. Windows has nothing to arrange: its own round trip covers both.
bool setDefaultOutputMuted(int muted)
{
#if defined(__APPLE__)
    const AudioDeviceID device = defaultOutputDevice();
    if (device == kAudioObjectUnknown) return false;
    AudioObjectPropertyAddress a{kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput,
                                 kAudioObjectPropertyElementMain};
    UInt32 value = muted ? 1u : 0u;
    return AudioObjectSetPropertyData(device, &a, 0, nullptr, sizeof(value), &value) == noErr;
#else
    (void)muted;
    return false;
#endif
}

} // namespace
#endif

void run_host_mute_tests()
{
    SECTION("Host mute — the speakers, not the capture");

#if !defined(MW_HAS_HOST_MUTE)
    std::fprintf(stderr, "  skipped: no host mute on this platform\n");
#else
    using mw::native::audio::HostMute;

    // The survey speaks in every case.
    std::string why;
    const HostMute::Strategy planned = HostMute::available(why);
    CHECK(!why.empty());
    std::fprintf(stderr, "  strategy: %s — %s\n", mw::native::audio::toString(planned),
                 why.c_str());

    const int foundMuted = defaultOutputMuted();
    const float volumeBefore = defaultOutputVolume();
    // Nothing readable at all (a CI runner with no playback device): there is
    // no round trip to observe, and the survey has to have said so. A machine
    // that HAS a device but no way to silence it still runs everything below —
    // engage() answering None is a case worth exercising.
    if (foundMuted < 0 && volumeBefore < 0.0f) {
        std::fprintf(stderr, "  skipped: no playback device on this machine\n");
        CHECK(planned == HostMute::Strategy::None);
        return;
    }

    // Speakers already muted: lift it so engage() has something to do, and put
    // it back before returning (see setDefaultOutputMuted).
    const bool arranged = foundMuted == 1 && setDefaultOutputMuted(0);
    if (arranged) std::fprintf(stderr, "  (unmuted first, so the write path is the one tested)\n");
    struct Restore
    {
        bool active;
        ~Restore()
        {
            if (active) setDefaultOutputMuted(1);
        }
    } restore{arranged};
    const int mutedBefore = arranged ? 0 : foundMuted;

    // Round trip: what engage() does, release() undoes.
    {
        HostMute mute;
        std::string how;
        const HostMute::Strategy done = mute.engage(how);
        CHECK(!how.empty());
        CHECK(done == planned);
        CHECK(mute.strategy() == done);
        std::fprintf(stderr, "  engage: %s\n", how.c_str());

        // Whichever strategy was picked, if this process can read its effect
        // from outside, it must actually be in effect while engaged.
#if defined(_WIN32)
        if (done == HostMute::Strategy::HardwareMute) CHECK_EQ(defaultOutputMuted(), 1);
#else
        if (done == HostMute::Strategy::EndpointMute) CHECK_EQ(defaultOutputMuted(), 1);
        if (done == HostMute::Strategy::VolumeZero) CHECK(defaultOutputVolume() <= 0.0001f);
#endif
        mute.release();
        CHECK(mute.strategy() == HostMute::Strategy::None);
        // ...and exactly as before once released — whichever way it was.
        CHECK_EQ(defaultOutputMuted(), mutedBefore);
        CHECK(defaultOutputVolume() == volumeBefore);

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
    CHECK(defaultOutputVolume() == volumeBefore);
#endif
}
