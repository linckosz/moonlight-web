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

#include "mw/native/NativeHost.h"

#include <QDebug>

namespace {

// VIDEO_FORMAT_* bits, as moonlight-common-c defines them and as the frontend
// already understands. Spelled out here rather than included: this file is the
// boundary, and pulling Limelight.h in for three constants would drag a GPL
// header into the one place that must stay able to talk to both sides.
constexpr int kVideoFormatH264 = 0x0001;
constexpr int kVideoFormatHevc = 0x0100;
constexpr int kVideoFormatAv1 = 0x0200;

/// Frame type as the relays read it: 1 is a keyframe (FRAME_TYPE_IDR).
constexpr int kFrameTypeKeyframe = 1;
constexpr int kFrameTypeDelta = 0;

/// The client's decodable formats, in the engine's own preference order.
std::vector<mw::native::Codec> codecsFromMask(int mask)
{
    std::vector<mw::native::Codec> codecs;
    // Order matters and is ours, not the mask's: AV1 first for its
    // royalty-free licence and quality per bit, then HEVC, then the floor.
    if (mask & kVideoFormatAv1) codecs.push_back(mw::native::Codec::Av1);
    if (mask & kVideoFormatHevc) codecs.push_back(mw::native::Codec::Hevc);
    if (mask & kVideoFormatH264) codecs.push_back(mw::native::Codec::H264);
    // An empty mask means the caller never filled it in. H.264 is the only
    // format every browser decodes, so it is the safe assumption — and a
    // wrong-but-decodable stream beats refusing to start.
    if (codecs.empty()) codecs.push_back(mw::native::Codec::H264);
    return codecs;
}

int maskFromCodec(mw::native::Codec codec)
{
    switch (codec) {
    case mw::native::Codec::Av1: return kVideoFormatAv1;
    case mw::native::Codec::Hevc: return kVideoFormatHevc;
    case mw::native::Codec::H264: break;
    }
    return kVideoFormatH264;
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
}

NativeMediaEngine::~NativeMediaEngine()
{
    stopConnection();
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

    std::string error;
    m_Session = mw::native::NativeHost::createSession(
        config,
        [this](const mw::native::EncodedFrame& frame) {
            onEncodedFrame(frame.data, frame.size, frame.keyframe, frame.frameNumber,
                           frame.presentUs, frame.submittedUs, frame.encodedUs);
        },
        // Audio and rumble land with their own backends; a null callback is the
        // engine's documented way of saying "not wanted".
        nullptr, nullptr,
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

    const mw::native::SessionInfo& info = m_Session->info();
    m_NegotiatedVideoFormat.store(maskFromCodec(info.codec), std::memory_order_release);
    m_Connected.store(true, std::memory_order_release);

    qInfo().noquote() << "[NativeMediaEngine] streaming" << describeSession();
    emit connectionStarted();
}

void NativeMediaEngine::onEncodedFrame(const void* data, size_t size, bool keyframe,
                                       uint32_t frameNumber, int64_t presentUs, int64_t submittedUs,
                                       int64_t encodedUs)
{
    if (!data || size == 0) return;

    // The one copy on this path, and it is unavoidable: the engine's buffer is
    // unlocked the moment this returns, while the relay's send is asynchronous.
    // The GameStream path makes the same copy — after having already paid for
    // RTP, FEC, decryption and reassembly to produce it.
    QByteArray frame(static_cast<const char*>(data), static_cast<int>(size));

    m_FramePresentationTimeUs.store(presentUs, std::memory_order_release);
    m_FrameSubmitTimeUs.store(submittedUs, std::memory_order_release);

    int64_t expected = 0;
    m_FirstFrameArrivalUs.compare_exchange_strong(expected, encodedUs, std::memory_order_acq_rel);

    // Measured, not reported: the real time from the display presenting the
    // frame to the encoder finishing it.
    if (encodedUs > presentUs) {
        m_ProcWindowTotalUs.fetch_add(encodedUs - presentUs, std::memory_order_acq_rel);
        m_ProcWindowCount.fetch_add(1, std::memory_order_acq_rel);
    }

    m_PendingVideoFrames.fetch_add(1, std::memory_order_acq_rel);

    // Emitted on the capture thread. The relays' connections decide whether
    // that stays direct — which is the point: the GameStream path pays a queued
    // hop here, and not paying it is one of the reasons this engine exists.
    emit videoFrameReady(frame, keyframe ? kFrameTypeKeyframe : kFrameTypeDelta,
                         static_cast<int>(frameNumber), presentUs);
}

void NativeMediaEngine::stopConnection()
{
    if (!m_Session) return;
    m_Connected.store(false, std::memory_order_release);
    m_Session->stop();
    m_Session.reset();
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
    return m_FirstFrameArrivalUs.load(std::memory_order_acquire) / 1000;
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

void NativeMediaEngine::sendControllerArrival(uint8_t controllerNumber, uint16_t activeGamepadMask,
                                              uint8_t type, bool hasRumble)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::ControllerArrival;
    event.controllerNumber = static_cast<uint8_t>(controllerNumber + m_ControllerOffset);
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
    event.controllerNumber = static_cast<uint8_t>(controllerNumber + m_ControllerOffset);
    event.activeGamepadMask = static_cast<uint16_t>(activeGamepadMask);
    event.buttonFlags = buttonFlags;
    event.leftTrigger = leftTrigger;
    event.rightTrigger = rightTrigger;
    event.leftStickX = leftStickX;
    event.leftStickY = leftStickY;
    event.rightStickX = rightStickX;
    event.rightStickY = rightStickY;
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

    // Re-press what the watchdog released but the client still holds. The
    // watchdog itself is shared session logic and lives above this class.
    for (const HeldKey& key : keys) {
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::KeyDown;
        event.keyCode = key.keyCode;
        event.modifiers = static_cast<uint8_t>(key.modifiers);
        event.keyFlags = static_cast<uint8_t>(key.flags);
        event.hold = key.hold;
        m_Session->sendInput(event);
    }

    for (int button = 1; button <= 5; ++button) {
        if ((buttonMask & (1u << (button - 1))) == 0) continue;
        mw::native::InputEvent event;
        event.type = mw::native::InputEvent::Type::MouseButtonDown;
        event.button = button;
        event.hold = buttonsHold;
        m_Session->sendInput(event);
    }
}

void NativeMediaEngine::releaseHeldInputs(bool includeHold)
{
    Q_UNUSED(includeHold);
    // The input backend tracks what it applied and lifts it. Nothing is held
    // here, so there is nothing for this class to release on its own.
}

void NativeMediaEngine::noteClientAlive()
{
    // Liveness drives the shared watchdog, not this engine.
}
