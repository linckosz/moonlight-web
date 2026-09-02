/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "FrameSentSink.h"
#include "IMediaEngine.h"

#include "mw/native/StageStats.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace mw::native {
class Session;
struct CursorUpdate;
struct EncodedFrame;
} // namespace mw::native

class InputWatchdog;

/**
 * @brief The media engine backed by this machine's own screen.
 *
 * The Qt-side adapter over `backend/native-host/`. It is deliberately thin: the
 * engine below knows nothing of Qt, and everything above it — the three relays,
 * the WSS fallback, the input watchdog — already speaks to IMediaEngine. So all
 * this class does is translate, and it must not accumulate policy of its own.
 *
 * ── What it does NOT do, and why ────────────────────────────────────────────
 *
 * No queue between the engine and the relay. The engine hands over a frame on
 * its capture thread and the relay sends it before returning, which is exactly
 * the arrangement the GameStream path spends a thread hop to approximate. That
 * hop — worker thread → queued Qt signal → relay thread — is one of the costs
 * this whole module exists to remove, so re-adding it here would be self-
 * defeating.
 *
 * ── Metrics ─────────────────────────────────────────────────────────────────
 *
 * `hostRttMs()` is 0 and `hostIpTtl()` is 0: there is no host to reach and no
 * datagram to inspect. Reporting a fabricated number would be worse than an
 * absent one, and the stats overlay already handles a zero.
 *
 * `takeHostProcessingLatencyMs()` is the one metric that gets BETTER: on the
 * GameStream path it is whatever the remote host chose to report, while here it
 * is measured — the real present→encoded time of every frame in the window.
 *
 * And it goes further: every frame carries its stamps from the display's
 * present to the encoder's output, and the sender thread reports when the last
 * fragment left (FrameSentSink). Six stages, mean and tail, in the stats
 * message every window and in the log once at the end of the session. Nothing
 * on this path is optimized on a guess.
 */
class NativeMediaEngine : public IMediaEngine, public FrameSentSink
{
    Q_OBJECT

public:
    /// Everything a native session needs. Small on purpose: the only genuinely
    /// user-chosen field is the display (§13 of the mission).
    struct StartParams
    {
        int displayId = -1;
        int width = 0;  ///< 0 = the display's native width
        int height = 0; ///< 0 = the display's native height
        int fps = 0;    ///< 0 = the display's own refresh rate
        int bitrateKbps = 20000;
        /// VIDEO_FORMAT_* bitmask of what the browser can decode.
        int clientVideoFormats = 0;
        bool hdr = false;
        bool yuv444 = false;
        /// Encode with intra-refresh. Only worth asking when the receiver will
        /// decode through a gap — see rideOutLoss in MediaDescriptor.h.
        bool intraRefresh = false;
    };

    explicit NativeMediaEngine(QObject* parent = nullptr);
    ~NativeMediaEngine() override;

    /// Build and start the pipeline. Not part of IMediaEngine, for the same
    /// reason MoonlightShim::startConnection is not: the parameters are this
    /// engine's own.
    ///
    /// Emits connectionStarted() on success and connectionFailed() otherwise,
    /// so the caller can treat both engines identically from there on.
    void startCapture(const StartParams& params);

    // ── IMediaEngine ────────────────────────────────────────────────────────

    void stopConnection() override;
    void interruptConnection() override;
    bool isConnected() const override { return m_Connected.load(std::memory_order_acquire); }

    void requestIdrFrame() override;
    void videoFrameDelivered() override;
    bool takeWorkerDroppedDelta() override;
    int64_t workerDropCount() const override;
    int pendingVideoFrames() const override;
    int negotiatedVideoFormat() const override;

    int audioSamplesPerFrame() const override;

    void sendKeyEvent(short keyCode, bool down, char modifiers, char flags,
                      bool hold = false) override;
    void sendUtf8Text(const QString& text) override;
    void sendMouseMove(short deltaX, short deltaY) override;
    void sendMousePosition(short x, short y, short referenceWidth, short referenceHeight) override;
    void sendMouseButton(bool down, int button, bool hold = false) override;
    void sendMouseScroll(short scrollAmount) override;
    void sendMouseHScroll(short scrollAmount) override;
    void sendControllerArrival(uint8_t controllerNumber, uint16_t activeGamepadMask, uint8_t type,
                               bool hasRumble) override;
    void sendControllerState(short controllerNumber, short activeGamepadMask, int buttonFlags,
                             unsigned char leftTrigger, unsigned char rightTrigger,
                             short leftStickX, short leftStickY, short rightStickX,
                             short rightStickY) override;
    void sendControllerRemoval(uint8_t controllerNumber, uint16_t activeGamepadMask) override;
    void setControllerOffset(int offset) override { m_ControllerOffset = offset; }
    void syncLockKeys(bool numLock, bool capsLock, bool scrollLock) override;
    void syncHeldInputs(const QVector<HeldKey>& keys, quint32 buttonMask,
                        bool buttonsHold) override;
    void releaseHeldInputs(bool includeHold) override;
    void noteClientAlive() override;

    double takeHostProcessingLatencyMs() override;
    int64_t frameSubmitTimeUs() const override;
    int64_t framePresentationTimeUs() const override;
    int64_t firstFrameArrivalSteadyMs() const override;
    FrameSentSink* frameSentSink() override { return this; }
    QJsonObject takeStageStats() override;

    // ── FrameSentSink ───────────────────────────────────────────────────────
    void frameSent(uint32_t frameNumber, int64_t firstByteUs, int64_t lastByteUs) override;

    /// Whether the engine draws the mouse pointer into the picture (gaming mode,
    /// and every touch screen) or reports its shape for the browser to draw
    /// (desktop). @p cursorFramePx is how wide the drawn pointer should be, in
    /// frame pixels, or 0 for its natural size — see Session. Safe at any time.
    void setCompositeCursor(bool composite, int cursorFramePx);

    /// How fast the client wants frames to keep coming while nothing on the
    /// host's screen moves — see Session::setFrameFloorFps. 0 leaves the engine
    /// its own liveness floor.
    ///
    /// Remembered as well as forwarded: the input channel opens before the
    /// capture does, so the client's first word on the subject usually arrives
    /// with no session to tell. It is deduplicated on the client, which would
    /// then never say it again. Safe at any time.
    void setFrameFloorFps(int fps);

    /// A human-readable description of what the session settled on, for the
    /// stats overlay: "NVIDIA GeForce RTX 4070 · NVENC HEVC 4:4:4". Empty until
    /// the session has started.
    QString describeSession() const;

    /// Whether the running stream really refreshes by intra-refresh.
    ///
    /// The relay reads this to decide whether it may ride out a gap instead of
    /// discarding deltas and demanding a keyframe. False until a session has
    /// started, and false whenever the encoder declined — so the answer is
    /// always what the stream DOES, never what was asked for.
    bool intraRefreshActive() const override;

private:
    void onEncodedFrame(const mw::native::EncodedFrame& frame);

    /// The session's stage figures, once, when it ends. Idempotent.
    void logStageSummary();

    /// Turn a borrowed cursor image into a PNG and emit it. Runs on the capture
    /// thread — the pixels do not outlive the call.
    void onCursor(const mw::native::CursorUpdate& cursor);

    std::unique_ptr<mw::native::Session> m_Session;

    /// The input dead-man switch (contract in InputWatchdog.h). Owned, on this
    /// engine's thread. Fed with the SHIFTED controller numbers, so what it
    /// neutralizes is the pad the host actually holds. Its sink speaks plain
    /// InputEvents — a key up, a button up, a centred pad — so the engine
    /// below needs no release message of its own: it already deduplicates,
    /// and a pad update with everything at zero IS a neutralization.
    InputWatchdog* m_Watchdog = nullptr;

    std::atomic<bool> m_Connected{false};

    /// Shifts this session's controller numbering so concurrent sessions do not
    /// collapse onto the host's controller 0. Written once before the session
    /// starts, read from the input path afterwards.
    int m_ControllerOffset = 0;

    /// Producer→consumer bookkeeping, mirroring MoonlightShim's so the relays'
    /// existing drop diagnostics keep working unchanged.
    std::atomic<int> m_PendingVideoFrames{0};
    std::atomic<int64_t> m_WorkerDropCount{0};
    std::atomic<bool> m_WorkerDroppedDelta{false};

    /// Rotating window for takeHostProcessingLatencyMs. Measured here, unlike
    /// the GameStream path where it is reported by the host.
    std::atomic<int64_t> m_ProcWindowTotalUs{0};
    std::atomic<int64_t> m_ProcWindowCount{0};

    std::atomic<int64_t> m_FrameSubmitTimeUs{0};

    /// Presentation time of the latest frame, in µs SINCE THE FIRST FRAME — the
    /// relay's contract (IMediaEngine::videoFrameReady), not the engine's
    /// absolute stamp.
    std::atomic<int64_t> m_FramePresentationTimeUs{0};

    /// Absolute steady_clock present time of the first frame: the epoch of every
    /// relative presentation time above, and what firstFrameArrivalSteadyMs()
    /// reports. Zero until the first frame.
    std::atomic<int64_t> m_FirstPresentUs{0};

    // ── Per-stage latency ───────────────────────────────────────────────────
    //
    // Fed from two threads: the capture thread records t₀..t₃ as the frame
    // comes out of the encoder, the sender thread closes the timeline with
    // t₄/t₅ once the last fragment is on the wire. One mutex, uncontended in
    // practice (two short critical sections per frame), guards both the
    // histograms and the ring below.
    std::mutex m_StageMutex;
    mw::native::StageStats m_Stages;
    bool m_StageSummaryLogged = false;

    /// What the sender needs to know about a frame it is about to report on:
    /// its present and its encode stamp. Indexed by frame number modulo the
    /// size, and checked against the number so a frame the sender dropped
    /// cannot be scored against a later frame's stamps.
    struct InFlight
    {
        uint32_t frameNumber = 0;
        bool valid = false;
        int64_t presentUs = 0;
        int64_t encodedUs = 0;
    };
    static constexpr size_t kInFlightRing = 256;
    std::array<InFlight, kInFlightRing> m_InFlight{};

    /// VIDEO_FORMAT_* of what the encoder actually produces, so the browser
    /// configures the right decoder.
    std::atomic<int> m_NegotiatedVideoFormat{0};

    /// The client's last word on the still-screen frame floor, replayed onto a
    /// session that starts after it was said. See setFrameFloorFps.
    std::atomic<int> m_FrameFloorFps{0};
};
