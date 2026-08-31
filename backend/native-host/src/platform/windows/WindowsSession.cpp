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

#include "../../capture/windows/DxgiDuplication.h"
#include "../../convert/windows/ColorConvert.h"
#include "../../core/Log.h"
#include "../../core/Probe.h"
#include "../../core/Session.h"
#include "../../encode/windows/NvencEncoder.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mw::native {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The Windows capture → encode → deliver pipeline.
///
/// ── One thread, no queue ────────────────────────────────────────────────────
///
/// A frame is captured, converted, encoded and handed to the consumer on the
/// same thread, and the consumer sends it before returning. There is no ring
/// buffer, no worker pool and no hand-off, because each of those would add
/// latency that nothing here would win back:
///
///  - a queue only helps when the producer is faster than the consumer, and
///    here the consumer IS the network — falling behind means the link is full,
///    and buffering into a full link adds delay without delivering more;
///  - a second thread would cost a wake-up per frame to overlap work that takes
///    less than a millisecond.
///
/// The loop blocks in AcquireNextFrame, which wakes on the display's own
/// present. So the pipeline is paced by the screen rather than by a timer we
/// chose, and an idle desktop costs nothing at all.
class WindowsSession final : public Session
{
public:
    WindowsSession(const SessionConfig& config, const SessionCallbacks& callbacks)
        : m_Config(config)
        , m_Callbacks(callbacks)
    {}

    ~WindowsSession() override { stop(); }

    bool start(std::string& error) override
    {
        if (m_Running.load()) return true;

        // Re-probe rather than trust the caller: a display can be unplugged
        // between the launch request and this call.
        const Capabilities caps = probe();
        const DisplayInfo* display = nullptr;
        for (const DisplayInfo& d : caps.displays) {
            if (d.id == m_Config.displayId) {
                display = &d;
                break;
            }
        }
        if (!display) {
            error = "that display is no longer connected";
            return false;
        }

        const GpuInfo* gpu = caps.gpuFor(*display);
        if (!gpu) {
            error = "could not identify the GPU driving that display";
            return false;
        }

        // DXGI wants the output's index WITHIN its adapter, which on a
        // multi-GPU machine is not the global display id.
        unsigned outputIndex = 0;
        for (const DisplayInfo& d : caps.displays) {
            if (d.id == display->id) break;
            if (d.gpuId == display->gpuId) ++outputIndex;
        }

        m_Capture = std::make_unique<capture::DxgiDuplication>(gpu->nativeHandle, outputIndex);
        if (!m_Capture->start(error)) return false;

        // 4:4:4 only when the client asked AND the encoder can. Silently
        // downgrading would be worse than saying so: the whole reason to ask
        // for it is text and UI legibility, and a stream that quietly returns
        // 4:2:0 looks like the setting does nothing.
        const bool yuv444 = m_Config.yuv444 && gpu->supports444;
        if (m_Config.yuv444 && !yuv444)
            log::info("[native] 4:4:4 requested but this encoder cannot — streaming 4:2:0");

        m_Converter = std::make_unique<convert::ColorConvert>();
        if (!m_Converter->init(m_Capture->device(), m_Capture->format(), m_Capture->width(),
                               m_Capture->height(), m_Config.width, m_Config.height,
                               yuv444 ? convert::ColorConvert::Chroma::C444
                                      : convert::ColorConvert::Chroma::C420,
                               error))
            return false;

        const Codec codec = m_Config.clientCodecs.empty() ? Codec::H264 : m_Config.clientCodecs[0];
        m_Encoder = std::make_unique<encode::NvencEncoder>();
        if (!m_Encoder->init(m_Capture->device(), codec, m_Converter->outputWidth(),
                             m_Converter->outputHeight(), m_Config.fps, m_Config.bitrateKbps,
                             yuv444, error))
            return false;

        m_Info = SessionInfo{};
        m_Info.displayId = display->id;
        m_Info.width = m_Converter->outputWidth();
        m_Info.height = m_Converter->outputHeight();
        m_Info.fps = m_Config.fps;
        m_Info.codec = codec;
        m_Info.encoder = gpu->encoders.empty() ? EncoderApi::None : gpu->encoders.front();
        m_Info.capture = caps.capture;
        m_Info.gpuName = gpu->name;
        m_Info.hdr = m_Config.hdr;
        m_Info.yuv444 = yuv444;
        // Counted, not estimated: one GPU→CPU read of the bitstream. Everything
        // upstream of it stays in VRAM on the capturing adapter.
        m_Info.copiesPerFrame = 1;
        m_Info.crossGpuCopy = false;

        // The first frame must be a keyframe — a client has nothing to decode
        // against otherwise.
        m_ForceKeyframe.store(true);
        m_Running.store(true);
        m_Thread = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        // Idempotent, and safe from inside a callback: the flag is checked by
        // the loop, and the join is skipped when we ARE the loop.
        const bool wasRunning = m_Running.exchange(false);
        if (m_Thread.joinable()) {
            if (std::this_thread::get_id() == m_Thread.get_id())
                m_Thread.detach();
            else
                m_Thread.join();
        }
        if (!wasRunning && !m_Encoder && !m_Capture) return;

        m_Encoder.reset();
        m_Converter.reset();
        m_Capture.reset();
    }

    const SessionInfo& info() const override { return m_Info; }

    void sendInput(const InputEvent&) override
    {
        // Input injection lands with the input backend. Silently ignored rather
        // than queued: a queue would deliver a burst of stale events the moment
        // it arrived, which is worse than having dropped them.
    }

    void requestKeyframe() override { m_ForceKeyframe.store(true); }

    void invalidateReference(uint32_t frameNumber) override
    {
        // NVENC can re-encode against an older still-valid frame instead of
        // sending a full IDR. Until that is wired through, degrade to the
        // honest fallback rather than silently doing nothing — a client that
        // asked for recovery must get some.
        (void)frameNumber;
        m_ForceKeyframe.store(true);
    }

    void setTargetBitrate(int kbps) override { m_PendingBitrate.store(kbps); }

private:
    void run()
    {
        // The capture timeout only bounds how long the loop sleeps when nothing
        // is presented; it is not a frame deadline. Short enough to notice a
        // stop() promptly, long enough that an idle desktop costs nothing.
        constexpr int kAcquireTimeoutMs = 100;

        uint32_t frameNumber = 0;
        std::string error;

        while (m_Running.load()) {
            if (const int kbps = m_PendingBitrate.exchange(0); kbps > 0) {
                if (!m_Encoder->setBitrate(kbps, error))
                    log::warning("[native] bitrate change refused: " + error);
            }

            capture::CapturedFrame frame;
            const capture::AcquireStatus status = m_Capture->acquire(kAcquireTimeoutMs, frame);

            if (status == capture::AcquireStatus::Timeout) continue;

            if (status == capture::AcquireStatus::Lost) {
                // A mode change, a resolution change, a desktop switch. The
                // duplication is rebuilt; the encoder is left alone because the
                // frame size has not necessarily changed.
                if (!m_Capture->start(error)) {
                    finish("capture could not be restarted: " + error);
                    return;
                }
                // Whatever the client had is now stale.
                m_ForceKeyframe.store(true);
                continue;
            }

            if (status != capture::AcquireStatus::Ok) {
                finish("capture failed");
                return;
            }

            const int64_t submittedUs = steadyNowUs();

            if (!m_Converter->convert(frame.texture, error)) {
                m_Capture->release();
                finish("colour conversion failed: " + error);
                return;
            }

            // Released before encoding: Desktop Duplication refuses the next
            // acquire while a frame is held, and the conversion has already
            // copied what it needs into the NV12 texture.
            m_Capture->release();

            const bool forceKeyframe = m_ForceKeyframe.exchange(false);
            encode::NvencOutput encoded;
            if (!m_Encoder->encode(m_Converter->output(), forceKeyframe, encoded, error)) {
                finish("encode failed: " + error);
                return;
            }

            if (encoded.data && encoded.size > 0 && m_Callbacks.onVideo) {
                EncodedFrame out;
                out.data = encoded.data;
                out.size = encoded.size;
                out.keyframe = encoded.keyframe;
                out.frameNumber = frameNumber++;
                out.presentUs = frame.presentUs;
                out.capturedUs = frame.capturedUs;
                out.submittedUs = submittedUs;
                out.encodedUs = steadyNowUs();

                // Delivered on this thread, and the consumer sends it before
                // returning. The buffer is unlocked immediately after, which is
                // what keeps the GPU→CPU copy at exactly one per frame.
                m_Callbacks.onVideo(out);
            }
            m_Encoder->releaseOutput();
        }
    }

    /// Report an unrecoverable end once, from the loop thread.
    void finish(const std::string& reason)
    {
        log::warning("[native] session ended: " + reason);
        m_Running.store(false);
        if (m_Callbacks.onEnded) m_Callbacks.onEnded(reason);
    }

    SessionConfig m_Config;
    SessionCallbacks m_Callbacks;
    SessionInfo m_Info;

    std::unique_ptr<capture::DxgiDuplication> m_Capture;
    std::unique_ptr<convert::ColorConvert> m_Converter;
    std::unique_ptr<encode::NvencEncoder> m_Encoder;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ForceKeyframe{true};
    /// Zero means "no change pending". Exchanged by the loop each iteration.
    std::atomic<int> m_PendingBitrate{0};
};

} // namespace

namespace detail {

std::unique_ptr<Session> createPlatformSession(const SessionConfig& config,
                                               const SessionCallbacks& callbacks,
                                               std::string& error)
{
    // Only NVENC is wired in so far. AMD and Intel adapters report no codec
    // (their capability queries are not written), so the Selector never routes
    // a session to them — reaching here with one would be a bug, not a
    // configuration the user could produce.
    if (!callbacks.onVideo) {
        error = "a session without a video callback would encode into nothing";
        return nullptr;
    }
    return std::make_unique<WindowsSession>(config, callbacks);
}

} // namespace detail
} // namespace mw::native
