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
    // ⚠️ Not a ceiling one can raise later. Declaring a MaxKbps above the target
    // to leave room for a future Reset was tried and measured on an N95: in CBR
    // the runtime flattens it straight back onto TargetKbps, exactly as the
    // documentation says it may ("ignored"). The buffer below is what actually
    // decides how far the per-frame budget may later rise.
    params.mfx.MaxKbps = params.mfx.TargetKbps;

    // The buffer, in KB, which is what actually enforces the latency: no single
    // frame may be so large that it takes several frame times to transmit. See
    // RateControl.h for why it has a frame-rate floor.
    //
    // ⚠️ Sized for the budget CEILING, not for the rate — on this vendor only.
    // It is the buffer Reset validates a later raise against, so a buffer sized
    // exactly for the rate is a session whose per-frame budget can never move.
    // The price is that it is also the VBV: budgetCeilingKbps() says why the
    // headroom is two and not more.
    //
    // ⚠️ The bench's `vbv=<frames>` means what it says on every vendor —
    // exactly N frames at the STREAM's rate — so it bypasses the headroom
    // entirely. Feeding it the padded rate instead would have made the one key
    // that measures the VBV unable to measure it, which is how a cost gets
    // assumed instead of counted.
    const uint32_t vbvRate = tuning.vbvFrames > 0
                                 ? static_cast<uint32_t>(bitrateKbps)
                                 : static_cast<uint32_t>(budgetBufferKbps(bitrateKbps, fps));
    const int frameKb = static_cast<int>(vbvBits(vbvRate, fps, tuning.vbvFrames)) / 8;
    const int bufferKb = frameKb > 0 ? frameKb : 1;
    params.mfx.BufferSizeInKB =
        static_cast<mfxU16>((bufferKb / multiplier) > 0 ? (bufferKb / multiplier) : 1);
    params.mfx.InitialDelayInKB = params.mfx.BufferSizeInKB;
}

void applyBitrateOnly(mfxVideoParam& params, int bitrateKbps)
{
    if (bitrateKbps <= 0) return;

    // ⚠️ One field is deliberately left where init() put it: the buffer.
    //
    // MFXVideoENCODE_Reset only accepts a change the encoder can make without
    // reallocating. Moving BufferSizeInKB — which is what a re-derived VBV
    // does — is exactly such a reallocation, and the runtime answers
    // MFX_ERR_INCOMPATIBLE_VIDEO_PARAM (-14) for the whole call. Measured on
    // an N95: every rate change the link governor asked for was refused, so
    // the Intel encoder ignored the link entirely while the log said the
    // bitrate had moved. A VBV a little roomier than the new rate deserves is
    // the cheap half of that trade.
    //
    // MaxKbps follows the target, as it does at init: in CBR the runtime
    // flattens the two together anyway, and leaving them apart would only make
    // a read-back of the running configuration lie.
    const mfxU16 multiplier = params.mfx.BRCParamMultiplier > 0 ? params.mfx.BRCParamMultiplier : 1;
    int scaled = bitrateKbps / multiplier;
    if (scaled < 1) scaled = 1;
    if (scaled > 65000) scaled = 65000;
    params.mfx.TargetKbps = static_cast<mfxU16>(scaled);
    params.mfx.MaxKbps = params.mfx.TargetKbps;
}

int budgetCeilingKbps(int bitrateKbps, int fps)
{
    if (bitrateKbps <= 0) return bitrateKbps;
    if (fps <= 0) fps = 60;

    // How much room the per-frame budget is given on this vendor, and why it is
    // not simply "all of it".
    //
    // ⚠️ Two cheaper routes were tried on an N95 and both were refused:
    //   · declaring a higher MaxKbps at init — CBR flattens it back onto the
    //     target, exactly as the documentation allows;
    //   · leaving the rate alone and telling Reset the frame rate is lower,
    //     which is arithmetically the same budget — refused too (-14). Reset
    //     turns down anything that moves the per-frame budget, whichever field
    //     it is written in.
    //
    // What is left is to size the bitstream buffer for the raise at init, since
    // that is what Reset checks against. But the buffer IS the latency
    // constraint (RateControl.h): a frame may occupy as much of the link as the
    // buffer allows. So the headroom is not "whatever anyone might ask for" —
    // that would be six times the rate, six frame times for one picture — but
    // exactly the range EffectiveCadence works in, which is the case that
    // actually improves the picture: a screen moving at half the stream's rate,
    // whose frames may then be twice the size. The refinement burst's ×3 on a
    // still screen stays out of reach, and is capped rather than refused.
    const int headroom = fps / EffectiveCadence::kMinFps;
    const int capped = headroom < 1 ? 1 : (headroom > kBudgetHeadroom ? kBudgetHeadroom : headroom);
    return bitrateKbps * capped;
}

int budgetBufferKbps(int bitrateKbps, int fps)
{
    // The buffer Reset measures a raise against is not "one frame at the new
    // rate" — measured on an N95, a raise to 40000 was refused with an 85 KB
    // buffer and accepted with 250 KB, which is three frames at 40000. So the
    // buffer this asks for is the ceiling times that factor; anything less and
    // the ceiling is decorative.
    constexpr int kResetBufferFrames = 3;
    return budgetCeilingKbps(bitrateKbps, fps) * kResetBufferFrames;
}

bool fillEncodeParams(mfxVideoParam& params, Codec codec, int width, int height, int fps,
                      int bitrateKbps, const EncoderTuning& tuning, bool hdr)
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
    params.mfx.LowPower = tuning.vplLowPower == EncoderTuning::Choice::Off ? MFX_CODINGOPTION_OFF
                                                                           : MFX_CODINGOPTION_ON;

    applyRateControl(params, fps, bitrateKbps, tuning);

    // No B-frames, and no keyframe the client did not ask for.
    params.mfx.GopRefDist = 1;
    params.mfx.GopPicSize = 0xFFFF; // effectively infinite
    params.mfx.IdrInterval = 0xFFFF;
    // ⚠️ Was 1, which made reference invalidation impossible by construction:
    // with a single reference there is no older picture to fall back on, so a
    // lost frame could only ever be answered with a keyframe. The extra
    // pictures are what let a loss cost a delta instead — same number, and the
    // same reason, as the NVENC DPB. The bench can still ask for 1 to measure
    // what they cost.
    params.mfx.NumRefFrame =
        static_cast<mfxU16>(tuning.dpbFrames > 0 ? tuning.dpbFrames : kDefaultRefFrames);

    if (hdr) {
        // The conversion pass hands over P010: 10 bits in the high end of each
        // 16-bit sample, which is what Shift says. Main10 is the only HEVC
        // profile that takes it, and naming it here rather than leaving the
        // runtime to guess is what keeps a 10-bit stream from being encoded as
        // if it were 8.
        params.mfx.FrameInfo.FourCC = MFX_FOURCC_P010;
        params.mfx.FrameInfo.BitDepthLuma = 10;
        params.mfx.FrameInfo.BitDepthChroma = 10;
        params.mfx.FrameInfo.Shift = 1;
        params.mfx.CodecProfile = MFX_PROFILE_HEVC_MAIN10;
    } else {
        params.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    }
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
                         mfxExtCodingOption2& option2, mfxExtCodingOption3& option3,
                         mfxExtVideoSignalInfo& signal, std::vector<mfxExtBuffer*>& buffers,
                         int fps, bool intraRefresh, const EncoderTuning& tuning, bool hdr)
{
    auto onOff = [](EncoderTuning::Choice c) {
        return c == EncoderTuning::Choice::On ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
    };

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

    const bool wantsOption2 = intraRefresh || tuning.vplMbBrc != EncoderTuning::Choice::Default ||
                              tuning.vplExtBrc != EncoderTuning::Choice::Default;
    if (wantsOption2) {
        std::memset(&option2, 0, sizeof(option2));
        option2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
        option2.Header.BufferSz = sizeof(option2);

        if (intraRefresh) {
            // Vertical: the wave sweeps by columns of macroblocks. Either axis
            // works; vertical is the conventional choice and matches what the
            // other two vendors do by default.
            option2.IntRefType = MFX_REFRESH_VERTICAL;
            option2.IntRefCycleSize = static_cast<mfxU16>(intraRefreshPeriodFrames(fps));
            // Leave the refreshed blocks at the frame's own quality: a positive
            // delta would make the healing band visibly coarser than what
            // surrounds it, which is precisely the artefact this avoids.
            option2.IntRefQPDelta = 0;
        }
        // Bench only: the engine leaves both to the runtime until the matrix
        // says otherwise.
        if (tuning.vplMbBrc != EncoderTuning::Choice::Default)
            option2.MBBRC = onOff(tuning.vplMbBrc);
        if (tuning.vplExtBrc != EncoderTuning::Choice::Default)
            option2.ExtBRC = onOff(tuning.vplExtBrc);

        buffers.push_back(reinterpret_cast<mfxExtBuffer*>(&option2));
    }

    const bool wantsOption3 = tuning.vplLowDelayBrc != EncoderTuning::Choice::Default ||
                              tuning.vplGamingScenario != EncoderTuning::Choice::Default ||
                              tuning.vplWinBrcFrames > 0;
    if (wantsOption3) {
        std::memset(&option3, 0, sizeof(option3));
        option3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
        option3.Header.BufferSz = sizeof(option3);

        if (tuning.vplLowDelayBrc != EncoderTuning::Choice::Default)
            option3.LowDelayBRC = onOff(tuning.vplLowDelayBrc);
        if (tuning.vplGamingScenario == EncoderTuning::Choice::On)
            option3.ScenarioInfo = MFX_SCENARIO_REMOTE_GAMING;
        if (tuning.vplWinBrcFrames > 0) {
            option3.WinBRCSize = static_cast<mfxU16>(tuning.vplWinBrcFrames);
            // The window's cap is the stream's own rate: the point of a window
            // is that a burst inside it is paid back before the window closes,
            // not that the average moves.
            option3.WinBRCMaxAvgKbps = params.mfx.TargetKbps;
        }

        buffers.push_back(reinterpret_cast<mfxExtBuffer*>(&option3));
    }

    if (hdr) {
        std::memset(&signal, 0, sizeof(signal));
        signal.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
        signal.Header.BufferSz = sizeof(signal);

        // ⚠️ Three integers, and without them HDR is not an error — it is a
        // washed-out grey picture that reads as a shader bug. A decoder told
        // nothing assumes BT.709 with an sRGB curve and runs it on PQ samples.
        // Same reasoning, same values, as the NVENC and AMF paths.
        signal.VideoFormat = 5;    // unspecified, as every path here writes
        signal.VideoFullRange = 0; // limited, which is what the shader produces
        signal.ColourDescriptionPresent = 1;
        signal.ColourPrimaries = 9;          // BT.2020
        signal.TransferCharacteristics = 16; // SMPTE ST 2084 (PQ)
        signal.MatrixCoefficients = 9;       // BT.2020 non-constant luminance

        buffers.push_back(reinterpret_cast<mfxExtBuffer*>(&signal));
    }

    params.ExtParam = buffers.data();
    params.NumExtParam = static_cast<mfxU16>(buffers.size());
}

} // namespace mw::native::encode
