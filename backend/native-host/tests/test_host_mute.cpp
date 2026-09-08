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
#elif defined(MW_NATIVE_LINUX_AUDIO)
#include "audio/linux/HostMute.h"
#endif

// What Linux reads out of the PipeWire graph as text: no PipeWire needed to
// exercise it, so it is tested wherever this suite runs.
#include "audio/linux/PipeWireDefaults.h"

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
// All three platforms have a HostMute now, with strategies of their own; the
// round trip below is the same for all of them, and asserts on whichever value
// the chosen strategy makes observable from outside. A runner with no playback
// device (CI) answers None with a reason, and the round trip is skipped rather
// than pretended.
//
// Linux is the one platform where reading the state back from OUTSIDE the class
// would mean a second PipeWire client — that is, this file re-implementing what
// it is testing. Its round trip therefore checks what the class itself reports
// and that nothing hangs; the external proof is a real session on the bench with
// `pactl get-sink-mute` watching, the way macOS was proved with osascript
// (design §19.17).

#if defined(_WIN32) || defined(__APPLE__) || defined(MW_NATIVE_LINUX_AUDIO)
#define MW_HAS_HOST_MUTE 1
namespace {

// Whether this platform lets the test read the effect back without becoming a
// second implementation of the thing it is testing. See the note above: Linux
// does not.
#if defined(_WIN32) || defined(__APPLE__)
constexpr bool kExternallyObservable = true;
#else
constexpr bool kExternallyObservable = false;
#endif

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
#elif defined(__APPLE__)
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
#else
// Linux: nothing is read back here on purpose (see kExternallyObservable).
int defaultOutputMuted()
{
    return -1;
}
float defaultOutputVolume()
{
    return -1.0f;
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
    SECTION("Host mute — reading the PipeWire graph's answer out of text");
    {
        using mw::native::audio::monitorCarriesVolume;
        using mw::native::audio::sinkNameFromDefaultJson;

        // What WirePlumber actually writes, taken from the bench.
        CHECK(sinkNameFromDefaultJson(
                  "{\"name\":\"alsa_output.pci-0000_c4_00.6.HiFi__hw_Generic_1__sink\"}") ==
              "alsa_output.pci-0000_c4_00.6.HiFi__hw_Generic_1__sink");
        CHECK(sinkNameFromDefaultJson("{ \"name\" : \"mw_null\" }") == "mw_null");
        // A name may carry an escaped quote; half a value is not a name, and
        // getting that wrong would mute the wrong sink.
        CHECK(sinkNameFromDefaultJson("{\"name\":\"od\\\"d\"}") == "od\"d");
        CHECK(sinkNameFromDefaultJson("{\"name\":\"unterminated").empty());
        CHECK(sinkNameFromDefaultJson("{\"name\":42}").empty());
        CHECK(sinkNameFromDefaultJson("{}").empty());
        CHECK(sinkNameFromDefaultJson("").empty());
        // The default output being unset is a normal state, not a parse error.
        CHECK(sinkNameFromDefaultJson("null").empty());

        // Absent is the case that decides for a mute: on every real output the
        // property is simply not there.
        CHECK(!monitorCarriesVolume(nullptr));
        CHECK(!monitorCarriesVolume("false"));
        CHECK(!monitorCarriesVolume(""));
        CHECK(monitorCarriesVolume("true"));
        CHECK(monitorCarriesVolume("1"));
    }

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
    if (kExternallyObservable && foundMuted < 0 && volumeBefore < 0.0f) {
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
#elif defined(__APPLE__)
        if (done == HostMute::Strategy::EndpointMute) CHECK_EQ(defaultOutputMuted(), 1);
        if (done == HostMute::Strategy::VolumeZero) CHECK(defaultOutputVolume() <= 0.0001f);
#else
        // Linux: nothing to read back from here that would not be a second
        // implementation of HostMute. What this run does prove is that engage()
        // returned at all — a round trip that never completes would hang the
        // suite rather than fail it — and that it agreed with the survey. The
        // mute itself is proved on the bench.
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
