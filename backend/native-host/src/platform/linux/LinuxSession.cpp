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
#if defined(MW_NATIVE_LINUX_PORTAL)
#include "../../capture/linux/PortalCapture.h"
#endif
#include "../../convert/linux/GlConvert.h"
#include "../../core/CadenceAlign.h"
#include "../../core/FrameCadence.h"
#include "../../core/Log.h"
#include "../../core/RestartBackoff.h"
#include "../../core/Session.h"
#include "../../encode/EncodeLoadCap.h"
#include "../../encode/RateControl.h"
#include "../../encode/RateGovernor.h"
#include "../../convert/linux/CpuConvert.h"
#include "../../encode/OpenH264Encoder.h"
#include "../../encode/linux/VaapiEncoder.h"
#include "../../input/linux/UinputGamepad.h"
#include "../../input/linux/UinputInput.h"
#if defined(MW_NATIVE_LINUX_AUDIO)
#include "../../audio/PacedOpusSink.h"
#include "../../audio/linux/HostMute.h"
#include "../../audio/linux/PipeWireCapture.h"
#endif

#include <algorithm>
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
//  - no intra-refresh on radeonsi 23.2 (SessionInfo says so), but reference
//    invalidation IS available: VA-API hands the reference list to us picture
//    by picture, so a lost frame heals with a delta on the GPU pair. Not on the
//    CPU pair — OpenH264 writes its own list;
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

/// Colour conversion and encoding as ONE object, so the loop is written once.
///
/// Two pairs wear this shape. The GPU pair — GlConvert rendering into the
/// surface VaapiEncoder owns — is the Linux path as it was; the CPU pair —
/// CpuConvert writing planes OpenH264Encoder reads — is the fallback tier for
/// a machine with no render node. Who owns the picture between the two halves
/// is reversed between the pairs (the encoder's surface, the converter's
/// planes), which is exactly why the loop must not know: it asks for a
/// conversion, then for an encode, and the pair sorts out the hand-off.
class VideoPipeline
{
public:
    virtual ~VideoPipeline() = default;

    virtual bool init(const capture::IScreenCapture& capture, Codec codec, int outputWidth,
                      int outputHeight, int fps, int bitrateKbps, bool intraRefresh,
                      const EncoderTuning& tuning, std::string& error) = 0;
    virtual bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                         const convert::CursorDraw& draw, std::string& error) = 0;
    virtual bool encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                        std::string& error) = 0;
    virtual void releaseOutput() = 0;
    virtual bool setBitrate(int kbps, std::string& error) = 0;
    virtual bool intraRefreshEnabled() const = 0;
    virtual int intraRefreshFrames() const = 0;
    /// Whether a frame the receiver lost can be healed by a delta rather than a
    /// keyframe. False on the CPU pair: OpenH264 writes its own reference list.
    virtual bool supportsReferenceInvalidation() const { return false; }
    virtual bool invalidateReference(uint32_t frameNumber, std::string& error)
    {
        (void)frameNumber;
        error = "reference invalidation is not available on this encoder";
        return false;
    }
    virtual int outputWidth() const = 0;
    virtual int outputHeight() const = 0;
    virtual int copiesPerFrame() const = 0;
    /// For the session's opening log line: the route and its cost. @p source
    /// names where the pixels came from, because the pair cannot know — the
    /// same CPU pair reads a scanout buffer on one machine and a portal's
    /// shared memory on another, and a line that says the wrong one is worse
    /// than no line.
    virtual std::string describe(const char* source) const = 0;
    /// The GPU pair's EGL context follows the capture thread; the thread gives
    /// it back before it ends. The CPU pair has nothing to give back.
    virtual void detachThread() {}
};

/// KMS → EGL → VA-API: the encoder owns the NV12 surface, the converter renders
/// into it. One copy per frame, the bitstream leaving VRAM.
class GpuPipeline final : public VideoPipeline
{
public:
    bool init(const capture::IScreenCapture& capture, Codec codec, int outputWidth,
              int outputHeight, int fps, int bitrateKbps, bool intraRefresh,
              const EncoderTuning& tuning, std::string& error) override
    {
        m_Converter = std::make_unique<convert::GlConvert>();
        if (!m_Converter->init(capture.renderNodePath(), capture.fourcc(), capture.width(),
                               capture.height(), outputWidth, outputHeight, error))
            return false;
        m_Encoder = std::make_unique<encode::VaapiEncoder>();
        if (!m_Encoder->init(capture.renderNodePath(), codec, m_Converter->outputWidth(),
                             m_Converter->outputHeight(), fps, bitrateKbps, intraRefresh, tuning,
                             error))
            return false;
        return m_Converter->bindTarget(m_Encoder->inputTarget(), error);
    }
    bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                 const convert::CursorDraw& draw, std::string& error) override
    {
        return m_Converter->convert(frame, cursor, draw, error);
    }
    bool encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                std::string& error) override
    {
        return m_Encoder->encode(forceKeyframe, frameNumber, out, error);
    }
    void releaseOutput() override { m_Encoder->releaseOutput(); }
    bool setBitrate(int kbps, std::string& error) override
    {
        return m_Encoder->setBitrate(kbps, error);
    }
    bool intraRefreshEnabled() const override { return m_Encoder->intraRefreshEnabled(); }
    int intraRefreshFrames() const override { return m_Encoder->intraRefreshFrames(); }
    bool supportsReferenceInvalidation() const override
    {
        return m_Encoder->supportsReferenceInvalidation();
    }
    bool invalidateReference(uint32_t frameNumber, std::string& error) override
    {
        return m_Encoder->invalidateReference(frameNumber, error);
    }
    int outputWidth() const override { return m_Converter->outputWidth(); }
    int outputHeight() const override { return m_Converter->outputHeight(); }
    int copiesPerFrame() const override { return 1; }
    std::string describe(const char* source) const override
    {
        return std::string("via VA-API — ") + source + " → EGL → VA-API, 1 copy (the bitstream)";
    }
    void detachThread() override
    {
        if (m_Converter) m_Converter->detachThread();
    }

private:
    std::unique_ptr<convert::GlConvert> m_Converter;
    std::unique_ptr<encode::VaapiEncoder> m_Encoder;
};

/// KMS → DMA-BUF mmap → CPU → OpenH264: the converter owns the I420 planes, the
/// encoder reads them. Two copies per frame — the pixels into the planes, the
/// bitstream out — on a machine that has no other way.
class CpuPipeline final : public VideoPipeline
{
public:
    bool init(const capture::IScreenCapture& capture, Codec codec, int outputWidth,
              int outputHeight, int fps, int bitrateKbps, bool intraRefresh,
              const EncoderTuning& tuning, std::string& error) override
    {
        (void)intraRefresh; // OpenH264 has none; reported false
        if (codec != Codec::H264) {
            error = std::string("OpenH264 encodes H.264 only, not ") + toString(codec);
            return false;
        }
        if (!m_Converter.init(capture.fourcc(), capture.width(), capture.height(), outputWidth,
                              outputHeight, error))
            return false;
        return m_Encoder.init(m_Converter.outputWidth(), m_Converter.outputHeight(), fps,
                              bitrateKbps, 0, tuning, error);
    }
    bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                 const convert::CursorDraw& draw, std::string& error) override
    {
        return m_Converter.convert(frame, cursor, draw, error);
    }
    bool encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                std::string& error) override
    {
        return m_Encoder.encode(m_Converter.picture(), forceKeyframe, frameNumber, out, error);
    }
    void releaseOutput() override { m_Encoder.releaseOutput(); }
    bool setBitrate(int kbps, std::string& error) override
    {
        return m_Encoder.setBitrate(kbps, error);
    }
    bool intraRefreshEnabled() const override { return false; }
    int intraRefreshFrames() const override { return 0; }
    int outputWidth() const override { return m_Converter.outputWidth(); }
    int outputHeight() const override { return m_Converter.outputHeight(); }
    int copiesPerFrame() const override { return 2; }
    std::string describe(const char* source) const override
    {
        // "mapped" covers both ways in: an mmap of a DMA-BUF on the scanout
        // route, memory the portal already mapped on the other.
        return std::string("via ") + encode::OpenH264Encoder::version() + " — " + source +
               " → mapped → CPU → OpenH264, 2 copies (the pixels, the bitstream), " +
               std::to_string(m_Converter.threads()) + "+" + std::to_string(m_Encoder.threads()) +
               " threads";
    }

private:
    convert::CpuConvert m_Converter;
    encode::OpenH264Encoder m_Encoder;
};

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
        // Only the scanout route has a connector to resolve. The portal route
        // has no monitor to name — the user picks one in its dialog — so there
        // is nothing here for it to find, and looking would fail on exactly the
        // machine that cannot read the card in the first place.
        if (m_Target.capture != CaptureApi::PipeWire) {
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
        } else {
            m_ConnectorName = "portal";
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
        const InputRects inputRects = readInputRects();
        {
            std::lock_guard<std::mutex> lock(m_InputMutex);
            auto sink = std::make_unique<input::UinputInput>();
            std::string inputError;
            if (sink->start(inputError)) {
                applyInputRects(*sink, inputRects);
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
            if (m_Config.muteHostAudio) {
                // Before the tap opens: the "silent output" strategy moves the
                // default sink, and the tap attaches to whatever is default
                // when IT starts.
                std::string how;
                m_HostMute.engage(how);
                log::info(std::string("[native] audio: ") + how);
                // What it achieved is read back into m_Info below: this
                // function clears m_Info AFTER this point (as the macOS one
                // does), so setting the flag here would be quietly wiped.
            }
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
        m_Info.width = m_Pipeline->outputWidth();
        m_Info.height = m_Pipeline->outputHeight();
        // What the cap scales from, fixed for the session.
        m_FullWidth = m_Info.width;
        m_FullHeight = m_Info.height;
        m_Info.fps = m_Config.fps;
        // What the pipeline was really built with — see buildPipeline: a CPU
        // pair forced by the portal's shared memory carries the codec down with
        // it, and the client is told H.264 rather than promised HEVC.
        m_Info.codec = m_Codec;
        // The encoder the pipeline actually built, not the one chosen on paper:
        // the portal's shared memory forces the CPU pair whatever the Selector
        // picked, and the client is told what it is really getting.
        m_Info.encoder = m_UsingCpuPair ? EncoderApi::Software : m_Target.encoder;
        m_Info.capture =
            m_Target.capture == CaptureApi::PipeWire ? CaptureApi::PipeWire : CaptureApi::Kms;
        m_Info.gpuName = m_Target.encodeGpuName;
        m_Info.hdr = false;
        m_Info.yuv444 = false;
        m_Info.intraRefresh = m_Pipeline->intraRefreshEnabled();
        m_Info.intraRefreshFrames = m_Pipeline->intraRefreshFrames();
        m_Info.referenceInvalidation = m_Pipeline->supportsReferenceInvalidation();
        // GPU pair: the scanout buffer is read in place and the encoder's
        // surface written in place, one copy remains (the bitstream leaving
        // VRAM). CPU pair: the pixels into the planes as well.
        m_Info.copiesPerFrame = m_Pipeline->copiesPerFrame();
        m_Info.crossGpuCopy = false;
#if defined(MW_NATIVE_LINUX_AUDIO)
        m_Info.audio = static_cast<bool>(m_Audio);
        m_Info.hostMuted = m_HostMute.strategy() != audio::HostMute::Strategy::None;
#else
        m_Info.audio = false;
#endif

        log::info(
            std::string("[native] session: ") + m_ConnectorName + " " +
            std::to_string(m_Info.width) + "x" + std::to_string(m_Info.height) + "@" +
            std::to_string(m_EncodeFps) + " " + toString(m_Info.codec) + " on " + m_Info.gpuName +
            " " +
            m_Pipeline->describe(m_Target.capture == CaptureApi::PipeWire ? "portal" : "KMS") +
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
        // After the tap, never before: releasing puts the default output back,
        // and the session manager would walk a running tap onto it.
        m_HostMute.release();
#endif
        if (!wasRunning && !m_Pipeline && !m_Capture) return;
        m_Pipeline.reset();
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
        if (!m_Info.referenceInvalidation) {
            // The CPU pair, or a pipeline that has not started: the receiver's
            // lost frame costs a keyframe, which is what SessionInfo promised.
            m_ForceKeyframe.store(true);
            return;
        }
        // Stored as +1 so that zero can mean "nothing pending" — frame 0 is a
        // real frame number. When several losses arrive before the next
        // picture, the OLDEST wins: it is the stricter of the two, and healing
        // against a picture older than both is correct for both.
        const uint32_t wanted = frameNumber + 1;
        uint32_t seen = m_PendingInvalidation.load();
        while ((seen == 0 || wanted < seen) &&
               !m_PendingInvalidation.compare_exchange_weak(seen, wanted)) {}
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

    /// Assigned into the callback bundle rather than kept beside it: openCapture
    /// already reads m_Callbacks.onPortalGrant, and two places to look for one
    /// listener is how one of them ends up stale. Registering after start() is
    /// a no-op by construction — openCapture has already run.
    void setPortalGrantCallback(PortalGrantCallback callback) override
    {
        m_Callbacks.onPortalGrant = std::move(callback);
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
#if defined(MW_NATIVE_LINUX_PORTAL)
        if (m_Target.capture == CaptureApi::PipeWire) {
            auto portal = std::make_unique<capture::PortalCapture>();
            portal->setRestoreToken(m_Config.portalRestoreToken);
            if (!portal->start(error)) return false;
            // A grant only comes back from a start that raised the dialog.
            // Handing it up is what spares the user every later one — the
            // consumer stores it and passes it back in SessionConfig.
            if (m_Callbacks.onPortalGrant) {
                const std::string granted = portal->restoreToken();
                if (!granted.empty() && granted != m_Config.portalRestoreToken)
                    m_Callbacks.onPortalGrant(granted);
            }
            m_PortalDmabuf = portal->dmabuf();
            m_Capture = std::move(portal);
            return true;
        }
#endif
        m_Capture = std::make_unique<capture::KmsCapture>(m_CardPath, m_ConnectorId);
        return m_Capture->start(error);
    }

    /// The two rectangles an absolute pointer needs: the display being captured,
    /// and the desktop it sits on — the union of every active output.
    ///
    /// The union matters because uinput's absolute device reports a fraction of
    /// its own axis and the compositor spreads that over the whole desktop. With
    /// the display alone, a second monitor is aimed at as if it were the only
    /// one, and the pointer lands somewhere else entirely.
    ///
    /// Read outside the input lock on purpose: this opens the DRM card, and
    /// inject() waits on that same lock. Only this card's outputs are counted —
    /// a desktop spanning two GPUs would need every card, which no host this
    /// engine runs on has, and getting it wrong there costs the same misplaced
    /// pointer we are fixing rather than anything worse.
    struct InputRects
    {
        capture::DesktopRect display;
        capture::DesktopRect desktop;
    };

    InputRects readInputRects() const
    {
        InputRects rects;
        rects.display = m_Capture->desktopRect();
        rects.desktop = rects.display;

        std::string listError;
        bool any = false;
        for (const capture::KmsOutput& out :
             capture::KmsCapture::listOutputs(m_CardPath, listError)) {
            if (!out.active || out.width <= 0 || out.height <= 0) continue;
            const capture::DesktopRect r{out.x, out.y, out.x + out.width, out.y + out.height};
            if (!any) {
                rects.desktop = r;
                any = true;
                continue;
            }
            rects.desktop.left = std::min(rects.desktop.left, r.left);
            rects.desktop.top = std::min(rects.desktop.top, r.top);
            rects.desktop.right = std::max(rects.desktop.right, r.right);
            rects.desktop.bottom = std::max(rects.desktop.bottom, r.bottom);
        }
        return rects;
    }

    static void applyInputRects(input::UinputInput& sink, const InputRects& rects)
    {
        sink.setDisplayRect(rects.display.left, rects.display.top, rects.display.right,
                            rects.display.bottom);
        sink.setDesktopRect(rects.desktop.left, rects.desktop.top, rects.desktop.right,
                            rects.desktop.bottom);
    }

    /// Converter and encoder against what the capture is handing out right
    /// now — the pair the Selector chose, not one guessed from the display.
    bool buildPipeline(int outputWidth, int outputHeight, std::string& error)
    {
        m_Pipeline.reset();
        // ⚠️ The portal may hand over SHARED MEMORY rather than a DMA-BUF —
        // which compositor and which driver decides, not us. EGL cannot import
        // that, so the GPU pair is impossible whatever the Selector chose on
        // paper, and the CPU pair is the only one that can read those pixels.
        // Deciding here rather than at selection time because the answer is not
        // known until the stream has negotiated.
        const bool sharedMemory = m_Target.capture == CaptureApi::PipeWire && !m_PortalDmabuf;
        if (sharedMemory && m_Target.encoder != EncoderApi::Software && !m_LoggedSharedMemory) {
            m_LoggedSharedMemory = true;
            log::info("[native] the portal gives shared memory, not DMA-BUF — encoding on the CPU, "
                      "which is the only route that can read it");
        }
        m_UsingCpuPair = m_Target.encoder == EncoderApi::Software || sharedMemory;

        // ⚠️ The codec has to follow the pair. The Selector picked HEVC because
        // the GPU offers it, and it was right about the GPU — but a pair that
        // encodes on the CPU encodes with OpenH264, which does H.264 and
        // nothing else. Left alone, a browser that prefers HEVC (Chrome does)
        // gets "OpenH264 encodes H.264 only" and no session at all: measured on
        // 08/09/2026, the first real browser session through the portal.
        //
        // Not a decision that could have been taken at selection time, for the
        // same reason the pair could not: whether the compositor hands over a
        // DMA-BUF or shared memory is known only once the stream has
        // negotiated, and on a DMA-BUF the Selector's HEVC is exactly right.
        m_Codec = m_Target.codec;
        if (m_UsingCpuPair && m_Codec != Codec::H264) {
            // Asked, not assumed. Every browser decodes H.264 and the list is
            // never empty here (the Selector rejects that before a session
            // exists), but a route that silently sends a codec the client did
            // not name is how a black picture with no error happens.
            const bool clientTakesH264 =
                std::find(m_Config.clientCodecs.begin(), m_Config.clientCodecs.end(),
                          Codec::H264) != m_Config.clientCodecs.end();
            if (!clientTakesH264) {
                error = std::string("this route encodes on the CPU, which can only produce H.264, "
                                    "and the client asked for ") +
                        toString(m_Codec) + " without it";
                return false;
            }
            if (!m_LoggedCodecDowngrade) {
                m_LoggedCodecDowngrade = true;
                log::info(std::string("[native] ") + toString(m_Codec) +
                          " was chosen for the GPU, but this route encodes on the CPU — "
                          "streaming H.264, which is what OpenH264 produces");
            }
            m_Codec = Codec::H264;
        }

        if (m_UsingCpuPair)
            m_Pipeline = std::make_unique<CpuPipeline>();
        else
            m_Pipeline = std::make_unique<GpuPipeline>();
        return m_Pipeline->init(*m_Capture, m_Codec, outputWidth, outputHeight, m_EncodeFps,
                                m_Config.bitrateKbps, m_Config.intraRefresh, m_Config.tuning,
                                error);
    }

    convert::CursorDraw cursorDraw() const
    {
        convert::CursorDraw draw;
        const int wanted = m_CursorFramePx.load();
        const capture::CursorState& cursor = m_Capture->cursor();
        // Sized on the ink, not the canvas: see CursorState::inkWidth. Scaled
        // by the frame/desktop ratio so the request is in frame pixels.
        if (wanted > 0 && cursor.inkWidth > 0 && m_Capture->width() > 0 && m_Pipeline &&
            m_Pipeline->outputWidth() > 0) {
            const float desktopPerFrame = static_cast<float>(m_Capture->width()) /
                                          static_cast<float>(m_Pipeline->outputWidth());
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
        // A new encoder holds no reconstructions: a loss named against the old
        // one means nothing, and the first picture is a keyframe regardless.
        m_PendingInvalidation.store(0);
        {
            const InputRects rects = readInputRects();
            std::lock_guard<std::mutex> lock(m_InputMutex);
            if (m_Input) applyInputRects(*m_Input, rects);
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
        if (m_Pipeline) m_Pipeline->detachThread();
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
            if (!m_Pipeline->setBitrate(effective.scaledKbps(kbps), error))
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
            if (!m_Pipeline->convert(frame, m_Capture->cursor(), cursorDraw(), error)) {
                finish("colour conversion failed: " + error);
                return false;
            }
            return emitPicture(stamps);
        };

        m_LoopStartUs = steadyNowUs();
        m_LoadCap.start(m_LoopStartUs);
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

            // Between frames, so the encoder is not holding anything.
            if (m_PendingResize.exchange(false)) applyLoadCap();

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
            if (!m_Pipeline->convert(frame, composite ? m_Capture->cursor() : kNoCursor,
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
        // Losses are named by the relay thread and applied here, on the thread
        // that owns the encoder — the same shape as the keyframe request, and
        // for the same reason: the reference list is encoder state.
        if (const uint32_t lost = m_PendingInvalidation.exchange(0); lost > 0) {
            std::string why;
            if (m_Pipeline->invalidateReference(lost - 1, why)) {
                log::info("[native] reference invalidated: frame " + std::to_string(lost - 1) +
                          " never reached the receiver, healing with a delta");
            } else {
                log::info("[native] cannot heal frame " + std::to_string(lost - 1) +
                          " with a delta (" + why + ") — sending a keyframe");
                m_ForceKeyframe.store(true);
            }
        }
        const bool forceKeyframe = m_ForceKeyframe.exchange(false);
        encode::EncoderOutput encoded;
        if (!m_Pipeline->encode(forceKeyframe, frameNumber, encoded, error)) {
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
            noteEncodeLoad(out.convertedUs, out.encodedUs);
        }
        m_Pipeline->releaseOutput();
        return true;
    }

    /// How long the encoder took, given to the cap — on the CPU tier only.
    ///
    /// On a hardware encoder this must stay silent: a few milliseconds against
    /// a frame interval is never the problem, and E4 and the link governor
    /// already own that space. It is the machine with no encoder at all where
    /// the encode duration IS the latency, and where trading pixels for it is
    /// the right bargain (EncodeLoadCap.h says why that way round).
    void noteEncodeLoad(int64_t convertedUs, int64_t encodedUs)
    {
        if (m_Target.encoder != EncoderApi::Software) return;
        if (encodedUs <= convertedUs) return;
        if (m_LoadCap.note(encodedUs - convertedUs, m_Cadence.intervalUs(), encodedUs))
            m_PendingResize.store(true);
    }

    /// Rebuild the pipeline at the size the cap now asks for. Called between
    /// frames, never inside emit(): the encoder there is holding a bitstream
    /// the sender has not finished with.
    void applyLoadCap()
    {
        const int width = encode::EncodeLoadCap::scaled(m_FullWidth, m_LoadCap.percent());
        const int height = encode::EncodeLoadCap::scaled(m_FullHeight, m_LoadCap.percent());
        if (width == m_Info.width && height == m_Info.height) return;

        std::string error;
        const int wasWidth = m_Info.width;
        const int wasHeight = m_Info.height;
        if (!buildPipeline(width, height, error)) {
            // Keep streaming at the size that worked rather than ending the
            // session over an optimisation: put the old one back, and if even
            // that fails there is nothing left to save.
            log::warning("[native] cpu cap: cannot encode at " + std::to_string(width) + "x" +
                         std::to_string(height) + " (" + error + ") — staying at " +
                         std::to_string(wasWidth) + "x" + std::to_string(wasHeight));
            if (!buildPipeline(wasWidth, wasHeight, error))
                finish("colour conversion failed: " + error);
            return;
        }
        m_Info.width = m_Pipeline->outputWidth();
        m_Info.height = m_Pipeline->outputHeight();
        m_ForceKeyframe.store(true);
        log::info("[native] cpu cap: " + std::to_string(wasWidth) + "x" +
                  std::to_string(wasHeight) + " -> " + std::to_string(m_Info.width) + "x" +
                  std::to_string(m_Info.height) + " (" + std::to_string(m_LoadCap.percent()) +
                  "% of the display) — the CPU encoder sets the latency, so pixels give way "
                  "before frames");
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

    /// Whichever route is giving us pictures — the scanout reader, or the
    /// portal on a machine that may not read it (IScreenCapture.h).
    std::unique_ptr<capture::IScreenCapture> m_Capture;
    std::unique_ptr<VideoPipeline> m_Pipeline;

    std::mutex m_InputMutex;
    std::unique_ptr<input::UinputInput> m_Input;
    std::unique_ptr<input::UinputGamepad> m_Gamepad;

#if defined(MW_NATIVE_LINUX_AUDIO)
    std::unique_ptr<audio::PacedOpusSink> m_Audio;
    std::unique_ptr<audio::PipeWireCapture> m_AudioTap;
    /// Releases in its destructor too, so a session torn down without stop()
    /// does not leave the machine silent.
    audio::HostMute m_HostMute;
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
    /// The frame the receiver says it never got, plus one; 0 means none.
    std::atomic<uint32_t> m_PendingInvalidation{0};

    /// Whether the portal handed over DMA-BUF (the GPU pair can import it) or
    /// shared memory (only the CPU pair can read it). Meaningless on the KMS
    /// route, which is always DMA-BUF.
    bool m_PortalDmabuf = false;
    bool m_LoggedSharedMemory = false;
    /// The codec the pipeline was really built with. Starts as the Selector's
    /// choice and is lowered to H.264 when the portal forces the CPU pair —
    /// see buildPipeline. Read by SessionInfo, so the client is never promised
    /// a codec the route cannot produce.
    Codec m_Codec = Codec::H264;
    /// Said once per session, like the shared-memory line beside it.
    bool m_LoggedCodecDowngrade = false;

    /// Which pair buildPipeline actually made. Not derivable from m_Target: the
    /// portal can force the CPU pair on a machine whose GPU could have encoded.
    bool m_UsingCpuPair = false;

    /// The size the session was opened at, which the cap scales FROM — never
    /// from the current one, or a run of reductions would compound.
    int m_FullWidth = 0;
    int m_FullHeight = 0;
    encode::EncodeLoadCap m_LoadCap;
    std::atomic<bool> m_PendingResize{false};

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
    if (target.encoder != EncoderApi::VaApi && target.encoder != EncoderApi::Software) {
        error = std::string("no Linux encoder for ") + toString(target.encoder);
        return nullptr;
    }
    return std::make_unique<LinuxSession>(config, target, callbacks);
}

} // namespace detail
} // namespace mw::native
