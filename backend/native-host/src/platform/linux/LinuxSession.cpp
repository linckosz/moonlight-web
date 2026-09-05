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

#include "../../capture/linux/KmsCapture.h"
#include "../../convert/linux/GlConvert.h"
#include "../../core/CadenceAlign.h"
#include "../../core/FrameCadence.h"
#include "../../core/Log.h"
#include "../../core/RestartBackoff.h"
#include "../../core/Session.h"
#include "../../encode/RateControl.h"
#include "../../encode/RateGovernor.h"
#include "../../encode/linux/VaapiEncoder.h"
#include "../../input/linux/UinputGamepad.h"
#include "../../input/linux/UinputInput.h"
#if defined(MW_NATIVE_LINUX_AUDIO)
#include "../../audio/PacedOpusSink.h"
#include "../../audio/linux/PipeWireCapture.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The Linux capture → encode → deliver pipeline.
//
// A port of WindowsSession, stage for stage: the same single thread with no
// queue, the same cadence gate, the same still-screen floor and refinement
// burst, the same three-layer bitrate (ceiling → link governor → per-frame
// budget), the same restart on a lost display. Where the two differ it is
// because the platform does — and each difference is marked where it lives:
//
//  - the picture the pointer-only path re-converts is the LAST KMS BUFFER,
//    held by its fd, not a copy (KmsCapture::acquire);
//  - the encoder owns the NV12 surface and the converter renders into it
//    (VaapiEncoder::inputTarget), the reverse of D3D11;
//  - no reference invalidation and no intra-refresh on radeonsi 23.2: a lost
//    frame costs a keyframe, and SessionInfo says so;
//  - the sound comes from PipeWire (the default output's monitor), a push
//    source like ScreenCaptureKit's tap, so it goes through PacedOpusSink
//    rather than owning its thread the way WASAPI does — and it is built only
//    where libpipewire is (MW_NATIVE_LINUX_AUDIO);
//  - no HDR.
//
// See §19 of docs/design/native-capture-encoder.md.

namespace mw::native {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct FrameStamps
{
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
    int64_t submittedUs = 0;
    int64_t convertedUs = 0;
};

FrameStamps resendStamps(int64_t nowUs)
{
    return FrameStamps{nowUs, nowUs, nowUs, nowUs};
}

std::string hzString(int milliHz)
{
    return std::to_string((milliHz + 500) / 1000);
}

constexpr int kMaxFloorFps = 480;

class LinuxSession final : public Session
{
public:
    LinuxSession(const SessionConfig& config, const ResolvedTarget& target,
                 const SessionCallbacks& callbacks)
        : m_Config(config)
        , m_Target(target)
        , m_Callbacks(callbacks)
    {}

    ~LinuxSession() override { stop(); }

    bool start(std::string& error) override
    {
        if (m_Running.load()) return true;

        // The Selector hands over the card (nativeHandle = its minor number)
        // and the display's index among that card's connected connectors —
        // the same contract DXGI's "output index within its adapter" fills on
        // Windows. Resolved back to a connector id here.
        m_CardPath = "/dev/dri/card" + std::to_string(m_Target.captureAdapterHandle);
        {
            std::string listError;
            unsigned index = 0;
            bool found = false;
            for (const capture::KmsOutput& out :
                 capture::KmsCapture::listOutputs(m_CardPath, listError)) {
                if (!out.connected) continue;
                if (index == m_Target.outputIndex) {
                    m_ConnectorId = out.connectorId;
                    m_ConnectorName = out.name;
                    found = true;
                    break;
                }
                ++index;
            }
            if (!found) {
                error = "the display is no longer connected to " + m_CardPath +
                        (listError.empty() ? "" : " (" + listError + ")");
                return false;
            }
        }

        if (!openCapture(error)) return false;
        m_DisplayMilliHz = m_Capture->refreshMilliHz();

        {
            std::string line;
            m_EncodeFps =
                chooseCadence(m_Config.clientRefreshMilliHz, m_Config.clientVsync, m_Cadence, line);
            m_CadenceFps = m_EncodeFps;
            log::info(line);
        }

        if (!buildPipeline(m_Config.width, m_Config.height, error)) return false;

        // Input: keyboard and mouse through uinput, the gamepad beside them.
        // Either refusing is "no input of that kind this session", never no
        // session — the udev rule is what grants both, and its absence is said
        // in words a user can act on.
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            auto sink = std::make_unique<input::UinputInput>();
            std::string inputError;
            if (sink->start(inputError)) {
                const capture::DesktopRect rect = m_Capture->desktopRect();
                sink->setDisplayRect(rect.left, rect.top, rect.right, rect.bottom);
                m_Input = std::move(sink);
            } else {
                log::warning("[native] input: keyboard and mouse unavailable — " + inputError);
            }
            auto pads = std::make_unique<input::UinputGamepad>([this](const RumbleEvent& rumble) {
                if (m_Callbacks.onRumble) m_Callbacks.onRumble(rumble);
            });
            std::string padError;
            if (pads->start(padError))
                m_Gamepad = std::move(pads);
            else
                log::info("[native] input: no virtual gamepad this session — " + padError);
        }

#if defined(MW_NATIVE_LINUX_AUDIO)
        // Audio, on the same terms as input: wanted only when the consumer
        // gave us somewhere to put it, and never a reason to fail the session.
        // The sink first — it owns the encoder and the 5 ms cadence — then the
        // capture that feeds it. A daemon that is there but has no output to
        // record is the capture's business (it retries); no daemon at all is
        // "no audio this session", said here.
        if (m_Callbacks.onAudio) {
            auto sink = std::make_unique<audio::PacedOpusSink>(m_Callbacks.onAudio);
            std::string audioError;
            if (!sink->start("PipeWire, the default output's monitor, 48 kHz stereo", audioError)) {
                log::warning("[native] audio unavailable, streaming silent: " + audioError);
            } else {
                auto* raw = sink.get();
                auto tap = std::make_unique<audio::PipeWireCapture>(
                    [raw](const float* pcm, size_t frames) { raw->push(pcm, frames); });
                if (tap->start(audioError)) {
                    m_Audio = std::move(sink);
                    m_AudioTap = std::move(tap);
                } else {
                    log::warning("[native] audio unavailable, streaming silent: " + audioError);
                }
            }
        }
#endif

        m_Info = SessionInfo{};
        m_Info.displayId = m_Target.displayId;
        m_Info.width = m_Converter->outputWidth();
        m_Info.height = m_Converter->outputHeight();
        m_Info.fps = m_Config.fps;
        m_Info.codec = m_Target.codec;
        m_Info.encoder = m_Target.encoder;
        m_Info.capture = CaptureApi::Kms;
        m_Info.gpuName = m_Target.encodeGpuName;
        m_Info.hdr = false;
        m_Info.yuv444 = false;
        m_Info.intraRefresh = m_Encoder->intraRefreshEnabled();
        m_Info.intraRefreshFrames = m_Encoder->intraRefreshFrames();
        m_Info.referenceInvalidation = false;
        // The scanout buffer is read in place and the encoder's surface is
        // written in place: one copy remains, the bitstream leaving VRAM.
        m_Info.copiesPerFrame = 1;
        m_Info.crossGpuCopy = false;
#if defined(MW_NATIVE_LINUX_AUDIO)
        m_Info.audio = static_cast<bool>(m_Audio);
#else
        m_Info.audio = false;
#endif

        log::info(std::string("[native] session: ") + m_ConnectorName + " " +
                  std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + "@" +
                  std::to_string(m_EncodeFps) + " " + toString(m_Info.codec) + " via VA-API on " +
                  m_Info.gpuName + " — KMS → EGL → VA-API, 1 copy (the bitstream)" +
                  (m_Info.audio ? ", with the host's audio (PipeWire, 48 kHz stereo)" : ""));

        m_Running.store(true);
        m_Thread = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        const bool wasRunning = m_Running.exchange(false);
        if (m_Thread.joinable()) {
            if (std::this_thread::get_id() == m_Thread.get_id())
                m_Thread.detach();
            else
                m_Thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) m_Input->stop();
            m_Input.reset();
            if (m_Gamepad) m_Gamepad->stop();
            m_Gamepad.reset();
        }
#if defined(MW_NATIVE_LINUX_AUDIO)
        // The tap goes first: tearing it down is what guarantees no sample
        // callback is still in flight when the sink it pushes into is freed.
        m_AudioTap.reset();
        m_Audio.reset();
#endif
        if (!wasRunning && !m_Encoder && !m_Capture) return;
        m_Encoder.reset();
        m_Converter.reset();
        m_Capture.reset();
    }

    const SessionInfo& info() const override { return m_Info; }

    void sendInput(const InputEvent& event) override
    {
        // On the caller's thread, never the capture thread's (§8).
        std::lock_guard<std::mutex> lock(m_InputMutex);
        using Type = InputEvent::Type;
        switch (event.type) {
        case Type::ControllerArrival:
            if (m_Gamepad) m_Gamepad->arrive(event);
            break;
        case Type::ControllerState:
            if (m_Gamepad) m_Gamepad->update(event);
            break;
        case Type::ControllerRemoval:
            if (m_Gamepad) m_Gamepad->remove(event);
            break;
        default:
            if (m_Input) m_Input->inject(event);
            break;
        }
    }

    void setCompositeCursor(bool composite, int cursorFramePx) override
    {
        const int wanted = cursorFramePx > 0 ? cursorFramePx : 0;
        if (m_CursorFramePx.exchange(wanted) != wanted && composite) m_CursorDirty.store(true);
        if (m_CompositeCursor.exchange(composite) == composite) return;
        log::info(composite ? "[native] cursor: drawn into the picture (gaming)"
                            : "[native] cursor: handed to the client to draw (desktop)");
        m_ResendCursor.store(true);
        if (composite) m_ForceKeyframe.store(true);
    }

    void setFrameFloorFps(int fps) override
    {
        if (fps < 0) fps = 0;
        if (fps > kMaxFloorFps) fps = kMaxFloorFps;
        if (m_FloorFps.exchange(fps) == fps) return;
        log::info("[native] still-screen floor: " +
                  (fps > 0 ? std::to_string(fps) + " fps" : std::string("the engine's own")));
    }

    void requestKeyframe() override { m_ForceKeyframe.store(true); }

    void invalidateReference(uint32_t frameNumber) override
    {
        // No reference invalidation through VA-API yet: the receiver's lost
        // frame costs a keyframe, which is what SessionInfo promised it.
        (void)frameNumber;
        m_ForceKeyframe.store(true);
    }

    void setTargetBitrate(int kbps) override { m_PendingBitrate.store(kbps); }

    void reportLink(const LinkFeedback& feedback) override
    {
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

    void setClientRefresh(int milliHz, bool vsync) override
    {
        if (milliHz < 1000) milliHz = 0;
        if (milliHz > 1000000) milliHz = 1000000;
        const bool same =
            m_ClientMilliHz.exchange(milliHz) == milliHz && m_ClientVsync.exchange(vsync) == vsync;
        if (same) return;
        m_ClientRefreshDirty.store(true);
    }

private:
    bool takeLinkFeedback(LinkFeedback& out)
    {
        std::lock_guard<std::mutex> lock(m_LinkMutex);
        if (!m_LinkPending) return false;
        out = m_LinkFeedback;
        m_LinkPending = false;
        return true;
    }

    bool openCapture(std::string& error)
    {
        m_Capture = std::make_unique<capture::KmsCapture>(m_CardPath, m_ConnectorId);
        return m_Capture->start(error);
    }

    /// Converter and encoder against what the capture is handing out right
    /// now. The encoder first: it owns the surface the converter renders into.
    bool buildPipeline(int outputWidth, int outputHeight, std::string& error)
    {
        m_Encoder.reset();
        m_Converter.reset();

        m_Converter = std::make_unique<convert::GlConvert>();
        if (!m_Converter->init(m_Capture->renderNodePath(), m_Capture->fourcc(), m_Capture->width(),
                               m_Capture->height(), outputWidth, outputHeight, error))
            return false;

        m_Encoder = std::make_unique<encode::VaapiEncoder>();
        if (!m_Encoder->init(m_Capture->renderNodePath(), m_Target.codec,
                             m_Converter->outputWidth(), m_Converter->outputHeight(), m_EncodeFps,
                             m_Config.bitrateKbps, m_Config.intraRefresh, m_Config.tuning, error))
            return false;

        return m_Converter->bindTarget(m_Encoder->inputTarget(), error);
    }

    convert::CursorDraw cursorDraw() const
    {
        convert::CursorDraw draw;
        const int wanted = m_CursorFramePx.load();
        const capture::CursorState& cursor = m_Capture->cursor();
        // Sized on the ink, not the canvas: see CursorState::inkWidth. Scaled
        // by the frame/desktop ratio so the request is in frame pixels.
        if (wanted > 0 && cursor.inkWidth > 0 && m_Capture->width() > 0 && m_Converter &&
            m_Converter->outputWidth() > 0) {
            const float desktopPerFrame = static_cast<float>(m_Capture->width()) /
                                          static_cast<float>(m_Converter->outputWidth());
            const float target = static_cast<float>(wanted) * desktopPerFrame;
            const float magnify = target / static_cast<float>(cursor.inkWidth);
            if (magnify > 1.0f) draw.magnify = magnify;
        }
        // KMS reports no hotspot (the compositor applied it): the image grows
        // around its top-left, which for the arrow IS the hotspot.
        return draw;
    }

    enum class Restart
    {
        Restarted,
        Stopped,
        Failed,
    };

    Restart restartCapture(std::string& error)
    {
        int failures = 0;
        for (;;) {
            if (!m_Running.load()) return Restart::Stopped;
            if (openCapture(error)) break;
            failures++;
            if (failures == 1)
                log::info("[native] display is away (reconfiguring, or off), waiting for it: " +
                          error);
            std::this_thread::sleep_for(std::chrono::milliseconds(restartRetryDelayMs(failures)));
        }
        if (failures > 0)
            log::info("[native] display is back after " + std::to_string(failures) + " attempt" +
                      (failures > 1 ? "s" : ""));
        m_DisplayMilliHz = m_Capture->refreshMilliHz();
        if (!buildPipeline(m_Info.width, m_Info.height, error)) return Restart::Failed;
        {
            const capture::DesktopRect rect = m_Capture->desktopRect();
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) m_Input->setDisplayRect(rect.left, rect.top, rect.right, rect.bottom);
        }
        m_ResendCursor.store(true);
        return Restart::Restarted;
    }

    void run() noexcept
    {
        try {
            runLoop();
            logCadence();
        } catch (const std::exception& e) {
            finish(std::string("the capture loop threw: ") + e.what());
        } catch (...) {
            finish("the capture loop threw an unknown exception");
        }
        // The EGL context followed this thread; give it back so stop(), on
        // the caller's thread, can bind it to tear the converter down.
        if (m_Converter) m_Converter->detachThread();
    }

    void runLoop()
    {
        // The reasoning for every constant here is in WindowsSession::runLoop;
        // the values are the same because the receiver is the same.
        constexpr int kAcquireTimeoutMs = 100;
        constexpr int64_t kIdleFloorUs = 500 * 1000;
        constexpr int64_t kRefineWindowUs = 1000 * 1000;
        constexpr int64_t kRefineDelayUs = 150 * 1000;
        constexpr int kRefineMaxFps = 60;

        const int refineFps =
            (m_Config.fps > 0 && m_Config.fps < kRefineMaxFps) ? m_Config.fps : kRefineMaxFps;
        const int64_t refineIntervalUs = 1000000 / refineFps;
        const int refineTimeoutMs = static_cast<int>(refineIntervalUs / 1000);

        uint32_t frameNumber = 0;
        std::string error;
        int64_t lastSentUs = steadyNowUs();
        int64_t lastRealUs = lastSentUs;
        encode::RefineConvergence refineConv;
        bool refineDone = false;
        int refinePasses = 0;
        int refineHeld = 0;
        size_t refineBytes = 0;
        size_t refineFirstBytes = 0;
        int refineLogged = 0;
        // Whether the frame the capture last handed out is still valid to
        // re-convert (it is until the next export, see KmsCapture::acquire).
        bool haveFrame = false;
        capture::KmsFrame frame;

        auto floorIntervalUs = [this, kIdleFloorUs]() -> int64_t {
            int fps = m_FloorFps.load(std::memory_order_relaxed);
            if (fps <= 0) return kIdleFloorUs;
            if (m_Config.fps > 0 && fps > m_Config.fps) fps = m_Config.fps;
            const int64_t interval = 1000000 / fps;
            return interval < kIdleFloorUs ? interval : kIdleFloorUs;
        };

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
        auto closeBurst = [&](const char* how) {
            if (refinePasses == 0) return;
            if (refineLogged < 3) {
                refineLogged++;
                log::info(
                    "[native] still picture refined: " + std::to_string(refineFirstBytes / 1024) +
                    " KB + " + std::to_string(refineBytes / 1024) + " KB over " +
                    std::to_string(refinePasses) + " passes, " + std::to_string(refineHeld) +
                    " held for the link (" + how + ")");
            }
            refinePasses = 0;
        };
        auto resetBurst = [&]() {
            refineConv.reset();
            refineDone = false;
            refinePasses = 0;
            refineHeld = 0;
            refineBytes = 0;
            refineFirstBytes = m_LastEmitBytes;
        };
        auto noteReal = [&]() {
            closeBurst("screen moved");
            lastSentUs = steadyNowUs();
            lastRealUs = lastSentUs;
            resetBurst();
        };
        auto emitPicture = [&](const FrameStamps& stamps) -> bool {
            if (!m_Cadence.admit(stamps.convertedUs)) return true;
            if (!emit(frameNumber, stamps, error)) return false;
            noteReal();
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
        // Re-convert the held frame with the pointer where it is now, and
        // emit. The KMS equivalent of the Windows desktop copy, without one.
        auto reconvertHeld = [&](const FrameStamps& stamps) -> bool {
            if (!m_Converter->convert(frame, m_Capture->cursor(), cursorDraw(), error)) {
                finish("colour conversion failed: " + error);
                return false;
            }
            return emitPicture(stamps);
        };

        m_LoopStartUs = steadyNowUs();
        while (m_Running.load()) {
            if (const int kbps = m_PendingBitrate.exchange(0); kbps > 0) {
                governor.setSetting(kbps);
                applyGovernor("ceiling moved");
            }
            if (m_ClientRefreshDirty.exchange(false)) {
                FrameCadence chosen{0};
                std::string line;
                const int fps =
                    chooseCadence(m_ClientMilliHz.load(), m_ClientVsync.load(), chosen, line);
                if (chosen.intervalUs() != m_Cadence.intervalUs() || fps != m_CadenceFps) {
                    m_Cadence = chosen;
                    m_CadenceFps = fps;
                    log::info(line + " (client screen changed mid-session)");
                    if (effective.retarget(fps))
                        applyBitrate(boosted ? encode::stillBitrateKbps(baseKbps) : baseKbps);
                }
            }
            {
                LinkFeedback fb;
                const int64_t nowMs = steadyNowUs() / 1000;
                if (takeLinkFeedback(fb)) {
                    if (governor.report(fb, nowMs))
                        applyGovernor(fb.resumed ? "the receiver is back from the background"
                                      : fb.gaps > 0 || fb.evictions > 0 ? "frames lost"
                                      : fb.owdRiseMs >= encode::RateGovernor::kOveruseMs
                                          ? "delay rising"
                                          : "quiet, raising");
                } else if (governor.tick(nowMs)) {
                    applyGovernor("no report from the receiver");
                }
            }

            const int64_t sinceRealUs = steadyNowUs() - lastRealUs;
            const bool refineSoon =
                sinceRealUs < (kRefineDelayUs + kRefineWindowUs) && !refineDone && haveFrame;
            const bool refining = refineSoon && sinceRealUs >= kRefineDelayUs;
            if (!refineSoon && !refineDone) closeBurst("window closed");

            const int64_t idleIntervalUs = floorIntervalUs();
            const int idleTimeoutMs = static_cast<int>(idleIntervalUs / 1000) < kAcquireTimeoutMs
                                          ? static_cast<int>(idleIntervalUs / 1000)
                                          : kAcquireTimeoutMs;
            const int timeoutMs = refineSoon ? refineTimeoutMs : idleTimeoutMs;

            capture::KmsFrame fresh;
            const capture::AcquireStatus status = m_Capture->acquire(timeoutMs, fresh);

            if (status != capture::AcquireStatus::Timeout && boosted) {
                boosted = false;
                applyBitrate(baseKbps);
            }

            reportCursor();

            if (status == capture::AcquireStatus::Timeout) {
                if (m_CursorDirty.exchange(false) && m_CompositeCursor.load() && haveFrame) {
                    const int64_t now = steadyNowUs();
                    if (!reconvertHeld(resendStamps(now))) return;
                    continue;
                }
                if (!haveFrame) continue;

                if (m_ForceKeyframe.load(std::memory_order_relaxed)) {
                    if (!emit(frameNumber, resendStamps(steadyNowUs()), error)) return;
                    lastSentUs = steadyNowUs();
                    continue;
                }
                if (steadyNowUs() - lastSentUs < (refining ? refineIntervalUs : idleIntervalUs))
                    continue;
                if (refining && !m_Link.drainedAt(steadyNowUs())) {
                    refineHeld++;
                    continue;
                }
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

            if (status == capture::AcquireStatus::PointerOnly) {
                if (!m_CompositeCursor.load() || !haveFrame) continue;
                m_CursorDirty.store(false);
                const int64_t submittedUs = steadyNowUs();
                if (!reconvertHeld(
                        FrameStamps{submittedUs, submittedUs, submittedUs, steadyNowUs()}))
                    return;
                continue;
            }

            if (status == capture::AcquireStatus::Lost) {
                haveFrame = false;
                closeBurst("display lost");
                switch (restartCapture(error)) {
                case Restart::Restarted: break;
                case Restart::Stopped:
                    finish("the session was stopped while the display was away");
                    return;
                case Restart::Failed: finish("capture could not be restarted: " + error); return;
                }
                m_ForceKeyframe.store(true);
                boosted = false;
                applyBitrate(baseKbps);
                lastRealUs = steadyNowUs();
                resetBurst();
                continue;
            }

            if (status != capture::AcquireStatus::Ok) {
                finish("capture failed");
                return;
            }

            m_PresentsSeen++;
            frame = fresh;
            haveFrame = true;
            const int64_t submittedUs = steadyNowUs();

            static const capture::CursorState kNoCursor;
            const bool composite = m_CompositeCursor.load();
            m_CursorDirty.store(false);
            if (!m_Converter->convert(frame, composite ? m_Capture->cursor() : kNoCursor,
                                      cursorDraw(), error)) {
                finish("colour conversion failed: " + error);
                return;
            }
            // Not released: the buffer stays held for the pointer-only path,
            // and acquire() closes it when the next one replaces it.
            if (!emitPicture(
                    FrameStamps{frame.presentUs, frame.capturedUs, submittedUs, steadyNowUs()}))
                return;
        }
    }

    bool emit(uint32_t& frameNumber, const FrameStamps& stamps, std::string& error)
    {
        const bool forceKeyframe = m_ForceKeyframe.exchange(false);
        encode::EncoderOutput encoded;
        if (!m_Encoder->encode(forceKeyframe, frameNumber, encoded, error)) {
            finish("encode failed: " + error);
            return false;
        }
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
            m_Callbacks.onVideo(out);
        }
        m_Encoder->releaseOutput();
        return true;
    }

    /// The pointer for a client that draws its own. KMS gives the image but
    /// no hotspot and no name: the client places it by its top-left, which for
    /// the arrow is right and for a crosshair is a few pixels off — noted in
    /// KmsCapture.h as the one thing this route does not give.
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
        update.visible = cursor.visible && cursor.width > 0 && cursor.height > 0;
        update.width = cursor.width;
        update.height = cursor.height;
        update.hotspotX = 0;
        update.hotspotY = 0;
        update.kind = "";
        update.scale =
            (m_Capture->width() > 0 && m_Info.width > 0)
                ? static_cast<float>(m_Info.width) / static_cast<float>(m_Capture->width())
                : 1.0f;
        update.pixels = update.visible ? cursor.pixels.data() : nullptr;
        m_Callbacks.onCursor(update);
    }

    int chooseCadence(int clientMilliHz, bool clientVsync, FrameCadence& cadence,
                      std::string& line) const
    {
        const int displayHz = (m_DisplayMilliHz + 500) / 1000;
        int fps = m_Config.fps > 0 ? m_Config.fps : displayHz;
        if (fps <= 0) fps = 60;

        AlignedCadence aligned;
        if (m_Config.fps > 0 && clientVsync)
            aligned = alignCadence(m_Config.fps, clientMilliHz, displayHz);

        if (aligned.aligned) {
            fps = aligned.fps;
            cadence = fps < displayHz ? FrameCadence::fromIntervalNs(aligned.intervalNs, displayHz)
                                      : FrameCadence(0, displayHz);
            line = "[native] cadence: " + std::to_string(fps) + " fps stream for a " +
                   hzString(clientMilliHz) + " Hz client presenting on vsync (" +
                   std::to_string(m_Config.fps) + " set) on a " + hzString(m_DisplayMilliHz) +
                   " Hz display" +
                   (cadence.enabled() ? " — the first present of each interval is encoded, at once"
                                      : " — every present is encoded");
            return fps;
        }
        cadence = FrameCadence(fps < displayHz ? fps : 0, displayHz);
        line = "[native] cadence: " + std::to_string(fps) + " fps stream on a " +
               hzString(m_DisplayMilliHz) + " Hz display" +
               (cadence.enabled() ? " — the first present of each interval is encoded, at once"
                                  : " — every present is encoded");
        return fps;
    }

    void logCadence()
    {
        if (m_PresentsSeen == 0) return;
        const double seconds = (steadyNowUs() - m_LoopStartUs) / 1e6;
        char span[32];
        std::snprintf(span, sizeof(span), "%.1f", seconds);
        std::string line = "[native] cadence: " + hzString(m_DisplayMilliHz) + " Hz display, " +
                           std::to_string(m_CadenceFps) + " fps stream — " +
                           std::to_string(m_PresentsSeen) + " presents in " + span + " s";
        if (m_Cadence.enabled())
            line += ", " + std::to_string(m_Cadence.skipped()) + " not carried";
        else
            line += ", every one carried";
        log::info(line);
    }

    void finish(const std::string& reason) noexcept
    {
        m_Running.store(false);
        try {
            log::warning("[native] session ended: " + reason);
            if (m_Callbacks.onEnded) m_Callbacks.onEnded(reason);
        } catch (...) {}
    }

    SessionConfig m_Config;
    ResolvedTarget m_Target;
    SessionCallbacks m_Callbacks;
    SessionInfo m_Info;

    std::string m_CardPath;
    uint32_t m_ConnectorId = 0;
    std::string m_ConnectorName;

    int m_DisplayMilliHz = 0;
    int m_EncodeFps = 0;
    FrameCadence m_Cadence{0};
    int m_CadenceFps = 0;
    std::atomic<int> m_ClientMilliHz{0};
    std::atomic<bool> m_ClientVsync{false};
    std::atomic<bool> m_ClientRefreshDirty{false};

    std::unique_ptr<capture::KmsCapture> m_Capture;
    std::unique_ptr<convert::GlConvert> m_Converter;
    std::unique_ptr<encode::VaapiEncoder> m_Encoder;

    std::mutex m_InputMutex;
    std::unique_ptr<input::UinputInput> m_Input;
    std::unique_ptr<input::UinputGamepad> m_Gamepad;

#if defined(MW_NATIVE_LINUX_AUDIO)
    std::unique_ptr<audio::PacedOpusSink> m_Audio;
    std::unique_ptr<audio::PipeWireCapture> m_AudioTap;
#endif

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ForceKeyframe{true};
    std::atomic<bool> m_CompositeCursor{true};
    std::atomic<int> m_CursorFramePx{0};
    std::atomic<int> m_FloorFps{0};
    std::atomic<bool> m_CursorDirty{false};
    std::atomic<bool> m_ResendCursor{false};
    std::atomic<int> m_PendingBitrate{0};

    std::mutex m_LinkMutex;
    LinkFeedback m_LinkFeedback;
    bool m_LinkPending = false;
    encode::LinkOccupancy m_Link;
    int m_LinkKbps = 0;

    bool m_LoggedFirstKeyframe = false;
    size_t m_LastEmitBytes = 0;
    int m_LastEmitQp = -1;
    int64_t m_PresentsSeen = 0;
    int64_t m_LoopStartUs = 0;
    uint64_t m_ReportedShape = 0;
    bool m_ReportedVisible = false;
};

} // namespace

namespace detail {

std::unique_ptr<Session> createPlatformSession(const SessionConfig& config,
                                               const ResolvedTarget& target,
                                               const SessionCallbacks& callbacks,
                                               std::string& error)
{
    if (target.encoder != EncoderApi::VaApi) {
        error = std::string("no Linux encoder for ") + toString(target.encoder);
        return nullptr;
    }
    return std::make_unique<LinuxSession>(config, target, callbacks);
}

} // namespace detail
} // namespace mw::native
