#include "MoonlightShim.h"

extern "C" {
#include "Limelight.h"
#include "PlatformSockets.h"
}

#include <QDebug>
#include <cstring>
#include <cstdarg>

MoonlightShim* MoonlightShim::s_Instance = nullptr;

MoonlightShim::MoonlightShim(QObject* parent)
    : QObject(parent)
{
}

MoonlightShim::~MoonlightShim()
{
    stopConnection();
}

void MoonlightShim::startConnection(const InitParams& params)
{
    if (m_WorkerThread) {
        qWarning() << "[MoonlightShim] Connection already in progress";
        return;
    }

    s_Instance = this;

    m_WorkerThread = QThread::create([this, params]() {
        SERVER_INFORMATION serverInfo;
        LiInitializeServerInformation(&serverInfo);

        QByteArray addrBytes = params.hostAddress.toUtf8();
        QByteArray appVerBytes = params.appVersion.toUtf8();
        QByteArray gfeVerBytes = params.gfeVersion.toUtf8();
        QByteArray rtspUrlBytes = params.rtspSessionUrl.toUtf8();

        serverInfo.address = addrBytes.constData();
        serverInfo.serverInfoAppVersion = appVerBytes.constData();
        serverInfo.serverInfoGfeVersion = gfeVerBytes.isEmpty() ? nullptr : gfeVerBytes.constData();
        serverInfo.rtspSessionUrl = rtspUrlBytes.isEmpty() ? nullptr : rtspUrlBytes.constData();
        serverInfo.serverCodecModeSupport = params.serverCodecModeSupport;

        STREAM_CONFIGURATION streamConfig;
        LiInitializeStreamConfiguration(&streamConfig);
        streamConfig.width = params.width;
        streamConfig.height = params.height;
        streamConfig.fps = params.fps;
        streamConfig.bitrate = params.bitrateKbps;
        streamConfig.packetSize = params.packetSize;
        streamConfig.streamingRemotely = STREAM_CFG_LOCAL;
        streamConfig.audioConfiguration = params.audioConfiguration;
        streamConfig.supportedVideoFormats = params.supportedVideoFormats;
        streamConfig.clientRefreshRateX100 = 6000;
        streamConfig.colorSpace = 0;
        streamConfig.colorRange = 0;
        streamConfig.encryptionFlags = ENCFLG_AUDIO | ENCFLG_VIDEO;

        memcpy(streamConfig.remoteInputAesKey, params.aesKey.constData(), 16);
        memset(streamConfig.remoteInputAesIv, 0, 16);
        uint32_t iv = static_cast<uint32_t>(params.rikeyid);
        streamConfig.remoteInputAesIv[0] = (iv >> 24) & 0xFF;
        streamConfig.remoteInputAesIv[1] = (iv >> 16) & 0xFF;
        streamConfig.remoteInputAesIv[2] = (iv >> 8) & 0xFF;
        streamConfig.remoteInputAesIv[3] = iv & 0xFF;

        DECODER_RENDERER_CALLBACKS drCallbacks;
        LiInitializeVideoCallbacks(&drCallbacks);
        drCallbacks.setup = drSetup;
        drCallbacks.start = drStart;
        drCallbacks.stop = drStop;
        drCallbacks.cleanup = drCleanup;
        drCallbacks.submitDecodeUnit = drSubmitDecodeUnit;
        drCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;

        AUDIO_RENDERER_CALLBACKS arCallbacks;
        LiInitializeAudioCallbacks(&arCallbacks);
        arCallbacks.init = arInit;
        arCallbacks.start = arStart;
        arCallbacks.stop = arStop;
        arCallbacks.cleanup = arCleanup;
        arCallbacks.decodeAndPlaySample = arDecodeAndPlaySample;
        arCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;

        CONNECTION_LISTENER_CALLBACKS clCallbacks;
        LiInitializeConnectionCallbacks(&clCallbacks);
        clCallbacks.stageStarting = clStageStarting;
        clCallbacks.stageComplete = clStageComplete;
        clCallbacks.stageFailed = clStageFailed;
        clCallbacks.connectionStarted = clConnectionStarted;
        clCallbacks.connectionTerminated = clConnectionTerminated;
        clCallbacks.logMessage = clLogMessage;
        clCallbacks.rumble = clRumble;
        clCallbacks.connectionStatusUpdate = clConnectionStatusUpdate;
        clCallbacks.setHdrMode = clSetHdrMode;

        int err = LiStartConnection(
            &serverInfo, &streamConfig,
            &clCallbacks, &drCallbacks, &arCallbacks,
            nullptr, 0,
            nullptr, 0);

        if (err != 0) {
            QString msg = QString("LiStartConnection failed: error %1 (socket=%2)")
                .arg(err).arg(LastSocketFail());
            fprintf(stderr, "[MoonlightShim] %s\n", qPrintable(msg));
            emit connectionFailed(msg);
        }
    });

    connect(m_WorkerThread, &QThread::finished,
            m_WorkerThread, &QObject::deleteLater);

    m_WorkerThread->start();
}

void MoonlightShim::stopConnection()
{
    if (m_WorkerThread && m_WorkerThread->isRunning()) {
        LiInterruptConnection();
        m_WorkerThread->wait(10000);
    }

    if (m_Connected) {
        LiStopConnection();
        m_Connected = false;
    }

    s_Instance = nullptr;
}

void MoonlightShim::interruptConnection()
{
    LiInterruptConnection();
}

// --- Video callbacks (called from moonlight internal threads) ---

int MoonlightShim::drSetup(int videoFormat, int width, int height, int redrawRate, void*, int)
{
    fprintf(stderr, "[MoonlightShim] drSetup: videoFormat=0x%x %dx%d @%d\n",
            videoFormat, width, height, redrawRate);
    fflush(stderr);
    return 0;
}

void MoonlightShim::drStart() {}
void MoonlightShim::drStop() {}
void MoonlightShim::drCleanup() {}

int MoonlightShim::drSubmitDecodeUnit(PDECODE_UNIT decodeUnit)
{
    if (!s_Instance) return DR_OK;

    // Each LENTRY from moonlight-common-c already contains an Annex B start code
    // (3 or 4 bytes: 00 00 01 or 00 00 00 01) followed by NAL data. Just concatenate
    // them as-is to form a valid Annex B byte stream. No need to prepend start codes.
    int bufCount = 0;
    int totalLen = 0;
    PLENTRY entry = decodeUnit->bufferList;
    while (entry) {
        totalLen += entry->length;
        bufCount++;
        entry = entry->next;
    }

    QByteArray frameData(totalLen, Qt::Uninitialized);
    char* ptr = frameData.data();
    entry = decodeUnit->bufferList;
    while (entry) {
        memcpy(ptr, entry->data, entry->length);
        ptr += entry->length;
        entry = entry->next;
    }

    static int frameCount = 0;
    frameCount++;
    if (frameCount <= 3 || frameCount % 120 == 0) {
        fprintf(stderr, "[MoonlightShim] drSubmitDecodeUnit frame=%d type=%d size=%d bufs=%d\n",
                decodeUnit->frameNumber, decodeUnit->frameType, totalLen, bufCount);

        // Log first 32 bytes of the first frame in hex
        if (frameCount == 1) {
            fprintf(stderr, "[MoonlightShim] First 32 bytes: ");
            for (int i = 0; i < 32 && i < totalLen; i++) {
                fprintf(stderr, "%02x ", (unsigned char)frameData[i]);
            }
            fprintf(stderr, "\n");

            // Log first 4 buffer entries individually
            PLENTRY e = decodeUnit->bufferList;
            for (int i = 0; i < 4 && e; i++) {
                fprintf(stderr, "[MoonlightShim]   buf[%d] len=%d first8=", i, e->length);
                for (int j = 0; j < 8 && j < e->length; j++) {
                    fprintf(stderr, "%02x ", (unsigned char)e->data[j]);
                }
                fprintf(stderr, "\n");
                e = e->next;
            }
        }
        fflush(stderr);
    }

    emit s_Instance->videoFrameReady(frameData, decodeUnit->frameType, decodeUnit->frameNumber);
    return DR_OK;
}

// --- Audio callbacks ---

int MoonlightShim::arInit(int, const POPUS_MULTISTREAM_CONFIGURATION, void*, int)
{
    return 0;
}

void MoonlightShim::arStart() {}
void MoonlightShim::arStop() {}
void MoonlightShim::arCleanup() {}

void MoonlightShim::arDecodeAndPlaySample(char* sampleData, int sampleLength)
{
    if (!s_Instance) return;
    QByteArray data(sampleData, sampleLength);
    emit s_Instance->audioSampleReady(data);
}

// --- Connection listener callbacks ---

void MoonlightShim::clStageStarting(int stage)
{
    if (s_Instance) emit s_Instance->stageChanged(stage);
}

void MoonlightShim::clStageComplete(int stage)
{
    Q_UNUSED(stage);
}

void MoonlightShim::clStageFailed(int stage, int errorCode)
{
    fprintf(stderr, "[MoonlightShim] Stage %d failed, error=%d\n", stage, errorCode);
    if (s_Instance)
        emit s_Instance->connectionFailed(
            QString("Stage %1 failed: %2").arg(stage).arg(errorCode));
}

void MoonlightShim::clConnectionStarted()
{
    fprintf(stderr, "[MoonlightShim] Connection started\n");
    if (s_Instance) {
        s_Instance->m_Connected = true;
        emit s_Instance->connectionStarted();
    }
}

void MoonlightShim::clConnectionTerminated(int errorCode)
{
    fprintf(stderr, "[MoonlightShim] Connection terminated, code=%d socketErr=%d\n",
            errorCode, LastSocketFail());
    if (s_Instance) {
        s_Instance->m_Connected = false;
        emit s_Instance->connectionTerminated(errorCode);
    }
}

void MoonlightShim::clLogMessage(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    fprintf(stderr, "[moonlight] %s", buffer);
    fflush(stderr);
}

void MoonlightShim::clRumble(unsigned short, unsigned short, unsigned short) {}
void MoonlightShim::clConnectionStatusUpdate(int) {}
void MoonlightShim::clSetHdrMode(bool) {}

// --- Input wrappers ---

void MoonlightShim::sendKeyEvent(short keyCode, bool down, char modifiers, char flags)
{
    if (!m_Connected) return;
    LiSendKeyboardEvent2(keyCode | 0x8000,
                         down ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                         modifiers, flags);
}

void MoonlightShim::sendMouseMove(short deltaX, short deltaY)
{
    if (!m_Connected) return;
    LiSendMouseMoveEvent(deltaX, deltaY);
}

void MoonlightShim::sendMouseButton(bool down, int button)
{
    if (!m_Connected) return;
    LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
                          static_cast<char>(button));
}

void MoonlightShim::sendMouseScroll(short scrollAmount)
{
    if (!m_Connected) return;
    LiSendHighResScrollEvent(scrollAmount);
}

// MOC
#include "moc_MoonlightShim.cpp"
