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
#include "../../encode/windows/AmfEncoder.h"
#include "../../encode/windows/NvencEncoder.h"
#include "../../encode/windows/VplEncoder.h"
#include "../../input/windows/Win32Input.h"

#include <atomic>
#include <chrono>
#include <exception>
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
    WindowsSession(const SessionConfig& config, const ResolvedTarget& target,
                   const SessionCallbacks& callbacks)
        : m_Config(config)
        , m_Target(target)
        , m_Callbacks(callbacks)
    {}

    ~WindowsSession() override { stop(); }

    bool start(std::string& error) override
    {
        if (m_Running.load()) return true;

        // Confirm the display is still there — it can be unplugged between the
        // launch request and this call. What is NOT re-decided here is which
        // GPU encodes: the Selector already worked that out, and re-deriving it
        // from the display is exactly the bug that made an AMD-driven monitor
        // try to open NVENC.
        const Capabilities caps = probe();
        const DisplayInfo* display = nullptr;
        for (const DisplayInfo& d : caps.displays) {
            if (d.id == m_Target.displayId) {
                display = &d;
                break;
            }
        }
        if (!display) {
            error = "that display is no longer connected";
            return false;
        }

        // Encoding on an adapter other than the one scanning the display out
        // needs the frame copied between GPUs, which is not written yet. Say so
        // plainly: the alternative is the encoder failing with a vendor error
        // that says nothing about the real cause.
        if (m_Target.crossGpuCopy) {
            error = "this display is driven by a GPU that cannot encode, and copying the "
                    "frame to another GPU is not supported yet";
            return false;
        }

        m_Capture = std::make_unique<capture::DxgiDuplication>(m_Target.captureAdapterHandle,
                                                               m_Target.outputIndex);
        if (!m_Capture->start(error)) return false;

        // 4:4:4 only when the client asked AND the encoder can. Silently
        // downgrading would be worse than saying so: the whole reason to ask
        // for it is text and UI legibility, and a stream that quietly returns
        // 4:2:0 looks like the setting does nothing.
        const bool yuv444 = m_Target.yuv444;
        if (m_Config.yuv444 && !yuv444)
            log::info("[native] 4:4:4 requested but this encoder cannot — streaming 4:2:0");

        m_Converter = std::make_unique<convert::ColorConvert>();
        if (!m_Converter->init(m_Capture->device(), m_Capture->format(), m_Capture->width(),
                               m_Capture->height(), m_Config.width, m_Config.height,
                               yuv444 ? convert::ColorConvert::Chroma::C444
                                      : convert::ColorConvert::Chroma::C420,
                               error))
            return false;

        // The encoder the Selector chose, not one guessed from the display.
        switch (m_Target.encoder) {
        case EncoderApi::Nvenc: m_Encoder = std::make_unique<encode::NvencEncoder>(); break;
        case EncoderApi::Amf: m_Encoder = std::make_unique<encode::AmfEncoder>(); break;
        case EncoderApi::Vpl: m_Encoder = std::make_unique<encode::VplEncoder>(); break;
        default:
            error =
                std::string("no encoder implementation for ") + toString(m_Target.encoder) + " yet";
            return false;
        }

        if (!m_Encoder->init(m_Capture->device(), m_Target.codec, m_Converter->outputWidth(),
                             m_Converter->outputHeight(), m_Config.fps, m_Config.bitrateKbps,
                             yuv444, m_Config.intraRefresh, error))
            return false;

        m_Info = SessionInfo{};
        m_Info.displayId = display->id;
        m_Info.width = m_Converter->outputWidth();
        m_Info.height = m_Converter->outputHeight();
        m_Info.fps = m_Config.fps;
        m_Info.codec = m_Target.codec;
        m_Info.encoder = m_Target.encoder;
        m_Info.capture = caps.capture;
        m_Info.gpuName = m_Target.encodeGpuName;
        m_Info.hdr = m_Target.hdr;
        m_Info.yuv444 = yuv444;
        // Reported, not requested: an encoder that declined it says so, and the
        // receiver must then keep its usual keyframe recovery.
        m_Info.intraRefresh = m_Encoder->intraRefreshEnabled();
        if (m_Config.intraRefresh && !m_Info.intraRefresh)
            log::info("[native] intra-refresh requested but this encoder declined it");
        // Counted, not estimated: one GPU→CPU read of the bitstream. Everything
        // upstream of it stays in VRAM on the capturing adapter.
        m_Info.copiesPerFrame = 1;
        m_Info.crossGpuCopy = m_Target.crossGpuCopy;

        // Input comes up last, and its failure is NOT fatal. A session that
        // streams but cannot inject is degraded; a session that refuses to
        // start because of input gives the user nothing at all. The log says
        // which one they got.
        {
            // Rumble travels the opposite way from every other input: the game
            // asks the pad to shake, and that has to reach the browser. Handed
            // straight to the session callback — it arrives on a ViGEm thread,
            // and the consumer is the one that knows how to marshal it.
            auto sink = std::make_unique<input::Win32Input>(m_Capture->desktopRect(),
                                                            [this](const RumbleEvent& event) {
                                                                if (m_Callbacks.onRumble)
                                                                    m_Callbacks.onRumble(event);
                                                            });
            std::string inputError;
            if (sink->start(inputError)) {
                std::lock_guard<std::mutex> lock(m_InputMutex);
                m_Input = std::move(sink);
            } else {
                log::warning("[native] input unavailable, streaming view-only: " + inputError);
            }
        }

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
        // Torn down before the capture, and under the lock, because inject()
        // runs on the network thread and may be in flight right now. Dropping
        // the sink releases whatever the user was still holding.
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            m_Input.reset();
        }

        if (!wasRunning && !m_Encoder && !m_Capture) return;

        m_Encoder.reset();
        m_Converter.reset();
        m_DesktopCopy.Reset();
        m_Capture.reset();
    }

    const SessionInfo& info() const override { return m_Info; }

    void sendInput(const InputEvent& event) override
    {
        // Injected HERE, on the caller's thread, never handed to the capture
        // thread: SendInput costs microseconds, and queueing it behind a frame
        // being encoded would add a whole frame time to the one path where
        // delay is felt directly.
        //
        // The lock only guards the sink's lifetime against stop(); it is
        // uncontended in steady state, since the capture thread never touches
        // input at all.
        std::lock_guard<std::mutex> lock(m_InputMutex);
        if (m_Input) m_Input->inject(event);
    }

    void setCompositeCursor(bool composite) override
    {
        // Read by the capture thread each frame. A change takes effect on the
        // next one, which is the whole point of it being runtime-settable.
        if (m_CompositeCursor.exchange(composite) == composite) return;
        log::info(composite ? "[native] cursor: drawn into the picture (immersive)"
                            : "[native] cursor: handed to the client to draw (desktop)");
        // The client that just took over drawing has never seen a shape, and
        // the shape only arrives from DXGI when it CHANGES — which, for a
        // pointer sitting still, may be never. Force one report.
        m_ResendCursor.store(true);
        // Going back to compositing means the picture must show the pointer
        // again, and the last frame the client has does not.
        if (composite) m_ForceKeyframe.store(true);
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
    /// The thread entry point. Nothing may escape it.
    ///
    /// An exception leaving a std::thread calls std::terminate, which aborts
    /// the whole worker PROCESS with no usable dump — 0xC0000409 raised from
    /// inside ucrtbase, past any handler. That is how a single bad frame took
    /// down a session and left nothing to read. Whatever goes wrong, it is
    /// turned into an ended session with a reason.
    void run() noexcept
    {
        try {
            runLoop();
        } catch (const std::exception& e) {
            finish(std::string("the capture loop threw: ") + e.what());
        } catch (...) {
            finish("the capture loop threw an unknown exception");
        }
    }

    void runLoop()
    {
        // The capture timeout only bounds how long the loop sleeps when nothing
        // is presented; it is not a frame deadline. Short enough to notice a
        // stop() promptly, long enough that an idle desktop costs nothing.
        constexpr int kAcquireTimeoutMs = 100;

        // How long a perfectly still desktop may go without sending anything.
        //
        // Desktop Duplication delivers frames on damage, so a screen where
        // nothing moves produces nothing at all — which is efficient and, to
        // the receiver, indistinguishable from a broken stream. The browser
        // declares starvation after 1000 ms and asks for a keyframe; that
        // request storm is then read as congestion and walks the quality ladder
        // all the way down, on a session that was never in trouble.
        //
        // So: a floor, comfortably inside that window. What goes out is the
        // frame already converted and unchanged, which an encoder turns into
        // almost nothing — a few hundred bytes of "everything is the same".
        constexpr int64_t kIdleFloorUs = 500 * 1000;

        uint32_t frameNumber = 0;
        std::string error;
        int64_t lastSentUs = steadyNowUs();

        while (m_Running.load()) {
            if (const int kbps = m_PendingBitrate.exchange(0); kbps > 0) {
                if (!m_Encoder->setBitrate(kbps, error))
                    log::warning("[native] bitrate change refused: " + error);
            }

            capture::CapturedFrame frame;
            const capture::AcquireStatus status = m_Capture->acquire(kAcquireTimeoutMs, frame);

            // Before the status is acted on, and on EVERY status. A client that
            // has just taken over drawing needs to be told what the pointer
            // looks like — including "there is none here" — and on a still
            // screen with the mouse on another display every single wake-up is
            // a timeout, so anything gated behind a frame would never run.
            reportCursor();

            if (status == capture::AcquireStatus::Timeout) {
                // Nothing moved. Hold the floor open so the receiver can tell a
                // quiet screen from a dead one — re-encoding what is already
                // converted, so this costs an encode and not a capture.
                if (steadyNowUs() - lastSentUs < kIdleFloorUs) continue;
                if (!m_Converter->outputWidth()) continue;
                if (!emit(frameNumber, steadyNowUs(), error)) return;
                lastSentUs = steadyNowUs();
                continue;
            }

            // Only the pointer moved. The desktop is untouched, so there is no
            // new texture.
            //
            // When the CLIENT draws the pointer there is nothing to do at all:
            // report the shape if it changed, and send not one byte of video.
            // That is the whole win of the out-of-band cursor — moving the mouse
            // over a still screen costs nothing, and the pointer moves at the
            // viewer's refresh rate instead of the stream's.
            //
            // When we composite, the picture HAS changed even though the desktop
            // did not, so it is re-converted from our own copy with the cursor
            // at its new place.
            if (status == capture::AcquireStatus::PointerOnly) {
                if (!m_CompositeCursor.load()) continue;
                if (!m_DesktopCopy) continue;
                if (!m_Converter->convert(m_DesktopCopy.Get(), m_Capture->cursor(), error)) {
                    finish("colour conversion failed: " + error);
                    return;
                }
                if (!emit(frameNumber, steadyNowUs(), error)) return;
                lastSentUs = steadyNowUs();
                continue;
            }

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

            // An empty state draws nothing: that is how the client-drawn mode
            // keeps the picture clean.
            static const capture::CursorState kNoCursor;
            const bool composite = m_CompositeCursor.load();
            if (!m_Converter->convert(frame.texture, composite ? m_Capture->cursor() : kNoCursor,
                                      error)) {
                m_Capture->release();
                finish("colour conversion failed: " + error);
                return;
            }

            // Keep a copy of the desktop for the pointer-only path above — only
            // when compositing, and only when the pointer is on this display.
            // When the client draws its own, or the pointer is hidden or on
            // another screen (a fullscreen game, the usual latency-critical
            // case), no pointer-only frame can ever need it: the copy is skipped
            // entirely and the frame path is exactly what it was before.
            if (composite && m_Capture->cursor().visible && !retainDesktop(frame.texture, error))
                log::warning("[native] could not keep a desktop copy: " + error);

            // Released before encoding: Desktop Duplication refuses the next
            // acquire while a frame is held, and the conversion has already
            // copied what it needs into the NV12 texture.
            m_Capture->release();

            if (!emit(frameNumber, submittedUs, error, &frame)) return;
            lastSentUs = steadyNowUs();
        }
    }

    /// Encode whatever the converter currently holds and hand it to the
    /// consumer. @p frame is the capture it came from, or null when this is a
    /// re-send (idle floor, pointer-only) with no new present to report.
    ///
    /// Returns false when the session must end; the reason has been reported.
    bool emit(uint32_t& frameNumber, int64_t submittedUs, std::string& error,
              const capture::CapturedFrame* frame = nullptr)
    {
        const bool forceKeyframe = m_ForceKeyframe.exchange(false);
        encode::EncoderOutput encoded;
        if (!m_Encoder->encode(m_Converter->output(), forceKeyframe, encoded, error)) {
            finish("encode failed: " + error);
            return false;
        }

        // The size of the first keyframe, once. It is the number that says
        // whether a still picture will look right: nothing follows it to refine
        // it, so on a static screen it IS the picture. Cheap, and it turns "it
        // looks soft" into a figure that can be compared across settings.
        if (encoded.keyframe && !m_LoggedFirstKeyframe) {
            m_LoggedFirstKeyframe = true;
            log::info("[native] first keyframe: " + std::to_string(encoded.size / 1024) + " KB (" +
                      std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + ")");
        }

        if (encoded.data && encoded.size > 0 && m_Callbacks.onVideo) {
            EncodedFrame out;
            out.data = encoded.data;
            out.size = encoded.size;
            out.keyframe = encoded.keyframe;
            out.frameNumber = frameNumber++;
            // A re-send has no present of its own. Reporting "now" for both
            // keeps the latency figures honest — it measures zero capture
            // latency because there was no capture, rather than inheriting a
            // stale present time and claiming half a second of delay.
            out.presentUs = frame ? frame->presentUs : submittedUs;
            out.capturedUs = frame ? frame->capturedUs : submittedUs;
            out.submittedUs = submittedUs;
            out.encodedUs = steadyNowUs();

            // Delivered on this thread, and the consumer sends it before
            // returning. The buffer is unlocked immediately after, which is
            // what keeps the GPU→CPU copy at exactly one per frame.
            m_Callbacks.onVideo(out);
        }
        m_Encoder->releaseOutput();
        return true;
    }

    /// Tell the client what the pointer looks like, when the client is the one
    /// drawing it.
    ///
    /// Sent on change only — shape, or appearing/disappearing. A pointer being
    /// moved around keeps one shape for thousands of frames, so in the case
    /// this feature exists for, this sends nothing at all.
    ///
    /// Position is deliberately NOT sent. The client knows where its own pointer
    /// is, better and sooner than we could tell it; sending ours would only give
    /// it something to disagree with.
    void reportCursor()
    {
        if (!m_Callbacks.onCursor || m_CompositeCursor.load()) return;

        const capture::CursorState& cursor = m_Capture->cursor();
        const bool forced = m_ResendCursor.exchange(false);
        if (!forced && cursor.shapeVersion == m_ReportedShape &&
            cursor.visible == m_ReportedVisible)
            return;

        m_ReportedShape = cursor.shapeVersion;
        m_ReportedVisible = cursor.visible;

        CursorUpdate update;
        update.visible = cursor.visible;
        update.width = cursor.width;
        update.height = cursor.height;
        // The capture stores the image's top-left, having already subtracted
        // the hotspot; the client needs the offset itself to place the image
        // against its own pointer.
        update.hotspotX = m_Capture->cursorHotspotX();
        update.hotspotY = m_Capture->cursorHotspotY();

        // Inverting pixels are flattened to black here rather than in the
        // capture, because the composited path genuinely inverts and must keep
        // the information. See CursorUpdate::pixels on why black is right.
        if (!cursor.pixels.empty() && cursor.width > 0 && cursor.height > 0) {
            m_CursorScratch = cursor.pixels;
            for (size_t i = 0; i < cursor.invert.size(); ++i) {
                if (!cursor.invert[i]) continue;
                m_CursorScratch[i * 4 + 0] = 0;
                m_CursorScratch[i * 4 + 1] = 0;
                m_CursorScratch[i * 4 + 2] = 0;
                m_CursorScratch[i * 4 + 3] = 0xFF;
            }
            update.pixels = m_CursorScratch.data();
        }

        m_Callbacks.onCursor(update);
    }

    /// Keep a private copy of the captured desktop, so a later frame that only
    /// moved the cursor can be rebuilt without a fresh capture.
    bool retainDesktop(ID3D11Texture2D* source, std::string& error)
    {
        if (!source) return false;

        if (!m_DesktopCopy) {
            D3D11_TEXTURE2D_DESC desc = {};
            source->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            if (FAILED(m_Capture->device()->CreateTexture2D(&desc, nullptr,
                                                            m_DesktopCopy.GetAddressOf()))) {
                error = "the GPU refused a scratch copy of the desktop";
                return false;
            }
        }

        // GPU to GPU, no system memory involved. It is a real cost — roughly a
        // tenth of a millisecond at 1440p — paid only while the pointer is on
        // this screen, and it is what buys a cursor that moves on a still
        // desktop.
        m_Capture->context()->CopyResource(m_DesktopCopy.Get(), source);
        return true;
    }

    /// Report an unrecoverable end once, from the loop thread.
    ///
    /// noexcept because it is called from run()'s catch block: a throw here
    /// would re-enter termination with the original cause already lost.
    void finish(const std::string& reason) noexcept
    {
        m_Running.store(false);
        try {
            log::warning("[native] session ended: " + reason);
            if (m_Callbacks.onEnded) m_Callbacks.onEnded(reason);
        } catch (...) {
            // Nothing left to report it to.
        }
    }

    SessionConfig m_Config;
    /// The Selector's decision. Read, never re-derived.
    ResolvedTarget m_Target;
    SessionCallbacks m_Callbacks;
    SessionInfo m_Info;

    std::unique_ptr<capture::DxgiDuplication> m_Capture;
    std::unique_ptr<convert::ColorConvert> m_Converter;
    /// The last captured desktop, kept only while the pointer is on this
    /// screen — see retainDesktop().
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_DesktopCopy;
    /// Held by interface: which vendor path this is was decided by the
    /// Selector, and the loop below neither knows nor needs to.
    std::unique_ptr<encode::IVideoEncoder> m_Encoder;

    /// Optional: a session with no input sink still streams, view-only. Guarded
    /// because it is created and destroyed on the session's thread but used on
    /// the network thread.
    std::mutex m_InputMutex;
    std::unique_ptr<input::IInputSink> m_Input;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ForceKeyframe{true};
    /// True — the default — draws the pointer into the picture. False reports
    /// its shape to the client, which draws it itself at its own refresh rate.
    std::atomic<bool> m_CompositeCursor{true};
    /// Forces one cursor report even though DXGI says the shape is unchanged.
    std::atomic<bool> m_ResendCursor{false};
    /// The shape the client has been told about, so an unchanged pointer is not
    /// re-sent on every frame.
    uint64_t m_ReportedShape = 0;
    bool m_ReportedVisible = false;
    bool m_LoggedFirstKeyframe = false;
    /// The flattened image handed to the client. Reused so a shape change does
    /// not allocate on the capture thread.
    std::vector<uint8_t> m_CursorScratch;
    /// Zero means "no change pending". Exchanged by the loop each iteration.
    std::atomic<int> m_PendingBitrate{0};
};

} // namespace

namespace detail {

std::unique_ptr<Session> createPlatformSession(const SessionConfig& config,
                                               const ResolvedTarget& target,
                                               const SessionCallbacks& callbacks,
                                               std::string& error)
{
    if (!callbacks.onVideo) {
        error = "a session without a video callback would encode into nothing";
        return nullptr;
    }
    // Which encoder to build is decided in start(), from target.encoder — the
    // Selector's choice, not a guess made here.
    return std::make_unique<WindowsSession>(config, target, callbacks);
}

} // namespace detail
} // namespace mw::native
