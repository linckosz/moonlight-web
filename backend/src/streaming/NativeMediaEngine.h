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

#include "IMediaEngine.h"

#include <atomic>
#include <memory>

namespace mw::native {
class Session;
struct CursorUpdate;
} // namespace mw::native

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
 */
class NativeMediaEngine : public IMediaEngine
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

    /// Whether the engine draws the mouse pointer into the picture (immersive,
    /// and every touch screen) or reports its shape for the browser to draw
    /// (desktop). @p cursorFramePx is how wide the drawn pointer should be, in
    /// frame pixels, or 0 for its natural size — see Session. Safe at any time.
    void setCompositeCursor(bool composite, int cursorFramePx);

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
    void onEncodedFrame(const void* data, size_t size, bool keyframe, uint32_t frameNumber,
                        int64_t presentUs, int64_t submittedUs, int64_t encodedUs);

    /// Turn a borrowed cursor image into a PNG and emit it. Runs on the capture
    /// thread — the pixels do not outlive the call.
    void onCursor(const mw::native::CursorUpdate& cursor);

    std::unique_ptr<mw::native::Session> m_Session;

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
    std::atomic<int64_t> m_FramePresentationTimeUs{0};
    std::atomic<int64_t> m_FirstFrameArrivalUs{0};

    /// VIDEO_FORMAT_* of what the encoder actually produces, so the browser
    /// configures the right decoder.
    std::atomic<int> m_NegotiatedVideoFormat{0};
};
