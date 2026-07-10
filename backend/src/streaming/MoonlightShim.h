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
#include <atomic>
#include <cstdint>
#include <memory>
#include <chrono>

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

    void sendKeyEvent(short keyCode, bool down, char modifiers, char flags);
    // Send UTF-8 text (virtual/soft keyboard input) to the host.
    void sendUtf8Text(const QString& text);
    void sendMouseMove(short deltaX, short deltaY);
    void sendMousePosition(short x, short y, short referenceWidth, short referenceHeight);
    void sendMouseButton(bool down, int button);
    void sendMouseScroll(short scrollAmount);

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

    // Request an IDR frame from the host (Sunshine).
    // Called when the browser needs a keyframe to configure its decoder.
    void requestIdrFrame();

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
