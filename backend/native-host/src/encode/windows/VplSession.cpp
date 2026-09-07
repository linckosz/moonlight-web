/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "VplSession.h"

#include "../../core/Log.h"
#include "../RateControl.h"

#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>

namespace mw::native::encode {
namespace {

/// Set one dispatcher property, each on its own config object — the dispatcher
/// treats a second property on the same config as a replacement of the first.
mfxStatus setProperty(const VplApi& api, mfxLoader loader, const char* name, mfxVariant value)
{
    mfxConfig config = api.CreateConfig(loader);
    if (!config) return MFX_ERR_MEMORY_ALLOC;

    // The dispatcher reads the variant's version field; a zeroed struct is not
    // a valid variant, whatever its payload says.
    value.Version.Version = MFX_VARIANT_VERSION;
    return api.SetConfigFilterProperty(config, reinterpret_cast<const mfxU8*>(name), value);
}

mfxStatus setPropertyU32(const VplApi& api, mfxLoader loader, const char* name, mfxU32 data)
{
    mfxVariant value = {};
    value.Type = MFX_VARIANT_TYPE_U32;
    value.Data.U32 = data;
    return setProperty(api, loader, name, value);
}

mfxStatus setPropertyPtr(const VplApi& api, mfxLoader loader, const char* name, void* data)
{
    mfxVariant value = {};
    value.Type = MFX_VARIANT_TYPE_PTR;
    value.Data.Ptr = static_cast<mfxHDL>(data);
    return setProperty(api, loader, name, value);
}

/// The DXGI adapter a D3D11 device was opened on, as a 64-bit LUID.
uint64_t adapterLuidOf(ID3D11Device* device)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) return 0;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi->GetAdapter(adapter.GetAddressOf()))) return 0;

    DXGI_ADAPTER_DESC desc = {};
    if (FAILED(adapter->GetDesc(&desc))) return 0;

    return (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
           static_cast<uint64_t>(desc.AdapterLuid.LowPart);
}

} // namespace

VplSession::~VplSession()
{
    close();
}

bool VplSession::open(ID3D11Device* device, std::string& error)
{
    close();

    if (!device) {
        error = "no D3D11 device";
        return false;
    }

    // Intel's runtime shares the device with our own capture and conversion
    // threads and does not serialize for us. Without this every session is a
    // race that shows up as a corrupted frame or a device removal, far from its
    // cause. It is idempotent and cheap, so it is asked for here rather than
    // trusted to the caller.
    {
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread))))
            multithread->SetMultithreadProtected(TRUE);
    }

    m_Api = VplApi::instance();
    if (!m_Api->available()) {
        error = m_Api->unavailableReason();
        return false;
    }

    // ── Two routes to a session, and the second one is the one that works ───
    //
    // ⚠️ The Media SDK taught everyone to create the session and then hand it a
    // device with MFXVideoCORE_SetHandle. On the oneVPL 2.x dispatcher that
    // fails with MFX_ERR_UNDEFINED_BEHAVIOR (-16) — "the same handle is
    // redefined ... or an internal handle has been created before this function
    // call": the runtime already made a D3D11 device of its own while creating
    // the session, so the slot is taken. Measured on an N95 / UHD Graphics,
    // driver 32.0.101.7088, where every adapter answered -16 and the machine
    // reported "no usable encoder" with Quick Sync sitting right there.
    //
    // Route A hands the device to the DISPATCHER instead, through two config
    // properties that belong to the dispatcher rather than to
    // mfxImplDescription (hence their absence from the headers). Route B is the
    // old one, kept for a runtime that does defer its device to us.
    //
    // Either way the session ends up on ONE device, and open() records which:
    // the encoder has to know whether the textures it is about to hand over
    // belong to that device or to a different one on the same adapter.
    for (int route = 0; route < 2; ++route) {
        const bool giveAtCreation = route == 0;

        m_Loader = m_Api->Load();
        if (!m_Loader) {
            error = "could not start the oneVPL dispatcher";
            return false;
        }

        // Hardware only. Without this filter oneVPL hands back its software
        // fallback, which encodes on the CPU — not what "Intel encoder" means
        // here, and it would read as a mysterious CPU spike rather than as the
        // honest "no hardware encoder on this machine".
        if (setPropertyU32(*m_Api, m_Loader, "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE) !=
            MFX_ERR_NONE) {
            error = "could not ask oneVPL for a hardware implementation";
            close();
            return false;
        }
        // Same reasoning one level down: D3D11 is the only acceleration mode
        // whose surfaces this pipeline can hand over.
        setPropertyU32(*m_Api, m_Loader, "mfxImplDescription.AccelerationMode",
                       MFX_ACCEL_MODE_VIA_D3D11);

        if (giveAtCreation) {
            if (setPropertyPtr(*m_Api, m_Loader, "mfxHDL", static_cast<void*>(device)) !=
                    MFX_ERR_NONE ||
                setPropertyU32(*m_Api, m_Loader, "mfxHandleType", MFX_HANDLE_D3D11_DEVICE) !=
                    MFX_ERR_NONE) {
                // A dispatcher too old to know the properties. Nothing to
                // report: route B is the answer for it.
                close();
                continue;
            }
        }

        // Index 0 among the implementations that passed the filter: with the
        // hardware filter applied, anything it returns is a real encoder.
        const mfxStatus created = m_Api->CreateSession(m_Loader, 0, &m_Session);
        if (created != MFX_ERR_NONE || !m_Session) {
            m_Session = nullptr;
            close();
            if (giveAtCreation) continue;
            error = std::string("no Intel hardware encoder available: ") +
                    VplApi::statusToString(created);
            return false;
        }

        if (!giveAtCreation) {
            // A refusal is not fatal here: the check below asks the session
            // which device it settled on, and a runtime that kept its own is
            // still usable — at the price of a bridge for the input surface.
            (void)m_Api->SetHandle(m_Session, MFX_HANDLE_D3D11_DEVICE, static_cast<mfxHDL>(device));
        }

        // ── Whose device is it, really ──────────────────────────────────────
        //
        // Asked rather than assumed. A session running on a device of the
        // runtime's own making takes none of OUR textures, and the mistake
        // would surface much later as an encode error with no obvious cause.
        // It also settles WHICH GPU, on a machine where DXGI lists the same
        // Intel chip three times (one real adapter, two indirect displays).
        mfxHDL inUse = nullptr;
        const mfxStatus asked = m_Api->GetHandle(m_Session, MFX_HANDLE_D3D11_DEVICE, &inUse);
        ID3D11Device* runtimeDevice = static_cast<ID3D11Device*>(inUse);
        if (asked != MFX_ERR_NONE || !runtimeDevice) {
            error = "oneVPL will not say which device it is using (" +
                    VplApi::statusToString(asked) + ")";
            close();
            return false;
        }

        // Taken now, reference and all, so that every exit below gives it back
        // through close().
        m_Device = runtimeDevice;

        const uint64_t wanted = adapterLuidOf(device);
        const uint64_t got = adapterLuidOf(runtimeDevice);
        if (got != wanted) {
            error = "oneVPL opened on adapter " + std::to_string(got) + " instead of " +
                    std::to_string(wanted);
            close();
            return false;
        }

        m_BorrowedDevice = runtimeDevice != device;

        // The allocator is what makes a MemId mean something. Registered before
        // anything is initialized, because the encoder allocates its own
        // reconstructed frames through it at EncodeInit time.
        m_Allocator = std::make_unique<VplFrameAllocator>(m_Device);
        const mfxStatus wired = m_Api->SetFrameAllocator(m_Session, m_Allocator->callbacks());
        if (wired != MFX_ERR_NONE) {
            error = "oneVPL refused the frame allocator (" + VplApi::statusToString(wired) + ")";
            close();
            return false;
        }

        log::info(
            std::string("[native] oneVPL session on ") +
            (m_BorrowedDevice ? "the runtime's own D3D11 device" : "this engine's D3D11 device") +
            (giveAtCreation ? " (handed to the dispatcher)" : " (set on the session)"));
        return true;
    }

    error = "the oneVPL dispatcher refused every route to a hardware session";
    close();
    return false;
}

void VplSession::close()
{
    if (m_Session && m_Api) {
        m_Api->Close(m_Session);
        m_Session = nullptr;
    }
    if (m_Loader && m_Api) {
        m_Api->Unload(m_Loader);
        m_Loader = nullptr;
    }
    // After MFXClose: the runtime frees its surfaces through the allocator on
    // the way out, so it must still be there when the session goes.
    m_Allocator.reset();
    // ⚠️ Never released. MFXVideoCORE_GetHandle is documented to addref the COM
    // interface it returns and to leave the release to the caller — Intel's
    // runtime does not. Giving back a reference we were never given frees the
    // device under its real owner: measured as an access violation inside
    // d3d11!CDevice::Release the moment the capability probe let its own ComPtr
    // go. The handle is borrowed, and the caller's device outlives the session.
    m_Device = nullptr;
    m_BorrowedDevice = false;
}

void applyRateControl(mfxVideoParam& params, int fps, int bitrateKbps, const EncoderTuning& tuning)
{
    if (fps <= 0) fps = 60;
    if (bitrateKbps <= 0) bitrateKbps = 20000;

    // TargetKbps and friends are 16-bit, so anything at or above 65535 kbps
    // needs the multiplier — and MoonlightWeb genuinely offers up to 150 Mbps.
    // Without this a 100 Mbps request would silently wrap to a fraction of
    // itself, which looks like the encoder ignoring the bitrate setting.
    mfxU16 multiplier = 1;
    while (bitrateKbps / multiplier > 65000)
        ++multiplier;
    params.mfx.BRCParamMultiplier = multiplier;
    params.mfx.TargetKbps = static_cast<mfxU16>(bitrateKbps / multiplier);
    params.mfx.MaxKbps = params.mfx.TargetKbps;

    // The buffer, in KB, which is what actually enforces the latency: no single
    // frame may be so large that it takes several frame times to transmit. See
    // RateControl.h for why it has a frame-rate floor.
    const int frameKb =
        static_cast<int>(vbvBits(static_cast<uint32_t>(bitrateKbps), fps, tuning.vbvFrames)) / 8;
    const int bufferKb = frameKb > 0 ? frameKb : 1;
    params.mfx.BufferSizeInKB =
        static_cast<mfxU16>((bufferKb / multiplier) > 0 ? (bufferKb / multiplier) : 1);
    params.mfx.InitialDelayInKB = params.mfx.BufferSizeInKB;
}

void applyBitrateOnly(mfxVideoParam& params, int bitrateKbps)
{
    if (bitrateKbps <= 0) return;

    // ⚠️ The buffer is deliberately left where init() put it.
    //
    // MFXVideoENCODE_Reset only accepts a change the encoder can make without
    // reallocating. Moving BufferSizeInKB — which is what a re-derived VBV
    // does — is exactly such a reallocation, and the runtime answers
    // MFX_ERR_INCOMPATIBLE_VIDEO_PARAM (-14) for the whole call. Measured on
    // an N95: every rate change the link governor asked for was refused, so
    // the Intel encoder ignored the link entirely while the log said the
    // bitrate had moved. A VBV a little roomier than the new rate deserves is
    // the cheap half of that trade.
    const mfxU16 multiplier = params.mfx.BRCParamMultiplier > 0 ? params.mfx.BRCParamMultiplier : 1;
    int scaled = bitrateKbps / multiplier;
    if (scaled < 1) scaled = 1;
    if (scaled > 65000) scaled = 65000;
    params.mfx.TargetKbps = static_cast<mfxU16>(scaled);
    params.mfx.MaxKbps = params.mfx.TargetKbps;
}

bool fillEncodeParams(mfxVideoParam& params, Codec codec, int width, int height, int fps,
                      int bitrateKbps, const EncoderTuning& tuning)
{
    std::memset(&params, 0, sizeof(params));

    switch (codec) {
    case Codec::H264: params.mfx.CodecId = MFX_CODEC_AVC; break;
    case Codec::Hevc: params.mfx.CodecId = MFX_CODEC_HEVC; break;
    case Codec::Av1: params.mfx.CodecId = MFX_CODEC_AV1; break;
    default: return false;
    }

    if (fps <= 0) fps = 60;
    if (bitrateKbps <= 0) bitrateKbps = 20000;

    // The surfaces come from our own conversion pass, in video memory.
    params.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    // No look-ahead in the pipeline: one frame in, one frame out.
    params.AsyncDepth = 1;

    // 7 = best speed. The brief weights latency at 70 % against 30 % quality,
    // and on Intel the target-usage scale costs far more time at the quality
    // end than it returns in picture. The bench may ask for another to check.
    params.mfx.TargetUsage = (tuning.vplTargetUsage >= 1 && tuning.vplTargetUsage <= 7)
                                 ? static_cast<mfxU16>(tuning.vplTargetUsage)
                                 : static_cast<mfxU16>(MFX_TARGETUSAGE_7);
    params.mfx.RateControlMethod = MFX_RATECONTROL_CBR;

    // The fixed-function encode engine (VDENC) rather than the shader-based one.
    // It is what "low power" means on Intel: fewer passes, less latency, at a
    // small cost in bits — the trade this engine takes everywhere else too. A
    // generation without it says so at Query, and init() retries without — see
    // VplEncoder::init.
    params.mfx.LowPower = MFX_CODINGOPTION_ON;

    applyRateControl(params, fps, bitrateKbps, tuning);

    // No B-frames, and no keyframe the client did not ask for.
    params.mfx.GopRefDist = 1;
    params.mfx.GopPicSize = 0xFFFF; // effectively infinite
    params.mfx.IdrInterval = 0xFFFF;
    params.mfx.NumRefFrame = 1;

    params.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    params.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    params.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    params.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(fps);
    params.mfx.FrameInfo.FrameRateExtD = 1;
    params.mfx.FrameInfo.CropX = 0;
    params.mfx.FrameInfo.CropY = 0;
    params.mfx.FrameInfo.CropW = static_cast<mfxU16>(width);
    params.mfx.FrameInfo.CropH = static_cast<mfxU16>(height);
    // Intel hardware wants 16-aligned width and height for the surface itself;
    // the crop above is what the picture really is.
    params.mfx.FrameInfo.Width = static_cast<mfxU16>((width + 15) & ~15);
    params.mfx.FrameInfo.Height = static_cast<mfxU16>((height + 15) & ~15);

    return true;
}

void attachEncodeOptions(mfxVideoParam& params, mfxExtCodingOption& option1,
                         mfxExtCodingOption2& option2, std::vector<mfxExtBuffer*>& buffers, int fps,
                         bool intraRefresh)
{
    std::memset(&option1, 0, sizeof(option1));
    option1.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
    option1.Header.BufferSz = sizeof(option1);

    // ⚠️ The rate control must not be HRD-conformant, and that is not a quality
    // decision — it is the only way the bitrate can move at all.
    //
    // With the HRD model on, oneVPL treats a bitrate change as a new sequence
    // and MFXVideoENCODE_Reset answers MFX_ERR_INCOMPATIBLE_VIDEO_PARAM (-14)
    // for anything short of an IDR. The link governor asks for a change about
    // twice a second on a moving link, so on the first Intel machine every
    // single one was refused: the encoder never followed the link, and the only
    // trace was a warning a session log would scroll past. The alternative —
    // a forced new sequence — buys conformance with a keyframe twice a second,
    // which is exactly the bitrate spike a congested link cannot take.
    option1.NalHrdConformance = MFX_CODINGOPTION_OFF;
    // Say in the VUI that one decoded picture is all a receiver must hold. Same
    // fix as bug B8 on NVENC, where its absence had Chrome's D3D11 decoder sit
    // on a whole DPB and turned a 8 ms path into a 200 ms one.
    option1.MaxDecFrameBuffering = 1;
    // Nothing downstream reads either, and both cost bytes on every frame.
    option1.AUDelimiter = MFX_CODINGOPTION_OFF;
    option1.PicTimingSEI = MFX_CODINGOPTION_OFF;

    buffers.clear();
    buffers.push_back(reinterpret_cast<mfxExtBuffer*>(&option1));

    if (intraRefresh) {
        std::memset(&option2, 0, sizeof(option2));
        option2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
        option2.Header.BufferSz = sizeof(option2);

        // Vertical: the wave sweeps by columns of macroblocks. Either axis
        // works; vertical is the conventional choice and matches what the other
        // two vendors do by default.
        option2.IntRefType = MFX_REFRESH_VERTICAL;
        option2.IntRefCycleSize = static_cast<mfxU16>(intraRefreshPeriodFrames(fps));
        // Leave the refreshed blocks at the frame's own quality: a positive
        // delta would make the healing band visibly coarser than what surrounds
        // it, which is precisely the artefact this is meant to avoid.
        option2.IntRefQPDelta = 0;

        buffers.push_back(reinterpret_cast<mfxExtBuffer*>(&option2));
    }

    params.ExtParam = buffers.data();
    params.NumExtParam = static_cast<mfxU16>(buffers.size());
}

} // namespace mw::native::encode
