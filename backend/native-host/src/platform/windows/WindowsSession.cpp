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

#include "../../audio/windows/WasapiLoopback.h"
#include "../../capture/windows/DxgiDuplication.h"
#include "../../convert/windows/ColorConvert.h"
#include "../../core/FrameCadence.h"
#include "../../core/Log.h"
#include "../../core/Probe.h"
#include "../../core/RestartBackoff.h"
#include "../../core/Session.h"
#include "../../encode/RateControl.h"
#include "../../encode/RateGovernor.h"
#include "../../encode/windows/AmfEncoder.h"
#include "../../encode/windows/NvencEncoder.h"
#include "../../encode/windows/VplEncoder.h"
#include "../../input/windows/Win32Input.h"
#include "CrossGpuBridge.h"

// GetCursorInfo/LoadCursorW, for naming the pointer — see currentCursorKind().
#include <windows.h>
// AvSetMmThreadCharacteristicsW: the capture loop runs as an MMCSS "Games" task.
#include <avrt.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mw::native {
namespace {

/// The most a composited pointer may be blown up past its real size.
///
/// Two reasons, and the second is the one that set the number. It is a 32-pixel
/// bitmap, so stretched far enough it stops reading as a pointer. And the client
/// asks for a size on the GLASS, which is constant by design — the same 18
/// pixels in portrait as in landscape — while the picture behind it is not: a
/// phone held upright shows a 16:9 desktop in a band a fifth of the screen tall,
/// and a pointer that keeps its screen size there eats a twelfth of the picture
/// it is supposed to be pointing into.
///
/// The cap is what says "not on a picture this small". It only ever binds when
/// the picture is displayed far smaller than it was encoded, which is exactly
/// that case: at 2.5 a phone in portrait draws a visible pointer that is no
/// longer out of scale with what is under it, and every larger picture — a
/// phone turned sideways, any zoom at all — is already below it and untouched.
constexpr float kMaxCursorMagnify = 2.5f;

/// The fastest still-screen floor a client may ask for.
///
/// The real limit is the stream's own frame rate and the loop applies it; this
/// one exists because the request arrives as a number from a browser and the
/// loop divides by it. High enough to be no limit at all in practice.
constexpr int kMaxFloorFps = 480;

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The stamps a picture carries from the capture to the encoder — t₀ to t₂ of
/// EncodedFrame.
struct FrameStamps
{
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
    int64_t submittedUs = 0;
    int64_t convertedUs = 0;
};

/// A re-send has no present of its own. Reporting "now" for everything keeps
/// the latency figures honest — it measures zero capture latency because there
/// was no capture, rather than inheriting a stale present time and claiming
/// half a second of delay.
FrameStamps resendStamps(int64_t nowUs)
{
    return FrameStamps{nowUs, nowUs, nowUs, nowUs};
}

/// "165", "144", "60" — the refresh rate as a person would say it.
std::string hzString(int milliHz)
{
    return std::to_string((milliHz + 500) / 1000);
}

/// Name the pointer currently on screen, as a CSS cursor keyword.
///
/// Windows hands out the SAME handle for a standard cursor to every process, so
/// comparing what is on screen against the system set identifies it exactly —
/// no image matching, no heuristics. An application with a cursor of its own
/// matches nothing and gets "", which is the honest answer: there is no keyword
/// for someone's custom artwork.
///
/// This is a different source from the DXGI shape — user32 rather than the
/// duplication — and that is why it lives here rather than in the capture.
const char* currentCursorKind()
{
    struct Known
    {
        // LPCTSTR, not an explicit wide string: the IDC_* macros follow the
        // project's character set, and naming a width here would only compile
        // for one of them.
        LPCTSTR id;
        const char* css;
    };
    // Ordered as the Win32 headers list them. IDC_UPARROW and IDC_SIZE have no
    // CSS equivalent worth inventing, so they fall through to "".
    static const Known kKnown[] = {
        {IDC_ARROW, "default"},    {IDC_IBEAM, "text"},           {IDC_WAIT, "wait"},
        {IDC_CROSS, "crosshair"},  {IDC_SIZENWSE, "nwse-resize"}, {IDC_SIZENESW, "nesw-resize"},
        {IDC_SIZEWE, "ew-resize"}, {IDC_SIZENS, "ns-resize"},     {IDC_SIZEALL, "move"},
        {IDC_NO, "not-allowed"},   {IDC_HAND, "pointer"},         {IDC_APPSTARTING, "progress"},
        {IDC_HELP, "help"},
    };

    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    if (!::GetCursorInfo(&info) || !info.hCursor) return "";

    for (const Known& known : kKnown) {
        // LoadCursor on a system cursor returns a shared handle and does not
        // need freeing; it is cheap enough to call per report and avoids
        // caching handles that a theme change could invalidate.
        if (info.hCursor == ::LoadCursor(nullptr, known.id)) return known.css;
    }
    return "";
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

        // Encoding on an adapter other than the one scanning the display out:
        // the frame crosses through system memory (CrossGpuBridge says what
        // that costs). The converter and the encoder are then built on the
        // encoder's device, and every picture passes the bridge on its way to
        // them. Opened once, here: the encoder's adapter does not change when
        // the duplication is lost and reopened.
        m_Bridge.reset();
        if (m_Target.crossGpuCopy) {
            auto bridge = std::make_unique<CrossGpuBridge>(m_Target.encodeAdapterHandle);
            if (!bridge->open(error)) return false;
            m_Bridge = std::move(bridge);
            log::warning("[native] cross-GPU copy: the display's frames are carried to '" +
                         m_Target.encodeGpuName +
                         "' through system memory — every zero-copy "
                         "figure of this engine is off on this session");
        }

        // HDR is carried only when the whole chain can: the Selector has checked
        // the display, the GPU and the codec, and the converter has the last
        // word — it turns FP16 scRGB into P010, or it does not. When it does
        // not, the capture is opened in 8-bit and DXGI hands over an HDR
        // desktop already tone-mapped to SDR, which is the picture an SDR
        // stream should carry. Opening in FP16 regardless is what made the
        // session fail at the click on every machine with Windows HDR on: the
        // converter refused a format nothing downstream could have used.
        if (m_Target.hdr &&
            !convert::ColorConvert::supportsSource(DXGI_FORMAT_R16G16B16A16_FLOAT)) {
            log::info("[native] HDR negotiated but this build converts SDR only — streaming SDR "
                      "from the tone-mapped desktop");
            m_Target.hdr = false;
        }

        m_Capture = std::make_unique<capture::DxgiDuplication>(m_Target.captureAdapterHandle,
                                                               m_Target.outputIndex, m_Target.hdr);
        if (!m_Capture->start(error)) return false;
        if (!convert::ColorConvert::supportsSource(m_Capture->format())) {
            error = "the display delivers frames in a format this build cannot convert (" +
                    std::to_string(static_cast<int>(m_Capture->format())) + ")";
            return false;
        }

        // 4:4:4 as the Selector granted it: it has already steered the codec to
        // one that carries it, or given it up with a log line of its own. Said
        // again here because this is the line a session log is read from, and
        // a stream that quietly returns 4:2:0 looks like the setting does
        // nothing.
        const bool yuv444 = m_Target.yuv444;
        if (m_Config.yuv444 && !yuv444)
            log::info("[native] 4:4:4 requested but not granted — streaming 4:2:0");

        // The rate the encoder's budget is dimensioned for, and whether the
        // loop has to be held to it. The Selector has already resolved "0, the
        // display's own" to the display's rounded refresh, so the setting is
        // always a number here; the guard below is for a config that bypassed
        // it. The cadence gate exists only when the display is FASTER than the
        // stream: at the display's own rate every present is encoded as it
        // comes, and a gate at that same rate would skip a present that
        // arrived a little early and wait a whole period for the next one.
        m_DisplayMilliHz = display->refreshMilliHz;
        const int displayHz = (display->refreshMilliHz + 500) / 1000;
        m_EncodeFps = m_Config.fps > 0 ? m_Config.fps : displayHz;
        if (m_EncodeFps <= 0) m_EncodeFps = 60;
        m_Cadence = FrameCadence(m_EncodeFps < displayHz ? m_EncodeFps : 0, displayHz);
        if (m_Cadence.enabled())
            log::info("[native] cadence: " + std::to_string(m_EncodeFps) + " fps stream on a " +
                      hzString(m_DisplayMilliHz) +
                      " Hz display — the first present of each interval is encoded, at once");
        else
            log::info("[native] cadence: " + std::to_string(m_EncodeFps) + " fps stream on a " +
                      hzString(m_DisplayMilliHz) + " Hz display — every present is encoded");

        // The client's own frame size is whatever it asked for, or the desktop
        // when it asked for nothing.
        if (!buildPipeline(m_Config.width, m_Config.height, error)) return false;

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
        // upstream of it stays in VRAM on the capturing adapter — unless the
        // bridge is in, which adds the readback and the upload of every frame.
        m_Info.copiesPerFrame = m_Bridge ? 3 : 1;
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

        // Audio, on the same terms as input: wanted only when the consumer gave
        // a callback, and its failure degrades the session (silent) rather
        // than refusing it. A machine with no playback device at all still
        // streams its screen.
        if (m_Callbacks.onAudio) {
            auto audio = std::make_unique<audio::WasapiLoopback>(m_Callbacks.onAudio);
            std::string audioError;
            if (audio->start(audioError)) {
                m_Audio = std::move(audio);
                m_Info.audio = true;
            } else {
                log::warning("[native] audio unavailable, streaming silent: " + audioError);
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
        // Joins the audio thread; its last packet has been delivered when this
        // returns, so the consumer can be torn down after us.
        m_Audio.reset();

        if (!wasRunning && !m_Encoder && !m_Capture) return;

        m_Encoder.reset();
        m_Converter.reset();
        m_DesktopCopy.Reset();
        // After the converter and the encoder, which live on its device.
        m_Bridge.reset();
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

    void setCompositeCursor(bool composite, int cursorFramePx) override
    {
        // Read by the capture thread each frame. A change takes effect on the
        // next one, which is the whole point of it being runtime-settable.
        const int wanted = cursorFramePx > 0 ? cursorFramePx : 0;
        if (m_CursorFramePx.exchange(wanted) != wanted && composite) {
            // The pointer is about to be drawn at a different size on a screen
            // that may not be moving at all — a viewer pinch-zooming a still
            // desktop. Nothing else in the loop would ever notice.
            m_CursorDirty.store(true);
        }
        if (m_CompositeCursor.exchange(composite) == composite) return;
        log::info(composite ? "[native] cursor: drawn into the picture (gaming)"
                            : "[native] cursor: handed to the client to draw (desktop)");
        // The client that just took over drawing has never seen a shape, and
        // the shape only arrives from DXGI when it CHANGES — which, for a
        // pointer sitting still, may be never. Force one report.
        m_ResendCursor.store(true);
        // Going back to compositing means the picture must show the pointer
        // again, and the last frame the client has does not.
        if (composite) m_ForceKeyframe.store(true);
    }

    void setFrameFloorFps(int fps) override
    {
        // Bounded here rather than trusted: the number crosses the network from
        // a page, and the loop divides by it. The stream's own rate is the other
        // half of the clamp and lives in the loop, which is where the two are
        // compared — see floorIntervalUs().
        if (fps < 0) fps = 0;
        if (fps > kMaxFloorFps) fps = kMaxFloorFps;
        if (m_FloorFps.exchange(fps) == fps) return;
        log::info("[native] still-screen floor: " +
                  (fps > 0 ? std::to_string(fps) + " fps" : std::string("the engine's own")));
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

    void reportLink(const LinkFeedback& feedback) override
    {
        // Kept for the loop, which owns the governor. Two reports between two
        // wake-ups fold into one: the worse delay and the summed counts, so
        // nothing the receiver saw is lost and the loop never runs the
        // governor twice for one moment.
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (m_LinkPending) {
            if (feedback.owdRiseMs > m_LinkFeedback.owdRiseMs)
                m_LinkFeedback.owdRiseMs = feedback.owdRiseMs;
            m_LinkFeedback.gaps += feedback.gaps;
            m_LinkFeedback.evictions += feedback.evictions;
            m_LinkFeedback.receivedFps = feedback.receivedFps;
        } else {
            m_LinkFeedback = feedback;
            m_LinkPending = true;
        }
    }

private:
    /// Take the receiver's latest report, if one arrived since the last call.
    bool takeLinkFeedback(LinkFeedback& out)
    {
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (!m_LinkPending) return false;
        out = m_LinkFeedback;
        m_LinkPending = false;
        return true;
    }

    /// The device the converter and the encoder are built on: the capture's,
    /// or the bridge's when the encoder sits on another GPU.
    ID3D11Device* pipelineDevice() const
    {
        if (m_Bridge) return m_Bridge->device();
        return m_Capture ? m_Capture->device() : nullptr;
    }

    /// The texture the converter is to read for @p captured — the texture
    /// itself in the ordinary case, or its copy on the encoder's GPU when the
    /// two differ. Null, with @p error set, when the copy failed.
    ID3D11Texture2D* pictureFor(ID3D11Texture2D* captured, std::string& error)
    {
        if (!m_Bridge) return captured;
        return m_Bridge->transfer(m_Capture->device(), m_Capture->context(), captured, error);
    }
    /// Build the converter and the encoder against whatever the capture is
    /// handing out RIGHT NOW — its device, its size, its format.
    ///
    /// @p outputWidth / @p outputHeight is the size the client decodes at; zero
    /// means "follow the desktop", which is what a session start passes when the
    /// client asked for no particular resolution. A rebuild passes the size back
    /// in, so the host changing its own mode does not change the client's
    /// geometry underneath a decoder that is already configured.
    bool buildPipeline(int outputWidth, int outputHeight, std::string& error)
    {
        // Released before the replacements are built, not after. Both hold a
        // reference to the D3D device they were made on, and an encoder holds a
        // hardware session — of which a consumer GPU has famously few. Building
        // the new one while the old is still open is how a rebuild fails with a
        // vendor error that says nothing about the real cause.
        m_DesktopCopy.Reset();
        m_Encoder.reset();
        m_Converter.reset();

        // On the encoder's device — the capture's, unless a bridge carries the
        // frames to another GPU, in which case both stages live over there and
        // read the bridge's copy.
        m_Converter = std::make_unique<convert::ColorConvert>();
        if (!m_Converter->init(pipelineDevice(), m_Capture->format(), m_Capture->width(),
                               m_Capture->height(), outputWidth, outputHeight,
                               m_Target.yuv444 ? convert::ColorConvert::Chroma::C444
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

        return m_Encoder->init(pipelineDevice(), m_Target.codec, m_Converter->outputWidth(),
                               m_Converter->outputHeight(), m_EncodeFps, m_Config.bitrateKbps,
                               m_Target.yuv444, m_Config.intraRefresh, m_Config.tuning, error);
    }

    /// The duplication was lost — a resolution change, a mode set, a desktop
    /// switch, a driver restart. Open it again and rebuild everything behind it.
    ///
    /// Everything, because DxgiDuplication::start() creates a NEW D3D11 device.
    /// The converter and the encoder were built on the old one, and their
    /// textures belong to a device the capture no longer uses: kept across the
    /// restart they convert nothing and encode nothing, which is precisely how
    /// changing the host's resolution mid-stream turned the picture black and
    /// then, a few seconds later, killed the session outright.
    ///
    /// What deliberately does NOT change is the size the client decodes at. The
    /// host's desktop may have gone from 1440p to 1080p; the stream stays at the
    /// resolution that was negotiated, and the converter — which scales anyway —
    /// absorbs the difference. A decoder reconfiguring mid-stream is a second
    /// black screen, and the viewer asked for neither.
    ///
    /// And it retries, for as long as the session runs. During a mode set the
    /// output is genuinely absent for a moment, so the first attempt failing is
    /// the normal case, not an error. Giving up there is what left the viewer
    /// staring at a dead stream with the host showing a "Keep these display
    /// settings?" dialog they could no longer reach — the one thing that has
    /// to keep working through a mode change is the ability to click Revert.
    ///
    /// ── The locked screen ───────────────────────────────────────────────────
    ///
    /// Win+L, a screensaver, a UAC prompt: Windows switches to the secure
    /// desktop, the duplication is lost, and DXGI refuses to open a new one
    /// until the user's desktop is back — for as long as that takes. The first
    /// version bounded the wait at ten seconds, so locking the PC from the
    /// stream ended the stream. Now the loop waits (RestartBackoff.h says how),
    /// and while it waits it keeps the stream ALIVE: the browser declares
    /// starvation after a second of silence and walks its quality ladder down
    /// on a session that is merely locked, so something has to go out.
    ///
    /// What goes out is a black picture, at the still-screen floor, encoded by
    /// the encoder the session still has. Black rather than the last desktop:
    /// a frozen desktop looks like a hang, and the viewer who has just pressed
    /// Win+L would press it again. Black says "the screen went away", which is
    /// the truth. Input keeps flowing the whole time — SendInput reaches the
    /// secure desktop — so a password typed into the dark unlocks the host,
    /// the duplication reopens, and the first frame back is a keyframe.
    ///
    /// The old converter and encoder are released only once the duplication
    /// is open again, and before the new pipeline is built: the encoder holds
    /// a hardware session — a consumer GPU has famously few — and building the
    /// replacement while the old one is still open is how a rebuild fails with
    /// a vendor error that says nothing about the real cause.
    enum class Restart
    {
        /// Capturing again on a fresh pipeline.
        Restarted,
        /// stop() was called while waiting. Nothing has been reported.
        Stopped,
        /// The session ended while waiting (an encode failed); finish() has
        /// already been called.
        Ended,
        /// The duplication reopened but the pipeline could not be rebuilt.
        /// @p error says why; nothing has been reported.
        Failed,
    };

    Restart restartCapture(uint32_t& frameNumber, int64_t floorIntervalUs, std::string& error)
    {
        // The longest the wait may sleep in one go, so a stop() is noticed and
        // a floor due sooner than the next attempt is met.
        constexpr int64_t kMaxSleepUs = 100 * 1000;
        // When "still waiting" is said once more in the log, so a session log
        // read afterwards shows a locked screen rather than a hung loop.
        constexpr int64_t kStillWaitingUs = 10 * 1000 * 1000;

        // The picture sent while the screen is away. Made now, on the device
        // the duplication was opened on — start() replaces that device, and a
        // failed start() leaves none at all — and the size of what was being
        // captured, which is what the converter is set up to take. Failing to
        // make one is not fatal: the last picture is re-sent instead, as the
        // idle floor does.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> blank = makeBlank(error);
        if (!blank)
            log::warning("[native] no blank picture for the wait, re-sending the last: " + error);
        bool blankShown = false;

        const int64_t lostUs = steadyNowUs();
        int64_t lastSentUs = 0;
        int64_t nextTryUs = lostUs;
        int failures = 0;
        bool saidStillWaiting = false;
        for (;;) {
            if (!m_Running.load()) return Restart::Stopped;

            int64_t nowUs = steadyNowUs();
            if (nowUs >= nextTryUs) {
                if (m_Capture->start(error)) break;
                failures++;
                nowUs = steadyNowUs();
                nextTryUs = nowUs + restartRetryDelayMs(failures) * 1000;
                if (failures == 1)
                    log::info("[native] display is away (reconfiguring, or locked), waiting for "
                              "it: " +
                              error);
            }
            if (!saidStillWaiting && nowUs - lostUs >= kStillWaitingUs) {
                saidStillWaiting = true;
                log::info("[native] display still away after 10 s, keeping the stream alive "
                          "until it returns");
            }

            // The floor, from the very first failure: the viewer's screen goes
            // dark the moment the host's did, not half a second later.
            if (m_Converter && m_Encoder &&
                (lastSentUs == 0 || nowUs - lastSentUs >= floorIntervalUs)) {
                if (blank && !blankShown) {
                    static const capture::CursorState kNoCursor;
                    if (m_Converter->convert(blank.Get(), kNoCursor, cursorDraw(), error)) {
                        blankShown = true;
                    } else {
                        log::warning("[native] could not draw the blank picture: " + error);
                        blank.Reset();
                    }
                }
                if (!emit(frameNumber, resendStamps(nowUs), error)) return Restart::Ended;
                lastSentUs = nowUs;
                nowUs = steadyNowUs();
            }

            int64_t sleepUs = nextTryUs - nowUs;
            if (lastSentUs != 0 && lastSentUs + floorIntervalUs - nowUs < sleepUs)
                sleepUs = lastSentUs + floorIntervalUs - nowUs;
            if (sleepUs > kMaxSleepUs) sleepUs = kMaxSleepUs;
            if (sleepUs < 1000) sleepUs = 1000;
            std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
        }

        if (failures > 0) {
            char span[32];
            std::snprintf(span, sizeof(span), "%.1f", (steadyNowUs() - lostUs) / 1e6);
            log::info("[native] display is back after " + std::string(span) + " s and " +
                      std::to_string(failures) + " failed attempt" + (failures > 1 ? "s" : ""));
        }

        // buildPipeline() releases the old converter and encoder before it
        // builds the new ones — see there.
        if (!buildPipeline(m_Info.width, m_Info.height, error)) return Restart::Failed;

        // Absolute mouse input is aimed at the display's rectangle on the
        // virtual desktop, and a resolution change is exactly what moves it.
        // Under the lock that guards inject(), which runs on the network thread.
        {
            const capture::DesktopRect& rect = m_Capture->desktopRect();
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) m_Input->setDisplayRect(rect.left, rect.top, rect.right, rect.bottom);
        }

        // The desktop changed size while the frame did not, so the pointer's
        // scale just moved (see CursorUpdate::scale) — and DXGI will not mention
        // the pointer again until its SHAPE changes, which for a cursor sitting
        // still may be never.
        m_ResendCursor.store(true);

        log::info("[native] capture restarted at " + std::to_string(m_Capture->width()) + "x" +
                  std::to_string(m_Capture->height()) + ", streaming " +
                  std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height));
        return Restart::Restarted;
    }

    /// A black picture the size and format of what the capture was delivering,
    /// on the converter's device (the capture's, or the bridge's) so it can be
    /// converted directly. See restartCapture for what it is for.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> makeBlank(std::string& error)
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> blank;
        ID3D11Device* device = pipelineDevice();
        const int width = m_Capture ? m_Capture->width() : 0;
        const int height = m_Capture ? m_Capture->height() : 0;
        if (!device || width <= 0 || height <= 0) {
            error = "the capture has no device to make it on";
            return blank;
        }

        // Zero in every channel: black, whichever of the two 8-bit layouts the
        // duplication used. Alpha is zero too, and the converter ignores it.
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = m_Capture->format();
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        const size_t pitch = static_cast<size_t>(width) * 4;
        std::vector<uint8_t> zeros(pitch * static_cast<size_t>(height), 0);
        D3D11_SUBRESOURCE_DATA initial = {};
        initial.pSysMem = zeros.data();
        initial.SysMemPitch = static_cast<UINT>(pitch);
        if (FAILED(device->CreateTexture2D(&desc, &initial, blank.GetAddressOf()))) {
            error = "the GPU refused a blank picture";
            blank.Reset();
        }
        return blank;
    }

    /// The thread entry point. Nothing may escape it.
    ///
    /// An exception leaving a std::thread calls std::terminate, which aborts
    /// the whole worker PROCESS with no usable dump — 0xC0000409 raised from
    /// inside ucrtbase, past any handler. That is how a single bad frame took
    /// down a session and left nothing to read. Whatever goes wrong, it is
    /// turned into an ended session with a reason.
    void run() noexcept
    {
        // MMCSS "Games": the scheduler gives this thread the same standing as
        // a game's render thread, ahead of the desktop's background work, for
        // the whole life of the session. Refusal (a service session, a policy)
        // is logged once and costs nothing — the loop is the same either way.
        DWORD mmcssTask = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTask);
        if (mmcss)
            log::info("capture thread on MMCSS Games");
        else
            log::info("MMCSS Games refused for the capture thread (error " +
                      std::to_string(GetLastError()) + ") — ordinary priority");
        try {
            runLoop();
            logCadence();
        } catch (const std::exception& e) {
            finish(std::string("the capture loop threw: ") + e.what());
        } catch (...) {
            finish("the capture loop threw an unknown exception");
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    void runLoop()
    {
        // The capture timeout only bounds how long the loop sleeps when nothing
        // is presented; it is not a frame deadline. Short enough to notice a
        // stop() promptly, long enough that an idle desktop costs nothing.
        constexpr int kAcquireTimeoutMs = 100;

        // How long a perfectly still desktop may go without sending anything,
        // when the client has asked for nothing in particular.
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
        //
        // This one only answers "is the stream alive", and it is deliberately
        // the CAPTURE's number rather than the client's: how long a still screen
        // may stay silent is a property of how exactly this platform reports
        // damage. On Desktop Duplication that report is exact — AcquireNextFrame
        // returns on the real present, 0.06 ms after it — so nothing is lost by
        // waiting here, and a client at a desk asks for no more. A platform
        // whose damage signal is vaguer raises this, and no client learns of it.
        //
        // A client that wants MORE than liveness — a game paused with the
        // pointer locked, where the picture is the only channel there is — asks
        // for a faster floor; see setFrameFloorFps.
        constexpr int64_t kIdleFloorUs = 500 * 1000;

        // ── Refining a picture that stopped moving ──────────────────────────
        //
        // The rate control is constant-bitrate and its budget is PER FRAME: a
        // frame may spend one VBV and no more. That bound is right while frames
        // keep coming, because the next one refines what this one could only
        // approximate — nobody ever sees the intermediate state.
        //
        // On a desktop that stops moving, nothing follows. The single frame that
        // carried the change IS the picture, quantized to fit one frame's
        // budget, and it stays that way on screen for as long as the user reads
        // it. That is the softness: not a wrong setting, a refinement that never
        // happened because the scene had no next frame to carry it.
        //
        // So the next frames are supplied, AND they are given something to spend.
        // Two halves, and the first alone does nothing:
        //
        //  - more frames. The converted texture is re-encoded at the stream's
        //    cadence for a short while after the last real capture, so the
        //    encoder codes the residual against its own reconstruction and each
        //    pass adds the detail the previous one had to drop;
        //  - a bigger budget, for exactly as long as that lasts. The measurement
        //    that mattered: after the VBV floor, a 1080p keyframe came out at
        //    112 KB against a 114 KB cap. Pinned, again. Handing the same
        //    ceiling to sixty more frames would have produced sixty more frames
        //    of the same softness — the encoder was never short of chances, it
        //    was short of bits. See encode/RateControl.h.
        //
        // Latency is bounded, not untouched. A VBV bounds how long a frame
        // occupies the link, which protects the frame AFTER it — and while the
        // screen is still, there is none. The moment something moves the
        // ordinary budget is restored BEFORE that frame is encoded. But the
        // passes already handed to the link are still in front of that frame,
        // and a burst sent as fast as the encoder produces it puts the WHOLE
        // burst there: a few hundred kilobytes, which on a 20 Mbps link is a
        // quarter of a second before the first frame of the movement arrives.
        // So a pass goes out only once the link has drained the previous one,
        // by the stream's own rate (LinkOccupancy) — the burst never runs more
        // than one pass ahead, and the boost is sized so that one pass is
        // 50 ms of link at most (kStillBoost). That is the price a badly timed
        // mouse pays, once.
        //
        // Three ways out, whichever comes first: the window, convergence, and
        // anything at all happening on screen.
        constexpr int64_t kRefineWindowUs = 1000 * 1000;
        // How still the screen must be before any of this starts. Without it,
        // the pause between two keystrokes counts as a still screen and the
        // budget is reconfigured twice per character typed. Short enough that a
        // screen someone is actually reading has settled long before they look.
        constexpr int64_t kRefineDelayUs = 150 * 1000;
        constexpr int kRefineMaxFps = 60;
        // "The encoder had nothing left to add" is decided by
        // encode::RefineConvergence — a tiny pass, a QP that stopped falling,
        // or the ceiling on passes — see there for why size alone was wrong.

        const int refineFps =
            (m_Config.fps > 0 && m_Config.fps < kRefineMaxFps) ? m_Config.fps : kRefineMaxFps;
        const int64_t refineIntervalUs = 1000000 / refineFps;
        // The acquire timeout is also the loop's sleep, so it has to be short
        // enough to let a refinement pass be due on time. Outside the window it
        // stays long: an idle desktop must not cost a wake-up every 16 ms.
        const int refineTimeoutMs = static_cast<int>(refineIntervalUs / 1000);

        uint32_t frameNumber = 0;
        std::string error;
        int64_t lastSentUs = steadyNowUs();
        // The last frame that came from an actual capture — what the refinement
        // window is measured from.
        int64_t lastRealUs = lastSentUs;
        encode::RefineConvergence refineConv;
        bool refineDone = false;
        // The QP the picture started the burst at, for the log: first → last
        // is the sharpening the burst bought, in the encoder's own unit.
        int refineFirstQp = -1;
        int refinePasses = 0;
        // Wake-ups at which a pass was due but the link had not drained the
        // previous one. In the log next to the passes: the pair says whether
        // the burst was shaped by convergence or by the link.
        int refineHeld = 0;
        size_t refineBytes = 0;
        size_t refineFirstBytes = 0;
        int refineLogged = 0;

        // The client's floor, as an interval, clamped by the stream's own rate.
        //
        // Recomputed every iteration rather than cached: it changes when the
        // viewer switches mouse mode, and that arrives on another thread.
        //
        // The clamp is the important half. A client asking for 30 fps on a
        // stream the viewer set to 20 must get 20 — the setting is the user's
        // own words about what their link can carry, and a floor is a request
        // from a page. Only a lower interval than the liveness floor is ever
        // taken, so a client cannot ask to be sent LESS than the receiver needs
        // to tell this stream from a dead one.
        auto floorIntervalUs = [this, kIdleFloorUs]() -> int64_t {
            int fps = m_FloorFps.load(std::memory_order_relaxed);
            if (fps <= 0) return kIdleFloorUs;
            if (m_Config.fps > 0 && fps > m_Config.fps) fps = m_Config.fps;
            const int64_t interval = 1000000 / fps;
            return interval < kIdleFloorUs ? interval : kIdleFloorUs;
        };

        // The stream's own bitrate, which the quality ladder may move under us,
        // and whether the still-screen budget is currently in its place.
        // ── Three layers decide what the encoder is told ────────────────────
        //
        //  1. the viewer's CEILING (the setting, moved by the frontend's ladder
        //     through setTargetBitrate);
        //  2. what the LINK can take right now, under it — the governor, fed by
        //     the receiver's reports (encode::RateGovernor). That is `baseKbps`,
        //     the rate the wire really carries and the link model is fed with;
        //  3. the budget PER FRAME for the rate frames really come at
        //     (encode::EffectiveCadence), and the still-screen boost on top.
        //
        // Every change goes through applyBitrate(), so the three never disagree
        // about what the encoder holds.
        encode::RateGovernor governor;
        governor.start(m_Config.bitrateKbps, steadyNowUs() / 1000);
        int baseKbps = governor.targetKbps();
        m_LinkKbps = baseKbps;
        bool boosted = false;
        encode::EffectiveCadence effective;
        effective.start(m_EncodeFps, steadyNowUs());
        auto applyBitrate = [&](int kbps) {
            if (kbps <= 0) return;
            if (!m_Encoder->setBitrate(effective.scaledKbps(kbps), error))
                log::warning("[native] bitrate change refused: " + error);
        };
        int cadenceLogged = 0;
        int governorLogged = 0;
        // The governor moved the wire's rate: the link model and whatever the
        // encoder currently holds (boosted or not) follow.
        auto applyGovernor = [&](const char* why) {
            baseKbps = governor.targetKbps();
            m_LinkKbps = baseKbps;
            applyBitrate(boosted ? encode::stillBitrateKbps(baseKbps) : baseKbps);
            if (governorLogged < 10 || governor.changes() % 10 == 0) {
                governorLogged++;
                log::info("[native] link: " + std::string(why) + " — encoding at " +
                          std::to_string(baseKbps) + " kbps of the " +
                          std::to_string(governor.settingKbps()) + " set");
            }
        };

        // The first few bursts, then silence. These are the numbers that say
        // whether any of this worked: what one frame's budget bought, against
        // what the picture actually converged to, and how often the link — not
        // the encoder — set the pace. More than a handful would be noise:
        // bursts happen every time the screen settles.
        //
        // Every way out is logged, not only convergence. A burst that ran to
        // the end of its window without ever going quiet used to leave no
        // trace at all — which is how a 55 Mbps stream spent 3 MB on every
        // pause of the mouse for weeks without anyone knowing — and a log that
        // only reports success is not a log of what happened.
        auto closeBurst = [&](const char* how) {
            if (refinePasses == 0) return;
            if (refineLogged < 3) {
                refineLogged++;
                std::string qp;
                if (refineFirstQp >= 0 && m_LastEmitQp >= 0)
                    qp = ", QP " + std::to_string(refineFirstQp) + " -> " +
                         std::to_string(m_LastEmitQp);
                log::info(
                    "[native] still picture refined: " + std::to_string(refineFirstBytes / 1024) +
                    " KB + " + std::to_string(refineBytes / 1024) + " KB over " +
                    std::to_string(refinePasses) + " passes, " + std::to_string(refineHeld) +
                    " held for the link" + qp + " (" + how + ")");
            }
            refinePasses = 0;
        };

        // A new picture is on the wire: whatever burst was running is over, and
        // the next one starts from this picture's own figures.
        auto resetBurst = [&]() {
            refineConv.reset();
            refineDone = false;
            refinePasses = 0;
            refineHeld = 0;
            refineBytes = 0;
            refineFirstBytes = m_LastEmitBytes;
            refineFirstQp = m_LastEmitQp;
        };

        // A picture changed — a present, or the pointer moving over a still
        // desktop — and went out. The refinement window is measured from here.
        auto noteReal = [&]() {
            closeBurst("screen moved");
            lastSentUs = steadyNowUs();
            lastRealUs = lastSentUs;
            resetBurst();
        };

        // ── Holding the loop to the stream's rate ───────────────────────────
        //
        // The display presents at its own refresh rate and the stream has a
        // rate of its own; when the display is faster, only the FIRST present
        // at or after each tick of the stream's grid is encoded — the moment
        // it arrives, never later. The others are converted all the same (the
        // converter's output texture is what the still-screen paths re-send,
        // so it has to be the freshest picture) but not encoded. Nothing ever
        // waits on this thread for a tick: a picture held on the host is
        // latency the viewer feels. The reasoning is on FrameCadence.
        auto emitPicture = [&](const FrameStamps& stamps) -> bool {
            if (!m_Cadence.admit(stamps.convertedUs)) return true;
            if (!emit(frameNumber, stamps, error)) return false;
            noteReal();
            // A real picture went out: one more frame in this second's count.
            // When the second closes on a different rate, the encoder's budget
            // follows — through the same setBitrate() the boost and the ladder
            // use, so the three never disagree about what the encoder holds.
            if (effective.noteFrame(steadyNowUs())) {
                applyBitrate(boosted ? encode::stillBitrateKbps(baseKbps) : baseKbps);
                if (cadenceLogged < 5 || effective.changes % 30 == 0) {
                    cadenceLogged++;
                    log::info("[native] frames arrive at " + std::to_string(effective.currentFps) +
                              " fps for a " + std::to_string(effective.configuredFps) +
                              " fps stream — encoder budget " +
                              (effective.scaling()
                                   ? std::to_string(effective.scaledKbps(baseKbps)) +
                                         " kbps per second of frames (" + std::to_string(baseKbps) +
                                         " on the wire)"
                                   : std::string("back to ") + std::to_string(baseKbps) + " kbps"));
                }
            }
            return true;
        };

        m_LoopStartUs = steadyNowUs();
        while (m_Running.load()) {
            if (const int kbps = m_PendingBitrate.exchange(0); kbps > 0) {
                // The ladder moves the CEILING, not the still-screen budget.
                // Applying it while boosted would drop the burst back to normal
                // mid-refinement; the boost is recomputed from the new base
                // instead, and the base takes over when the burst ends.
                governor.setSetting(kbps);
                applyGovernor("ceiling moved");
            }

            // The receiver's word on the link, and the silence of it.
            {
                LinkFeedback fb;
                const int64_t nowMs = steadyNowUs() / 1000;
                if (takeLinkFeedback(fb)) {
                    if (governor.report(fb, nowMs))
                        applyGovernor(fb.gaps > 0 || fb.evictions > 0 ? "frames lost"
                                      : fb.owdRiseMs >= encode::RateGovernor::kOveruseMs
                                          ? "delay rising"
                                          : "quiet, raising");
                } else if (governor.tick(nowMs)) {
                    applyGovernor("no report from the receiver");
                }
            }

            // Decided before the acquire because it also chooses how long the
            // acquire may sleep. `refineSoon` keeps the loop responsive through
            // the settling delay as well, so a pass is not up to 100 ms late.
            const int64_t sinceRealUs = steadyNowUs() - lastRealUs;
            const bool refineSoon = sinceRealUs < (kRefineDelayUs + kRefineWindowUs) &&
                                    !refineDone && m_Converter->outputWidth() != 0;
            const bool refining = refineSoon && sinceRealUs >= kRefineDelayUs;
            // The window ran out on a burst still going: say so, once.
            if (!refineSoon && !refineDone) closeBurst("window closed");

            // The acquire timeout is the loop's sleep, so a floor faster than
            // it would simply never be met: at 15 fps the frame is due every
            // 66 ms and a 100 ms sleep delivers 10. It only ever shortens the
            // wait — a client that asked for nothing still sleeps the full
            // 100 ms on an idle desktop.
            const int64_t idleIntervalUs = floorIntervalUs();
            const int idleTimeoutMs = static_cast<int>(idleIntervalUs / 1000) < kAcquireTimeoutMs
                                          ? static_cast<int>(idleIntervalUs / 1000)
                                          : kAcquireTimeoutMs;
            const int timeoutMs = refineSoon ? refineTimeoutMs : idleTimeoutMs;

            capture::CapturedFrame frame;
            const capture::AcquireStatus status = m_Capture->acquire(timeoutMs, frame);

            // Anything but a timeout means the screen is alive again, and the
            // frame about to be encoded is a moving one. Restore the ordinary
            // budget BEFORE it is encoded, never after: that ordering is the
            // whole reason the boost costs no latency.
            if (status != capture::AcquireStatus::Timeout && boosted) {
                boosted = false;
                applyBitrate(baseKbps);
            }

            // Before the status is acted on, and on EVERY status. A client that
            // has just taken over drawing needs to be told what the pointer
            // looks like — including "there is none here" — and on a still
            // screen with the mouse on another display every single wake-up is
            // a timeout, so anything gated behind a frame would never run.
            reportCursor();

            if (status == capture::AcquireStatus::Timeout) {
                // Nothing moved on the desktop — but the pointer we draw onto it
                // is about to be drawn at a different size, and nothing else in
                // this loop would ever notice. A viewer pinch-zooming a still
                // screen is exactly that: no present, no pointer motion, and a
                // cursor that has to resize anyway.
                if (m_CursorDirty.exchange(false) && m_CompositeCursor.load() && m_DesktopCopy) {
                    ID3D11Texture2D* picture = pictureFor(m_DesktopCopy.Get(), error);
                    if (!picture ||
                        !m_Converter->convert(picture, m_Capture->cursor(), cursorDraw(), error)) {
                        finish("colour conversion failed: " + error);
                        return;
                    }
                    if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                    noteReal();
                    continue;
                }

                // Nothing moved. Two reasons to send anyway: the refinement
                // passes above, and — once those are done — the floor, which is
                // either "prove the stream is alive" or whatever faster rate the
                // client asked to keep settling at. Both re-encode what is
                // already converted, so this costs an encode and not a capture.
                if (!m_Converter->outputWidth()) continue;
                if (steadyNowUs() - lastSentUs < (refining ? refineIntervalUs : idleIntervalUs))
                    continue;

                // A pass waits for the link, never the other way round: the
                // previous pass — or the last frames of the movement that just
                // ended — must have left before another few hundred kilobytes
                // are put behind them. Held, not skipped: the window keeps
                // running and the next wake-up asks again. The liveness floor
                // is not gated (a still frame at the ordinary budget is a few
                // hundred bytes), and a boosted pass is at most 50 ms of link,
                // so the hold can never reach the floor's 500 ms.
                if (refining && !m_Link.drainedAt(steadyNowUs())) {
                    refineHeld++;
                    continue;
                }

                // The budget goes up before the first pass, not after it: the
                // whole point is that this frame is the one that gets to spend.
                if (refining && !boosted) {
                    boosted = true;
                    applyBitrate(encode::stillBitrateKbps(baseKbps));
                }

                if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                lastSentUs = steadyNowUs();

                if (!refining) continue;
                refinePasses++;
                refineBytes += m_LastEmitBytes;
                switch (refineConv.notePass(m_LastEmitBytes, m_LastEmitQp)) {
                case encode::RefineConvergence::Verdict::Continue: break;
                case encode::RefineConvergence::Verdict::Converged:
                    refineDone = true;
                    closeBurst("converged");
                    break;
                case encode::RefineConvergence::Verdict::Capped:
                    refineDone = true;
                    closeBurst("pass cap");
                    break;
                }
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
                m_CursorDirty.store(false);
                const int64_t submittedUs = steadyNowUs();
                ID3D11Texture2D* picture = pictureFor(m_DesktopCopy.Get(), error);
                if (!picture ||
                    !m_Converter->convert(picture, m_Capture->cursor(), cursorDraw(), error)) {
                    finish("colour conversion failed: " + error);
                    return;
                }
                // No present of its own — the picture changed when the pointer
                // did, so that is its t₀. A picture like any other from here:
                // gated by the cadence if one is running, and when it goes out
                // a new refinement window opens, because a pointer that stops
                // moving leaves a composited frame that deserves sharpening
                // exactly like any other.
                if (!emitPicture(FrameStamps{submittedUs, submittedUs, submittedUs, steadyNowUs()}))
                    return;
                continue;
            }

            if (status == capture::AcquireStatus::Lost) {
                // A mode change, a resolution change, a desktop switch, a
                // locked screen: the whole chain behind the duplication goes
                // with it, and the loop lives inside restartCapture until the
                // display is back (see there).
                switch (restartCapture(frameNumber, floorIntervalUs(), error)) {
                case Restart::Restarted: break;
                case Restart::Ended: return;
                case Restart::Stopped:
                    finish("the session was stopped while the display was away");
                    return;
                case Restart::Failed: finish("capture could not be restarted: " + error); return;
                }
                // Whatever the client had is now stale: the encoder is a new one
                // and has no reference frames, and the picture it is about to
                // send is a different desktop.
                m_ForceKeyframe.store(true);
                // The bitrate lives in the encoder that was just replaced, so
                // the ladder's last word has to be said again — and the still
                // boost, if it was in place, went with the old encoder.
                boosted = false;
                applyBitrate(baseKbps);
                // A new picture deserves its own refinement window rather than
                // whatever the old one had reached.
                closeBurst("display lost");
                lastRealUs = steadyNowUs();
                resetBurst();
                continue;
            }

            if (status != capture::AcquireStatus::Ok) {
                finish("capture failed");
                return;
            }

            m_PresentsSeen++;
            const int64_t submittedUs = steadyNowUs();

            // An empty state draws nothing: that is how the client-drawn mode
            // keeps the picture clean.
            static const capture::CursorState kNoCursor;
            const bool composite = m_CompositeCursor.load();
            m_CursorDirty.store(false);
            // Through the bridge first when the encoder is on another GPU;
            // the texture itself otherwise. Counted in the convert stage.
            ID3D11Texture2D* picture = pictureFor(frame.texture, error);
            if (!picture ||
                !m_Converter->convert(picture, composite ? m_Capture->cursor() : kNoCursor,
                                      cursorDraw(), error)) {
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

            // t₂ once the capture is given back, so the stage reads as the
            // whole of what stands between the acquire and the encoder.
            if (!emitPicture(
                    FrameStamps{frame.presentUs, frame.capturedUs, submittedUs, steadyNowUs()}))
                return;
        }
    }

    /// Encode whatever the converter currently holds and hand it to the
    /// consumer. @p stamps are the picture's own t₀..t₂ — a capture's, or
    /// "now" for a re-send (idle floor, refinement) that has no present of
    /// its own; see resendStamps.
    ///
    /// Returns false when the session must end; the reason has been reported.
    bool emit(uint32_t& frameNumber, const FrameStamps& stamps, std::string& error)
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

        m_LastEmitBytes = encoded.size;
        m_LastEmitQp = encoded.avgQp;
        m_Link.sent(steadyNowUs(), encoded.size, m_LinkKbps);

        if (encoded.data && encoded.size > 0 && m_Callbacks.onVideo) {
            EncodedFrame out;
            out.data = encoded.data;
            out.size = encoded.size;
            out.keyframe = encoded.keyframe;
            out.frameNumber = frameNumber++;
            out.avgQp = encoded.avgQp;
            out.presentUs = stamps.presentUs;
            out.capturedUs = stamps.capturedUs;
            out.submittedUs = stamps.submittedUs;
            out.convertedUs = stamps.convertedUs;
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
        // The KIND is checked too, not just the shape version. An application
        // can swap between two standard cursors without DXGI ever handing over
        // a new bitmap — it caches shapes it has already sent — so a client
        // following the name alone would never see the change.
        const char* kind = currentCursorKind();
        const bool kindChanged = std::strcmp(kind, m_ReportedKind.c_str()) != 0;
        // And the scale, which changes without the shape doing anything at all:
        // the host switching display mode resizes the desktop under a pointer
        // that keeps its bitmap. See CursorUpdate::scale.
        const float scale = cursorScale();
        const bool scaleChanged = scale != m_ReportedScale;
        if (!forced && !kindChanged && !scaleChanged && cursor.shapeVersion == m_ReportedShape &&
            cursor.visible == m_ReportedVisible)
            return;

        m_ReportedShape = cursor.shapeVersion;
        m_ReportedVisible = cursor.visible;
        m_ReportedKind = kind;
        m_ReportedScale = scale;

        CursorUpdate update;
        update.visible = cursor.visible;
        update.kind = kind;
        update.width = cursor.width;
        update.height = cursor.height;
        update.scale = scale;
        // The capture stores the image's top-left, having already subtracted
        // the hotspot; the client needs the offset itself to place the image
        // against its own pointer.
        update.hotspotX = m_Capture->cursorHotspotX();
        update.hotspotY = m_Capture->cursorHotspotY();

        // Inverting pixels are flattened here rather than in the capture,
        // because the composited path genuinely inverts and must keep the
        // information. See CursorUpdate::pixels for why white-on-black.
        if (!cursor.pixels.empty() && cursor.width > 0 && cursor.height > 0) {
            m_CursorScratch = cursor.pixels;
            flattenInvert(cursor);
            update.pixels = m_CursorScratch.data();
        }

        m_Callbacks.onCursor(update);
    }

    /// How much the converter shrinks (or stretches) the desktop on its way into
    /// the frame — which is exactly what the pointer bitmap must be multiplied
    /// by. See CursorUpdate::scale.
    ///
    /// Width only, matching the client's own convention: the two rectangles
    /// share an aspect ratio in every case a client can ask for, and taking one
    /// axis avoids a pointer that is a different shape from the one the host is
    /// showing.
    float cursorScale() const
    {
        const int captured = m_Capture ? m_Capture->width() : 0;
        const int framed = m_Converter ? m_Converter->outputWidth() : 0;
        if (captured <= 0 || framed <= 0) return 1.0f;
        return static_cast<float>(framed) / static_cast<float>(captured);
    }

    /// How much bigger than life to draw the composited pointer, so it comes out
    /// the width the client asked for.
    ///
    /// The client asks in frame pixels because that is the only unit both sides
    /// can compute — it knows how many of them fit across the viewer's screen,
    /// we know how many of them the pointer currently covers. Nobody has to
    /// agree on a cursor size, a DPI or a phone.
    ///
    /// Never below 1: the request exists to make a pointer visible on a small
    /// screen, and a viewer zoomed in far enough that the natural size already
    /// exceeds the target wants the natural size, not a shrunken one. Capped
    /// because this is a 32-pixel bitmap being stretched — past 4× it stops
    /// looking like a pointer and starts looking like a bug.
    float cursorMagnification() const
    {
        const int wanted = m_CursorFramePx.load();
        if (wanted <= 0 || !m_Capture) return 1.0f;
        // The drawn extent, not the canvas: Windows pads the same arrow into
        // 32, 48 or 64-pixel buffers depending on where it came from, and sizing
        // on the buffer made the pointer change size every time the shape
        // changed hands. The LONGER side of the ink, so a tall I-beam and a
        // wide resize arrow both come out the size the client asked for rather
        // than the narrow one being blown up. See CursorState::inkWidth.
        const capture::CursorState& shape = m_Capture->cursor();
        const int ink = shape.inkWidth > shape.inkHeight ? shape.inkWidth : shape.inkHeight;
        const int shapeWidth = ink > 0 ? ink : shape.width;
        if (shapeWidth <= 0) return 1.0f;
        const float natural = static_cast<float>(shapeWidth) * cursorScale();
        if (!(natural > 0.0f)) return 1.0f;
        const float magnify = static_cast<float>(wanted) / natural;
        if (!(magnify > 1.0f)) return 1.0f;
        return magnify > kMaxCursorMagnify ? kMaxCursorMagnify : magnify;
    }

    /// Everything the converter needs about the pointer that the capture does
    /// not already tell it.
    convert::CursorDraw cursorDraw() const
    {
        convert::CursorDraw draw;
        draw.magnify = cursorMagnification();
        if (m_Capture) {
            draw.hotspotX = m_Capture->cursorHotspotX();
            draw.hotspotY = m_Capture->cursorHotspotY();
        }
        return draw;
    }

    /// Turn the inverting pixels in m_CursorScratch into something a client can
    /// draw: white fill, black outline.
    ///
    /// An inverting pixel says "show the opposite of whatever is behind me",
    /// which is how one bare stroke stays legible on a white page and on a dark
    /// text field. No image format can say that, so it has to be resolved to
    /// fixed colours — and a single colour cannot work on both backgrounds,
    /// which is what made a black I-beam disappear into a dark input.
    ///
    /// The outline is traced only into pixels the cursor left fully transparent,
    /// so an ordinary coloured cursor that happens to carry a few inverting
    /// pixels keeps its own artwork intact. Nothing is written outside the
    /// bitmap: an outline pixel that would fall off the edge is simply not
    /// drawn, which costs a sliver of a shape that already reaches the border.
    void flattenInvert(const capture::CursorState& cursor)
    {
        const int w = cursor.width;
        const int h = cursor.height;
        if (cursor.invert.size() != static_cast<size_t>(w) * static_cast<size_t>(h)) return;

        auto paint = [this](size_t i, uint8_t v) {
            m_CursorScratch[i * 4 + 0] = v;
            m_CursorScratch[i * 4 + 1] = v;
            m_CursorScratch[i * 4 + 2] = v;
            m_CursorScratch[i * 4 + 3] = 0xFF;
        };

        // The outline reads the ORIGINAL alpha, so it must be traced before the
        // fill overwrites it — hence two passes over the same buffer rather than
        // one that would outline the pixels it just painted.
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (cursor.invert[i]) continue;
                if (m_CursorScratch[i * 4 + 3] != 0) continue; // the cursor's own pixel

                bool touches = false;
                for (int dy = -1; dy <= 1 && !touches; ++dy) {
                    for (int dx = -1; dx <= 1 && !touches; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        touches = cursor.invert[static_cast<size_t>(ny) * w + nx] != 0;
                    }
                }
                if (touches) paint(i, 0x00);
            }
        }

        for (size_t i = 0; i < cursor.invert.size(); ++i)
            if (cursor.invert[i]) paint(i, 0xFF);
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
    /// One line at the end: what the display produced against what the stream
    /// carried. The number that says whether the cadence did its job — and,
    /// on a display faster than the stream, how much the wire was spared.
    void logCadence()
    {
        if (m_PresentsSeen == 0) return;
        const double seconds = (steadyNowUs() - m_LoopStartUs) / 1e6;
        char span[32];
        std::snprintf(span, sizeof(span), "%.1f", seconds);
        std::string line = "[native] cadence: " + hzString(m_DisplayMilliHz) + " Hz display, " +
                           std::to_string(m_EncodeFps) + " fps stream — " +
                           std::to_string(m_PresentsSeen) + " presents in " + span + " s";
        if (m_Cadence.enabled()) {
            const int64_t skipped = m_Cadence.skipped();
            const int perSecond =
                seconds > 0 ? static_cast<int>(static_cast<double>(skipped) / seconds + 0.5) : 0;
            line += ", " + std::to_string(skipped) + " not carried (" + std::to_string(perSecond) +
                    "/s)";
        } else {
            line += ", every one carried";
        }
        log::info(line);

        // What the bridge cost, when there was one: the figure that says how
        // much of this session's convert stage was the copy and not the pass.
        if (m_Bridge && m_Bridge->transfers() > 0) {
            char mean[32], peak[32];
            std::snprintf(mean, sizeof(mean), "%.2f", m_Bridge->meanUs() / 1000.0);
            std::snprintf(peak, sizeof(peak), "%.2f", m_Bridge->maxUs() / 1000.0);
            log::info("[native] cross-GPU copy: " + std::to_string(m_Bridge->transfers()) +
                      " frames of " + std::to_string(m_Bridge->bytesPerFrame() / (1024 * 1024)) +
                      " MB, " + mean + " ms mean, " + peak + " ms max");
        }
    }

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

    /// The rate the encoder's budget is dimensioned for — the setting, or the
    /// display's own refresh when the setting is 0. See start().
    int m_EncodeFps = 0;
    int m_DisplayMilliHz = 0;
    /// Holds the loop to the stream's rate; disabled at fps 0. Owned by the
    /// capture thread once the loop runs.
    FrameCadence m_Cadence{0};
    /// Presents the display delivered (AcquireStatus::Ok), for the log.
    int64_t m_PresentsSeen = 0;
    int64_t m_LoopStartUs = 0;

    std::unique_ptr<capture::DxgiDuplication> m_Capture;
    /// Present only when the encoder sits on another GPU than the display's:
    /// carries every frame across, and owns the device the converter and the
    /// encoder are then built on. See CrossGpuBridge.
    std::unique_ptr<CrossGpuBridge> m_Bridge;
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

    /// Optional too: the host's playback, captured and encoded on its own
    /// thread. Null when the consumer asked for none or no device could open.
    std::unique_ptr<audio::WasapiLoopback> m_Audio;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ForceKeyframe{true};
    /// True — the default — draws the pointer into the picture. False reports
    /// its shape to the client, which draws it itself at its own refresh rate.
    std::atomic<bool> m_CompositeCursor{true};
    /// How wide the client wants the composited pointer, in frame pixels; 0 for
    /// the size it has on the desktop. See Session::setCompositeCursor.
    std::atomic<int> m_CursorFramePx{0};
    /// The rate the client wants kept up on a screen that is not moving, in
    /// fps; 0 for the loop's own liveness floor. See setFrameFloorFps.
    std::atomic<int> m_FloorFps{0};
    /// The composited pointer must be redrawn although nothing on the desktop
    /// moved — its requested size changed under a still screen.
    std::atomic<bool> m_CursorDirty{false};
    /// Forces one cursor report even though DXGI says the shape is unchanged.
    std::atomic<bool> m_ResendCursor{false};
    /// The shape the client has been told about, so an unchanged pointer is not
    /// re-sent on every frame.
    uint64_t m_ReportedShape = 0;
    bool m_ReportedVisible = false;
    std::string m_ReportedKind;
    /// Deliberately not 1: the first report must go out whatever the scale is,
    /// and a sentinel that no ratio can equal is what guarantees it.
    float m_ReportedScale = 0.0f;
    bool m_LoggedFirstKeyframe = false;
    /// Bytes the last emit() produced. The refinement loop reads it to know
    /// when a still picture has stopped improving.
    size_t m_LastEmitBytes = 0;
    /// The average QP the encoder reported for the last emit(), -1 when it
    /// reports none. The refinement loop's second witness of convergence.
    int m_LastEmitQp = -1;
    /// The flattened image handed to the client. Reused so a shape change does
    /// not allocate on the capture thread.
    std::vector<uint8_t> m_CursorScratch;
    /// Zero means "no change pending". Exchanged by the loop each iteration.
    std::atomic<int> m_PendingBitrate{0};
    /// The receiver's latest report on the link, folded until the loop takes
    /// it — see reportLink().
    std::mutex m_LinkMutex;
    LinkFeedback m_LinkFeedback;
    bool m_LinkPending = false;
    /// What the link is still carrying, by the stream's own rate. Fed by every
    /// emit(); read by the refinement burst, which does not send a pass into a
    /// link that has not finished with the previous one. See RateControl.h.
    encode::LinkOccupancy m_Link;
    /// The rate the link is modelled at: the stream's bitrate, never the
    /// still-screen boost — that one is what is being paced, not the pipe.
    int m_LinkKbps = 0;
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
