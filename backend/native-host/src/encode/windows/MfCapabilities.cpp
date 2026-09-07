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

#include "MfCapabilities.h"
#include "MfApi.h"

#include "../../core/Log.h"

#include <mferror.h>

namespace mw::native::encode {
namespace {

/// One codec, in the two vocabularies this file has to speak.
struct CodecEntry
{
    Codec codec;
    const GUID& subtype;
};

/// Narrow a wide string without pulling in a locale. Friendly names are ASCII
/// in practice ("Intel® ..." being the one exception seen, whose registered
/// sign is dropped rather than mangled).
std::string narrow(const wchar_t* text)
{
    std::string out;
    if (!text) return out;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p < 0x80) out.push_back(static_cast<char>(*p));
    }
    return out;
}

std::string friendlyName(IMFActivate* activate)
{
    wchar_t* name = nullptr;
    UINT32 length = 0;
    if (FAILED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length)))
        return "unnamed transform";
    const std::string out = narrow(name);
    ::CoTaskMemFree(name);
    return out.empty() ? "unnamed transform" : out;
}

/// Whether at least one transform of `flags` turns NV12 into `subtype`.
///
/// Enumeration alone is the question asked, and that is a deliberate limit: it
/// proves a transform is registered for the conversion, not that it will accept
/// a particular resolution and bitrate. Instantiating each candidate and setting
/// media types would answer that, and would cost hundreds of milliseconds inside
/// a probe that runs on every host-list refresh. The session's own init() is
/// where a transform that lied gets found out, and it fails there with a real
/// message rather than silently.
bool anyTransform(UINT32 flags, const GUID& subtype, std::string& name)
{
    const MfApi* api = MfApi::instance();

    MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video, subtype};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr =
        api->TEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input, &output, &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) ::CoTaskMemFree(activates);
        return false;
    }

    // SORTANDFILTER put the preferred transform first, so the first is the one
    // a session would get.
    name = friendlyName(activates[0]);
    for (UINT32 i = 0; i < count; ++i)
        activates[i]->Release();
    ::CoTaskMemFree(activates);
    return true;
}

} // namespace

MfCaps queryMfCapabilities()
{
    MfCaps caps;

    const MfApi* api = MfApi::instance();
    if (!api->available()) {
        caps.diagnostic = api->unavailableReason();
        return caps;
    }

    // MFStartup is reference counted and safe to call more than once; the probe
    // pairs it with a Shutdown so a machine that never streams does not keep the
    // platform up.
    const HRESULT started = api->Startup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(started)) {
        caps.diagnostic = "MFStartup failed: " + MfApi::hresultToString(started);
        return caps;
    }

    // The order is the order of preference, and it is not the enum's.
    //
    // H.264 comes first here although Codec's own order puts AV1 and HEVC ahead
    // of it, because this tier exists for machines that could not encode at all:
    // H.264 is the codec every browser decodes in *hardware*, and a host too
    // weak to encode in hardware must not also push the client into a software
    // decoder. HEVC is offered after it for the case where the transform that
    // answered is hardware and the client prefers it — the Selector still walks
    // the client's own order, so this list only says what is on offer.
    const CodecEntry kCodecs[] = {
        {Codec::H264, MFVideoFormat_H264},
        {Codec::Hevc, MFVideoFormat_HEVC},
    };

    // Hardware first, and the whole list is taken from whichever tier answered:
    // mixing a hardware H.264 with a software HEVC would let the client's
    // preference silently choose the software one on a machine that had real
    // silicon available.
    const UINT32 kHardware = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    const UINT32 kSoftware = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                             MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    for (const bool hardware : {true, false}) {
        const UINT32 flags = hardware ? kHardware : kSoftware;
        for (const CodecEntry& entry : kCodecs) {
            std::string name;
            if (!anyTransform(flags, entry.subtype, name)) continue;
            if (caps.codecs.empty()) caps.name = name;
            caps.codecs.push_back(entry.codec);
        }
        if (!caps.codecs.empty()) {
            caps.hardware = hardware;
            break;
        }
    }

    api->Shutdown();

    if (caps.codecs.empty()) {
        caps.diagnostic = "Media Foundation is present but registers no encoder that takes NV12";
        return caps;
    }

    caps.usable = true;
    return caps;
}

} // namespace mw::native::encode
