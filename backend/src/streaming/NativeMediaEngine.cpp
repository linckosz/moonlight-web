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

#include "NativeMediaEngine.h"
#include "InputWatchdog.h"
#include "NativeBench.h"

#include "mw/native/NativeHost.h"

#include <QBuffer>
#include <QDebug>
#include <QImage>

namespace {

// VIDEO_FORMAT_* bits, as moonlight-common-c defines them and as the frontend
// already understands. Spelled out here rather than included: this file is the
// boundary, and pulling Limelight.h in for three constants would drag a GPL
// header into the one place that must stay able to talk to both sides.
//
// Whole-codec MASKS on the way in (any profile of the codec means the browser
// decodes that codec), specific profiles on the way out. The first version had
// AV1 as 0x0200 — which is HEVC Main10 — so AV1 was never selectable on the
// native host and an HDR HEVC request read as an AV1 one. Found by the encoder
// bench on 04/09/2026, when an AV1 stream came back as HEVC.
constexpr int kVideoFormatMaskH264 = 0x000F;
constexpr int kVideoFormatMaskHevc = 0x0F00;
constexpr int kVideoFormatMaskAv1 = 0xF000;
constexpr int kVideoFormatH264 = 0x0001;        // High
constexpr int kVideoFormatH264High444 = 0x0004; // High 4:4:4 8-bit
constexpr int kVideoFormatHevc = 0x0100;        // Main
constexpr int kVideoFormatHevcMain10 = 0x0200;  // Main10
constexpr int kVideoFormatHevcRext444 = 0x0400; // RExt 4:4:4 8-bit
constexpr int kVideoFormatAv1 = 0x1000;         // Main 8-bit
constexpr int kVideoFormatAv1Main10 = 0x2000;   // Main 10-bit
constexpr int kVideoFormatAv1High444 = 0x4000;  // High 4:4:4 8-bit

/// Frame type as the relays read it: 1 is a keyframe (FRAME_TYPE_IDR).
constexpr int kFrameTypeKeyframe = 1;
constexpr int kFrameTypeDelta = 0;

/// The client's decodable formats, in the engine's own preference order.
std::vector<mw::native::Codec> codecsFromMask(int mask)
{
    std::vector<mw::native::Codec> codecs;
    // Order matters and is ours, not the mask's: AV1 first for its
    // royalty-free licence and quality per bit, then HEVC, then the floor.
    if (mask & kVideoFormatMaskAv1) codecs.push_back(mw::native::Codec::Av1);
    if (mask & kVideoFormatMaskHevc) codecs.push_back(mw::native::Codec::Hevc);
    if (mask & kVideoFormatMaskH264) codecs.push_back(mw::native::Codec::H264);
    // An empty mask means the caller never filled it in. H.264 is the only
    // format every browser decodes, so it is the safe assumption — and a
    // wrong-but-decodable stream beats refusing to start.
    if (codecs.empty()) codecs.push_back(mw::native::Codec::H264);
    return codecs;
}

/// The VIDEO_FORMAT_* bit of what the session really produces — codec AND
/// profile, since 4:4:4 and 10-bit are what the bits distinguish.
int formatFromSession(const mw::native::SessionInfo& info)
{
    switch (info.codec) {
    case mw::native::Codec::Av1:
        return info.yuv444 ? kVideoFormatAv1High444
               : info.hdr  ? kVideoFormatAv1Main10
                           : kVideoFormatAv1;
    case mw::native::Codec::Hevc:
        return info.yuv444 ? kVideoFormatHevcRext444
               : info.hdr  ? kVideoFormatHevcMain10
                           : kVideoFormatHevc;
    case mw::native::Codec::H264: break;
    }
    return info.yuv444 ? kVideoFormatH264High444 : kVideoFormatH264;
}

} // namespace

NativeMediaEngine::NativeMediaEngine(QObject* parent)
    : IMediaEngine(parent)
{
    // Route the engine's logging into ours once, so a native session explains
    // itself in the same log file as everything else.
    mw::native::NativeHost::setLogSink([](int level, const std::string& message) {
        const QString text = QString::fromStdString(message);
        switch (level) {
        case 0: qDebug().noquote() << text; break;
        case 2: qWarning().noquote() << text; break;
        case 3: qCritical().noquote() << text; break;
        default: qInfo().noquote() << text; break;
        }
    });

    // What letting go means here: the same events the relays send, on the
    // same path. A session that is already gone drops them, like every other
    // input after stopConnection().
    InputWatchdog::Sink sink;
    sink.releaseKey = [this](const HeldKey& k) {
        if (!m_Session) return;
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::KeyUp;
        event.keyCode = k.keyCode;
        event.keyFlags = static_cast<uint8_t>(k.flags);
        m_Session->sendInput(event);
    };
    sink.releaseButton = [this](int button) {
        if (!m_Session) return;
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::MouseButtonUp;
        event.button = button;
        m_Session->sendInput(event);
    };
    sink.neutralizePad = [this](short controller, short mask) {
        if (!m_Session) return;
        // Already shifted when it was noted: goes on the wire as it is.
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::ControllerState;
        event.controllerNumber = static_cast<uint8_t>(controller);
        event.activeGamepadMask = static_cast<uint16_t>(mask);
        m_Session->sendInput(event);
    };
    m_Watchdog = new InputWatchdog(std::move(sink), this);
}

NativeMediaEngine::~NativeMediaEngine()
{
    // Qualified: during destruction the vtable is already ours, so the call is
    // never dispatched to an override — say so rather than let it read as one.
    NativeMediaEngine::stopConnection();
}

void NativeMediaEngine::startCapture(const StartParams& params)
{
    mw::native::SessionConfig config;
    config.displayId = params.displayId;
    config.width = params.width;
    config.height = params.height;
    config.fps = params.fps;
    config.bitrateKbps = params.bitrateKbps;
    config.clientCodecs = codecsFromMask(params.clientVideoFormats);
    config.hdr = params.hdr;
    config.yuv444 = params.yuv444;
    config.intraRefresh = params.intraRefresh;
    // The client's screen: what /start carried, unless a `clientrefresh`
    // message already said otherwise (a session that starts after the
    // client's window moved).
    if (m_ClientRefreshKnown.load(std::memory_order_acquire)) {
        config.clientRefreshMilliHz = m_ClientRefreshMilliHz.load(std::memory_order_relaxed);
        config.clientVsync = m_ClientVsync.load(std::memory_order_relaxed);
    } else {
        config.clientRefreshMilliHz = params.clientRefreshMilliHz;
        config.clientVsync = params.clientVsync;
    }

    // The bench's encoder knobs, on a real session, from the environment: the
    // one way to put two encoder settings in front of a person on the same
    // screen (plan v2 §5, the A/B). Never set in production — nothing in the
    // product writes it — and logged loudly when it is, so a stray variable
    // cannot pass for the engine's own choice.
    const QString tuningSpec = qEnvironmentVariable("MW_NATIVE_TUNING");
    if (!tuningSpec.isEmpty()) {
        QString parseError;
        if (parseEncoderTuningSpec(tuningSpec, config.tuning, config.encodeGpuId, parseError))
            qWarning().noquote() << "[NativeMediaEngine] MW_NATIVE_TUNING in effect:" << tuningSpec
                                 << "— this session does not run the engine's own settings";
        else
            qWarning().noquote() << "[NativeMediaEngine] MW_NATIVE_TUNING ignored:" << parseError;
    }

    std::string error;
    m_Session = mw::native::NativeHost::createSession(
        config, [this](const mw::native::EncodedFrame& frame) { onEncodedFrame(frame); },
        // Opus packets, 5 ms each, from the engine's audio thread. Emitted as
        // the same signal the GameStream engine emits from moonlight-common-c's
        // audio thread: the relays already copy the bytes onto their own
        // thread and stamp the RTP clock by audioSamplesPerFrame(), which is
        // the 240 the engine produces.
        [this](const mw::native::AudioPacket& packet) {
            if (!packet.data || packet.size == 0) return;
            emit audioSampleReady(QByteArray(reinterpret_cast<const char*>(packet.data),
                                             static_cast<qsizetype>(packet.size)));
        },
        // Rumble is the only signal that travels host → browser. It arrives on
        // a ViGEm callback thread, so it is emitted as a queued signal rather
        // than touched directly: the relay that forwards it lives on another
        // thread and owns a DataChannel that is not thread-safe.
        [this](const mw::native::RumbleEvent& event) {
            QMetaObject::invokeMethod(
                this,
                [this, event]() {
                    emit rumble(event.controllerNumber, event.lowFrequencyMotor,
                                event.highFrequencyMotor);
                },
                Qt::QueuedConnection);
        },
        [this](const mw::native::CursorUpdate& cursor) { onCursor(cursor); },
        [this](const std::string& reason) {
            m_Connected.store(false, std::memory_order_release);
            // connectionTerminated is what the relays already watch for, so an
            // engine that dies on its own tears the session down exactly like a
            // GameStream host that dropped.
            emit connectionTerminated(-1);
            qWarning() << "[NativeMediaEngine] session ended:" << QString::fromStdString(reason);
        },
        error);

    if (!m_Session) {
        qWarning() << "[NativeMediaEngine] could not create session:"
                   << QString::fromStdString(error);
        emit connectionFailed(QString::fromStdString(error));
        return;
    }

    if (!m_Session->start(error)) {
        qWarning() << "[NativeMediaEngine] could not start session:"
                   << QString::fromStdString(error);
        m_Session.reset();
        emit connectionFailed(QString::fromStdString(error));
        return;
    }

    // Whatever the client asked for before there was a session to ask. It says
    // it once, on a channel that opens ahead of the capture, and deduplicates
    // after — so replaying it here is what makes the first minute of a session
    // behave like every minute after it.
    if (const int floor = m_FrameFloorFps.load(std::memory_order_acquire); floor > 0)
        m_Session->setFrameFloorFps(floor);

    const mw::native::SessionInfo& info = m_Session->info();
    m_NegotiatedVideoFormat.store(formatFromSession(info), std::memory_order_release);
    m_Connected.store(true, std::memory_order_release);

    qInfo().noquote() << "[NativeMediaEngine] streaming" << describeSession();
    emit connectionStarted();
}

void NativeMediaEngine::setDirectFrameSink(FrameSink sink)
{
    std::lock_guard<std::mutex> lock(m_SinkMutex);
    m_DirectSink = std::move(sink);
}

void NativeMediaEngine::onEncodedFrame(const mw::native::EncodedFrame& encoded)
{
    if (!encoded.data || encoded.size == 0) return;

    const int64_t presentUs = encoded.presentUs;
    const int64_t encodedUs = encoded.encodedUs;
    const uint32_t frameNumber = encoded.frameNumber;

    // The host-side stages this thread can close, and the stamps the sender
    // will need to close the rest. Under the lock, briefly: the sender thread
    // may be reporting the previous frame right now.
    {
        std::lock_guard<std::mutex> lock(m_StageMutex);
        m_Stages.record(mw::native::Stage::Acquire, encoded.capturedUs - presentUs);
        m_Stages.record(mw::native::Stage::Convert, encoded.convertedUs - encoded.submittedUs);
        m_Stages.record(mw::native::Stage::Encode, encodedUs - encoded.convertedUs);
        InFlight& slot = m_InFlight[frameNumber % kInFlightRing];
        slot.frameNumber = frameNumber;
        slot.valid = true;
        slot.presentUs = presentUs;
        slot.encodedUs = encodedUs;
    }

    // The relay's contract for the frame's presentation time is the shim's:
    // microseconds since the FIRST frame, added to firstFrameArrivalSteadyMs()
    // to get back to the steady clock. The engine hands us an absolute
    // steady_clock stamp, so the epoch has to be taken out here, or the sum
    // counts the clock twice and the client sees capture time run at 2×.
    //
    // The epoch is the first frame's own present time — a real display present,
    // not an arrival at the relay — so what the client receives IS the present
    // time on our steady clock, to the millisecond.
    int64_t epochUs = 0; // on failure, receives the stamp already stored
    if (m_FirstPresentUs.compare_exchange_strong(epochUs, presentUs, std::memory_order_acq_rel))
        epochUs = presentUs;
    const int64_t relativePresentUs = presentUs > epochUs ? presentUs - epochUs : 0;

    m_FramePresentationTimeUs.store(relativePresentUs, std::memory_order_release);
    m_FrameSubmitTimeUs.store(encoded.submittedUs, std::memory_order_release);

    // Measured, not reported: the real time from the display presenting the
    // frame to the encoder finishing it.
    if (encodedUs > presentUs) {
        m_ProcWindowTotalUs.fetch_add(encodedUs - presentUs, std::memory_order_acq_rel);
        m_ProcWindowCount.fetch_add(1, std::memory_order_acq_rel);
    }

    // The zero-copy path: the relay that owns the video takes the frame from
    // the encoder's own buffer, here, on this thread, and fragments it before
    // we return. No QByteArray, no signal, no queue.
    {
        std::lock_guard<std::mutex> lock(m_SinkMutex);
        if (m_DirectSink) {
            FrameView view;
            view.data = encoded.data;
            view.size = encoded.size;
            view.keyframe = encoded.keyframe;
            view.frameNumber = frameNumber;
            view.presentationTimeUs = relativePresentUs;
            // Same bookkeeping as the signal path: the consumer balances it with
            // videoFrameDelivered(), so the relays' drop diagnostics stay true.
            m_PendingVideoFrames.fetch_add(1, std::memory_order_acq_rel);
            m_DirectSink(view);
            return;
        }
    }

    // The signal path — every other listener: the media-track relay, the
    // WebSocket fallback, the legacy WSS relay. One copy, unavoidable here: the
    // engine's buffer is unlocked the moment this returns, while a queued
    // receiver reads it later. The GameStream path makes the same copy — after
    // having already paid for RTP, FEC, decryption and reassembly to produce
    // it.
    QByteArray frame(reinterpret_cast<const char*>(encoded.data), static_cast<int>(encoded.size));

    m_PendingVideoFrames.fetch_add(1, std::memory_order_acq_rel);

    // Emitted on the capture thread. The relays' connections decide whether
    // that stays direct — which is the point: the GameStream path pays a queued
    // hop here, and not paying it is one of the reasons this engine exists.
    emit videoFrameReady(frame, encoded.keyframe ? kFrameTypeKeyframe : kFrameTypeDelta,
                         static_cast<int>(frameNumber), relativePresentUs);
}

void NativeMediaEngine::frameSent(uint32_t frameNumber, int64_t firstByteUs, int64_t lastByteUs)
{
    std::lock_guard<std::mutex> lock(m_StageMutex);
    InFlight& slot = m_InFlight[frameNumber % kInFlightRing];
    // A frame the sender evicted never gets here, and its slot is overwritten
    // by a later frame; the number check is what stops that later frame's
    // stamps from being scored against this one's send.
    if (!slot.valid || slot.frameNumber != frameNumber) return;
    slot.valid = false;
    m_Stages.record(mw::native::Stage::Queue, firstByteUs - slot.encodedUs);
    m_Stages.record(mw::native::Stage::Send, lastByteUs - firstByteUs);
    m_Stages.record(mw::native::Stage::Total, lastByteUs - slot.presentUs);
}

QJsonObject NativeMediaEngine::takeStageStats()
{
    std::array<mw::native::StageSummary, static_cast<size_t>(mw::native::Stage::Count)> window;
    {
        std::lock_guard<std::mutex> lock(m_StageMutex);
        window = m_Stages.takeWindow();
    }
    // Microseconds, as integers: the client divides. A stage with no sample in
    // this window is left out rather than reported as zero, which would read as
    // "instant" on an overlay.
    QJsonObject out;
    for (size_t i = 0; i < window.size(); ++i) {
        const mw::native::StageSummary& s = window[i];
        if (s.count <= 0) continue;
        QJsonObject stage;
        stage["n"] = static_cast<qint64>(s.count);
        stage["avg"] = static_cast<qint64>(s.meanUs);
        stage["p50"] = static_cast<qint64>(s.p50Us);
        stage["p95"] = static_cast<qint64>(s.p95Us);
        stage["p99"] = static_cast<qint64>(s.p99Us);
        stage["max"] = static_cast<qint64>(s.maxUs);
        out[QString::fromLatin1(mw::native::toString(static_cast<mw::native::Stage>(i)))] = stage;
    }
    return out;
}

void NativeMediaEngine::logStageSummary()
{
    std::string line;
    int64_t frames = 0;
    {
        std::lock_guard<std::mutex> lock(m_StageMutex);
        if (m_StageSummaryLogged) return;
        m_StageSummaryLogged = true;
        frames = m_Stages.sessionFrames();
        line = mw::native::StageStats::describe(m_Stages.session());
    }
    if (line.empty()) return;
    // The one line a session leaves behind about where its time went. Tail
    // figures are what to read: a mean hides the frame that was felt.
    // "sent frames": the count of frames the sender closed the timeline for.
    // A frame replayed from the relay's pre-open keyframe buffer, or dropped
    // before the channel opened, has host-side stamps but no send — the per-
    // stage n in the line can therefore be a little higher.
    qInfo().noquote() << "[NativeMediaEngine] host stages over" << frames
                      << "sent frames:" << QString::fromStdString(line);
}

void NativeMediaEngine::stopConnection()
{
    if (!m_Session) return;
    m_Connected.store(false, std::memory_order_release);
    m_Session->stop();
    m_Session.reset();
    logStageSummary();
    emit connectionStopped();
}

void NativeMediaEngine::interruptConnection()
{
    // Nothing to unwind: there is no protocol handshake to abandon and no
    // socket to abort, so an interrupt is just a stop.
    stopConnection();
}

void NativeMediaEngine::requestIdrFrame()
{
    if (m_Session) m_Session->requestKeyframe();
}

void NativeMediaEngine::videoFrameDelivered()
{
    m_PendingVideoFrames.fetch_sub(1, std::memory_order_acq_rel);
}

bool NativeMediaEngine::takeWorkerDroppedDelta()
{
    return m_WorkerDroppedDelta.exchange(false, std::memory_order_acq_rel);
}

int64_t NativeMediaEngine::workerDropCount() const
{
    return m_WorkerDropCount.load(std::memory_order_relaxed);
}

int NativeMediaEngine::pendingVideoFrames() const
{
    return m_PendingVideoFrames.load(std::memory_order_acquire);
}

int NativeMediaEngine::negotiatedVideoFormat() const
{
    return m_NegotiatedVideoFormat.load(std::memory_order_acquire);
}

int NativeMediaEngine::audioSamplesPerFrame() const
{
    // 5 ms at 48 kHz, matching what the pipeline already carries. Real audio
    // capture will confirm or replace it.
    return 240;
}

double NativeMediaEngine::takeHostProcessingLatencyMs()
{
    const int64_t count = m_ProcWindowCount.exchange(0, std::memory_order_acq_rel);
    const int64_t total = m_ProcWindowTotalUs.exchange(0, std::memory_order_acq_rel);
    if (count <= 0) return 0.0;
    return static_cast<double>(total) / static_cast<double>(count) / 1000.0;
}

int64_t NativeMediaEngine::frameSubmitTimeUs() const
{
    return m_FrameSubmitTimeUs.load(std::memory_order_acquire);
}

int64_t NativeMediaEngine::framePresentationTimeUs() const
{
    return m_FramePresentationTimeUs.load(std::memory_order_acquire);
}

int64_t NativeMediaEngine::firstFrameArrivalSteadyMs() const
{
    // The epoch the relative presentation times are counted from — see
    // onEncodedFrame. Named for the shim's contract; here it is the first
    // frame's display present, which is the better t₀.
    return m_FirstPresentUs.load(std::memory_order_acquire) / 1000;
}

void NativeMediaEngine::onCursor(const mw::native::CursorUpdate& cursor)
{
    // Encoded HERE, on the capture thread, because the pixels are borrowed and
    // stop being valid the moment this returns. A PNG of a 32×32 cursor is a
    // couple of hundred bytes and this runs only when the shape changes, so the
    // cost is invisible next to a frame.
    QByteArray png;
    if (cursor.visible && cursor.pixels && cursor.width > 0 && cursor.height > 0) {
        // Format_ARGB32 is BGRA in memory on a little-endian machine, which is
        // exactly what the engine hands over. The copy() is not optional: the
        // QImage would otherwise keep pointing at a buffer we do not own.
        const QImage image(cursor.pixels, cursor.width, cursor.height, cursor.width * 4,
                           QImage::Format_ARGB32);
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.copy().save(&buffer, "PNG");
    }

    QMetaObject::invokeMethod(
        this,
        [this, png, x = cursor.hotspotX, y = cursor.hotspotY, visible = cursor.visible,
         kind = QString::fromLatin1(cursor.kind ? cursor.kind : ""),
         scale = static_cast<double>(cursor.scale)]() {
            emit cursorShapeChanged(png, x, y, visible, kind, scale);
        },
        Qt::QueuedConnection);
}

void NativeMediaEngine::setCompositeCursor(bool composite, int cursorFramePx)
{
    if (m_Session) m_Session->setCompositeCursor(composite, cursorFramePx);
}

void NativeMediaEngine::setFrameFloorFps(int fps)
{
    m_FrameFloorFps.store(fps, std::memory_order_release);
    if (m_Session) m_Session->setFrameFloorFps(fps);
}

void NativeMediaEngine::invalidateReference(uint32_t frameNumber)
{
    if (m_Session) m_Session->invalidateReference(frameNumber);
}

void NativeMediaEngine::setClientRefresh(int milliHz, bool vsync)
{
    m_ClientRefreshMilliHz.store(milliHz, std::memory_order_relaxed);
    m_ClientVsync.store(vsync, std::memory_order_relaxed);
    m_ClientRefreshKnown.store(true, std::memory_order_release);
    if (m_Session) m_Session->setClientRefresh(milliHz, vsync);
}

bool NativeMediaEngine::referenceInvalidation() const
{
    return m_Session && m_Connected.load(std::memory_order_acquire) &&
           m_Session->info().referenceInvalidation;
}

void NativeMediaEngine::reportLink(const mw::native::LinkFeedback& feedback)
{
    mw::native::LinkFeedback fb = feedback;
    fb.evictions += m_Evictions.exchange(0, std::memory_order_relaxed);
    if (m_Session) m_Session->reportLink(fb);
}

bool NativeMediaEngine::intraRefreshActive() const
{
    return m_Session && m_Session->info().intraRefresh;
}

QString NativeMediaEngine::describeSession() const
{
    if (!m_Session) return {};
    const mw::native::SessionInfo& info = m_Session->info();

    QString text = QString::fromStdString(info.gpuName) + QStringLiteral(" · ") +
                   QString::fromUtf8(mw::native::toString(info.encoder)) + QLatin1Char(' ') +
                   QString::fromUtf8(mw::native::toString(info.codec));
    if (info.yuv444) text += QStringLiteral(" 4:4:4");
    if (info.hdr) text += QStringLiteral(" HDR");
    if (info.intraRefresh) text += QStringLiteral(" intra-refresh");
    if (info.crossGpuCopy) text += QStringLiteral(" [cross-GPU copy]");
    return text;
}

// ── Input ───────────────────────────────────────────────────────────────────
//
// Injection lands with the input backend; these translate and forward. The
// engine ignores what it cannot yet apply rather than queueing it, because a
// queue would deliver a burst of stale events the moment it drained.

void NativeMediaEngine::sendKeyEvent(short keyCode, bool down, char modifiers, char flags,
                                     bool hold)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = down ? mw::native::InputEvent::Type::KeyDown : mw::native::InputEvent::Type::KeyUp;
    event.keyCode = keyCode;
    event.modifiers = static_cast<uint8_t>(modifiers);
    event.keyFlags = static_cast<uint8_t>(flags);
    event.hold = hold;
    HeldKey k;
    k.keyCode = keyCode;
    k.modifiers = modifiers;
    k.flags = flags;
    k.hold = hold;
    m_Watchdog->noteKey(k, down);
    m_Session->sendInput(event);
}

void NativeMediaEngine::sendUtf8Text(const QString& text)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::Utf8Text;
    event.text = text.toStdString();
    m_Session->sendInput(event);
}

void NativeMediaEngine::sendMouseMove(short deltaX, short deltaY)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::MouseMoveRelative;
    event.deltaX = deltaX;
    event.deltaY = deltaY;
    m_Session->sendInput(event);
}

void NativeMediaEngine::sendMousePosition(short x, short y, short referenceWidth,
                                          short referenceHeight)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::MouseMoveAbsolute;
    event.positionX = x;
    event.positionY = y;
    event.referenceWidth = referenceWidth;
    event.referenceHeight = referenceHeight;
    m_Session->sendInput(event);
}

void NativeMediaEngine::sendMouseButton(bool down, int button, bool hold)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = down ? mw::native::InputEvent::Type::MouseButtonDown
                      : mw::native::InputEvent::Type::MouseButtonUp;
    event.button = button;
    event.hold = hold;
    m_Watchdog->noteButton(button, down, hold);
    m_Session->sendInput(event);
}

void NativeMediaEngine::sendMouseScroll(short scrollAmount)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::MouseScrollVertical;
    event.scrollAmount = scrollAmount;
    m_Session->sendInput(event);
}

void NativeMediaEngine::sendMouseHScroll(short scrollAmount)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::MouseScrollHorizontal;
    event.scrollAmount = scrollAmount;
    m_Session->sendInput(event);
}

namespace {

/// A client's controller number shifted by the session's offset, kept in the
/// byte the engine reads it from. Saturated rather than wrapped: an offset that
/// carries the number past 255 must land on "no such slot", never back on 0 —
/// which is the owner's own pad.
uint8_t shiftedController(int controllerNumber, int offset)
{
    const int shifted = controllerNumber + offset;
    if (shifted < 0) return 0;
    if (shifted > 255) return 255;
    return static_cast<uint8_t>(shifted);
}

} // namespace

void NativeMediaEngine::sendControllerArrival(uint8_t controllerNumber, uint16_t activeGamepadMask,
                                              uint8_t type, bool hasRumble)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::ControllerArrival;
    event.controllerNumber = shiftedController(controllerNumber, m_ControllerOffset);
    event.activeGamepadMask = activeGamepadMask;
    event.controllerType = type;
    event.hasRumble = hasRumble;
    m_Session->sendInput(event);
}

void NativeMediaEngine::sendControllerState(short controllerNumber, short activeGamepadMask,
                                            int buttonFlags, unsigned char leftTrigger,
                                            unsigned char rightTrigger, short leftStickX,
                                            short leftStickY, short rightStickX, short rightStickY)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::ControllerState;
    event.controllerNumber = shiftedController(controllerNumber, m_ControllerOffset);
    event.activeGamepadMask = static_cast<uint16_t>(activeGamepadMask);
    event.buttonFlags = buttonFlags;
    event.leftTrigger = leftTrigger;
    event.rightTrigger = rightTrigger;
    event.leftStickX = leftStickX;
    event.leftStickY = leftStickY;
    event.rightStickX = rightStickX;
    event.rightStickY = rightStickY;
    // A pad only sends on change, so a stick pushed and left there goes
    // silent exactly like a held key: tracked so the watchdog can centre it
    // if the link dies mid-input.
    m_Watchdog->notePad(static_cast<short>(event.controllerNumber), activeGamepadMask,
                        InputWatchdog::padAtRest(buttonFlags, leftTrigger, rightTrigger, leftStickX,
                                                 leftStickY, rightStickX, rightStickY));
    m_Session->sendInput(event);
}

void NativeMediaEngine::sendControllerRemoval(uint8_t controllerNumber, uint16_t activeGamepadMask)
{
    if (!m_Session) return;
    // Just the removal: the engine zeroes the pad on its way out (see
    // VigemGamepad::remove), so sending a neutral state first would only plug in
    // a pad that was never there in order to unplug it.
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::ControllerRemoval;
    event.controllerNumber = shiftedController(controllerNumber, m_ControllerOffset);
    event.activeGamepadMask = activeGamepadMask;
    m_Session->sendInput(event);
}

void NativeMediaEngine::syncLockKeys(bool numLock, bool capsLock, bool scrollLock)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::LockKeySync;
    event.numLock = numLock;
    event.capsLock = capsLock;
    event.scrollLock = scrollLock;
    m_Session->sendInput(event);
}

void NativeMediaEngine::syncHeldInputs(const QVector<HeldKey>& keys, quint32 buttonMask,
                                       bool buttonsHold)
{
    if (!m_Session) return;

    // Release what drifted, then re-press what the watchdog released but the
    // client still holds: only the difference, as the watchdog works it out.
    //
    // A resync press is still marked as such, so the input backend can tell
    // it apart from the user acting: re-applying a press that never went away
    // is an extra character on the keyboard and a second click on the mouse.
    const InputWatchdog::SyncDiff diff = m_Watchdog->sync(keys, buttonMask, buttonsHold);

    for (const HeldKey& key : diff.release) {
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::KeyUp;
        event.keyCode = key.keyCode;
        event.keyFlags = static_cast<uint8_t>(key.flags);
        m_Session->sendInput(event);
    }
    for (const HeldKey& key : diff.press) {
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::KeyDown;
        event.keyCode = key.keyCode;
        event.modifiers = static_cast<uint8_t>(key.modifiers);
        event.keyFlags = static_cast<uint8_t>(key.flags);
        event.hold = key.hold;
        event.resync = true;
        m_Session->sendInput(event);
    }
    for (int button : diff.buttonsUp) {
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::MouseButtonUp;
        event.button = button;
        m_Session->sendInput(event);
    }
    for (int button : diff.buttonsDown) {
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::MouseButtonDown;
        event.button = button;
        event.hold = buttonsHold;
        event.resync = true;
        m_Session->sendInput(event);
    }

    if (!diff.press.isEmpty() || !diff.buttonsDown.isEmpty()) {
        qInfo() << "[NativeMediaEngine] Input resync: re-pressed" << diff.press.size()
                << "key(s) and" << diff.buttonsDown.size() << "button(s) released during a stall";
    }
}

void NativeMediaEngine::releaseHeldInputs(bool includeHold)
{
    m_Watchdog->release(includeHold);
}

void NativeMediaEngine::noteClientAlive()
{
    m_Watchdog->noteClientAlive();
}
