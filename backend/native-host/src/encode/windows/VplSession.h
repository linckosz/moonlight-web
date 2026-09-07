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
#include "VplFrameAllocator.h"
#include "mw/native/Capabilities.h"

#include <d3d11.h>

#include "mw/native/EncoderTuning.h"

#include <memory>
#include <string>
#include <vector>

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
///
/// ⚠️ The device is given to the DISPATCHER, before the session exists, and not
/// to MFXVideoCORE_SetHandle afterwards: on oneVPL 2.x the dispatcher has
/// already made a device of its own by then and answers -16, "the same handle
/// is redefined". See open() — the first real Intel machine cost us that one.
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
    /// The D3D11 device the runtime is really working on — checked at open(),
    /// so a surface handed to the encoder can be trusted to belong to it.
    ID3D11Device* device() const { return m_Device; }
    /// True when that device is the runtime's own rather than the one open()
    /// was given — the case where a texture from this engine cannot be handed
    /// to the encoder as it is.
    bool borrowedDevice() const { return m_BorrowedDevice; }

private:
    const VplApi* m_Api = nullptr;
    mfxLoader m_Loader = nullptr;
    mfxSession m_Session = nullptr;
    /// Borrowed, never owned — see close() for what releasing it costs.
    ID3D11Device* m_Device = nullptr;
    bool m_BorrowedDevice = false;
    /// Outlives the session by construction: close() drops it last.
    std::unique_ptr<VplFrameAllocator> m_Allocator;
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
/// @p tuning is the bench's overrides (TargetUsage, VBV); the default is the
/// engine's own choice and what every real session passes.
///
/// Returns false when the codec has no oneVPL FourCC (nothing does today, but
/// the enum can grow).
/// @p hdr switches the input to P010 and the profile to HEVC Main10. The
/// COLOUR description that goes with it is a separate extension buffer — see
/// attachEncodeOptions — because a 10-bit stream whose VUI still says BT.709
/// sRGB is displayed washed out rather than refused.
bool fillEncodeParams(mfxVideoParam& params, Codec codec, int width, int height, int fps,
                      int bitrateKbps, const EncoderTuning& tuning = EncoderTuning{},
                      bool hdr = false);

/// Chain the extension buffers this pipeline always wants onto @p params, plus
/// intra-refresh when @p intraRefresh is set.
///
/// Always: no HRD conformance (without which the bitrate cannot be changed at
/// all — see the body), a VUI that asks the receiver to hold one picture, and
/// no access-unit delimiters or picture-timing SEI. When asked: intra-refresh
/// sweeping the picture over two seconds' worth of frames at @p fps, the same
/// duration the NVENC and AMF paths use (RateControl.h), so the three vendors
/// behave alike from the receiver's side.
///
/// @p option1, @p option2 and @p buffers are the CALLER's storage and must stay
/// alive for as long as @p params is in use — the runtime reads the chain again
/// on Reset, and a dangling extension buffer there is a use-after-free it
/// cannot warn about. Taking them by reference rather than allocating here is
/// what makes that ownership impossible to get wrong.
void attachEncodeOptions(mfxVideoParam& params, mfxExtCodingOption& option1,
                         mfxExtCodingOption2& option2, mfxExtCodingOption3& option3,
                         mfxExtVideoSignalInfo& signal, std::vector<mfxExtBuffer*>& buffers,
                         int fps, bool intraRefresh, const EncoderTuning& tuning = EncoderTuning{},
                         bool hdr = false);

/// Write the rate-control fields — and only those — into an existing block.
///
/// Split out of fillEncodeParams so that changing the bitrate mid-session can
/// touch what it means to touch and nothing else. Rebuilding the whole block
/// instead is what cost oneVPL its intra-refresh (a fresh block has no
/// extension chain) and its runtime corrections (a fresh block has never been
/// through EncodeQuery) on every rate change — and the link governor makes one
/// about twice a second.
void applyRateControl(mfxVideoParam& params, int fps, int bitrateKbps, const EncoderTuning& tuning);

/// The subset of that a running encoder will actually accept: the target rate,
/// and nothing else. See the body for why the buffer must not move.
void applyBitrateOnly(mfxVideoParam& params, int bitrateKbps);

/// The most the per-frame budget may rise to during a session, and therefore
/// what init() must size the bitstream buffer for. See the body: Reset refuses
/// every cheaper way of raising it.
int budgetCeilingKbps(int bitrateKbps, int fps);

/// The rate the bitstream buffer must be sized for so that a raise to
/// budgetCeilingKbps() is accepted at all. Bigger than the ceiling — see body.
int budgetBufferKbps(int bitrateKbps, int fps);

/// How many pictures the encoder keeps to predict from when the bench has not
/// asked for another number. See VplSession.cpp for why it is not one.
constexpr int kDefaultRefFrames = 4;

/// How many times the stream's rate a single frame's budget may reach. Two,
/// because that is the whole range EffectiveCadence works in (its floor is half
/// of a 60 fps stream), and because each step costs a frame time of VBV.
constexpr int kBudgetHeadroom = 2;

} // namespace mw::native::encode
