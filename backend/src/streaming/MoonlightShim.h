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

#include <QObject>
#include <QThread>
#include <QPointer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <memory>
#include <chrono>

class QTimer;

struct _SERVER_INFORMATION;
struct _STREAM_CONFIGURATION;
struct _DECODE_UNIT;
struct _OPUS_MULTISTREAM_CONFIGURATION;
typedef struct _SERVER_INFORMATION SERVER_INFORMATION;
typedef struct _STREAM_CONFIGURATION STREAM_CONFIGURATION;
typedef struct _DECODE_UNIT DECODE_UNIT, *PDECODE_UNIT;
typedef struct _OPUS_MULTISTREAM_CONFIGURATION OPUS_MULTISTREAM_CONFIGURATION,
    *POPUS_MULTISTREAM_CONFIGURATION;

class MoonlightShim : public QObject
{
    Q_OBJECT

public:
    struct InitParams
    {
        QString hostAddress;
        QString appVersion;
        QString gfeVersion;
        QString rtspSessionUrl;
        int serverCodecModeSupport = 0;

        int width = 1920;
        int height = 1080;
        int fps = 60;
        int bitrateKbps = 20000;
        int packetSize = 1024;
        int supportedVideoFormats = 0x0001; // VIDEO_FORMAT_H264
        int colorSpace = 1;                 // 0=BT.601 1=BT.709 SDR, 6=BT.2020+P(Q(HDR10)
        int colorRange = 0;                 // 0=Limited(TV), 1=Full(PC)
        int audioConfiguration = 0;
        bool slowOpus = false; // request 10ms Opus frames (half the packet rate) for mobile

        QByteArray aesKey; // 16 bytes
        int rikeyid = 0;
    };

    explicit MoonlightShim(QObject* parent = nullptr);
    ~MoonlightShim() override;

    void startConnection(const InitParams& params);
    void stopConnection();
    void interruptConnection();

    bool isConnected() const { return m_Connected; }

    // `hold` marks a press the client wants kept down through a brief link
    // stall (movement keys in gaming mode) — see the input watchdog below.
    void sendKeyEvent(short keyCode, bool down, char modifiers, char flags, bool hold = false);
    // --- Toggle-lock sync (NumLock / CapsLock / ScrollLock) ---
    // Snapshot the host's real lock-key state, only possible when the
    // streamed host IS this machine (same gate as clipboard sync). Must run
    // on the main thread (GetKeyState reads the calling thread's input
    // state); Session calls it before the connection starts. Without a
    // snapshot (remote host or non-Windows backend) syncLockKeys() falls back
    // to each lock's hardware default: NumLock on, Caps and Scroll off.
    void captureHostLockState(bool hostIsSelf);
    // Align the host's toggle locks with the client's (browser 'locksync'
    // message): tap each lock whose captured host state differs from the
    // client's. Thread-safe, synchronous — the taps are enqueued before any
    // input event the caller sends next, so the keydown that triggered the
    // sync still lands after the locks are aligned.
    void syncLockKeys(bool numLock, bool capsLock, bool scrollLock);
    // Send UTF-8 text (virtual/soft keyboard input) to the host.
    void sendUtf8Text(const QString& text);
    void sendMouseMove(short deltaX, short deltaY);
    void sendMousePosition(short x, short y, short referenceWidth, short referenceHeight);
    void sendMouseButton(bool down, int button, bool hold = false);
    // Both scroll axes hold sub-notch amounts back and emit whole 120-unit
    // notches while quantization is on (see quantizeScroll); the carry lives
    // for the session.
    void sendMouseScroll(short scrollAmount);
    // Horizontal wheel — Sunshine protocol extension (no-op on GeForce Experience hosts).
    void sendMouseHScroll(short scrollAmount);
    // Quantize scroll to whole notches, for a host that throws sub-notch
    // amounts away (Linux — see HostOsProbe.h). On by default because that is
    // the safe way to be wrong: a host that keeps the leftovers only scrolls
    // less smoothly, while one that does not scrolls not at all.
    //
    // Settable at any time, including mid-stream: the OS is sometimes only
    // established once the host's packets start arriving, and the carries below
    // make the switch lossless in both directions.
    void setScrollQuantization(bool enabled)
    {
        m_QuantizeScroll.store(enabled, std::memory_order_relaxed);
    }
    // IP TTL of the first datagram this connection received from the host, 0
    // while none has been read. The one thing on the wire that says which OS
    // family the host belongs to — see HostOsProbe.h.
    int hostIpTtl() const;

    // --- Game controller (gamepad) ---
    // Announce a newly connected controller (preferred over an empty state event):
    // lets the host pick the best emulated controller type and capabilities.
    void sendControllerArrival(uint8_t controllerNumber, uint16_t activeGamepadMask, uint8_t type,
                               bool hasRumble);
    // Send a full controller state snapshot (buttons + triggers + sticks).
    // Used for updates and, with an empty payload + cleared mask bit, for removal.
    void sendControllerState(short controllerNumber, short activeGamepadMask, int buttonFlags,
                             unsigned char leftTrigger, unsigned char rightTrigger,
                             short leftStickX, short leftStickY, short rightStickX,
                             short rightStickY);
    /// Shift this session's controller numbering (see applyControllerOffset).
    /// Zero — the owner's own sessions — keeps the browser's numbering. Set
    /// once before the connection starts.
    void setControllerOffset(int offset) { m_ControllerOffset = offset; }

    // ── Input watchdog (dead-man switch) ────────────────────────────────────
    //
    // The input channel is ordered+reliable: a link stall does not lose events,
    // it DELAYS them. A press that already landed therefore stays down on the
    // host for the whole stall while its release waits in the SCTP queue, and
    // the guest OS typematic turns one keystroke into dozens ("rrrrrrr..." for
    // a 2 s freeze). A stuck key also outlives a link that never comes back.
    //
    // Nothing the client sends during the stall can fix this — its correction
    // would ride the same blocked queue. So the host side watches for silence:
    // every message from the client refreshes a liveness timestamp, and when it
    // goes stale we release whatever is still held. The client heartbeats its
    // full held-input state (only while something IS held), so the next message
    // after the stall both refreshes liveness and re-presses anything the user
    // is genuinely still holding — see syncHeldInputs().
    //
    // Two grace periods: `hold` inputs (movement keys/buttons in gaming mode)
    // get the long one, so a network hiccup does not stop a player mid-stride;
    // everything else gets the short one, chosen to fire BEFORE the typematic
    // delay (~500 ms on Windows) so no repeat is ever generated.
    static constexpr int kInputWatchdogTickMs = 100;
    static constexpr int kInputStaleMs = 250;
    static constexpr int kInputStaleHoldMs = 3000;

    /// A press currently applied to the host, kept so the watchdog can release
    /// it and a heartbeat can restore it.
    struct HeldKey
    {
        short keyCode = 0;
        char modifiers = 0;
        char flags = 0;
        bool hold = false;
    };

    /// Refresh the client-liveness timestamp. Call from every relay on every
    /// inbound client message, whatever its type.
    void noteClientAlive();

    /// Release every held key/button (and neutralize every non-idle gamepad).
    /// `includeHold` also releases the inputs flagged `hold`.
    void releaseHeldInputs(bool includeHold);

    /// Reconcile the host with the client's authoritative held-input state
    /// (its heartbeat): press what the watchdog released but the user still
    /// holds, release what drifted. `buttonMask` is 1 << (button - 1).
    void syncHeldInputs(const QVector<HeldKey>& keys, quint32 buttonMask, bool buttonsHold);

    // Request an IDR frame from the host (Sunshine).
    // Called when the browser needs a keyframe to configure its decoder.
    void requestIdrFrame();

    // ── Host encoder wake-up (still-screen deadlock) ────────────────────────
    //
    // Sunshine only encodes on damage, and LiRequestIdrFrame only applies to
    // the NEXT frame it encodes. On a still host screen (a text page, a paused
    // desktop) nothing is captured, so a client whose decoder lost its
    // reference — a keyframe lost on the browser link during congestion, a
    // delta gated after a worker-side drop — waits for an IDR that can never
    // come. The picture stays black until someone moves the host's mouse.
    //
    // So when IDRs are being asked for and the host has sent NOTHING for a
    // while, move the pointer one pixel and back: that damage is what makes
    // Sunshine capture, and the pending IDR rides the frame it then encodes.
    // Guarded three ways — the host must be silent (a host that still encodes
    // needs no help), one nudge per cooldown, and a handful of attempts per
    // stall (if the pointer is not what's blocking, shaking it is noise).
    static constexpr int kEncoderIdleMs = 2000;
    static constexpr int kWakeNudgeCooldownMs = 3000;
    static constexpr int kMaxWakeNudges = 5;

    /// Whether this session may nudge the host pointer (see above). Off for a
    /// share whose guest was not given keyboard/mouse: their stalled decoder
    /// must not move a pointer they are not allowed to touch.
    void setWakeNudgeAllowed(bool allowed)
    {
        m_WakeNudgeAllowed.store(allowed, std::memory_order_release);
    }

    // Metrics for stats overlay.
    // One-way backend↔Sunshine latency (ms): ENet control-stream RTT / 2 when
    // available (continuously updated, like moonlight-qt's "network latency"),
    // falling back to the IDR round-trip estimate for very old hosts.
    double hostRttMs() const;
    // Average Sunshine host processing latency (capture→encode, ms) over the
    // frames received since the previous call — a rotating window like
    // moonlight-qt's per-second stats windows. Returns 0 when the host doesn't
    // report it. Read-and-reset: only one consumer (the active relay).
    double takeHostProcessingLatencyMs();
    int64_t lastDecodeLatencyUs() const
    {
        return m_LastDecodeLatencyUs.load(std::memory_order_acquire);
    }
    int64_t frameSubmitTimeUs() const
    {
        return m_FrameSubmitTimeUs.load(std::memory_order_acquire);
    }
    int64_t framePresentationTimeUs() const
    {
        return m_FramePresentationTimeUs.load(std::memory_order_acquire);
    }
    int64_t firstFrameArrivalTimeUs() const
    {
        return m_FirstFrameArrivalTimeUs.load(std::memory_order_acquire);
    }
    int64_t firstFrameArrivalSteadyMs() const
    {
        return m_FirstFrameArrivalTimeUs.load(std::memory_order_acquire) / 1000;
    }
    int64_t frameHostProcessingLatencyTenthMs() const
    {
        return m_FrameHostProcessingLatencyTenthMs.load(std::memory_order_acquire);
    }

    // Negotiated video format set by drSetup during LiStartConnection.
    // Returns the VIDEO_FORMAT_* mask chosen by Sunshine, or 0 before negotiation.
    int negotiatedVideoFormat() const
    {
        return m_NegotiatedVideoFormat.load(std::memory_order_acquire);
    }

    // Opus frame size (samples per channel @ 48 kHz) negotiated in arInit. Used
    // to pace the RTP audio timestamp by a clean per-packet increment (a jittery
    // arrival-time clock makes the browser's NetEq time-stretch → robotic audio).
    // Default 240 (5 ms) until arInit runs.
    int audioSamplesPerFrame() const
    {
        return m_AudioSamplesPerFrame.load(std::memory_order_acquire);
    }

    // Called by the relay at the head of onVideoFrame() to balance the
    // worker→main pending frame counter (incremented before each emit).
    void videoFrameDelivered() { m_PendingVideoFrames.fetch_sub(1, std::memory_order_acq_rel); }

    // Consume the worker-side delta drop flag (true once per drop episode).
    // The relay uses it to arm awaiting-IDR recovery on the main thread.
    bool takeWorkerDroppedDelta()
    {
        return m_WorkerDroppedDelta.exchange(false, std::memory_order_acq_rel);
    }

    // Diagnostics for the relay's periodic drop-counter log line.
    int64_t workerDropCount() const { return m_WorkerDropCount.load(std::memory_order_relaxed); }
    int pendingVideoFrames() const { return m_PendingVideoFrames.load(std::memory_order_acquire); }

signals:
    void stageChanged(int stage);
    void connectionStarted();
    void connectionFailed(const QString& error);
    void connectionTerminated(int errorCode);
    // presentationTimeUs travels WITH the frame through the queued connection:
    // relays must not re-read the shim's "latest frame" atomics at drain time,
    // or a drained burst gets stamped with one shared timestamp (defeats the
    // frontend's out-of-order frame filter on reordering links).
    void videoFrameReady(QByteArray data, int frameType, int frameNumber,
                         qint64 presentationTimeUs);
    void audioSampleReady(QByteArray data);
    void connectionStopped();
    // Host requested controller rumble (forwarded to the browser's vibration API).
    void rumble(int controllerNumber, int lowFreqMotor, int highFreqMotor);

private:
    QPointer<QThread> m_WorkerThread;
    std::atomic<bool> m_Connected{false};
    std::atomic<bool> m_Stopping{false};
    std::atomic<bool> m_CleanupDone{false};
    // Host lock-key state bitmask (1=Num, 2=Caps, 4=Scroll) captured at
    // session start; -1 = unknown (remote host or unsupported platform).
    // Written on the main thread before the connection, read/updated from
    // the relay thread that performs the sync.
    std::atomic<int> m_HostLockState{-1};

    // Scroll quantization (see quantizeScroll). The carries hold the amount
    // not yet worth a notch; both are only touched from the relay thread that
    // decodes input messages, so they need no synchronization of their own.
    // The flag is not: it is written from whichever thread learns the host's
    // OS, which is not the one reading it.
    std::atomic<bool> m_QuantizeScroll{true};
    int m_VScrollCarry = 0;
    int m_HScrollCarry = 0;

    static std::atomic<MoonlightShim*> s_Instance;

    // Last connection-setup failure, recorded by clStageFailed and read by
    // startConnection()'s retry loop (which decides whether to retry silently or
    // surface the error). Stage 0 = no stageFailed callback fired.
    std::atomic<int> m_LastFailedStage{0};
    std::atomic<int> m_LastFailedError{0};

    // Metrics (written from worker thread, read from main thread)
    std::atomic<double> m_HostRttMs{0.0};
    std::atomic<int64_t> m_LastDecodeLatencyUs{0};
    std::atomic<int64_t> m_FrameSubmitTimeUs{0};
    std::atomic<int64_t> m_IdrRequestTimeUs{0};
    // Arrival time of the last frame the HOST sent, whatever happens to it
    // downstream (recorded before the worker-side drop, which says nothing
    // about whether Sunshine is still encoding). Drives the encoder wake-up.
    std::atomic<int64_t> m_LastHostFrameTimeUs{0};
    std::atomic<int64_t> m_LastWakeNudgeUs{0};
    // Connection-up time, the reference the wake-up measures against until the
    // session's first frame arrives.
    std::atomic<int64_t> m_ConnectedTimeUs{0};
    std::atomic<int> m_ConsecutiveWakeNudges{0};
    std::atomic<bool> m_WakeNudgeAllowed{true};
    std::atomic<int64_t> m_FramePresentationTimeUs{0}; // presentationTimeUs from DECODE_UNIT
    std::atomic<int64_t> m_FirstFrameArrivalTimeUs{
        0}; // steady_clock::now() at first frame arrival (us since epoch)
    std::atomic<int64_t> m_FrameHostProcessingLatencyTenthMs{
        0}; // frameHostProcessingLatency (tenths of ms)
    // Host processing latency window accumulators (reset by
    // takeHostProcessingLatencyMs on each stats tick).
    std::atomic<int64_t> m_HostProcWindowTotalTenthMs{0};
    std::atomic<int64_t> m_HostProcWindowCount{0};

    // Negotiated video format (set by drSetup during LiStartConnection).
    // 0 = unknown, 0x0001 = H.264, 0x0100 = HEVC, 0x0200 = AV1.
    std::atomic<int> m_NegotiatedVideoFormat{0};

    // Opus samples-per-frame negotiated in arInit (48 kHz). Default 240 (5 ms).
    std::atomic<int> m_AudioSamplesPerFrame{240};

    // Worker→main queue bound: frames emitted via videoFrameReady but not yet
    // processed by the relay. Deltas are dropped worker-side when it saturates.
    std::atomic<int> m_PendingVideoFrames{0};
    std::atomic<bool> m_WorkerDroppedDelta{false};
    std::atomic<int64_t> m_WorkerDropCount{0};

    // Balances MacActivity begin/end (App Nap suppression) across the
    // startConnection → finishCleanup lifecycle, whatever teardown path runs.
    bool m_ActivityHeld = false;

    // ── Input watchdog state (contract in the public section) ───────────────
    // The relay input path runs on the relay thread, but the clipboard paste
    // path injects its chord from the main thread — so the held-input state is
    // mutex-guarded, and only the QTimer (thread-affine) is hopped back onto
    // the shim's own thread.
    QMutex m_InputStateMutex;
    QTimer* m_InputWatchdog = nullptr;
    QTimer* m_EncoderWakeTimer = nullptr;
    QElapsedTimer m_ClientAliveTimer; // invalid until the first client message
    QHash<short, HeldKey> m_HeldKeys;
    quint32 m_HeldButtons = 0; // 1 << (button - 1)
    bool m_HeldButtonsHold = false;
    QHash<short, short> m_ActiveGamepads; // controller index → active mask
    /// Offset applied to every controller number this session sends, so
    /// concurrent sessions do not collapse onto the host's controller 0.
    int m_ControllerOffset = 0;
    void applyControllerOffset(short& controllerNumber, short& activeGamepadMask) const;
    bool m_ShortStaleFired = false; // don't re-log/re-release each tick
    bool m_LongStaleFired = false;
    // The watchdog only arms once the client has proved it heartbeats its held
    // state (first 'inputstate' message). Without that handshake an older
    // cached frontend — which goes silent while a key is legitimately held —
    // would have its keys released out from under it.
    bool m_HeartbeatSeen = false;

    /// Force the host to capture a frame when it has gone silent with an IDR
    /// pending (contract above requestIdrFrame's wake-up section).
    void maybeWakeHostEncoder();
    /// Keep checking after the request that armed it: the deadlock is a state,
    /// not an event. The IDR is normally asked for the instant the host goes
    /// quiet — too early for the silence to be measurable — and nothing else
    /// runs afterwards, since every other code path here is driven by frames
    /// that are precisely what stopped coming.
    void armEncoderWakeTimer();
    void onEncoderWakeTick();

    /// Run the watchdog only while something is actually held down.
    void updateInputWatchdog();
    void onInputWatchdogTick();
    bool anythingHeld() const
    {
        return !m_HeldKeys.isEmpty() || m_HeldButtons != 0 || !m_ActiveGamepads.isEmpty();
    }

    void finishCleanup();
    void blockingStopConnection();

    // Video callbacks
    static int drSetup(int videoFormat, int width, int height, int redrawRate, void* context,
                       int drFlags);
    static void drStart(void);
    static void drStop(void);
    static void drCleanup(void);
    static int drSubmitDecodeUnit(PDECODE_UNIT decodeUnit);

    // Audio callbacks
    static int arInit(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                      void* context, int arFlags);
    static void arStart(void);
    static void arStop(void);
    static void arCleanup(void);
    static void arDecodeAndPlaySample(char* sampleData, int sampleLength);

    // Connection listener callbacks
    static void clStageStarting(int stage);
    static void clStageComplete(int stage);
    static void clStageFailed(int stage, int errorCode);
    static void clConnectionStarted(void);
    static void clConnectionTerminated(int errorCode);
    static void clLogMessage(const char* format, ...);
    static void clRumble(unsigned short controller, unsigned short low, unsigned short high);
    static void clConnectionStatusUpdate(int status);
    static void clSetHdrMode(bool enabled);
};
