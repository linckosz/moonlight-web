#pragma once

#include <QObject>
#include <QThread>
#include <QPointer>
#include <QByteArray>
#include <atomic>
#include <cstdint>
#include <memory>

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
    void sendMouseMove(short deltaX, short deltaY);
    void sendMouseButton(bool down, int button);
    void sendMouseScroll(short scrollAmount);

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
