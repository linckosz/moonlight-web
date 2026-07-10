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

#include "MoonlightShim.h"

#include "common/MacActivity.h"

extern "C" {
#include "Limelight.h"
#include "PlatformSockets.h"
}

#include <QDebug>
#include <QThread>
#include <QMetaObject>
#include <cstring>
#include <cstdarg>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

std::atomic<MoonlightShim*> MoonlightShim::s_Instance{nullptr};

MoonlightShim::MoonlightShim(QObject* parent)
    : QObject(parent)
{}

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
    // Only clear if we are still the registered instance. During an overlapping
    // teardown a newer session's shim may already own s_Instance — never clobber
    // it (that would silently drop the new connection's decode callbacks).
    MoonlightShim* expected = this;
    s_Instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void MoonlightShim::startConnection(const InitParams& params)
{
    if (m_WorkerThread) {
        qWarning() << "[MoonlightShim] Connection already in progress";
        return;
    }

    // Keep macOS from App Nap'ing this windowless process while streaming —
    // timer coalescing on a background process stalls the relay event loop
    // past the 3-frame pending bound and turns the stream into IDR churn.
    // Released in finishCleanup() (single guaranteed teardown point).
    MacActivity::beginStreaming();
    m_ActivityHeld = true;

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
        // Mobile: advertise a "slow" decoder so moonlight-common-c requests 10ms
        // Opus frames instead of 5ms — halves the packet rate (less RTP/FEC/crypto
        // overhead on constrained networks) with negligible quality loss.
        if (params.slowOpus) arCallbacks.capabilities |= CAPABILITY_SLOW_OPUS_DECODER;

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

        // Silently retry transient early-stage failures. Right after a MWServer
        // (re)start Sunshine can reset the RTSP handshake (stage 4) or refuse the
        // control stream (stage 8) for a second or two — commonly because it is
        // still releasing a previous session. These recover on their own; a short
        // wait + retry avoids bouncing the failure to the browser (which the user
        // otherwise works around by clicking Launch several times). On failure
        // LiStartConnection() cleans up internally, so we must NOT call
        // LiStopConnection() between attempts — just call it again. Later stages
        // (video/input start) are not retried here.
        constexpr int kMaxAttempts = 4;
        constexpr int kRetryDelayMs = 750;
        int err = 0;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            // The user may have quit (browser close / /quit) during the retry
            // wait: LiInterruptConnection() was called, so don't start again.
            if (m_Stopping.load()) break;

            m_LastFailedStage.store(0, std::memory_order_release);
            err = LiStartConnection(&serverInfo, &streamConfig, &clCallbacks, &drCallbacks,
                                    &arCallbacks, nullptr, 0, nullptr, 0);
            if (err == 0) break; // connected

            int failedStage = m_LastFailedStage.load(std::memory_order_acquire);
            bool retriable = failedStage > STAGE_NONE && failedStage <= STAGE_CONTROL_STREAM_START;
            if (m_Stopping.load() || !retriable || attempt == kMaxAttempts) break;

            qWarning() << "[MoonlightShim] Stage" << failedStage << "failed (err=" << err
                       << ") — transient, retry" << attempt << "/" << (kMaxAttempts - 1) << "in"
                       << kRetryDelayMs << "ms";

            // Interruptible wait: bail out at once if the user quits mid-retry.
            for (int waited = 0; waited < kRetryDelayMs && !m_Stopping.load(); waited += 50)
                QThread::msleep(50);
        }

        if (err != 0 && !m_Stopping.load()) {
            int failedStage = m_LastFailedStage.load(std::memory_order_acquire);
            // Preserve the "Stage N failed: E" wording the frontend already parses
            // when a stage callback fired; otherwise fall back to the raw error.
            QString msg = failedStage > STAGE_NONE
                              ? QString("Stage %1 failed: %2")
                                    .arg(failedStage)
                                    .arg(m_LastFailedError.load(std::memory_order_acquire))
                              : QString("LiStartConnection failed: error %1 (socket=%2)")
                                    .arg(err)
                                    .arg(LastSocketFail());
            qWarning() << "[MoonlightShim] Connection failed after" << kMaxAttempts
                       << "attempt(s):" << msg;
            emit connectionFailed(msg);
        }
    });

    m_WorkerThread->start();
}

void MoonlightShim::stopConnection()
{
    // The shim shares the relay's session thread. When stopConnection() is
    // called cross-thread (main: Session::quit, /quit), marshal it so the
    // QThread::finished connect/deleteLater bookkeeping runs on the owning
    // thread. Queued (non-blocking) avoids deadlock; LiInterruptConnection
    // inside still fires promptly (it only sets an atomic flag).
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this]() { stopConnection(); }, Qt::QueuedConnection);
        return;
    }

    // Atomic exchange prevents re-entrant calls (may be called from multiple
    // paths: relay->stop(), Session::quit(), ~MoonlightShim()).
    if (m_Stopping.exchange(true)) {
        qInfo() << "[MoonlightShim::stopConnection] Already stopping, skip re-entrant call";
        return;
    }

    qInfo() << "[MoonlightShim::stopConnection] ENTER"
            << "m_Connected=" << m_Connected.load() << "m_WorkerThread=" << m_WorkerThread;

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
        qInfo() << "[MoonlightShim::stopConnection] Worker thread already finished, calling "
                   "LiStopConnection() now";
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
        qInfo() << "[MoonlightShim::stopConnection] Race: thread finished between isFinished() and "
                   "connect()";
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

    if (m_ActivityHeld) {
        m_ActivityHeld = false;
        MacActivity::endStreaming();
    }

    emit connectionStopped();
}

void MoonlightShim::interruptConnection()
{
    LiInterruptConnection();
}

// --- Video callbacks (called from moonlight internal threads) ---

int MoonlightShim::drSetup(int videoFormat, int width, int height, int redrawRate, void*, int)
{
    qInfo() << "[MoonlightShim] drSetup: videoFormat=" << Qt::hex << videoFormat << Qt::dec << width
            << "x" << height << "@" << redrawRate;

    // Store the negotiated video format so the session can report the
    // actual codec to the frontend (not just the user preference).
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (instance) {
        instance->m_NegotiatedVideoFormat.store(videoFormat, std::memory_order_release);
    }

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

    // Worker→relay queue bound: if the relay thread is backed up, drop deltas
    // here (before the buffer copy) so latency cannot build in the event queue.
    // Keyframes always pass; the relay arms awaiting-IDR via the dropped flag.
    //
    // 12, not 3: video arrives from the host over the network and Wi-Fi
    // aggregation delivers frames in bursts (3-4 at once every few seconds on a
    // MacBook backend). The relay drains the queue in well under a frame
    // interval since it moved to a dedicated thread, so a burst is transient —
    // but with a bound of 3 every burst dropped a delta and forced an IDR
    // round-trip (~200 KB keyframe), i.e. a visible stutter. 12 absorbs ~200 ms
    // of burst at 60 fps while still capping runaway latency if the relay
    // thread genuinely stalls.
    static constexpr int kMaxPendingVideoFrames = 12;
    if (decodeUnit->frameType != 1 &&
        instance->m_PendingVideoFrames.load(std::memory_order_acquire) >= kMaxPendingVideoFrames) {
        instance->m_WorkerDroppedDelta.store(true, std::memory_order_release);
        int64_t dropCount = instance->m_WorkerDropCount.fetch_add(1, std::memory_order_relaxed) + 1;
        // qWarning (not stderr): this drop is the usual trigger of IDR churn and
        // must be visible in the log file to diagnose it from a user capture.
        if (dropCount <= 3 || dropCount % 120 == 0) {
            qWarning() << "[MoonlightShim] Dropped delta worker-side (relay thread backlog),"
                       << "total=" << dropCount;
        }
        return DR_OK;
    }

    // Record frame submit time for decode latency measurement.
    // The relay reads this in sendFragmented() to compute end-to-end pipeline latency.
    auto submitTime = std::chrono::steady_clock::now();
    instance->m_FrameSubmitTimeUs.store(
        std::chrono::duration_cast<std::chrono::microseconds>(submitTime.time_since_epoch())
            .count(),
        std::memory_order_release);

    // ── End-to-end latency tracking ─────────────────────────────────────────
    // Extract the frame's presentation time from the decode unit.
    // presentationTimeUs has its epoch at the first captured frame (set to 0).
    // The frame's capture time on the steady_clock:
    //   captureSteadyMs = (firstFrameArrivalTimeUs + presentationTimeUs) / 1000
    instance->m_FramePresentationTimeUs.store(decodeUnit->presentationTimeUs,
                                              std::memory_order_release);

    // Track host processing latency (capture → encode delay on Sunshine).
    // FrameHostProcessingLatency is in 1/10 ms units. Value is 0 when unknown.
    instance->m_FrameHostProcessingLatencyTenthMs.store(decodeUnit->frameHostProcessingLatency,
                                                        std::memory_order_release);
    if (decodeUnit->frameHostProcessingLatency != 0) {
        instance->m_HostProcWindowTotalTenthMs.fetch_add(decodeUnit->frameHostProcessingLatency,
                                                         std::memory_order_relaxed);
        instance->m_HostProcWindowCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Track the steady_clock arrival time of the first frame.
    // This serves as the reference epoch for capture time calculation:
    //   captureSteadyMs = (firstFrameArrivalTimeUs + presentationTimeUs) / 1000
    // The frontend estimates current steady_clock time from periodic stats
    // messages (streamSteadyMs + performance.now() delta) and subtracts
    // the capture time to get end-to-end latency.
    if (instance->m_FirstFrameArrivalTimeUs.load(std::memory_order_acquire) == 0) {
        int64_t nowUs =
            std::chrono::duration_cast<std::chrono::microseconds>(submitTime.time_since_epoch())
                .count();
        instance->m_FirstFrameArrivalTimeUs.store(nowUs, std::memory_order_release);
        qInfo() << "[MoonlightShim] First frame arrival at steadyUs=" << nowUs
                << "presentationTimeUs=" << decodeUnit->presentationTimeUs
                << "hostProcessingLatency=" << decodeUnit->frameHostProcessingLatency;
    }

    // If a keyframe arrives and we had an outstanding IDR request, measure RTT.
    // hostRttMs = round-trip / 2 (one-way latency from backend to Sunshine).
    if (decodeUnit->frameType == 1) { // FRAME_TYPE_IDR
        int64_t reqTs = instance->m_IdrRequestTimeUs.load(std::memory_order_acquire);
        if (reqTs > 0) {
            int64_t nowUs =
                std::chrono::duration_cast<std::chrono::microseconds>(submitTime.time_since_epoch())
                    .count();
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
    if (frameCount <= 3) {
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
        std::chrono::duration_cast<std::chrono::microseconds>(concatEnd - submitTime).count(),
        std::memory_order_release);

    // Use the local copy — safe even if main thread clears s_Instance concurrently.
    // Count the in-flight frame before emitting (decremented in relay onVideoFrame).
    instance->m_PendingVideoFrames.fetch_add(1, std::memory_order_acq_rel);
    emit instance->videoFrameReady(frameData, decodeUnit->frameType, decodeUnit->frameNumber,
                                   static_cast<qint64>(decodeUnit->presentationTimeUs));
    return DR_OK;
}

// --- Audio callbacks ---

int MoonlightShim::arInit(int, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void*, int)
{
    // Capture the Opus frame size so the relay can pace the RTP audio timestamp
    // by a clean per-packet increment (avoids robotic audio from a jittery clock).
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (instance && opusConfig && opusConfig->samplesPerFrame > 0) {
        instance->m_AudioSamplesPerFrame.store(opusConfig->samplesPerFrame,
                                               std::memory_order_release);
    }
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
    // Record the failure only — do NOT emit connectionFailed here. Emitting on the
    // first stage failure would abort the whole session before any retry: right
    // after a MWServer (re)start Sunshine can transiently reset the RTSP handshake
    // (stage 4) or refuse the control stream (stage 8). startConnection()'s retry
    // loop reads these after LiStartConnection() returns and decides whether to
    // retry silently or surface the error.
    instance->m_LastFailedStage.store(stage, std::memory_order_release);
    instance->m_LastFailedError.store(errorCode, std::memory_order_release);
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
    fprintf(stderr, "[MoonlightShim] Connection terminated, code=%d socketErr=%d\n", errorCode,
            LastSocketFail());
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
    // No fflush: stderr is unbuffered by default and this path can be hot.
    fprintf(stderr, "[moonlight] %s", buffer);
}

void MoonlightShim::clRumble(unsigned short controller, unsigned short low, unsigned short high)
{
    MoonlightShim* instance = s_Instance.load(std::memory_order_acquire);
    if (!instance || instance->m_Stopping.load()) return;
    // Forward to the relay (queued: this runs on the moonlight worker thread).
    emit instance->rumble(controller, low, high);
}
void MoonlightShim::clConnectionStatusUpdate(int) {}
void MoonlightShim::clSetHdrMode(bool) {}

// --- Input wrappers ---

void MoonlightShim::sendKeyEvent(short keyCode, bool down, char modifiers, char flags)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    LiSendKeyboardEvent2(keyCode | 0x8000, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, modifiers,
                         flags);
}

void MoonlightShim::captureHostLockState(bool hostIsSelf)
{
#ifdef Q_OS_WIN
    if (hostIsSelf) {
        int state = 0;
        if (GetKeyState(VK_NUMLOCK) & 0x01) state |= 0x1;
        if (GetKeyState(VK_CAPITAL) & 0x01) state |= 0x2;
        if (GetKeyState(VK_SCROLL) & 0x01) state |= 0x4;
        m_HostLockState.store(state, std::memory_order_release);
        qInfo() << "[MoonlightShim] Host lock state captured:"
                << "NumLock" << bool(state & 0x1) << "CapsLock" << bool(state & 0x2) << "ScrollLock"
                << bool(state & 0x4);
        return;
    }
#else
    Q_UNUSED(hostIsSelf);
#endif
    m_HostLockState.store(-1, std::memory_order_release);
}

void MoonlightShim::syncLockKeys(bool numLock, bool capsLock, bool scrollLock)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    // -1 = no snapshot (remote host / unsupported platform): assume the host
    // starts with every lock off — the common case, and all a client can do
    // without a protocol-level lock-state field.
    const int hostState = m_HostLockState.load(std::memory_order_acquire);
    const struct
    {
        short vk;
        int bit;
        const char* name;
        bool client;
    } locks[] = {
        {0x90, 0x1, "NumLock", numLock},       // VK_NUMLOCK
        {0x14, 0x2, "CapsLock", capsLock},     // VK_CAPITAL
        {0x91, 0x4, "ScrollLock", scrollLock}, // VK_SCROLL
    };
    for (const auto& lock : locks) {
        const bool host = hostState > 0 && (hostState & lock.bit);
        if (host == lock.client) continue;
        qInfo() << "[MoonlightShim] Lock sync:" << lock.name << "host" << host << "-> client"
                << lock.client;
        sendKeyEvent(lock.vk, true, 0, 0);
        sendKeyEvent(lock.vk, false, 0, 0);
    }
}

void MoonlightShim::sendUtf8Text(const QString& text)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    if (text.isEmpty()) return;
    QByteArray utf8 = text.toUtf8();
    LiSendUtf8TextEvent(utf8.constData(), static_cast<unsigned int>(utf8.size()));
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

// Standard gamepad button set exposed by the browser Gamepad API
// ("standard mapping"): A/B/X/Y, dpad, bumpers, start/back, stick clicks, guide.
static constexpr int kStandardGamepadButtons =
    A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG |
    RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG;

void MoonlightShim::sendControllerArrival(uint8_t controllerNumber, uint16_t activeGamepadMask,
                                          uint8_t type, bool hasRumble)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    // Browser triggers are always analog; rumble depends on the gamepad's
    // vibrationActuator (reported by the client). Gyro/touchpad/LED are not
    // reachable via the Gamepad API, so they are not advertised.
    uint16_t caps = LI_CCAP_ANALOG_TRIGGERS;
    if (hasRumble) caps |= LI_CCAP_RUMBLE;
    LiSendControllerArrivalEvent(controllerNumber, activeGamepadMask, type, kStandardGamepadButtons,
                                 caps);
}

void MoonlightShim::sendControllerState(short controllerNumber, short activeGamepadMask,
                                        int buttonFlags, unsigned char leftTrigger,
                                        unsigned char rightTrigger, short leftStickX,
                                        short leftStickY, short rightStickX, short rightStickY)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    LiSendMultiControllerEvent(controllerNumber, activeGamepadMask, buttonFlags, leftTrigger,
                               rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);
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
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count(),
                             std::memory_order_release);
    qInfo() << "[MoonlightShim] Calling LiRequestIdrFrame() to request IDR from Sunshine";
    LiRequestIdrFrame();
}

double MoonlightShim::hostRttMs() const
{
    // ENet control-stream RTT (same source as moonlight-qt's "Average network
    // latency"). Only valid between LiStartConnection and LiStopConnection.
    if (m_Connected.load(std::memory_order_acquire)) {
        uint32_t rttMs = 0;
        uint32_t rttVarianceMs = 0;
        if (LiGetEstimatedRttInfo(&rttMs, &rttVarianceMs) && rttMs > 0) {
            return rttMs / 2.0;
        }
    }
    return m_HostRttMs.load(std::memory_order_acquire);
}

double MoonlightShim::takeHostProcessingLatencyMs()
{
    const int64_t count = m_HostProcWindowCount.exchange(0, std::memory_order_acq_rel);
    const int64_t totalTenthMs =
        m_HostProcWindowTotalTenthMs.exchange(0, std::memory_order_acq_rel);
    if (count <= 0) return 0.0;
    return (totalTenthMs / 10.0) / count;
}
