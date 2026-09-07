/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "VplEncoder.h"

#include "../../core/Log.h"
#include "../RateControl.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace mw::native::encode {
namespace {

/// How long to wait for one frame, in milliseconds. A deadline, not a schedule:
/// encoding takes a few milliseconds, and reaching this means the encoder has
/// stopped answering.
constexpr mfxU32 kSyncTimeoutMs = 100;

/// How many times to retry while the device reports itself busy.
///
/// MFX_WRN_DEVICE_BUSY is oneVPL's documented "ask again shortly", and it is
/// expected under load rather than exceptional. The wait between attempts is a
/// yield, not a sleep: on Windows the default timer granularity is 15.6 ms, so
/// a "short" sleep here would cost more than the encode — the mistake that made
/// the AMD path read 15.66 ms a frame until it was found.
constexpr int kBusyRetries = 200;

/// How many times to ask again when the deadline above passes with the frame
/// still in flight.
///
/// ⚠️ Was ten — one second — on the reasoning that no working frame takes that
/// long. Measured otherwise on the N95: with a 1440p60 clip playing on the host
/// AND the browser decoding on the same four cores, single frames went past a
/// second, then past three, and the session died mid-benchmark each time. A
/// stream producing one frame a second is unusable, but ENDING it is worse than
/// being slow — the cadence and the link governor are there to degrade it
/// gracefully, and the viewer can lower the resolution. A hundred attempts is
/// ten seconds: nothing a working encoder ever needs, and still a bound, so an
/// encoder that has genuinely stopped is reported rather than waited on for
/// ever.
constexpr int kSyncAttempts = 100;

/// Ask the runtime, in the way its own header prescribes, whether it will take
/// a reference-list control block at all.
///
/// Query mode 1: attach the buffer with the fields of interest set, and see
/// whether they survive. A runtime without long-term references either fails
/// the call or zeroes them — either way the answer is no, and the session then
/// runs exactly as it did before, healing losses with keyframes.
bool longTermReferencesWork(const VplApi& api, mfxSession session, const mfxVideoParam& base)
{
    mfxExtAVCRefListCtrl probe = {};
    probe.Header.BufferId = MFX_EXTBUFF_AVC_REFLIST_CTRL;
    probe.Header.BufferSz = sizeof(probe);
    probe.ApplyLongTermIdx = 1;
    probe.LongTermRefList[0].FrameOrder = 0;
    probe.LongTermRefList[0].LongTermIdx = 1;

    mfxExtBuffer* chain[1] = {reinterpret_cast<mfxExtBuffer*>(&probe)};

    mfxVideoParam in = base;
    in.ExtParam = chain;
    in.NumExtParam = 1;

    mfxExtAVCRefListCtrl outCtrl = probe;
    mfxExtBuffer* outChain[1] = {reinterpret_cast<mfxExtBuffer*>(&outCtrl)};
    mfxVideoParam out = base;
    out.ExtParam = outChain;
    out.NumExtParam = 1;

    const mfxStatus status = api.EncodeQuery(session, &in, &out);
    if (status != MFX_ERR_NONE && status != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM &&
        status != MFX_WRN_VIDEO_PARAM_CHANGED)
        return false;
    return outCtrl.ApplyLongTermIdx != 0;
}

} // namespace

VplEncoder::~VplEncoder()
{
    stop();
}

bool VplEncoder::init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                      int bitrateKbps, bool yuv444, bool hdr, bool intraRefresh,
                      const EncoderTuning& tuning, std::string& error)
{
    stop();

    if (!device || width <= 0 || height <= 0) {
        error = "invalid encoder parameters";
        return false;
    }
    if (yuv444) {
        // The capability query does not claim 4:4:4 either, so the Selector
        // should never route such a session here. Refusing loudly beats
        // encoding 4:2:0 while the overlay says 4:4:4.
        error = "4:4:4 is not implemented on the Intel encoder path";
        return false;
    }
    if (hdr) {
        // Same shape as 4:4:4: the silicon has 10-bit, this path does not, and
        // the capability query is made to say so (see VplCapabilities.cpp).
        // ⚠️ 07/09/2026: the 8-bit path is now verified on real Intel hardware
        // (N95 / UHD Graphics), but nothing has ever run a P010 frame through
        // it — claiming HDR here would be exactly the kind of unwatched colour
        // pipeline that makes a stream subtly wrong for a year.
        error = "HDR is not implemented on the Intel encoder path";
        return false;
    }

    m_Codec = codec;
    m_Width = width;
    m_Height = height;
    m_Fps = fps > 0 ? fps : 60;
    m_Tuning = tuning;
    m_CeilingSeen = false;
    m_SlowFrameSeen = false;

    if (!m_Session.open(device, error)) return false;

    if (!fillEncodeParams(m_Params, codec, width, height, m_Fps, bitrateKbps, m_Tuning)) {
        error = "no oneVPL codec for this format";
        stop();
        return false;
    }

    // Query the BASE parameters, before any extension buffer is attached.
    //
    // Query's output block would otherwise need an extension chain of its own:
    // copying m_Params wholesale hands it our buffer as both input and output,
    // which some runtimes accept and others do not. Validating the plain
    // parameters and letting Init judge the extension is unambiguous.
    auto accepted = [](mfxStatus s) {
        return s == MFX_ERR_NONE || s == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM ||
               s == MFX_WRN_VIDEO_PARAM_CHANGED;
    };

    mfxVideoParam corrected = m_Params;
    mfxStatus queried = m_Session.api()->EncodeQuery(m_Session.handle(), &m_Params, &corrected);
    if (!accepted(queried) && m_Params.mfx.LowPower == MFX_CODINGOPTION_ON) {
        // No fixed-function engine for this codec on this generation. The
        // shader-based one still encodes, more slowly; refusing the session
        // over it would be worse than the extra milliseconds.
        log::info("[native] oneVPL: no low-power engine for " + std::string(toString(codec)) +
                  " here (" + VplApi::statusToString(queried) + ") — using the general one");
        m_Params.mfx.LowPower = MFX_CODINGOPTION_OFF;
        corrected = m_Params;
        queried = m_Session.api()->EncodeQuery(m_Session.handle(), &m_Params, &corrected);
    }
    if (accepted(queried)) {
        m_Params = corrected;
    } else {
        error = std::string("this GPU cannot encode ") + toString(codec) + ": " +
                VplApi::statusToString(queried);
        stop();
        return false;
    }

    m_IntraRefresh = false;
    attachEncodeOptions(m_Params, m_CodingOption, m_CodingOption2, m_CodingOption3, m_ExtBuffers,
                        m_Fps, intraRefresh, m_Tuning);

    mfxStatus started = m_Session.api()->EncodeInit(m_Session.handle(), &m_Params);

    if (intraRefresh && accepted(started)) {
        m_IntraRefresh = true;
    } else if (intraRefresh) {
        // Intra-refresh is an optimisation, not a requirement. A generation
        // that refuses it must still stream — falling back to keyframes costs
        // the receiver its self-repair, not its picture. The rest of the chain
        // stays: without it the bitrate could not move.
        log::warning(std::string("[native] oneVPL declined intra-refresh (") +
                     VplApi::statusToString(started) + ") — falling back to keyframes");
        attachEncodeOptions(m_Params, m_CodingOption, m_CodingOption2, m_CodingOption3,
                            m_ExtBuffers, m_Fps, false, m_Tuning);
        started = m_Session.api()->EncodeInit(m_Session.handle(), &m_Params);
    }

    if (!accepted(started)) {
        error = std::string("could not initialize the Intel encoder: ") +
                VplApi::statusToString(started);
        stop();
        return false;
    }

    // ── What the encoder actually settled on ────────────────────────────────
    //
    // Read back rather than assumed, for two reasons. The compressed buffer has
    // to be big enough for the largest keyframe, and guessing that fails at the
    // worst possible moment. And Reset — which the link governor calls about
    // twice a second — refuses a block that differs from the running
    // configuration in any field it considers static, including the ones the
    // runtime filled in for itself at Init (profile, level, reference count).
    // Rebuilding Reset's input from our pre-Init block is what had every rate
    // change on the first Intel machine answered with -14.
    mfxVideoParam actual = {};
    actual.mfx.CodecId = m_Params.mfx.CodecId;
    if (!m_ExtBuffers.empty()) {
        actual.ExtParam = m_ExtBuffers.data();
        actual.NumExtParam = static_cast<mfxU16>(m_ExtBuffers.size());
    }
    if (m_Session.api()->EncodeGetVideoParam(m_Session.handle(), &actual) == MFX_ERR_NONE) {
        if (actual.mfx.BufferSizeInKB > 0) {
            const mfxU32 multiplier =
                actual.mfx.BRCParamMultiplier > 0 ? actual.mfx.BRCParamMultiplier : 1;
            m_BitstreamData.resize(static_cast<size_t>(actual.mfx.BufferSizeInKB) * multiplier *
                                   1024);
        }
        // ExtParam still points into m_ExtBuffers, a member that outlives this.
        m_Params = actual;
    }
    // A floor regardless: some runtimes report a buffer sized for the average
    // frame, and a keyframe is several times that.
    if (m_BitstreamData.size() < 4u * 1024u * 1024u) m_BitstreamData.resize(4u * 1024u * 1024u);

    std::memset(&m_Bitstream, 0, sizeof(m_Bitstream));
    m_Bitstream.Data = m_BitstreamData.data();
    m_Bitstream.MaxLength = static_cast<mfxU32>(m_BitstreamData.size());

    // The highest budget Reset will accept — set by the buffer init() sized.
    m_CeilingKbps = budgetCeilingKbps(bitrateKbps > 0 ? bitrateKbps : 20000, m_Fps);

    // ── Reference invalidation ──────────────────────────────────────────────
    //
    // The pictures kept to predict from, minus the one the codec always needs
    // for the ordinary delta: those are the ones a repair can fall back on.
    // Asked of the runtime rather than assumed, and answered honestly to
    // /start: a receiver told the stream repairs itself will stop asking for
    // keyframes, so promising it wrongly is worse than not promising it.
    m_RepairPending = false;
    const int refs = static_cast<int>(m_Params.mfx.NumRefFrame);
    const int slots = refs > 1 ? refs - 1 : 0;
    const bool longTerm =
        slots > 0 && longTermReferencesWork(*m_Session.api(), m_Session.handle(), m_Params);
    m_Slots = ReferenceSlots(longTerm ? slots : 0, ReferenceSlots::strideFor(m_Fps, slots));

    const std::string overrides = tuning.describe();
    log::info(
        "[native] oneVPL ready: " + std::to_string(width) + "x" + std::to_string(height) + "@" +
        std::to_string(m_Fps) + " " + toString(codec) + " 4:2:0 CBR " +
        std::to_string(bitrateKbps) + " kbps, VBV " +
        std::to_string(m_Params.mfx.BufferSizeInKB * m_Params.mfx.BRCParamMultiplier) + " KB" +
        (m_IntraRefresh
             ? ", intra-refresh over " + std::to_string(intraRefreshPeriodFrames(m_Fps)) + " frames"
             : ", keyframes on demand") +
        ", TU" + std::to_string(m_Params.mfx.TargetUsage) +
        (m_Slots.enabled()
             ? ", " + std::to_string(m_Slots.count()) + " long-term references every " +
                   std::to_string(m_Slots.stride()) + " frames (reach " +
                   std::to_string(m_Slots.reachFrames()) + " frames)"
             : ", no reference invalidation") +
        (overrides.empty() ? "" : " [bench: " + overrides + "]"));
    return true;
}

bool VplEncoder::invalidateReference(uint32_t frameNumber, std::string& error)
{
    if (!m_Session.isOpen() || !m_Slots.enabled()) {
        error = "reference invalidation is not available on this encoder";
        return false;
    }

    // Every picture from the lost one on predicts, directly or not, from it:
    // the repair has to reach further back than the loss, never merely past it.
    const int clean = m_Slots.cleanSlotBefore(frameNumber);
    if (clean < 0) {
        error = "the loss is older than every long-term reference this encoder holds";
        return false;
    }

    m_RepairPending = true;
    m_RepairLost = frameNumber;
    m_RepairFrom = m_Slots.frameAt(clean);
    m_Slots.dropFrom(frameNumber);
    return true;
}

bool VplEncoder::encode(ID3D11Texture2D* surface, bool forceKeyframe, uint32_t frameNumber,
                        EncoderOutput& out, std::string& error)
{
    if (!m_Session.isOpen()) {
        error = "the encoder is not initialized";
        return false;
    }
    if (m_OutputHeld) {
        error = "the previous frame was not released";
        return false;
    }
    if (!surface) {
        error = "no input surface";
        return false;
    }

    // The zero-copy hand-off: the texture our conversion pass wrote, named by
    // handle. Second element is the subresource index — 0, since the conversion
    // output is a plain non-array texture.
    mfxHDLPair handles = {};
    handles.first = static_cast<mfxHDL>(surface);
    handles.second = nullptr;

    mfxFrameSurface1 input = {};
    input.Info = m_Params.mfx.FrameInfo;
    input.Data.MemId = static_cast<mfxMemId>(&handles);
    // The name the receiver will use if this picture never arrives. oneVPL's
    // reference lists are written in FrameOrder, so this one field is what makes
    // "frame N was lost" a sentence the encoder understands.
    input.Data.FrameOrder = frameNumber;

    mfxEncodeCtrl ctrl = {};
    mfxEncodeCtrl* ctrlPtr = nullptr;
    if (forceKeyframe) {
        ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
        ctrlPtr = &ctrl;
    }

    // ── The reference list this picture is encoded against ──────────────────
    //
    // Two independent things go in the same block: marking THIS picture as a
    // long-term reference when its turn comes round, and — after a loss — both
    // refusing the pictures the loss spoiled and naming the one to predict from
    // instead. A repair frame does both: it must itself become a reference, or
    // the next loss would have nothing recent to fall back on.
    const int markSlot = m_Slots.enabled() ? m_Slots.slotFor(frameNumber, forceKeyframe) : -1;
    if (markSlot >= 0 || m_RepairPending) {
        std::memset(&m_RefCtrl, 0, sizeof(m_RefCtrl));
        m_RefCtrl.Header.BufferId = MFX_EXTBUFF_AVC_REFLIST_CTRL;
        m_RefCtrl.Header.BufferSz = sizeof(m_RefCtrl);
        // Every unused entry has to say so explicitly: a zeroed FrameOrder is a
        // valid picture number, not an empty slot.
        for (auto& e : m_RefCtrl.PreferredRefList)
            e.FrameOrder = MFX_FRAMEORDER_UNKNOWN;
        for (auto& e : m_RefCtrl.RejectedRefList)
            e.FrameOrder = MFX_FRAMEORDER_UNKNOWN;
        for (auto& e : m_RefCtrl.LongTermRefList)
            e.FrameOrder = MFX_FRAMEORDER_UNKNOWN;

        if (markSlot >= 0) {
            m_RefCtrl.LongTermRefList[0].FrameOrder = frameNumber;
            m_RefCtrl.LongTermRefList[0].LongTermIdx = static_cast<mfxU16>(markSlot);
            m_RefCtrl.ApplyLongTermIdx = 1;
        }
        if (m_RepairPending) {
            m_RefCtrl.PreferredRefList[0].FrameOrder = m_RepairFrom;
            m_RefCtrl.RejectedRefList[0].FrameOrder = m_RepairLost;
            // One reference, and it is the named one: a list of preferences the
            // encoder may ignore would leave it free to predict from a picture
            // the receiver never got, which is the whole failure being repaired.
            m_RefCtrl.NumRefIdxL0Active = 1;
        }

        m_CtrlBuffers[0] = reinterpret_cast<mfxExtBuffer*>(&m_RefCtrl);
        ctrl.ExtParam = m_CtrlBuffers;
        ctrl.NumExtParam = 1;
        ctrlPtr = &ctrl;
    }

    m_Bitstream.DataOffset = 0;
    m_Bitstream.DataLength = 0;

    mfxSyncPoint sync = nullptr;
    mfxStatus status = MFX_ERR_NONE;
    for (int attempt = 0; attempt < kBusyRetries; ++attempt) {
        status = m_Session.api()->EncodeFrameAsync(m_Session.handle(), ctrlPtr, &input,
                                                   &m_Bitstream, &sync);
        if (status == MFX_WRN_DEVICE_BUSY) {
            std::this_thread::yield();
            continue;
        }
        break;
    }

    if (status == MFX_ERR_MORE_DATA) {
        // The encoder swallowed the frame and wants another before it emits
        // anything. With AsyncDepth 1 and no B-frames this should not happen —
        // saying so is more useful than pretending a frame was produced.
        error = "the Intel encoder asked for more input than this pipeline provides";
        return false;
    }
    if (status != MFX_ERR_NONE || !sync) {
        error = std::string("encode failed: ") + VplApi::statusToString(status);
        return false;
    }

    // ⚠️ A timeout is not a failure, and treating it as one killed the first
    // real Intel session this engine ever ran.
    //
    // SyncOperation answers MFX_WRN_IN_EXECUTION when its deadline passes with
    // the frame still in the encoder — "ask again", exactly like
    // MFX_WRN_DEVICE_BUSY above. On an N95 with the browser decoding on the
    // same four cores, one frame in a few hundred took longer than 100 ms, and
    // the session ended at that frame with "still executing (1)" after twelve
    // frames. The deadline stays short so a genuinely dead encoder is still
    // caught quickly; what changes is that a slow frame gets asked about again.
    mfxStatus synced = MFX_WRN_IN_EXECUTION;
    for (int attempt = 0; attempt < kSyncAttempts; ++attempt) {
        synced = m_Session.api()->SyncOperation(m_Session.handle(), sync, kSyncTimeoutMs);
        if (synced != MFX_WRN_IN_EXECUTION && synced != MFX_WRN_DEVICE_BUSY) break;
        if (attempt == 0 && !m_SlowFrameSeen) {
            m_SlowFrameSeen = true;
            log::warning("[native] oneVPL: a frame took longer than " +
                         std::to_string(kSyncTimeoutMs) +
                         " ms to encode — this GPU is at its limit for this resolution");
        }
    }
    if (synced != MFX_ERR_NONE) {
        error =
            std::string("waiting for the encoded frame failed: ") + VplApi::statusToString(synced);
        return false;
    }

    m_OutputHeld = true;
    out.data = m_Bitstream.Data + m_Bitstream.DataOffset;
    out.size = m_Bitstream.DataLength;
    out.keyframe = (m_Bitstream.FrameType & (MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR)) != 0;

    // Recorded only once the picture exists. A keyframe empties the decoded
    // picture buffer, so every long-term reference goes with it — including the
    // one a repair in flight was about to predict from.
    if (out.keyframe) {
        m_Slots.clear();
        m_RepairPending = false;
    }
    if (markSlot >= 0) m_Slots.marked(markSlot, frameNumber);
    if (m_RepairPending) {
        log::info("[native] oneVPL healed frame " + std::to_string(m_RepairLost) +
                  " with a delta against frame " + std::to_string(m_RepairFrom));
        m_RepairPending = false;
    }
    return true;
}

void VplEncoder::releaseOutput()
{
    if (!m_OutputHeld) return;
    // Nothing to unlock: the buffer is ours. Resetting the fill marks it free
    // for the next frame, and clearing the flag is what lets encode() run again.
    m_Bitstream.DataOffset = 0;
    m_Bitstream.DataLength = 0;
    m_OutputHeld = false;
}

bool VplEncoder::setBitrate(int bitrateKbps, std::string& error)
{
    if (!m_Session.isOpen() || bitrateKbps <= 0) {
        error = "the encoder is not initialized";
        return false;
    }

    // ⚠️ The block is MUTATED, never rebuilt, and that is the whole fix.
    //
    // Rebuilding it with fillEncodeParams — which is what this did — threw away
    // two things nobody would notice going:
    //
    //   · the extension chain, so intra-refresh silently stopped, while
    //     intraRefreshEnabled() went on answering true from the flag init()
    //     set. The receiver is then told the stream repairs itself, rides out a
    //     loss waiting for a wave that will never come, and gives up on G2's
    //     15 s guard.
    //   · the runtime's own corrections, applied by EncodeQuery at init() and
    //     absent from any freshly built block.
    //
    // And it runs constantly: the link governor changes the bitrate about twice
    // a second on a moving link, so intra-refresh survived roughly half a
    // second of real streaming. Same shape as bug B4 on AMF — a mid-session
    // re-application that quietly drops what init() had settled.
    // ⚠️ Upwards, this encoder only moves as far as Init told it it could.
    //
    // MFXVideoENCODE_Reset refuses (-14, "requires additional memory
    // allocation") any target above what Init was sized for, and it refuses the
    // WHOLE call: the rate stays where it was. Measured on an N95, where the
    // effective-cadence budget (E4) asked for 32000 on a stream set to 20000
    // because the desktop was only moving at half the stream's rate — twice a
    // second, each one refused with a warning.
    //
    // The answer is not to cap the request but to buy the room up front:
    // init() sizes the bitstream buffer for budgetCeilingKbps(), which is what
    // Reset validates a raise against. This is the last line of defence — a
    // request beyond even that ceiling is capped rather than lost, because a
    // refused Reset would leave the rate exactly where it was.
    int wanted = bitrateKbps;
    if (wanted > m_CeilingKbps) {
        if (!m_CeilingSeen) {
            m_CeilingSeen = true;
            log::info("[native] oneVPL: the per-frame budget is capped at " +
                      std::to_string(m_CeilingKbps) +
                      " kbps, twice the stream's rate — see "
                      "budgetCeilingKbps for what the rest costs");
        }
        wanted = m_CeilingKbps;
        const int multiplier =
            m_Params.mfx.BRCParamMultiplier > 0 ? m_Params.mfx.BRCParamMultiplier : 1;
        if (static_cast<int>(m_Params.mfx.TargetKbps) * multiplier == wanted) return true;
    }

    mfxVideoParam params = m_Params;
    applyBitrateOnly(params, wanted);

    // Reset keeps the session and its surfaces; only the rate control changes.
    const mfxStatus status = m_Session.api()->EncodeReset(m_Session.handle(), &params);
    if (status != MFX_ERR_NONE && status != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM &&
        status != MFX_WRN_VIDEO_PARAM_CHANGED) {
        const int multiplier =
            m_Params.mfx.BRCParamMultiplier > 0 ? m_Params.mfx.BRCParamMultiplier : 1;
        error = "could not change the bitrate to " + std::to_string(bitrateKbps) + " kbps from " +
                std::to_string(static_cast<int>(m_Params.mfx.TargetKbps) * multiplier) +
                " (multiplier " + std::to_string(multiplier) + ", VBV " +
                std::to_string(static_cast<int>(m_Params.mfx.BufferSizeInKB) * multiplier) +
                " KB, " + std::to_string(m_Params.NumExtParam) +
                " ext): " + VplApi::statusToString(status);
        return false;
    }
    // ExtParam still points into m_ExtBuffers, a member that outlives this.
    m_Params = params;
    return true;
}

void VplEncoder::stop()
{
    m_Slots = ReferenceSlots();
    m_RepairPending = false;
    releaseOutput();
    if (m_Session.isOpen()) m_Session.api()->EncodeClose(m_Session.handle());
    m_Session.close();
    m_BitstreamData.clear();
    std::memset(&m_Bitstream, 0, sizeof(m_Bitstream));
}

} // namespace mw::native::encode
