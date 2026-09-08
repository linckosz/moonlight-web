/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "HostMute.h"

#include "../../core/Log.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cmath>

namespace mw::native::audio {

namespace {

AudioObjectPropertyAddress outputAddr(AudioObjectPropertySelector selector, UInt32 element)
{
    return {selector, kAudioObjectPropertyScopeOutput, element};
}

AudioDeviceID defaultOutput()
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

std::string nameOf(AudioDeviceID device)
{
    AudioObjectPropertyAddress a{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                 kAudioObjectPropertyElementMain};
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &name) != noErr || !name)
        return "the default output";
    char buffer[256] = {0};
    const bool ok = CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? std::string(buffer) : std::string("the default output");
}

bool settable(AudioDeviceID device, AudioObjectPropertySelector selector, UInt32 element)
{
    AudioObjectPropertyAddress a = outputAddr(selector, element);
    if (!AudioObjectHasProperty(device, &a)) return false;
    Boolean can = false;
    return AudioObjectIsPropertySettable(device, &a, &can) == noErr && can;
}

/// The two channels CoreAudio calls this device's stereo pair, or 1 and 2 when
/// it does not say. Only consulted when the master element carries nothing.
std::vector<UInt32> stereoChannels(AudioDeviceID device)
{
    AudioObjectPropertyAddress a =
        outputAddr(kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyElementMain);
    UInt32 pair[2] = {1, 2};
    UInt32 size = sizeof(pair);
    AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, pair);
    return {pair[0], pair[1]};
}

/// Where a property can actually be written: the master element alone when it
/// takes it, the stereo pair otherwise, nothing when neither does.
std::vector<UInt32> writableElements(AudioDeviceID device, AudioObjectPropertySelector selector)
{
    if (settable(device, selector, kAudioObjectPropertyElementMain))
        return {static_cast<UInt32>(kAudioObjectPropertyElementMain)};
    std::vector<UInt32> elements;
    for (UInt32 channel : stereoChannels(device))
        if (settable(device, selector, channel)) elements.push_back(channel);
    return elements;
}

bool readMute(AudioDeviceID device, UInt32 element, UInt32& value)
{
    AudioObjectPropertyAddress a = outputAddr(kAudioDevicePropertyMute, element);
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &value) == noErr;
}

bool writeMute(AudioDeviceID device, UInt32 element, UInt32 value)
{
    AudioObjectPropertyAddress a = outputAddr(kAudioDevicePropertyMute, element);
    return AudioObjectSetPropertyData(device, &a, 0, nullptr, sizeof(value), &value) == noErr;
}

bool readVolume(AudioDeviceID device, UInt32 element, Float32& value)
{
    AudioObjectPropertyAddress a = outputAddr(kAudioDevicePropertyVolumeScalar, element);
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &value) == noErr;
}

bool writeVolume(AudioDeviceID device, UInt32 element, Float32 value)
{
    AudioObjectPropertyAddress a = outputAddr(kAudioDevicePropertyVolumeScalar, element);
    return AudioObjectSetPropertyData(device, &a, 0, nullptr, sizeof(value), &value) == noErr;
}

/// A volume this close to zero is silence; comparing scalars for equality
/// after a round trip through the driver would be wishful.
bool isSilent(Float32 volume)
{
    return volume <= 0.0001f;
}

struct Plan
{
    AudioDeviceID device = kAudioObjectUnknown;
    std::string name;
    std::vector<UInt32> muteElements;
    std::vector<UInt32> volumeElements;
};

bool survey(Plan& plan, std::string& why)
{
    plan.device = defaultOutput();
    if (plan.device == kAudioObjectUnknown) {
        why = "no default output device";
        return false;
    }
    plan.name = nameOf(plan.device);
    plan.muteElements = writableElements(plan.device, kAudioDevicePropertyMute);
    plan.volumeElements = writableElements(plan.device, kAudioDevicePropertyVolumeScalar);
    return true;
}

std::string quoted(const std::string& name)
{
    return "\"" + name + "\"";
}

} // namespace

const char* toString(HostMute::Strategy s)
{
    switch (s) {
    case HostMute::Strategy::EndpointMute: return "endpoint mute";
    case HostMute::Strategy::VolumeZero: return "volume to zero";
    case HostMute::Strategy::None: break;
    }
    return "none";
}

HostMute::~HostMute()
{
    release();
}

HostMute::Strategy HostMute::available(std::string& why)
{
    Plan plan;
    if (!survey(plan, why)) return Strategy::None;
    if (!plan.muteElements.empty()) {
        why = quoted(plan.name) + " mutes on its endpoint";
        return Strategy::EndpointMute;
    }
    if (!plan.volumeElements.empty()) {
        why = quoted(plan.name) + " has no mute — its volume would be taken to zero";
        return Strategy::VolumeZero;
    }
    why = quoted(plan.name) + " has neither a settable mute nor a settable volume";
    return Strategy::None;
}

HostMute::Strategy HostMute::engage(std::string& how)
{
    release();

    Plan plan;
    std::string why;
    if (!survey(plan, why)) {
        how = "the host keeps hearing its audio: " + why;
        return m_Strategy;
    }

    if (!plan.muteElements.empty()) {
        // Already muted — by the user, or by a session that ended badly. Claim
        // the strategy so the caller reports a silent host, but save nothing:
        // release() must not unmute what it did not mute.
        bool allMuted = true;
        for (UInt32 element : plan.muteElements) {
            UInt32 value = 0;
            if (!readMute(plan.device, element, value) || value == 0) {
                allMuted = false;
                break;
            }
        }
        if (allMuted) {
            m_Strategy = Strategy::EndpointMute;
            m_Device = plan.device;
            how = quoted(plan.name) + " is already muted — left as it is";
            return m_Strategy;
        }

        bool wrote = true;
        for (UInt32 element : plan.muteElements) {
            UInt32 previous = 0;
            readMute(plan.device, element, previous);
            if (!writeMute(plan.device, element, 1)) {
                wrote = false;
                break;
            }
            m_Saved.push_back({element, static_cast<float>(previous)});
        }
        if (wrote) {
            m_Strategy = Strategy::EndpointMute;
            m_Device = plan.device;
            how = "speakers muted on " + quoted(plan.name) +
                  " (the capture keeps hearing the mix — it does not go through this)";
            return m_Strategy;
        }
        // A device that advertises a settable mute and refuses it is treated
        // like one without: undo the half-written state and try the volume.
        for (const Saved& saved : m_Saved)
            writeMute(plan.device, saved.element, static_cast<UInt32>(saved.previous));
        m_Saved.clear();
    }

    if (!plan.volumeElements.empty()) {
        bool allSilent = true;
        for (UInt32 element : plan.volumeElements) {
            Float32 value = 1.0f;
            if (!readVolume(plan.device, element, value) || !isSilent(value)) {
                allSilent = false;
                break;
            }
        }
        if (allSilent) {
            m_Strategy = Strategy::VolumeZero;
            m_Device = plan.device;
            how = quoted(plan.name) + " is already at zero — left as it is";
            return m_Strategy;
        }

        bool wrote = true;
        for (UInt32 element : plan.volumeElements) {
            Float32 previous = 0.0f;
            readVolume(plan.device, element, previous);
            if (!writeVolume(plan.device, element, 0.0f)) {
                wrote = false;
                break;
            }
            m_Saved.push_back({element, previous});
        }
        if (wrote) {
            m_Strategy = Strategy::VolumeZero;
            m_Device = plan.device;
            how = "volume of " + quoted(plan.name) +
                  " taken to zero for the session (this output has no mute)";
            return m_Strategy;
        }
        for (const Saved& saved : m_Saved)
            writeVolume(plan.device, saved.element, saved.previous);
        m_Saved.clear();
    }

    how = "the host keeps hearing its audio: " + quoted(plan.name) +
          " has neither a settable mute nor a settable volume";
    return m_Strategy;
}

void HostMute::release()
{
    const auto device = static_cast<AudioDeviceID>(m_Device);
    for (const Saved& saved : m_Saved) {
        if (m_Strategy == Strategy::EndpointMute) {
            UInt32 now = 0;
            // Still muted = still ours to lift. Unmuted meanwhile = the user's
            // doing, and already what they want.
            if (readMute(device, saved.element, now) && now != 0)
                writeMute(device, saved.element, static_cast<UInt32>(saved.previous));
        } else if (m_Strategy == Strategy::VolumeZero) {
            Float32 now = 0.0f;
            if (readVolume(device, saved.element, now) && isSilent(now)) {
                if (!writeVolume(device, saved.element, saved.previous))
                    log::warning("[native] audio: could not put the output volume back — set it "
                                 "again from the menu bar");
            }
        }
    }
    m_Saved.clear();
    m_Strategy = Strategy::None;
    m_Device = 0;
}

} // namespace mw::native::audio
