/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#pragma once

#include "VplApi.h"
#include "mw/native/Capabilities.h"

#include <d3d11.h>

#include <string>

namespace mw::native::encode {

/// A oneVPL session bound to one specific D3D11 device.
///
/// Shared by the capability query and the encoder, because both need exactly
/// the same thing and getting it subtly different between them is how a probe
/// ends up promising what the encoder cannot deliver.
///
/// ── Why binding the device is what picks the GPU ───────────────────────────
///
/// oneVPL's dispatcher can be filtered by implementation, adapter index or
/// device id, all of which mean matching Intel's enumeration against DXGI's by
/// hand. Handing the session OUR D3D11 device instead — the one already opened
/// on the adapter that scans out the display — makes the runtime use that
/// adapter and nothing else. It is the same move AMF's InitDX11 makes, and it
/// removes a whole class of "answered for the wrong GPU" mistakes.
class VplSession
{
public:
    VplSession() = default;
    ~VplSession();

    VplSession(const VplSession&) = delete;
    VplSession& operator=(const VplSession&) = delete;

    /// Open a hardware session on @p device. Returns false with a reason when
    /// oneVPL is absent, has no hardware implementation, or refuses the device.
    bool open(ID3D11Device* device, std::string& error);
    void close();

    bool isOpen() const { return m_Session != nullptr; }
    mfxSession handle() const { return m_Session; }
    const VplApi* api() const { return m_Api; }

private:
    const VplApi* m_Api = nullptr;
    mfxLoader m_Loader = nullptr;
    mfxSession m_Session = nullptr;
};

/// Fill @p params for a low-latency screen stream.
///
/// Every value here is a latency decision taken against oneVPL's defaults,
/// which target transcoding:
///
///  - `AsyncDepth = 1` — the encoder may not run ahead. Anything more buys
///    throughput with frames held back, which is the opposite of the trade
///    this engine makes.
///  - `GopRefDist = 1` — no B-frames. One would reference a picture not yet
///    sent, costing a whole frame of delay.
///  - `GopPicSize` effectively infinite — a periodic keyframe is a bitrate
///    spike, and on a congested link the spike causes the loss that provokes
///    the request for another. Keyframes are emitted on demand instead.
///  - CBR with a one-frame buffer — the buffer is what actually enforces low
///    latency, capping how far ahead the encoder may spend.
///
/// Returns false when the codec has no oneVPL FourCC (nothing does today, but
/// the enum can grow).
bool fillEncodeParams(mfxVideoParam& params, Codec codec, int width, int height, int fps,
                      int bitrateKbps);

} // namespace mw::native::encode
