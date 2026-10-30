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

#pragma once

#include "mw/native/Capabilities.h"
#include "mw/native/EncoderTuning.h"
#include "../../../capture/windows/IWindowsCapture.h"
#include "../../../convert/CursorDraw.h"
#include "../../../convert/ScaleFilter.h"
#include "../../../encode/EncoderOutput.h"

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <string>

namespace mw::native {

/// Everything between the captured picture and the bitstream, on Windows: the
/// colour conversion, the encoder, and whatever carries a frame from one to the
/// other — the cross-GPU bridge today, a D3D12 pipeline tomorrow (design §32).
///
/// ── Why a seam here ─────────────────────────────────────────────────────────
///
/// The session owns what is the same whichever way a picture is encoded: the
/// capture, the loop and its cadence, the rate governor, the still-screen
/// boost and the load cap. What differs is how a picture becomes a bitstream,
/// and that is all this interface is. The D3D11 path came first and was moved
/// behind it without a change of behaviour — same calls, same order, same log
/// lines — so that a second pipeline could be written beside it rather than
/// threaded through the session.
///
/// ── Lifetime ────────────────────────────────────────────────────────────────
///
/// open() once per session, before the first build: whatever survives capture
/// restarts (the bridge's device). Then, on every (re)build — session start, a
/// capture that came back on a new device, the load cap — teardown(),
/// raisePriority(), buildConverter() and buildEncoder(), in that order; the
/// session speaks between the last two (tone map, SDR white), which is why the
/// build comes in two halves. close() at stop().
///
/// Capture thread only, like everything the loop touches.
class WindowsVideoPipeline
{
public:
    /// What an encode came to. D3D11 is never Lost: an error there ends the
    /// session, as it always did. A D3D12 pipeline answers Lost when the
    /// device went away, and the session then rebuilds on D3D11 (§32).
    enum class EncodeResult
    {
        Ok,
        Failed,
        Lost,
    };

    /// The converter half of a build.
    struct ConverterBuild
    {
        /// The size the client decodes at; 0 follows the desktop.
        int outputWidth = 0;
        int outputHeight = 0;
        bool yuv444 = false;
        /// P010 BT.2020 PQ out. Only ever true over an FP16 capture: the
        /// session reconciles it with the capture's format before building.
        bool hdr = false;
        convert::ScaleFilter filter = convert::ScaleFilter::Bilinear;
        /// Take MWSR's statistics of every picture (SessionConfig::mwsrZones),
        /// where the chain has the pass and the picture qualifies.
        bool mwsrStats = false;
    };

    /// The encoder half of a build.
    struct EncoderBuild
    {
        /// The Selector's choice — never re-derived from the display.
        EncoderApi encoder = EncoderApi::None;
        Codec codec = Codec::H264;
        int fps = 0;
        int bitrateKbps = 0;
        bool yuv444 = false;
        bool hdr = false;
        bool intraRefresh = false;
        EncoderTuning tuning;
        /// The D3D12 route's encoder, as the choice resolved it
        /// (VideoPipelineChoice::encoder12); D3D11 reads nothing of it.
        EncoderTuning::Encoder12 encoder12 = EncoderTuning::Encoder12::VideoEncode;
    };

    virtual ~WindowsVideoPipeline() = default;

    WindowsVideoPipeline(const WindowsVideoPipeline&) = delete;
    WindowsVideoPipeline& operator=(const WindowsVideoPipeline&) = delete;

    /// "d3d11" — for the log and the bench.
    virtual const char* kind() const = 0;

    /// Once per session, before the first build. @p crossGpuCopy asks for the
    /// frames to be carried to the adapter @p encodeAdapterLuid (named
    /// @p encodeGpuName in the log) — see CrossGpuBridge.
    virtual bool open(bool crossGpuCopy, uint64_t encodeAdapterLuid,
                      const std::string& encodeGpuName, std::string& error) = 0;

    /// Release the converter and the encoder before their replacements are
    /// built — never after: an encoder holds a hardware session, and a
    /// consumer GPU has famously few. @p keepHeld keeps the held desktop (the
    /// load cap rebuilds on the same capture, whose picture is still good).
    virtual void teardown(bool keepHeld) = 0;

    /// Ahead of the game on the GPU, on every (re)build: the devices this
    /// pipeline owns (the capture's is the session's). See StreamPriority.
    virtual void raisePriority() = 0;

    /// Both halves are built against @p capture as it stands: its device, its
    /// size, its format.
    virtual bool buildConverter(const capture::IWindowsCapture& capture,
                                const ConverterBuild& build, std::string& error) = 0;
    virtual bool buildEncoder(const capture::IWindowsCapture& capture, const EncoderBuild& build,
                              std::string& error) = 0;

    /// Everything, the bridge included, in the order stop() always released
    /// it: encoder, converter, held desktop, bridge.
    virtual void close() = 0;

    virtual bool hasEncoder() const = 0;
    /// Both halves are there: a picture can be converted and encoded.
    virtual bool built() const = 0;

    // ── Pictures ───────────────────────────────────────────────────────────

    /// Convert a freshly captured texture, the pointer drawn in. @p retain also
    /// keeps a copy of the desktop for the pointer-only and still-screen paths
    /// (a failure there is logged, not fatal). Without it, the copy an earlier
    /// conversion kept is let go: it is a desktop older than the picture the
    /// client has, and the pointer redrawn over it would bring that back. False
    /// when the conversion itself failed, with @p error set.
    virtual bool convert(capture::IWindowsCapture& capture, ID3D11Texture2D* captured,
                         const capture::CursorState& cursor, const convert::CursorDraw& draw,
                         bool retain, std::string& error) = 0;

    /// Whether a copy of the desktop as last converted is held — see convert().
    virtual bool hasHeld() const = 0;

    /// Convert the held desktop again, with @p cursor where it is now.
    virtual bool convertHeld(capture::IWindowsCapture& capture, const capture::CursorState& cursor,
                             const convert::CursorDraw& draw, std::string& error) = 0;

    /// Make the black picture a restart sends while the display is away: the
    /// size and format @p capture was delivering, made NOW, before the capture
    /// is reopened on a new device. False, with @p error set, when it could
    /// not be made — no capture, or the GPU refused — and the last picture is
    /// re-sent instead.
    virtual bool prepareBlank(const capture::IWindowsCapture* capture, std::string& error) = 0;
    virtual bool hasBlank() const = 0;
    /// Convert the black picture — no pointer, no capture involved.
    virtual bool convertBlank(const convert::CursorDraw& draw, std::string& error) = 0;
    virtual void releaseBlank() = 0;

    /// Called right before the captured frame is given back to the capture.
    /// Nothing on D3D11; a D3D12 pipeline orders the release after its reads
    /// there.
    virtual void beforeCaptureRelease() = 0;

    /// Before IWindowsCapture::retake(): let go of every captured surface kept
    /// — a view of the last one, the surfaces opened in another API — once
    /// nothing reads them any more. The capture's device and what was built
    /// on it stay; the held copy is the pipeline's own, and stays too.
    virtual void forgetCaptured() = 0;

    // ── The encoder ────────────────────────────────────────────────────────

    /// Encode what the converter holds. Blocking: the bitstream is ready.
    virtual EncodeResult encode(bool forceKeyframe, uint32_t frameNumber,
                                encode::EncoderOutput& out, std::string& error) = 0;
    /// Before the next encode().
    virtual void releaseOutput() = 0;
    virtual bool setBitrate(int bitrateKbps, std::string& error) = 0;
    virtual bool intraRefreshEnabled() const = 0;
    virtual int intraRefreshHorizonFrames() const = 0;
    virtual bool supportsReferenceInvalidation() const = 0;
    virtual bool invalidateReference(uint32_t frameNumber, std::string& error) = 0;

    // ── Two pictures in flight (plan Phase 10, pipelined=1) ────────────────
    //
    // The loop above encodes one picture at a time: the capture thread waits
    // for the bitstream before it acquires the next present, so where the
    // conversion and the encode together take more than the frame interval,
    // presents are missed. Pipelined, the encode and the delivery run on a
    // thread of their own, and the capture thread converts the next picture
    // meanwhile: two outputs, at most two pictures in flight — one encoding,
    // one converted and waiting — and a waiting picture is dropped for a newer
    // one rather than queued. A bench key: nothing in the product asks for it.

    /// Encodes the picture a job was posted for: encode(), for the picture
    /// converted when the job was posted rather than the latest.
    using EncodePicture = std::function<EncodeResult(
        bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out, std::string& error)>;
    /// A picture's encode and delivery, run on the encode thread — which owns
    /// the encoder while it runs: invalidateReference() and releaseOutput()
    /// are the job's to call there, as emit() calls them on the capture
    /// thread.
    using EncodeJob = std::function<void(const EncodePicture& encode)>;

    /// Whether this build runs pipelined: two outputs and the encode thread.
    virtual bool pipelined() const { return false; }
    /// Hand the picture the last conversion wrote to the encode thread, which
    /// runs @p job when it gets to it — at once when it is idle. A job still
    /// waiting there is dropped for this one: the newest picture wins.
    virtual void encodeLater(EncodeJob job) { (void)job; }
    /// Wait until the encode thread holds no picture — before the capture
    /// thread uses the encoder itself (a re-send, a rebuild).
    virtual void settle() {}

    // ── Output and colour ──────────────────────────────────────────────────

    /// The size the converter writes; 0 before the first build.
    virtual int outputWidth() const = 0;
    virtual int outputHeight() const = 0;
    /// Memory copies between capture and the wire (SessionInfo::copiesPerFrame).
    virtual int copiesPerFrame() const = 0;
    virtual bool toneMapsToSdr() const = 0;
    virtual bool scRgbSource() const = 0;
    virtual void setSdrWhite(float scRgbWhite) = 0;

    // ── The resample guard ─────────────────────────────────────────────────

    virtual convert::ScaleFilter scaleFilter() const = 0;
    virtual bool takeResampleCost(int64_t& costUs) = 0;
    virtual bool dropResample() = 0;

    // ── MWSR's sharpness by zone (plan B §12 c) ────────────────────────────

    /// The zones' codes of the picture last converted, decided from its
    /// statistics: @p codes takes up to mwsrzones::kZones of them, row by row
    /// on a grid of @p zonesX × @p zonesY (the full grid, or the light one on
    /// a GPU the full pass costs too much), @p scale the upscale the decision
    /// assumed. False when there are none — no pass on this chain (D3D12),
    /// none asked, a picture that does not qualify, a GPU neither pass fits,
    /// or statistics the GPU has not finished: the frame goes without a grid.
    virtual bool takeMwsrZones(uint8_t* codes, int& zonesX, int& zonesY, float& scale)
    {
        (void)codes;
        (void)zonesX;
        (void)zonesY;
        (void)scale;
        return false;
    }

    /// The pipeline's own end-of-session figures, if it has any.
    virtual void logEndOfSession() const = 0;

    // ── The bench's trace of a conversion (ClickTrace's frame rows) ─────────

    /// When the CPU steps of the last conversion ended, on the engine's
    /// steady clock: the capture context's signal flushed (0 for a held
    /// picture converted again, which signals nothing), the list ready to
    /// record (a conversion the cadence skipped waited for first), the list
    /// recorded, the list executed and the fences signalled. What a busy GPU
    /// stretches on the N95 (plan « Gels du lien », 21/10/2026).
    struct ConvertTrace
    {
        int64_t signalledUs = 0;
        int64_t listUs = 0;
        int64_t recordedUs = 0;
        int64_t executedUs = 0;
    };

    /// The last conversion's steps; false where the pipeline keeps none
    /// (D3D11), and the session traces its own stamps alone.
    virtual bool lastConvertTrace(ConvertTrace& out) const
    {
        (void)out;
        return false;
    }

protected:
    WindowsVideoPipeline() = default;
};

} // namespace mw::native
