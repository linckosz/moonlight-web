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
#include "AudioPathLog.h"
#include "FeedInfo.h"
#include "FeedPublisher.h"
#include "FeedSubscriber.h"
#include "HidCollectionsJson.h"
#include "HidParkedJson.h"
#include "InputWatchdog.h"
#include "NativeBench.h"
#include "backend/VirtualDisplay.h"
#include "common/Logger.h"

#include "mw/native/FeedWire.h"
#include "mw/native/NativeHost.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>

#include <chrono>

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

/// The bench's audio log (`audiolog=1`): the engine's half of a packet, on the
/// steady clock the pacer and the relay stamp with. Nothing when it is off.
void noteAudioEmitted(const mw::native::AudioPacket& packet)
{
    AudioPathLog& log = AudioPathLog::instance();
    if (!log.enabled()) return;
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    log.emitted(packet.capturedUs, nowUs, packet.queuedFrames, packet.peak, packet.silence,
                packet.size);
}
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

/// The GPU's maker in one word, for the encoders whose name does not say it:
/// D3D12 VE and Vulkan Video run on anyone's silicon, and "D3D12 VE" alone
/// left the overlay's reader guessing which card was encoding (Bruno, 28/09).
/// Read off the adapter's name, which every driver fills with its maker's.
QString vendorWord(const std::string& gpuName)
{
    const QString name = QString::fromStdString(gpuName);
    if (name.contains(QLatin1String("NVIDIA"), Qt::CaseInsensitive))
        return QStringLiteral("NVIDIA");
    if (name.contains(QLatin1String("AMD"), Qt::CaseInsensitive) ||
        name.contains(QLatin1String("Radeon"), Qt::CaseInsensitive))
        return QStringLiteral("AMD");
    if (name.contains(QLatin1String("Intel"), Qt::CaseInsensitive)) return QStringLiteral("Intel");
    return {};
}

/// @p name followed by the maker in brackets, when there is one to name.
QString withVendor(const QString& name, const mw::native::SessionInfo& info)
{
    const QString vendor = vendorWord(info.gpuName);
    return vendor.isEmpty() ? name : name + QStringLiteral(" (") + vendor + QLatin1Char(')');
}

/// The encoder as the overlay names it: the D3D12 route's own ("D3D12 VE",
/// "NVENC (D3D12)") while that chain runs, "Vulkan Video" while Linux's does,
/// the Selector's ("NVENC", "AMF", "oneVPL", "VA-API") otherwise — followed
/// by "(D3D11)" when D3D12 was asked for and D3D11 runs, so whoever picked
/// D3D12 in the admin, or streams an Intel GPU (Auto's D3D12), sees at a
/// glance that it did not take. The two names that fit any vendor carry the
/// GPU's maker: "D3D12 VE (Intel)". On Linux the encoder's name already says
/// which chain runs. Why is the log's to say.
QString encoderLabel(const mw::native::SessionInfo& info)
{
    if (info.videoPipeline == mw::native::VideoPipeline::D3d12 && !info.videoEncoder.empty()) {
        const QString name = QString::fromStdString(info.videoEncoder);
        return name == QLatin1String("D3D12 VE") ? withVendor(name, info) : name;
    }
    if (info.videoPipeline == mw::native::VideoPipeline::Vulkan)
        return withVendor(QStringLiteral("Vulkan Video"), info);
    QString name = QString::fromUtf8(mw::native::toString(info.encoder));
    if (info.videoPipelineRefused && info.videoPipeline == mw::native::VideoPipeline::D3d11)
        name += QStringLiteral(" (D3D11)");
    return name;
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
        // Noted as the input path sent it: goes on the wire as it is.
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
    // A guest on the shared feed captures nothing: its pictures come down the
    // feed's pipe, its session is input and audio.
    if (!params.feedPipe.isEmpty()) {
        startSubscriber(params);
        return;
    }

    mw::native::SessionConfig config;
    config.displayId = params.displayId;
    config.width = params.width;
    config.height = params.height;
    config.fps = params.fps;
    config.maxFps = params.maxFps;
    config.bitrateKbps = params.bitrateKbps;
    config.clientCodecs = codecsFromMask(params.clientVideoFormats);
    config.hdr = params.hdr;
    config.yuv444 = params.yuv444;
    config.intraRefresh = params.intraRefresh;
    config.intraRefreshRequired = params.intraRefreshRequired;
    config.governorFloorPercent = params.governorFloorPercent;
    config.followDisplayShape = params.followDisplayShape;
    config.fitRequestedBox = params.fitRequestedBox;
    config.allowUpscale = params.allowUpscale;
    config.matchClientDisplay = params.matchClientDisplay;
    config.fallbackWidth = params.fallbackWidth;
    config.fallbackHeight = params.fallbackHeight;
    // A display the session makes for itself (the portal's virtual monitor on
    // Linux) runs at the rate the server gives its own virtual display: the
    // same rule, MW_VDD_REFRESH included, so both platforms bench alike.
    config.virtualRefreshHz =
        VirtualDisplay::refreshForStream(params.fps, qEnvironmentVariableIntValue("MW_VDD_REFRESH"),
                                         VirtualDisplay::kFasterThanStream);
    config.allowElevatedInput = params.viewerAdmin;
    config.muteHostAudio = params.muteHostAudio;
    // The consent this machine was already given, replayed. Empty on the very
    // first session of an install that captures through the portal, which is
    // the one time the user sees a dialog.
    config.portalRestoreToken = params.portalRestoreToken.toStdString();
    config.virtualPrimary = params.virtualPrimary;
    config.gamescopeApp = params.gamescopeApp.toStdString();
    config.gamescopeCommand = params.gamescopeCommand.toStdString();
    // The picture chain the admin chose. A bench's MW_NATIVE_TUNING below
    // still wins over it with its pipeline= key (VideoPipelineChoice.h).
    config.videoPipeline = params.videoPipeline;
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
    config.clientTearingReal = params.clientTearingReal;
    // The client cuts each decoded frame to the size the session says.
    config.clientCropsToFrame = params.cropsToFrame;
    // And reads MWSR's zones, which the frames then carry (FrameView).
    config.mwsrZones = params.mwsrZones;

    // The bench's encoder knobs, on a real session, from the environment: the
    // one way to put two encoder settings in front of a person on the same
    // screen (plan v2 §5, the A/B). Or, when the variable is absent, from
    // settings.json's hand-added "native_tuning", which the server read at
    // /start and handed down: a SYSTEM worker gets its environment from the
    // launcher service, built from SYSTEM's, and its own AppData is
    // systemprofile's, so neither the variable nor the file would reach it.
    // Never set in production — nothing in the product writes either — and
    // logged loudly when one is, so a stray knob cannot pass for the engine's
    // own choice.
    QString tuningSource;
    const QString tuningSpec = effectiveTuningSpec(params.tuningSpec, &tuningSource);
    if (!tuningSpec.isEmpty()) {
        QString parseError;
        if (parseEncoderTuningSpec(tuningSpec, config.tuning, config.encodeGpuId, parseError))
            qWarning().noquote() << "[NativeMediaEngine]" << tuningSource
                                 << "in effect:" << tuningSpec
                                 << "— this session does not run the engine's own settings";
        else
            qWarning().noquote() << "[NativeMediaEngine]" << tuningSource
                                 << "ignored:" << parseError;
    }
    // The client's decoder takes no reference repairs (/start's
    // ref_invalidation): one reference, and a keyframe for a loss. One
    // reference is not enough on its own — NVENC and VA-API keep invalidating
    // with it — so the engine turns every repair into a keyframe as well
    // (invalidateReference, referenceInvalidation).
    m_ClientTakesRepairs.store(params.refInvalidation, std::memory_order_release);
    if (!params.refInvalidation && mw::native::refuseReferenceRepairs(config.tuning))
        qInfo().noquote() << "[NativeMediaEngine] the client's decoder takes no reference "
                             "repairs: one reference, keyframes on loss";
    m_NameLinkDropsKey.store(config.tuning.nameLinkDrops, std::memory_order_release);
    m_AudioSamplesPerFrame.store(config.tuning.audioFrameSamples(), std::memory_order_release);

    std::string error;
    // Opus packets, 5 ms each (10 or 20 under `audioframe=`), from the
    // engine's audio thread. Emitted as the same signal the GameStream engine
    // emits from moonlight-common-c's audio thread: the relays already copy
    // the bytes onto their own thread and stamp the RTP clock by
    // audioSamplesPerFrame(), the frame the engine produces. None at all for the guests' feed,
    // whose guests each capture their own.
    mw::native::AudioCallback onAudio;
    if (params.captureAudio) {
        onAudio = [this](const mw::native::AudioPacket& packet) {
            if (!packet.data || packet.size == 0) return;
            noteAudioEmitted(packet);
            emit audioSampleReady(QByteArray(reinterpret_cast<const char*>(packet.data),
                                             static_cast<qsizetype>(packet.size)));
        };
    }
    m_Session = mw::native::NativeHost::createSession(
        config, [this](const mw::native::EncodedFrame& frame) { onEncodedFrame(frame); },
        std::move(onAudio),
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

    // Registered BEFORE start(), unlike every other listener here: asking the
    // portal for a screencast is what raises the dialog, so the grant comes
    // back from inside start() and a listener attached after it has missed the
    // only call there will ever be. Emitted directly — the callback runs on
    // this thread, and the connection this signal reaches is queued or not by
    // its own receiver's affinity.
    m_Session->setPortalGrantCallback([this](const std::string& token) {
        emit portalGrantReceived(QString::fromStdString(token));
    });
    // Before start() as well: that is where a host with no pointing device
    // has Mouse Keys turned on. From the capture and stopping threads after.
    m_Session->setMouseKeysCallback([this](bool on, const mw::native::MouseKeysState& saved) {
        QJsonObject state;
        if (on) {
            state = QJsonObject{
                {QStringLiteral("flags"), static_cast<qint64>(saved.flags)},
                {QStringLiteral("maxSpeed"), static_cast<qint64>(saved.maxSpeed)},
                {QStringLiteral("timeToMaxSpeed"), static_cast<qint64>(saved.timeToMaxSpeed)},
                {QStringLiteral("ctrlSpeed"), static_cast<qint64>(saved.ctrlSpeed)}};
        }
        emit mouseKeysChanged(on, state);
    });
    // The relay's send queue, for linkhold=: its video channel may well be
    // made before the session.
    {
        std::lock_guard<std::mutex> lk(m_LinkProbeMutex);
        if (m_LinkBusyProbe) m_Session->setLinkBusyProbe(m_LinkBusyProbe);
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

    // The gate speaks from the injecting thread; the relay that forwards it
    // lives on another and owns a DataChannel that is not thread-safe, so it
    // is re-emitted as a queued signal, like rumble.
    m_Session->setInputGateCallback([this](const mw::native::InputGate& gate) {
        const bool blocked = gate.blocked;
        const QString reason = QString::fromLatin1(gate.reason ? gate.reason : "");
        const QString window = QString::fromStdString(gate.window);
        QMetaObject::invokeMethod(
            this,
            [this, blocked, reason, window]() { emit inputGateChanged(blocked, reason, window); },
            Qt::QueuedConnection);
    });

    // Same thread story as the gate: said on the capture thread, forwarded by
    // a relay that lives elsewhere. The feed says it to its subscribers too,
    // with the session as it now stands (frame size, the display's place).
    m_Session->setDisplayFormatCallback([this](const mw::native::DisplayFormat& f) {
        if (m_Publisher) {
            publishInfo();
            m_Publisher->publishControl(
                QJsonObject{{QStringLiteral("type"), QStringLiteral("displayFormat")},
                            {QStringLiteral("displayWidth"), f.displayWidth},
                            {QStringLiteral("displayHeight"), f.displayHeight},
                            {QStringLiteral("frameWidth"), f.frameWidth},
                            {QStringLiteral("frameHeight"), f.frameHeight},
                            {QStringLiteral("displayHdr"), f.displayHdr},
                            {QStringLiteral("hdr"), f.hdr},
                            {QStringLiteral("hdrCapable"), f.hdrCapable}});
        }
        QMetaObject::invokeMethod(
            this,
            [this, f]() {
                emit displayFormatChanged(f.displayWidth, f.displayHeight, f.frameWidth,
                                          f.frameHeight, f.displayHdr, f.hdr, f.hdrCapable);
            },
            Qt::QueuedConnection);
    });

    const mw::native::SessionInfo& info = m_Session->info();
    m_NegotiatedVideoFormat.store(formatFromSession(info), std::memory_order_release);
    m_Connected.store(true, std::memory_order_release);

    // The feed worker: what its subscribers join, before its first picture.
    m_Publisher = params.feedPublisher;
    if (m_Publisher) publishInfo();

    qInfo().noquote() << "[NativeMediaEngine] streaming" << describeSession();
    emit connectionStarted();
}

void NativeMediaEngine::publishInfo()
{
    if (!m_Publisher || !m_Session) return;
    const mw::native::SessionInfo& info = m_Session->info();
    m_Publisher->setInfo(
        feedinfo::toJson(info, formatFromSession(info), describeEncoder(), describeSession()));
}

void NativeMediaEngine::startSubscriber(const StartParams& params)
{
    m_Subscriber =
        std::make_unique<FeedSubscriber>(params.feedPipe, params.feedToken, params.feedSlot);
    m_Subscriber->setFrameCallback([this](const mw::native::feed::Header& header,
                                          const uint8_t* data,
                                          size_t size) { onFeedFrame(header, data, size); });
    // Emitted on the subscriber's thread; handled on this engine's.
    connect(m_Subscriber.get(), &FeedSubscriber::controlReceived, this,
            &NativeMediaEngine::onFeedControl, Qt::QueuedConnection);
    connect(m_Subscriber.get(), &FeedSubscriber::disconnected, this, &NativeMediaEngine::onFeedLost,
            Qt::QueuedConnection);
    connect(m_Subscriber.get(), &FeedSubscriber::rejoined, this, &NativeMediaEngine::onFeedRejoined,
            Qt::QueuedConnection);
    m_FeedNeedsKeyframe.store(true, std::memory_order_release);

    QJsonObject info;
    QString why;
    // The feed may be starting at this very moment (the first guest's join is
    // what launches it): its pipe gets a few seconds to answer.
    if (!m_Subscriber->start(8000, &info, &why) || !takeFeedInfo(info)) {
        if (why.isEmpty()) why = QStringLiteral("the feed described no session");
        qWarning() << "[NativeMediaEngine] cannot join the guests' shared feed:" << why;
        m_Subscriber.reset();
        emit connectionFailed(QStringLiteral("The shared stream is not answering: ") + why);
        return;
    }

    mw::native::SessionConfig config;
    config.videoSource = mw::native::VideoSource::External;
    mw::native::SessionInfo feed;
    {
        std::lock_guard<std::mutex> lock(m_FeedMutex);
        feed = m_FeedInfo;
    }
    config.displayId = feed.displayId;
    config.externalLeft = feed.desktopLeft;
    config.externalTop = feed.desktopTop;
    config.externalRight = feed.desktopRight;
    config.externalBottom = feed.desktopBottom;
    config.allowElevatedInput = params.viewerAdmin;
    config.muteHostAudio = params.muteHostAudio;

    std::string error;
    m_Session = mw::native::NativeHost::createSession(
        config, nullptr,
        [this](const mw::native::AudioPacket& packet) {
            if (!packet.data || packet.size == 0) return;
            noteAudioEmitted(packet);
            emit audioSampleReady(QByteArray(reinterpret_cast<const char*>(packet.data),
                                             static_cast<qsizetype>(packet.size)));
        },
        [this](const mw::native::RumbleEvent& event) {
            QMetaObject::invokeMethod(
                this,
                [this, event]() {
                    emit rumble(event.controllerNumber, event.lowFrequencyMotor,
                                event.highFrequencyMotor);
                },
                Qt::QueuedConnection);
        },
        nullptr,
        [this](const std::string& reason) {
            m_Connected.store(false, std::memory_order_release);
            emit connectionTerminated(-1);
            qWarning() << "[NativeMediaEngine] session ended:" << QString::fromStdString(reason);
        },
        error);
    if (!m_Session || !m_Session->start(error)) {
        qWarning() << "[NativeMediaEngine] could not start the guest's session:"
                   << QString::fromStdString(error);
        m_Session.reset();
        m_Subscriber->stop();
        m_Subscriber.reset();
        emit connectionFailed(QString::fromStdString(error));
        return;
    }
    m_Session->setInputGateCallback([this](const mw::native::InputGate& gate) {
        const bool blocked = gate.blocked;
        const QString reason = QString::fromLatin1(gate.reason ? gate.reason : "");
        const QString window = QString::fromStdString(gate.window);
        QMetaObject::invokeMethod(
            this,
            [this, blocked, reason, window]() { emit inputGateChanged(blocked, reason, window); },
            Qt::QueuedConnection);
    });

    m_Connected.store(true, std::memory_order_release);
    qInfo().noquote() << "[NativeMediaEngine] streaming" << describeSession();
    emit connectionStarted();
    // The feed is already running: this guest starts from its next keyframe.
    requestIdrFrame();
}

bool NativeMediaEngine::takeFeedInfo(const QJsonObject& info)
{
    mw::native::SessionInfo feed;
    int format = 0;
    QString encoder;
    QString description;
    if (!feedinfo::fromJson(info, feed, format, encoder, description)) return false;
    {
        std::lock_guard<std::mutex> lock(m_FeedMutex);
        m_FeedInfo = feed;
        m_FeedEncoder = encoder;
        m_FeedDescription = description;
    }
    m_NegotiatedVideoFormat.store(format, std::memory_order_release);
    return true;
}

void NativeMediaEngine::onFeedFrame(const mw::native::feed::Header& header, const uint8_t* data,
                                    size_t size)
{
    // Joined mid-stream, or back after the feed died: nothing to decode
    // against until a keyframe (asked for when this began).
    if (m_FeedNeedsKeyframe.load(std::memory_order_acquire)) {
        if (!header.keyframe) return;
        m_FeedNeedsKeyframe.store(false, std::memory_order_release);
    }
    // The feed's own stamps, from its display's present: the same clock in
    // both processes. The hop down the pipe is counted in "queue".
    mw::native::EncodedFrame frame;
    frame.data = data;
    frame.size = size;
    frame.keyframe = header.keyframe;
    frame.frameNumber = header.frameNumber;
    frame.presentUs = header.presentUs;
    frame.capturedUs = header.capturedUs;
    frame.submittedUs = header.submittedUs;
    frame.convertedUs = header.convertedUs;
    frame.encodedUs = header.encodedUs;
    onEncodedFrame(frame);
}

void NativeMediaEngine::onFeedControl(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("info")) {
        // The feed rebuilt: its frame, or the display's place on the desktop.
        if (!takeFeedInfo(message)) return;
        mw::native::SessionInfo feed;
        {
            std::lock_guard<std::mutex> lock(m_FeedMutex);
            feed = m_FeedInfo;
        }
        if (m_Session)
            m_Session->setExternalDesktop(feed.desktopLeft, feed.desktopTop, feed.desktopRight,
                                          feed.desktopBottom);
        return;
    }
    if (type == QLatin1String("displayFormat")) {
        emit displayFormatChanged(message.value(QStringLiteral("displayWidth")).toInt(),
                                  message.value(QStringLiteral("displayHeight")).toInt(),
                                  message.value(QStringLiteral("frameWidth")).toInt(),
                                  message.value(QStringLiteral("frameHeight")).toInt(),
                                  message.value(QStringLiteral("displayHdr")).toBool(),
                                  message.value(QStringLiteral("hdr")).toBool(),
                                  message.value(QStringLiteral("hdrCapable")).toBool());
        return;
    }
    if (type == QLatin1String("codec")) {
        // The feed changes codec for everyone (a guest who decodes no HEVC
        // arrived) and ends in a moment: the browser is told, and comes back
        // into the new feed the way it comes back from its own codec fallback.
        // A page that does not act loses the feed with everyone else's.
        const QString codec = message.value(QStringLiteral("codec")).toString();
        qInfo() << "[NativeMediaEngine] the shared feed goes" << codec
                << "— this guest's browser rejoins in it";
        emit sharedFeedCodecChanged(codec);
        return;
    }
    if (type == QLatin1String("bye")) {
        qInfo() << "[NativeMediaEngine] the shared feed ends — so does this guest's session";
        m_Connected.store(false, std::memory_order_release);
        emit connectionTerminated(-1);
    }
}

void NativeMediaEngine::onFeedRejoined(const QJsonObject& info)
{
    m_FeedNeedsKeyframe.store(true, std::memory_order_release);
    onFeedControl(info); // the feed's session as it came back, desktop included
    requestIdrFrame();
}

void NativeMediaEngine::onFeedLost(const QString& why)
{
    if (!m_Connected.load(std::memory_order_acquire)) return;
    qWarning() << "[NativeMediaEngine] the shared feed went away:" << why;
    m_Connected.store(false, std::memory_order_release);
    emit connectionTerminated(-1);
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

    // The feed worker: every picture to the guests' pipe. It is a copy per
    // subscriber, never a wait on one (FeedPublisher). The worker has no
    // relay of its own, so nothing else wants the frame.
    if (m_Publisher) {
        FeedPublisher::Stamps stamps;
        stamps.frameNumber = frameNumber;
        stamps.presentUs = presentUs;
        stamps.capturedUs = encoded.capturedUs;
        stamps.submittedUs = encoded.submittedUs;
        stamps.convertedUs = encoded.convertedUs;
        stamps.encodedUs = encodedUs;
        m_Publisher->publishFrame(encoded.data, encoded.size, encoded.keyframe, stamps);
        std::lock_guard<std::mutex> lock(m_SinkMutex);
        if (!m_DirectSink) return;
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
            view.mwsrZones = encoded.mwsrZones;
            view.mwsrZonesX = encoded.mwsrZonesX;
            view.mwsrZonesY = encoded.mwsrZonesY;
            view.mwsrScale = encoded.mwsrScale;
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
    // A guest's pipe first: no picture may reach the relay once this returns.
    if (m_Subscriber) {
        m_Subscriber->stop();
        m_Subscriber.reset();
    }
    {
        // The recreated devices leave the session that made them, at rest,
        // for the next stream to take back (HidParking): a game left running
        // keeps its wheel and the force feedback it set up on it.
        std::lock_guard<std::mutex> lock(m_HidMutex);
        m_Hid.reset();
    }
    if (!m_Session) return;
    m_Connected.store(false, std::memory_order_release);
    m_Session->stop();
    const mw::native::Session::CaptureHoldStatus hold = m_Session->captureHold();
    if (hold.judged && hold.displayMilliHz > 0) {
        m_CaptureHold = QJsonObject{
            {QStringLiteral("held"), hold.held},
            {QStringLiteral("display_mhz"), hold.displayMilliHz},
            {QStringLiteral("gpu"), QString::fromStdString(hold.gpu)},
            {QStringLiteral("why"), QString::fromStdString(hold.why)},
            {QStringLiteral("dense_s"), hold.denseSeconds},
            {QStringLiteral("locked"), hold.locked},
            {QStringLiteral("carried"), hold.carried},
            {QStringLiteral("folded"), hold.folded},
            {QStringLiteral("busy"), hold.busy},
        };
    }
    writeClickTrace();
    m_Session.reset();
    m_Publisher = nullptr;
    logStageSummary();
    emit connectionStopped();
}

void NativeMediaEngine::writeClickTrace()
{
    // Like the relay's frame log (relaylog=1): beside this process's log,
    // where the bench picks it up.
    const std::string csv = m_Session ? m_Session->clickTraceCsv() : std::string();
    if (csv.empty()) return;
    const QString logFile = Logger::instance()->logFilePath();
    const QString dir = logFile.isEmpty() ? QDir::tempPath() : QFileInfo(logFile).absolutePath();
    const QString path = dir + QStringLiteral("/click-trace-%1-%2.csv")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentMSecsSinceEpoch());
    QFile file(path);
    const bool ok =
        file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(csv.data(), static_cast<qint64>(csv.size())) == static_cast<qint64>(csv.size());
    qInfo() << "[NativeMediaEngine] click trace" << (ok ? "written to" : "could NOT be written to")
            << path;
}

void NativeMediaEngine::interruptConnection()
{
    // Nothing to unwind: there is no protocol handshake to abandon and no
    // socket to abort, so an interrupt is just a stop.
    stopConnection();
}

void NativeMediaEngine::requestIdrFrame()
{
    // A guest asks the feed, which groups every guest's asking into one.
    if (m_Subscriber) {
        m_Subscriber->sendControl(QJsonObject{{QStringLiteral("type"), QStringLiteral("idr")}});
        return;
    }
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
    // 5 ms at 48 kHz, the pipeline's frame, unless the session's tuning asked
    // for 10 or 20 (`audioframe=`): the capture encodes exactly that.
    return m_AudioSamplesPerFrame.load(std::memory_order_acquire);
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
    // A position report carries no shape: nothing to encode, one small signal.
    if (cursor.positionOnly) {
        QMetaObject::invokeMethod(
            this,
            [this, x = static_cast<double>(cursor.x), y = static_cast<double>(cursor.y),
             visible = cursor.visible]() { emit cursorMoved(x, y, visible); },
            Qt::QueuedConnection);
        return;
    }

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
    // The shared feed keeps its own pace (60 fps, every refresh): nothing one
    // guest's page asks of the cadence reaches the others' pictures.
    if (m_Subscriber) return;
    m_FrameFloorFps.store(fps, std::memory_order_release);
    if (m_Session) m_Session->setFrameFloorFps(fps);
}

void NativeMediaEngine::releaseInputBlock()
{
    if (m_Session) m_Session->releaseInputBlock();
}

void NativeMediaEngine::invalidateReference(uint32_t frameNumber)
{
    // No repair by reference on the shared feed (referenceInvalidation says
    // so): a lost frame there is the wave's to heal, or a keyframe's. Nor for
    // a client whose decoder refuses them: a keyframe it is.
    if (m_Subscriber || !m_ClientTakesRepairs.load(std::memory_order_acquire)) {
        requestIdrFrame();
        return;
    }
    if (m_Session) m_Session->invalidateReference(frameNumber);
}

void NativeMediaEngine::setClientRefresh(int milliHz, bool vsync)
{
    if (m_Subscriber) return; // the feed's pace, see setFrameFloorFps
    m_ClientRefreshMilliHz.store(milliHz, std::memory_order_relaxed);
    m_ClientVsync.store(vsync, std::memory_order_relaxed);
    m_ClientRefreshKnown.store(true, std::memory_order_release);
    if (m_Session) m_Session->setClientRefresh(milliHz, vsync);
}

void NativeMediaEngine::setClientFpsCap(int fps)
{
    if (m_Subscriber) return; // the feed's pace, see setFrameFloorFps
    if (m_Session) m_Session->setClientFpsCap(fps);
}

void NativeMediaEngine::setClientDecodeQueue(int depth)
{
    if (m_Subscriber) return; // the feed's pace, see setFrameFloorFps
    if (m_Session) m_Session->setClientDecodeQueue(depth);
}

void NativeMediaEngine::setLinkBusyProbe(std::function<bool()> probe)
{
    if (m_Subscriber) return; // the feed's pace, see setFrameFloorFps
    std::lock_guard<std::mutex> lk(m_LinkProbeMutex);
    m_LinkBusyProbe = std::move(probe);
    if (m_Session) m_Session->setLinkBusyProbe(m_LinkBusyProbe);
}

void NativeMediaEngine::setClientVsyncGrid(double periodUs, int64_t phaseUs, int64_t leadUs,
                                           bool tearing, bool steady, double budgetFps)
{
    if (m_Subscriber) return; // the feed's pace, see setFrameFloorFps
    if (m_Session)
        m_Session->setClientVsyncGrid(periodUs, phaseUs, leadUs, tearing, steady, budgetFps);
}

mw::native::FpsStep NativeMediaEngine::setClientFpsStep(int fps)
{
    if (m_Subscriber || !m_Session) {
        mw::native::FpsStep refused;
        refused.askedFps = fps;
        refused.why = m_Subscriber ? "the shared feed keeps its pace" : "no session";
        return refused;
    }
    return m_Session->setClientFpsStep(fps);
}

mw::native::CadenceStatus NativeMediaEngine::cadenceStatus() const
{
    if (m_Subscriber || !m_Session) return {};
    return m_Session->cadenceStatus();
}

NativeMediaEngine::VsyncGridStatus NativeMediaEngine::vsyncGridStatus() const
{
    VsyncGridStatus out;
    if (!m_Session) return out;
    const mw::native::Session::VsyncGridStatus s = m_Session->vsyncGridStatus();
    out.wanted = s.wanted;
    out.followed = s.followed;
    out.aimed = s.aimed;
    out.presentUs = s.presentUs;
    return out;
}

void NativeMediaEngine::setClientBitrate(int kbps)
{
    // The shared feed's rate is the feed's: sized for the height the owner
    // chose, followed down by the slowest guest's link.
    if (m_Subscriber) return;
    // The same bounds as the setting itself (AppSettings::setStreamBitrate).
    if (kbps < 1000 || kbps > 150000) {
        qWarning() << "[NativeMediaEngine] Ignoring a client bitrate of" << kbps << "kbps";
        return;
    }
    if (m_Session) m_Session->setTargetBitrate(kbps);
}

bool NativeMediaEngine::referenceInvalidation() const
{
    // Never on the shared feed: a repair aimed at one guest's loss would be
    // every guest's next frame. The feed's intra-refresh repairs instead.
    if (m_Subscriber) return false;
    // Nor for a client that asked for none, whatever the encoder can do.
    return m_Session && m_Connected.load(std::memory_order_acquire) &&
           mw::native::healsByReference(m_Session->info().referenceInvalidation,
                                        m_ClientTakesRepairs.load(std::memory_order_acquire));
}

bool NativeMediaEngine::nameLinkDrops() const
{
    if (m_Subscriber) return false;
    using Choice = mw::native::EncoderTuning::Choice;
    const Choice key = m_NameLinkDropsKey.load(std::memory_order_acquire);
    if (key != Choice::Default) return key == Choice::On;
    // Read off what runs, not what was asked: a D3D12 chain that fell back to
    // D3D11 mid-session says so in the same SessionInfo.
    if (!m_Session || !m_Connected.load(std::memory_order_acquire)) return false;
    const mw::native::SessionInfo& info = m_Session->info();
    return mw::native::nameLinkDropsByDefault(info.videoPipeline, info.encoder,
                                              info.videoEncoder12);
}

void NativeMediaEngine::reportLink(const mw::native::LinkFeedback& feedback)
{
    mw::native::LinkFeedback fb = feedback;
    const int evicted = m_Evictions.exchange(0, std::memory_order_relaxed);
    // Carried to this engine's own governor only: the guests' feed follows
    // their delays and SCTP drops, and its arbiter knows nothing of these.
    fb.overflows += m_Overflows.exchange(0, std::memory_order_relaxed);
    // A guest's link goes to the feed, which follows the slowest guest's
    // (down to its floor) — this one's governor has no encoder to move.
    if (m_Subscriber) {
        // Its own sender's evictions are the pipe's bursts more than its
        // link (DataChannelRelay::handleVideoFrame): the link speaks through
        // SCTP drops and the delay, so neither those frames nor the holes they
        // leave on the page lower the feed for everyone (12/10 bench: 10 → 6
        // Mbps on evictions alone, nothing lost on the wire).
        fb.gaps = std::max(0, fb.gaps - evicted);
        m_Subscriber->sendControl(QJsonObject{{QStringLiteral("type"), QStringLiteral("link")},
                                              {QStringLiteral("owdRiseMs"), fb.owdRiseMs},
                                              {QStringLiteral("gaps"), fb.gaps},
                                              {QStringLiteral("evictions"), fb.evictions},
                                              {QStringLiteral("fps"), fb.receivedFps},
                                              {QStringLiteral("resumed"), fb.resumed}});
        return;
    }
    fb.evictions += evicted;
    if (m_Session) m_Session->reportLink(fb);
}

bool NativeMediaEngine::intraRefreshActive() const
{
    if (m_Subscriber) {
        std::lock_guard<std::mutex> lock(m_FeedMutex);
        return m_FeedInfo.intraRefresh;
    }
    return m_Session && m_Session->info().intraRefresh;
}

int NativeMediaEngine::intraRefreshFrames() const
{
    if (m_Subscriber) {
        std::lock_guard<std::mutex> lock(m_FeedMutex);
        return m_FeedInfo.intraRefresh ? m_FeedInfo.intraRefreshFrames : 0;
    }
    return intraRefreshActive() ? m_Session->info().intraRefreshFrames : 0;
}

bool NativeMediaEngine::sessionInfo(mw::native::SessionInfo& out) const
{
    if (!m_Session || !m_Connected.load(std::memory_order_acquire)) return false;
    if (m_Subscriber) {
        // The feed's session, with this guest's own audio beside it.
        const mw::native::SessionInfo& own = m_Session->info();
        std::lock_guard<std::mutex> lock(m_FeedMutex);
        out = m_FeedInfo;
        out.audio = own.audio;
        out.hostMuted = own.hostMuted;
        return true;
    }
    out = m_Session->info();
    return true;
}

QString NativeMediaEngine::describeSession() const
{
    if (m_Subscriber) {
        std::lock_guard<std::mutex> lock(m_FeedMutex);
        return QStringLiteral("the guests' shared feed · ") + m_FeedDescription;
    }
    if (!m_Session) return {};
    const mw::native::SessionInfo& info = m_Session->info();

    QString text = QString::fromStdString(info.gpuName) + QStringLiteral(" · ") +
                   encoderLabel(info) + QLatin1Char(' ') +
                   QString::fromUtf8(mw::native::toString(info.codec));
    if (info.yuv444) text += QStringLiteral(" 4:4:4");
    if (info.hdr) text += QStringLiteral(" HDR");
    if (info.intraRefresh) text += QStringLiteral(" intra-refresh");
    if (mw::native::healsByReference(info.referenceInvalidation,
                                     m_ClientTakesRepairs.load(std::memory_order_acquire)) &&
        nameLinkDrops())
        text += QStringLiteral(" [link drops named]");
    if (info.crossGpuCopy) text += QStringLiteral(" [cross-GPU copy]");
    if (info.hostMuted) text += QStringLiteral(" [host muted]");
    return text;
}

QString NativeMediaEngine::describeEncoder() const
{
    if (m_Subscriber) {
        std::lock_guard<std::mutex> lock(m_FeedMutex);
        return m_FeedEncoder;
    }
    if (!m_Session) return {};
    return encoderLabel(m_Session->info());
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
    event.modifiersKnown = true;
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

void NativeMediaEngine::sendSecureAttention()
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::SecureAttention;
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

void NativeMediaEngine::sendKeyChar(const QString& ch, bool down, char modifiers,
                                    const CharKeyHints& hints)
{
    if (!m_Session) return;
    // The one host where a character can stay a real key: the platform layer
    // resolves it in the machine's own layout and presses THAT key, so the
    // scancode is real and Raw Input, DirectInput and typematic all still see
    // it. Deliberately outside the watchdog — a re-press of a character during
    // a stall would type it again rather than re-assert it.
    mw::native::InputEvent event;
    event.type =
        down ? mw::native::InputEvent::Type::CharDown : mw::native::InputEvent::Type::CharUp;
    event.text = ch.toStdString();
    event.modifiers = static_cast<uint8_t>(modifiers);
    event.modifiersKnown = true;
    // A chord of the touch keyboard's bar: the key carrying the character,
    // its US position the fallback for a layout with none.
    event.chord = hints.chord;
    event.keyCode = hints.positionVk;
    event.altGraph = hints.altGraph;
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
/// The browser's controller number, as this engine keeps it (see
/// setControllerOffset), on the wire's byte.
uint8_t padNumber(int controllerNumber)
{
    if (controllerNumber < 0) return 0;
    if (controllerNumber > 255) return 255;
    return static_cast<uint8_t>(controllerNumber);
}

} // namespace

void NativeMediaEngine::sendControllerArrival(uint8_t controllerNumber, uint16_t activeGamepadMask,
                                              uint8_t type, bool hasRumble)
{
    if (!m_Session) return;
    mw::native::InputEvent event;
    event.type = mw::native::InputEvent::Type::ControllerArrival;
    event.controllerNumber = padNumber(controllerNumber);
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
    event.controllerNumber = padNumber(controllerNumber);
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
    event.controllerNumber = padNumber(controllerNumber);
    event.activeGamepadMask = activeGamepadMask;
    m_Session->sendInput(event);
}

QString NativeMediaEngine::hidUnavailableReason() const
{
    return QString::fromStdString(mw::native::HidPassthrough::unavailableReason());
}

QString NativeMediaEngine::hidAttach(int slot, const QJsonObject& message)
{
    if (!m_Session) return QStringLiteral("no native session");
    QString error;
    mw::native::HidDeviceInfo device;
    device.collections =
        collectionsFromJson(message.value(QStringLiteral("collections")).toArray(), &error);
    if (!error.isEmpty()) return QStringLiteral("unreadable collections: ") + error;
    device.name = message.value(QStringLiteral("productName")).toString().left(120).toStdString();
    device.vendorId = static_cast<uint16_t>(message.value(QStringLiteral("vendorId")).toInt());
    device.productId = static_cast<uint16_t>(message.value(QStringLiteral("productId")).toInt());
    device.forceFeedback =
        message.value(QStringLiteral("forceFeedback")).toBool() && hidForceFeedback();

    std::lock_guard<std::mutex> lock(m_HidMutex);
    if (!m_Hid) {
        // What the host's OS asks of a device leaves on a backend thread; the
        // relay that sends it to the page lives on this engine's thread.
        m_Hid =
            std::make_unique<mw::native::HidPassthrough>([this](const mw::native::HidRequest& r) {
                const int kind = static_cast<int>(r.kind);
                const QByteArray data(reinterpret_cast<const char*>(r.data.data()),
                                      static_cast<int>(r.data.size()));
                QMetaObject::invokeMethod(
                    this,
                    [this, r, kind, data]() { emit hidRequest(r.slot, kind, r.reportId, data); },
                    Qt::QueuedConnection);
            });
        m_Hid->setFfbSink([this](const mw::native::HidFfb& op) {
            QJsonObject m = hidFfbToJson(op);
            m[QStringLiteral("type")] = QStringLiteral("hidffb");
            m[QStringLiteral("slot")] = op.slot;
            QMetaObject::invokeMethod(this, [this, m]() { emit hidFfb(m); }, Qt::QueuedConnection);
        });
        m_Hid->setParking(&mw::native::HidParking::process());
    }
    const std::string why = m_Hid->attach(slot, device);
    if (!why.empty()) {
        qWarning().noquote() << "[NativeMediaEngine] HID device refused:"
                             << QString::fromStdString(why);
        return QString::fromStdString(why);
    }
    return {};
}

void NativeMediaEngine::hidAttached(int slot)
{
    std::lock_guard<std::mutex> lock(m_HidMutex);
    if (m_Hid) m_Hid->attached(slot);
}

void NativeMediaEngine::hidInput(const uint8_t* frame, size_t size)
{
    if (size < 4) return;
    std::lock_guard<std::mutex> lock(m_HidMutex);
    if (!m_Hid) return;
    const uint16_t seq = static_cast<uint16_t>(frame[2] | (frame[3] << 8));
    m_Hid->input(frame[0], frame[1], seq, frame + 4, size - 4);
}

bool NativeMediaEngine::hidForceFeedback() const
{
    // Windows: pid.dll drives the PID block the recreated device gets. Linux:
    // the wheel is recreated through uinput, whose effects the host services
    // (hid-pidff and hid-logitech-hidpp refuse a uhid device).
    return mw::native::HidPassthrough::forceFeedbackAvailable();
}

void NativeMediaEngine::hidReply(int slot, int reportId, const QByteArray& data)
{
    if (slot < 0 || reportId < 0 || reportId > 0xFF || data.isEmpty()) return;
    std::lock_guard<std::mutex> lock(m_HidMutex);
    if (!m_Hid) return;
    m_Hid->reply(slot, static_cast<uint8_t>(reportId),
                 reinterpret_cast<const uint8_t*>(data.constData()),
                 static_cast<size_t>(data.size()));
}

bool NativeMediaEngine::hidRelaysHidpp() const
{
#ifdef Q_OS_LINUX
    return true;
#else
    return false;
#endif
}

void NativeMediaEngine::hidDetach(int slot)
{
    std::lock_guard<std::mutex> lock(m_HidMutex);
    if (m_Hid) m_Hid->detach(slot);
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
        event.modifiersKnown = true;
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

qint64 NativeMediaEngine::noteClientAlive()
{
    return m_Watchdog->noteClientAlive();
}
