#include "MoonlightShim.h"

extern "C" {
#include "Limelight.h"
#include "PlatformSockets.h"
}

#include <QDebug>
#include <cstring>
#include <cstdarg>

std::atomic<MoonlightShim*> MoonlightShim::s_Instance{nullptr};

MoonlightShim::MoonlightShim(QObject* parent)
    : QObject(parent)
{
}

MoonlightShim::~MoonlightShim()
{
    // Use blocking stop to ensure cleanup completes before destruction.
    // Non-blocking stopConnection() defers LiStopConnection() via the
    // QThread::finished signal, but the destructor cannot wait for that
    // event asynchronously. A blocking call ensures LiStopConnection()
    // runs before s_Instance is cleared and this object is destroyed.
    if (!m_CleanupDone.load(std::memory_order_acquire)) {
        blockingStopConnection();
    }
    s_Instance.store(nullptr, std::memory_order_release);
}

void MoonlightShim::startConnection(const InitParams& params)
{
    if (m_WorkerThread) {
        qWarning() << "[MoonlightShim] Connection already in progress";
        return;
    }

    s_Instance.store(this, std::memory_order_release);

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
        streamConfig.colorSpace = params.colorSpace;
        streamConfig.colorRange = params.colorRange;
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

    m_WorkerThread->start();
}

void MoonlightShim::stopConnection()
{
    // Atomic exchange prevents re-entrant calls (may be called from multiple
    // paths: relay->stop(), Session::quit(), ~MoonlightShim()).
    if (m_Stopping.exchange(true)) {
        qInfo() << "[MoonlightShim::stopConnection] Already stopping, skip re-entrant call";
        return;
    }

    qInfo() << "[MoonlightShim::stopConnection] ENTER"
            << "m_Connected=" << m_Connected.load()
            << "m_WorkerThread=" << m_WorkerThread;

    m_Connected = false;

    // LiInterruptConnection() is thread-safe — just sets an atomic flag in
    // moonlight-common-c. The worker thread's LiStartConnection() will check
    // this flag (in ENet recv loops) and return early.
    LiInterruptConnection();

    // Take ownership of the worker thread pointer. From this point on,
    // m_WorkerThread is nullptr — the thread will be cleaned up via the
    // captured lambda or by blockingStopConnection().
    QThread* thread = m_WorkerThread;
    m_WorkerThread = nullptr;

    if (!thread) {
        qInfo() << "[MoonlightShim::stopConnection] No worker thread existed";
        // LiStartConnection() was never called; no LiStopConnection() needed.
        return;
    }

    // Thread exists — check if it already finished.
    if (thread->isFinished()) {
        qInfo() << "[MoonlightShim::stopConnection] Worker thread already finished, calling LiStopConnection() now";
        thread->deleteLater();
        finishCleanup();
        return;
    }

    // Thread is still running.  Defer cleanup via the QThread::finished signal:
    // the lambda will execute on the main thread (this QObject's thread) after
    // the worker thread exits LiStartConnection() and run() returns.
    //
    // Race condition: between isFinished() and connect() below, the thread
    // could finish and emit finished().  To handle this, we check isFinished()
    // again AFTER connect().
    qInfo() << "[MoonlightShim::stopConnection] Deferring LiStopConnection() via QThread::finished";

    connect(thread, &QThread::finished, this, [this, thread]() {
        thread->deleteLater();
        finishCleanup();
    });

    // Post-connect race check: did the thread finish between isFinished() and connect()?
    if (thread->isFinished()) {
        qInfo() << "[MoonlightShim::stopConnection] Race: thread finished between isFinished() and connect()";
        // The finished signal was already emitted (or was queued cross-thread).
        // cleanupFn is safe to call directly; m_CleanupDone atomically guards
        // against the queued event running it a second time.
        finishCleanup();
    }
}

void MoonlightShim::blockingStopConnection()
{
    // Called from the destructor when deferred cleanup hasn't completed yet.
    // May also be called if stopConnection() was never invoked.
    if (!m_Stopping.exchange(true)) {
        m_Connected = false;
        LiInterruptConnection();
    }

    QThread* thread = m_WorkerThread;
    m_WorkerThread = nullptr;

    if (thread) {
        if (thread->isRunning()) {
            qInfo() << "[MoonlightShim::blockingStopConnection] Waiting for worker thread ...";
            if (!thread->wait(10000)) {
                qWarning() << "[MoonlightShim::blockingStopConnection] TIMEOUT (10s)";
            }
        }
        thread->deleteLater();
    }

    if (!m_CleanupDone.load(std::memory_order_acquire)) {
        finishCleanup();
    }
}

void MoonlightShim::finishCleanup()
{
    // Atomic guard: LiStopConnection() must be called exactly once, no matter
    // how many paths race to invoke this function (finished signal, direct call
    // from stopConnection, direct call from blockingStopConnection).
    if (m_CleanupDone.exchange(true)) {
        qInfo() << "[MoonlightShim::finishCleanup] Already done, skipping";
        return;
    }

    qInfo() << "[MoonlightShim::finishCleanup] Calling LiStopConnection() ...";
    LiStopConnection();
    qInfo() << "[MoonlightShim::finishCleanup] LiStopConnection() returned OK";

    emit connectionStopped();
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

    // Store the negotiated video format so the session can report the
    // actual codec to the frontend (not just the user preference).
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (instance) {
        instance->m_NegotiatedVideoFormat.store(videoFormat, std::memory_order_release);
    }

    fflush(stderr);
    return 0;
}

void MoonlightShim::drStart() {}
void MoonlightShim::drStop() {}
void MoonlightShim::drCleanup() {}

int MoonlightShim::drSubmitDecodeUnit(PDECODE_UNIT decodeUnit)
{
    // Take a local copy of the atomic s_Instance to close the TOCTOU window.
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (!instance) return DR_OK;

    // Bail early during shutdown
    if (instance->m_Stopping.load(std::memory_order_acquire)) {
        return DR_OK;
    }

    // Record frame submit time for decode latency measurement.
    // The relay reads this in sendFragmented() to compute end-to-end pipeline latency.
    auto submitTime = std::chrono::steady_clock::now();
    instance->m_FrameSubmitTimeUs.store(
        std::chrono::duration_cast<std::chrono::microseconds>(
            submitTime.time_since_epoch()).count(),
        std::memory_order_release);

    // If a keyframe arrives and we had an outstanding IDR request, measure RTT.
    // hostRttMs = round-trip / 2 (one-way latency from backend to Sunshine).
    if (decodeUnit->frameType == 1) { // FRAME_TYPE_IDR
        int64_t reqTs = instance->m_IdrRequestTimeUs.load(std::memory_order_acquire);
        if (reqTs > 0) {
            int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                submitTime.time_since_epoch()).count();
            double rttUs = static_cast<double>(nowUs - reqTs);
            // Only update if RTT is plausible (< 10s)
            if (rttUs > 0 && rttUs < 10'000'000) {
                instance->m_HostRttMs.store(rttUs / 2000.0, std::memory_order_release);
            }
            // Reset the request timestamp to avoid reusing stale measurements
            instance->m_IdrRequestTimeUs.store(0, std::memory_order_release);
        }
    }

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

    // Measure the time spent in buffer concatenation — contributes to decode latency.
    auto concatEnd = std::chrono::steady_clock::now();
    instance->m_LastDecodeLatencyUs.store(
        std::chrono::duration_cast<std::chrono::microseconds>(
            concatEnd - submitTime).count(),
        std::memory_order_release);

    // Use the local copy — safe even if main thread clears s_Instance concurrently.
    emit instance->videoFrameReady(frameData, decodeUnit->frameType, decodeUnit->frameNumber);
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
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (!instance) return;
    if (instance->m_Stopping.load(std::memory_order_acquire)) return;
    QByteArray data(sampleData, sampleLength);
    emit instance->audioSampleReady(data);
}

// --- Connection listener callbacks ---

void MoonlightShim::clStageStarting(int stage)
{
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (!instance || instance->m_Stopping.load()) return;
    emit instance->stageChanged(stage);
}

void MoonlightShim::clStageComplete(int stage)
{
    Q_UNUSED(stage);
}

void MoonlightShim::clStageFailed(int stage, int errorCode)
{
    fprintf(stderr, "[MoonlightShim] Stage %d failed, error=%d\n", stage, errorCode);
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (!instance || instance->m_Stopping.load()) return;
    emit instance->connectionFailed(
        QString("Stage %1 failed: %2").arg(stage).arg(errorCode));
}

void MoonlightShim::clConnectionStarted()
{
    fprintf(stderr, "[MoonlightShim] Connection started\n");
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (!instance || instance->m_Stopping.load()) return;
    instance->m_Connected.store(true, std::memory_order_release);
    emit instance->connectionStarted();
}

void MoonlightShim::clConnectionTerminated(int errorCode)
{
    fprintf(stderr, "[MoonlightShim] Connection terminated, code=%d socketErr=%d\n",
            errorCode, LastSocketFail());
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (!instance || instance->m_Stopping.load()) return;
    instance->m_Connected.store(false, std::memory_order_release);
    emit instance->connectionTerminated(errorCode);
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
    if (!m_Connected.load(std::memory_order_acquire)) return;
    LiSendKeyboardEvent2(keyCode | 0x8000,
                         down ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                         modifiers, flags);
}

void MoonlightShim::sendMouseMove(short deltaX, short deltaY)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    LiSendMouseMoveEvent(deltaX, deltaY);
}

void MoonlightShim::sendMousePosition(short x, short y, short referenceWidth, short referenceHeight)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    LiSendMousePositionEvent(x, y, referenceWidth, referenceHeight);
}

void MoonlightShim::sendMouseButton(bool down, int button)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
                          static_cast<char>(button));
}

void MoonlightShim::sendMouseScroll(short scrollAmount)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    LiSendHighResScrollEvent(scrollAmount);
}

void MoonlightShim::requestIdrFrame()
{
    if (!m_Connected.load(std::memory_order_acquire)) {
        qWarning() << "[MoonlightShim] requestIdrFrame skipped — not connected";
        return;
    }
    // Record the request timestamp for RTT measurement.
    // When the next keyframe arrives in drSubmitDecodeUnit(), we compute
    // hostRttMs = (now - requestTimestamp) / 2 (half round-trip).
    m_IdrRequestTimeUs.store(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_release);
    qInfo() << "[MoonlightShim] Calling LiRequestIdrFrame() to request IDR from Sunshine";
    LiRequestIdrFrame();
}

