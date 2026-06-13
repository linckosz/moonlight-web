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
typedef struct _OPUS_MULTISTREAM_CONFIGURATION OPUS_MULTISTREAM_CONFIGURATION, *POPUS_MULTISTREAM_CONFIGURATION;

class MoonlightShim : public QObject
{
    Q_OBJECT

public:
    struct InitParams {
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
        int colorSpace = 1;   // 0=BT.601 1=BT.709 SDR, 6=BT.2020+P(Q(HDR10)
        int colorRange = 0;   // 0=Limited(TV), 1=Full(PC)
        int audioConfiguration = 0;

        QByteArray aesKey;  // 16 bytes
        int rikeyid = 0;
    };

    explicit MoonlightShim(QObject* parent = nullptr);
    ~MoonlightShim();

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

    // Request an IDR frame from the host (Sunshine).
    // Called when the browser needs a keyframe to configure its decoder.
    void requestIdrFrame();

    // Metrics for stats overlay
    double hostRttMs() const { return m_HostRttMs.load(std::memory_order_acquire); }
    int64_t lastDecodeLatencyUs() const { return m_LastDecodeLatencyUs.load(std::memory_order_acquire); }
    int64_t frameSubmitTimeUs() const { return m_FrameSubmitTimeUs.load(std::memory_order_acquire); }
    int64_t framePresentationTimeUs() const { return m_FramePresentationTimeUs.load(std::memory_order_acquire); }
    int64_t firstFrameArrivalTimeUs() const { return m_FirstFrameArrivalTimeUs.load(std::memory_order_acquire); }
    int64_t firstFrameArrivalSteadyMs() const { return m_FirstFrameArrivalTimeUs.load(std::memory_order_acquire) / 1000; }
    int64_t frameHostProcessingLatencyTenthMs() const { return m_FrameHostProcessingLatencyTenthMs.load(std::memory_order_acquire); }

    // Negotiated video format set by drSetup during LiStartConnection.
    // Returns the VIDEO_FORMAT_* mask chosen by Sunshine, or 0 before negotiation.
    int negotiatedVideoFormat() const { return m_NegotiatedVideoFormat.load(std::memory_order_acquire); }

    // Called by the relay at the head of onVideoFrame() to balance the
    // worker→main pending frame counter (incremented before each emit).
    void videoFrameDelivered() { m_PendingVideoFrames.fetch_sub(1, std::memory_order_acq_rel); }

    // Consume the worker-side delta drop flag (true once per drop episode).
    // The relay uses it to arm awaiting-IDR recovery on the main thread.
    bool takeWorkerDroppedDelta() { return m_WorkerDroppedDelta.exchange(false, std::memory_order_acq_rel); }

signals:
    void stageChanged(int stage);
    void connectionStarted();
    void connectionFailed(const QString& error);
    void connectionTerminated(int errorCode);
    void videoFrameReady(QByteArray data, int frameType, int frameNumber);
    void audioSampleReady(QByteArray data);
    void connectionStopped();

private:
    QPointer<QThread> m_WorkerThread;
    std::atomic<bool> m_Connected{false};
    std::atomic<bool> m_Stopping{false};
    std::atomic<bool> m_CleanupDone{false};
    static std::atomic<MoonlightShim*> s_Instance;

    // Metrics (written from worker thread, read from main thread)
    std::atomic<double> m_HostRttMs{0.0};
    std::atomic<int64_t> m_LastDecodeLatencyUs{0};
    std::atomic<int64_t> m_FrameSubmitTimeUs{0};
    std::atomic<int64_t> m_IdrRequestTimeUs{0};
    std::atomic<int64_t> m_FramePresentationTimeUs{0};     // presentationTimeUs from DECODE_UNIT
    std::atomic<int64_t> m_FirstFrameArrivalTimeUs{0};     // steady_clock::now() at first frame arrival (us since epoch)
    std::atomic<int64_t> m_FrameHostProcessingLatencyTenthMs{0}; // frameHostProcessingLatency (tenths of ms)

    // Negotiated video format (set by drSetup during LiStartConnection).
    // 0 = unknown, 0x0001 = H.264, 0x0100 = HEVC, 0x0200 = AV1.
    std::atomic<int> m_NegotiatedVideoFormat{0};

    // Worker→main queue bound: frames emitted via videoFrameReady but not yet
    // processed by the relay. Deltas are dropped worker-side when it saturates.
    std::atomic<int> m_PendingVideoFrames{0};
    std::atomic<bool> m_WorkerDroppedDelta{false};

    void finishCleanup();
    void blockingStopConnection();

    // Video callbacks
    static int  drSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags);
    static void drStart(void);
    static void drStop(void);
    static void drCleanup(void);
    static int  drSubmitDecodeUnit(PDECODE_UNIT decodeUnit);

    // Audio callbacks
    static int  arInit(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags);
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
