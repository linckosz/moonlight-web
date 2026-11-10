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

#include "WindowsVideoPipeline.h"

#include "../../../convert/windows/d3d12/ColorConvert12.h"
#include "../../../core/D3d12Fault.h"
#include "../../../encode/windows/d3d12/IVideoEncoder12.h"
#include "../d3d12/D3d12Device.h"
#include "../d3d12/DdaInterop.h"

#include <wrl/client.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace mw::native {

/// The D3D12 chain (plan pipeline-video-d3d12-v2, §3.1). One picture, one
/// thread, no queue of frames:
///
///  1. the capture's D3D11 context signals fence A right after the acquire;
///  2. the conversion queue waits for it on the GPU, converts the surface
///     opened in D3D12 into the encoder's input, and signals fence B and the
///     pipeline's own fence;
///  3. the capture context waits for fence B on the GPU before ReleaseFrame,
///     so the compositor cannot write the surface under the read;
///  4. the encoder waits for the pipeline's fence on its own queue, and the
///     CPU waits for the bitstream — the frame's only CPU wait.
///
/// Anything that goes wrong once a session is streaming — a device removed,
/// a fence past its deadline, an encoder error, the header guard — makes
/// encode() answer Lost, and the session goes back to D3D11 for good. A
/// conversion that fails is kept for that answer rather than returned: the
/// session ends on a failed conversion, and D3D12 must never end a session
/// D3D11 could have carried.
///
/// MW_D3D12_FAULT makes each of those happen on demand, at a picture chosen
/// (D3d12Fault, plan C8.1): the way back to D3D11 is watched on the bench
/// rather than trusted.
///
/// ── Pipelined (pipelined=1, plan Phase 10) ──────────────────────────────────
///
/// The converter writes two outputs in turn and step 4 moves to a thread of
/// its own, which encodes and delivers while the capture thread converts the
/// next picture — into the output the encoder is not reading. A picture
/// converted while one is encoding and another waits replaces the waiting
/// one: at most two in flight, never a queue. D3D12 Video Encode only: the
/// vendors' SDKs register one input, and AMF hands it back through a fence of
/// its own.
class D3d12VideoPipeline final : public WindowsVideoPipeline
{
public:
    /// @p tuning: the bench's D3D12 knobs, fixed for the session (queues,
    /// handshake, GPU timing); the defaults are the engine's own. @p encoder12:
    /// the encoder the chain ends in (VideoPipelineChoice), which decides what
    /// open() asks of the device.
    D3d12VideoPipeline(const EncoderTuning& tuning, EncoderTuning::Encoder12 encoder12);
    ~D3d12VideoPipeline() override;

    const char* kind() const override { return "d3d12"; }
    EncoderTuning::Encoder12 encoder12() const { return m_Encoder12; }

    /// Why the GPU on @p encodeAdapterLuid has no D3D12 Video Encode, once a
    /// chain opened there has found out — Windows 10, a driver without one —
    /// and "" until then. Kept for the process, a worker being one session:
    /// the builds after the first take D3D11 from the start, rather than make
    /// a D3D12 chain to tear down each time Auto asks for one (Intel's line).
    static std::string videoEncodeMissing(uint64_t encodeAdapterLuid);

    /// @p crossGpuCopy is refused (the bridge is D3D11's); the device is the
    /// one on @p encodeAdapterLuid, which is then the display's. A chain that
    /// ends in D3D12 Video Encode refuses a device without it here, before
    /// anything is made on it; the vendors' SDKs need no such thing.
    bool open(bool crossGpuCopy, uint64_t encodeAdapterLuid, const std::string& encodeGpuName,
              std::string& error) override;
    void teardown(bool keepHeld) override;
    /// The queues were made at their priority in open(); the capture's device
    /// is the session's to raise.
    void raisePriority() override {}
    bool buildConverter(const capture::IWindowsCapture& capture, const ConverterBuild& build,
                        std::string& error) override;
    bool buildEncoder(const capture::IWindowsCapture& capture, const EncoderBuild& build,
                      std::string& error) override;
    void close() override;
    bool hasEncoder() const override { return m_Encoder != nullptr; }
    bool built() const override { return m_Converter && m_Encoder; }

    bool convert(capture::IWindowsCapture& capture, ID3D11Texture2D* captured,
                 const capture::CursorState& cursor, const convert::CursorDraw& draw, bool retain,
                 std::string& error) override;
    bool hasHeld() const override { return m_Converter && m_Converter->held() != nullptr; }
    bool convertHeld(capture::IWindowsCapture& capture, const capture::CursorState& cursor,
                     const convert::CursorDraw& draw, std::string& error) override;
    /// The black picture needs nothing from the capture: the converter clears
    /// its own output. Still false without a converter to clear.
    bool prepareBlank(const capture::IWindowsCapture* capture, std::string& error) override;
    bool hasBlank() const override { return m_BlankReady; }
    bool convertBlank(const convert::CursorDraw& draw, std::string& error) override;
    void releaseBlank() override { m_BlankReady = false; }
    void beforeCaptureRelease() override;
    void forgetCaptured() override;

    EncodeResult encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                        std::string& error) override;
    void releaseOutput() override
    {
        if (m_Encoder) m_Encoder->releaseOutput();
    }
    /// Pipelined, the target waits for the encode thread's next picture when
    /// it is busy: the encoder is its while a job runs.
    bool setBitrate(int bitrateKbps, std::string& error) override;

    bool pipelined() const override { return m_Pipelined; }
    void encodeLater(EncodeJob job) override;
    void settle() override;
    bool intraRefreshEnabled() const override
    {
        return m_Encoder && m_Encoder->intraRefreshEnabled();
    }
    int intraRefreshHorizonFrames() const override
    {
        return m_Encoder ? m_Encoder->intraRefreshHorizonFrames() : 0;
    }
    bool supportsReferenceInvalidation() const override
    {
        return m_Encoder && m_Encoder->supportsReferenceInvalidation();
    }
    bool invalidateReference(uint32_t frameNumber, std::string& error) override;

    int outputWidth() const override { return m_Converter ? m_Converter->outputWidth() : 0; }
    int outputHeight() const override { return m_Converter ? m_Converter->outputHeight() : 0; }
    /// The bitstream's read, as on D3D11: the surface is read in place.
    int copiesPerFrame() const override { return 1; }
    bool toneMapsToSdr() const override { return m_Converter && m_Converter->toneMapsToSdr(); }
    bool scRgbSource() const override { return m_Converter && m_Converter->scRgbSource(); }
    void setSdrWhite(float scRgbWhite) override;

    convert::ScaleFilter scaleFilter() const override;
    bool takeResampleCost(int64_t& costUs) override;
    bool dropResample() override;

    void logEndOfSession() const override;

    bool lastConvertTrace(ConvertTrace& out) const override
    {
        out = m_ConvertTrace;
        return true;
    }

    /// The converter's output, for the tests.
    ID3D12Resource* output() const { return m_Converter ? m_Converter->output() : nullptr; }

private:
    /// A converter at @p codedWidth × @p codedHeight, the held copy and the
    /// SDR white carried over from the one it replaces.
    bool makeConverter(int codedWidth, int codedHeight, std::string& error);
    /// A fresh list, its allocator reset: the previous one has completed (the
    /// encode that followed it waited for it).
    ID3D12GraphicsCommandList* beginList(std::string& error);
    /// Close and submit the list behind a wait for fence A (@p acquired, 0 for
    /// none), then signal fence B (captured pictures) and the pipeline's own.
    bool submit(uint64_t acquired, bool signalCapture, std::string& error);
    /// A failure kept for encode() to answer Lost with.
    void lose(const std::string& reason);
    /// The queue idle: nothing of ours still reads or writes a resource.
    void drain();

    // ── MW_D3D12_FAULT (D3d12Fault) ────────────────────────────────────────

    /// Counts a conversion, and says the fault armed for it: None but for
    /// the Nth.
    D3d12Fault::Kind nextConversion();
    /// What a conversion's fault does once its list is recorded: Convert
    /// fails it (false, with the reason); Timeout stalls its submission;
    /// Removed and Encode wait for the encode that follows.
    bool armFault(D3d12Fault::Kind fault, std::string& error);
    /// Timeout: the conversion queue waits on a fence only the CPU signals.
    bool stall(std::string& error);
    /// Timeout: the queue let go, once the chain has given up on it.
    void releaseStall();
    /// Removed: ID3D12Device5::RemoveDevice, as a TDR would, before the
    /// picture of @p conversion is encoded.
    void removeDevice(uint64_t conversion);

    // ── Pipelined ──────────────────────────────────────────────────────────

    /// A converted picture, as the encode that takes it needs to know it.
    struct Output
    {
        /// m_Converted's value once the conversion into it is done.
        uint64_t ready = 0;
        int timerSlot = -1;
        D3d12Fault::Kind fault = D3d12Fault::Kind::None;
        /// Which conversion it was (MW_D3D12_FAULT's count).
        uint64_t conversion = 0;
    };

    /// A picture's encode, from output @p output as @p picture says it was
    /// written: encode()'s body, on whichever thread owns the encoder. A
    /// failure is kept in @p lost rather than lose(): the encode thread may be
    /// the one failing. The conversion's timer slot is read or freed.
    EncodeResult encodeOutput(int output, const Output& picture, bool forceKeyframe,
                              uint32_t frameNumber, encode::EncoderOutput& out, std::string& lost,
                              std::string& error);
    /// The conversion just submitted into m_Target, for the encode that takes it.
    void noteConverted();
    /// The output the conversion about to be recorded writes: never the one
    /// encoding; the waiting one, dropped, when the other is encoding.
    int reserveOutput();
    /// A failure the encode thread kept, made the chain's.
    void absorbEncodeLoss();
    void startEncodeThread();
    void stopEncodeThread();
    void encodeLoop();

    EncoderTuning m_Tuning;
    EncoderTuning::Encoder12 m_Encoder12 = EncoderTuning::Encoder12::VideoEncode;
    std::shared_ptr<d3d12::D3d12Device> m_Device;
    d3d12::Queue m_Queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_Allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_List;
    /// Signalled after each conversion: what the encoder waits for.
    d3d12::GpuFence m_Converted;
    uint64_t m_ConvertedValue = 0;
    d3d12::DdaInterop m_Interop;
    d3d12::DdaSync m_Sync = d3d12::DdaSync::Gpu;
    /// Fence B's value for the picture being released, 0 when none.
    uint64_t m_ReleaseAfter = 0;
    ID3D11DeviceContext* m_CaptureContext = nullptr;

    d3d12::QueueTimer m_Timer;
    bool m_Timing = false;
    int m_TimerSlot = -1;
    /// The last conversion's CPU steps (lastConvertTrace).
    ConvertTrace m_ConvertTrace;

    ConverterBuild m_ConverterBuild;
    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    /// IWindowsCapture::turn, for every converter made on this build.
    int m_SourceTurn = 0;
    std::unique_ptr<convert::ColorConvert12> m_Converter;
    /// The held desktop between a teardown that keeps it and the next build.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_KeptHeld;
    std::unique_ptr<encode::IVideoEncoder12> m_Encoder;
    bool m_BlankReady = false;
    /// Why the chain cannot go on, once it cannot: encode() answers Lost.
    std::string m_Lost;

    // End-of-session figures.
    uint64_t m_Frames = 0;
    uint64_t m_TimedFrames = 0;
    int64_t m_GpuConvertUs = 0;

    // MW_D3D12_FAULT.
    D3d12Fault m_Fault;
    uint64_t m_Conversions = 0;
    bool m_StallNext = false;
    /// The fence a stalled conversion waits for, kept until the queue is
    /// drained.
    Microsoft::WRL::ComPtr<ID3D12Fence> m_Stall;
    /// Removed or Encode, for the next encode().
    D3d12Fault::Kind m_EncodeFault = D3d12Fault::Kind::None;

    // Pipelined. The capture thread records conversions; the encode thread
    // runs jobs; m_JobLock guards what the two hand each other.
    bool m_Pipelined = false;
    Output m_Outputs[2];
    /// The output the last conversion wrote: what encode() re-sends.
    int m_Latest = 0;
    /// The output the conversion being recorded writes.
    int m_Target = 0;
    std::thread m_EncodeThread;
    std::mutex m_JobLock;
    std::condition_variable m_JobPosted;
    std::condition_variable m_JobDone;
    EncodeJob m_Waiting;
    int m_WaitingOutput = -1;
    int m_EncodingOutput = -1;
    bool m_StopEncoding = false;
    /// A target the capture thread set while a job ran, for the next one.
    int m_PendingKbps = 0;
    /// Why the encode thread's last picture failed, until the chain takes it.
    std::string m_EncodeLost;
    /// m_Timer between the two threads.
    std::mutex m_TimerLock;
    uint64_t m_Dropped = 0;
    uint64_t m_Overlapped = 0;
    uint64_t m_Jobs = 0;
    bool m_SaidNotPipelined = false;

    /// What an encoder was built to code: keep12 takes a set-aside encoder
    /// back only for the same (plan C11.4).
    struct EncoderShape
    {
        EncoderTuning::Encoder12 encoder12 = EncoderTuning::Encoder12::VideoEncode;
        Codec codec = Codec::Hevc;
        int width = 0;
        int height = 0;
        int fps = 0;
        bool hdr = false;
        bool intraRefresh = false;
        bool operator==(const EncoderShape& o) const
        {
            return encoder12 == o.encoder12 && codec == o.codec && width == o.width &&
                   height == o.height && fps == o.fps && hdr == o.hdr &&
                   intraRefresh == o.intraRefresh;
        }
    };
    EncoderShape m_EncoderShape;
    /// keep12: the encoder a teardown set aside, and what it codes.
    std::unique_ptr<encode::IVideoEncoder12> m_Parked;
    EncoderShape m_ParkedShape;
    uint64_t m_KeptEncoders = 0;
};

} // namespace mw::native
