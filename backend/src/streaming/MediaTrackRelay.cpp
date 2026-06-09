#include "MediaTrackRelay.h"
#include "MoonlightShim.h"

extern "C" {
#include "Limelight.h"
}

#include <rtc/rtc.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QDebug>
#include <QMap>
#include <cstring>
#include <random>
#include <chrono>

MediaTrackRelay::MediaTrackRelay(MoonlightShim* shim, QObject* parent)
    : RelayBase(parent)
    , m_Shim(shim)
{
    qInfo() << "[MediaTrackRelay] Created";

    connect(m_Shim, &MoonlightShim::videoFrameReady,
            this, &MediaTrackRelay::onVideoFrame);
    connect(m_Shim, &MoonlightShim::audioSampleReady,
            this, &MediaTrackRelay::onAudioSample);
    connect(m_Shim, &MoonlightShim::connectionTerminated,
            this, &MediaTrackRelay::onShimConnectionTerminated);

    // ICE connection timeout: emit iceTimedOut() if PC doesn't reach
    // Connected within 3s after setRemoteDescription().
    m_IceCheckTimer = new QTimer(this);
    m_IceCheckTimer->setSingleShot(true);
    connect(m_IceCheckTimer, &QTimer::timeout, this, &MediaTrackRelay::onIceCheckTimeout);

    // Stats timer: sends periodic stats to the browser via Input DC.
    m_StatsTimer = new QTimer(this);
    m_StatsTimer->setInterval(2000);
    connect(m_StatsTimer, &QTimer::timeout, this, &MediaTrackRelay::onStatsTimerTick);
}

MediaTrackRelay::~MediaTrackRelay()
{
    qInfo() << "[MediaTrackRelay] Destructor";
    stop();
}

bool MediaTrackRelay::prepare(const rtc::Configuration& config, bool)
{
    if (m_Pc) {
        qWarning() << "[MediaTrackRelay] already prepared";
        return false;
    }

    setupPeerConnection(config);
    return true;
}

bool MediaTrackRelay::setRemoteDescription(const std::string& sdp)
{
    if (!m_Pc) {
        qWarning() << "[MediaTrackRelay] No PeerConnection for setRemoteDescription";
        return false;
    }
    try {
        m_Pc->setRemoteDescription(rtc::Description(sdp));
        qInfo() << "[MediaTrackRelay] Remote description set — starting ICE timeout (3s)";
        if (m_IceCheckTimer) {
            m_IceCheckTimer->start(3000);
        }
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[MediaTrackRelay] setRemoteDescription failed:" << e.what();
        return false;
    }
}

bool MediaTrackRelay::addRemoteCandidate(const std::string& candidate, const std::string& mid)
{
    if (!m_Pc) return false;
    try {
        m_Pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[MediaTrackRelay] addRemoteCandidate failed:" << e.what();
        return false;
    }
}

void MediaTrackRelay::setupPeerConnection(const rtc::Configuration& config)
{
    qInfo() << "[MediaTrackRelay] Creating PeerConnection (media mode)";

    m_Pc = std::make_shared<rtc::PeerConnection>(config);

    // --- Local description callback ---
    m_Pc->onLocalDescription([this](const rtc::Description& sdp) {
        qInfo() << "[MediaTrackRelay] Local SDP generated, type=" << sdp.typeString()
                << "hasAudioOrVideo=" << sdp.hasAudioOrVideo()
                << "hasApplication=" << sdp.hasApplication();
        emit signalingSdpReady(std::string(sdp));
    });

    // --- Local ICE candidate callback ---
    m_Pc->onLocalCandidate([this](const rtc::Candidate& candidate) {
        rtc::Candidate modCandidate = candidate;

        // UPnP: rewrite host candidates with public IP
        if (m_ForceHostPublic && !m_PublicIP.empty() && m_PublicPort > 0 &&
            candidate.type() == rtc::Candidate::Type::Host) {

            std::string candStr = candidate.candidate();
            size_t firstSpace = candStr.find(' ');
            bool isIpv4 = true;
            if (firstSpace != std::string::npos &&
                candStr.find(':', firstSpace + 1) != std::string::npos) {
                isIpv4 = false;
            }

            if (isIpv4) {
                try {
                    modCandidate.changeAddress(m_PublicIP, m_PublicPort);
                    qInfo() << "[MediaTrackRelay] Rewrote host candidate:"
                            << QString::fromStdString(candidate.candidate())
                            << "->" << QString::fromStdString(m_PublicIP)
                            << ":" << m_PublicPort;
                } catch (const std::exception& e) {
                    qWarning() << "[MediaTrackRelay] Failed to rewrite candidate:" << e.what();
                }
            }
        }

        // UPnP: suppress IPv6 candidates
        if (m_SuppressIPv6) {
            std::string candStr = std::string(modCandidate.candidate());
            size_t space = candStr.find(' ');
            if (space != std::string::npos &&
                candStr.find(':', space + 1) != std::string::npos) {
                return; // Skip IPv6
            }
        }

        emit signalingIceCandidate(
            std::string(modCandidate.candidate()),
            std::string(modCandidate.mid()));
    });

    // --- State change callback ---
    m_Pc->onStateChange([this](rtc::PeerConnection::State state) {
        qInfo() << "[MediaTrackRelay] PC state changed to" << static_cast<int>(state);
        if (state == rtc::PeerConnection::State::Connected) {
            qInfo() << "[MediaTrackRelay] PeerConnection connected";
            if (m_IceCheckTimer) {
                m_IceCheckTimer->stop();
            }
        } else if (state == rtc::PeerConnection::State::Disconnected ||
                   state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed) {
            if (!m_Stopping.exchange(true)) {
                m_Connected = false;
                qInfo() << "[MediaTrackRelay] PC disconnected/failed/closed";
                emit sessionEnded();
            }
        }
    });

    // --- Gathering state ---
    m_Pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        qInfo() << "[MediaTrackRelay] ICE gathering state:" << static_cast<int>(state);
    });

    // Create the media tracks + DataChannels
    createTracksAndChannels();
}

void MediaTrackRelay::createTracksAndChannels()
{
    if (!m_Pc) return;

    qInfo() << "[MediaTrackRelay] Creating video track + audio/input DataChannels";

    // ── Video track (H.264, server->browser, RTP) ──────────────────────────
    // libdatachannel generates the SDP media section (m=video) automatically.
    // H264RtpPacketizer with LongStartSequence handles Annex B -> RTP.
    // Payload type 96 (dynamic, standard for H.264 in WebRTC).
    {
        auto videoDesc = rtc::Description::Video("video", rtc::Description::Direction::SendOnly);
        videoDesc.addH264Codec(96); // Uses default profile: 42e01f, packetization-mode=1

        m_VideoTrack = m_Pc->addTrack(videoDesc);
        if (m_VideoTrack) {
            // Generate a random SSRC
            std::random_device rd;
            uint32_t ssrc = static_cast<uint32_t>(rd());

            auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc, "video", 96, rtc::H264RtpPacketizer::ClockRate);
            auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::H264RtpPacketizer::Separator::LongStartSequence,
                rtpConfig);
            // Chain: packetizer → RtcpNackResponder → PliHandler
            // RtcpNackResponder retransmits RTP packets on NACK with a 128-packet buffer.
            auto nackResponder = std::make_shared<rtc::RtcpNackResponder>(128);
            packetizer->addToChain(nackResponder);
            // PLI handler requests a keyframe from Sunshine on decoder PLI.
            auto pliHandler = std::make_shared<rtc::PliHandler>([this]() {
                if (m_Shim && !m_Stopping.load()) {
                    m_Shim->requestIdrFrame();
                }
            });
            nackResponder->addToChain(pliHandler);
            m_VideoTrack->setMediaHandler(packetizer);

            m_VideoTrack->onOpen([this]() {
                qInfo() << "[MediaTrackRelay] Video Track open";
                QMetaObject::invokeMethod(this, [this]() {
                    sendBufferedKeyframe();
                }, Qt::QueuedConnection);
            });
            m_VideoTrack->onClosed([this]() {
                qInfo() << "[MediaTrackRelay] Video Track closed";
            });

            qInfo() << "[MediaTrackRelay] Video track created (H.264, PT=96)";
        } else {
            qWarning() << "[MediaTrackRelay] Failed to create video track";
        }
    }

    // ── Audio DataChannel (server->browser, PCM16) ─────────────────────────
    // Reliable, ordered — audio quality degrades with packet loss.
    {
        rtc::DataChannelInit audioConfig;
        audioConfig.negotiated = true;
        audioConfig.id = 0;

        m_AudioDc = m_Pc->createDataChannel("audio", audioConfig);
        if (m_AudioDc) {
            m_AudioDc->onOpen([this]() {
                qInfo() << "[MediaTrackRelay] Audio DataChannel open";
            });
            m_AudioDc->onClosed([this]() {
                qInfo() << "[MediaTrackRelay] Audio DataChannel closed";
            });
        }
    }

    // ── Input DataChannel (bidirectional, JSON text) ──────────────────────
    {
        rtc::DataChannelInit inputConfig;
        inputConfig.negotiated = true;
        inputConfig.id = 1;

        m_InputDc = m_Pc->createDataChannel("input", inputConfig);
        if (m_InputDc) {
            m_InputDc->onOpen([this]() {
                qInfo() << "[MediaTrackRelay] Input DataChannel open";
                m_Connected = true;
                emit dataChannelsOpen();

                // Start periodic stats timer
                if (m_StatsTimer) {
                    m_StatsTimer->start();
                    qInfo() << "[MediaTrackRelay] Stats timer started (2s interval)";
                }
            });
            m_InputDc->onClosed([this]() {
                qInfo() << "[MediaTrackRelay] Input DataChannel closed";
            });

            m_InputDc->onMessage([this](const std::variant<rtc::binary, rtc::string>& msg) {
                if (std::holds_alternative<rtc::string>(msg)) {
                    std::string text = std::get<rtc::string>(msg);
                    QMetaObject::invokeMethod(this, [this, text]() {
                        onInputMessage(text);
                    }, Qt::QueuedConnection);
                }
            });
        }
    }

    qInfo() << "[MediaTrackRelay] Tracks+Channels created (video=Track, audio=DC#0, input=DC#1)";
}

// ── Video forwarding (via RTP media track) ──────────────────────────────────────

void MediaTrackRelay::onVideoFrame(const QByteArray& data, int frameType, int)
{
    if (m_Stopping.load()) {
        static int dropCount = 0;
        if (++dropCount <= 3)
            qInfo() << "[MediaTrackRelay] onVideoFrame dropped — m_Stopping=true";
        return;
    }

    bool isKeyframe = (frameType == 1);

    // Buffer keyframes arriving before the Video Track is ready
    if (isKeyframe && (!m_VideoTrack || !m_VideoTrack->isOpen())) {
        m_BufferedKeyframe = data;
        m_HaveBufferedKeyframe = true;
        qInfo() << "[MediaTrackRelay] Buffered keyframe size=" << data.size()
                << "(Track ready=" << (m_VideoTrack && m_VideoTrack->isOpen()) << ")";
        return;
    }

    if (!m_VideoTrack || !m_VideoTrack->isOpen()) {
        static int noTrackCount = 0;
        if (++noTrackCount <= 5)
            qInfo() << "[MediaTrackRelay] onVideoFrame dropped — Video Track not ready";
        return;
    }

    static int logCounter = 0;
    logCounter++;
    if (logCounter <= 5 || logCounter % 120 == 0) {
        qInfo() << "[MediaTrackRelay] Video frame #" << logCounter
                << "size=" << data.size() << "type=" << frameType
                << "rtpTimestamp=" << m_RtpTimestamp;
    }

    // Create FrameInfo with isKeyFrame flag. The H264RtpPacketizer reads this
    // to decide STAP-A (single NAL) vs FU-A (fragmented) packetization.
    auto frameInfo = std::make_shared<rtc::FrameInfo>(m_RtpTimestamp);
    frameInfo->isKeyFrame = isKeyframe;

    // Copy data into binary format for sendFrame
    rtc::binary bin(static_cast<size_t>(data.size()));
    if (data.size() > 0) {
        std::memcpy(bin.data(), data.constData(), static_cast<size_t>(data.size()));
    }

    try {
        m_VideoTrack->sendFrame(std::move(bin), *frameInfo);
    } catch (const std::exception& e) {
        if (!m_Stopping.load()) {
            qWarning() << "[MediaTrackRelay] sendFrame error:" << e.what();
        }
        return;
    }

    m_FrameCount++;

    // Advance RTP timestamp: 90000 Hz / 60 fps = 1500 per frame
    m_RtpTimestamp += 1500;

    if (m_FrameCount <= 3 || m_FrameCount % 300 == 0) {
        qInfo() << "[MediaTrackRelay] Sent video frame #" << m_FrameCount
                << "size=" << data.size() << "isKeyframe=" << isKeyframe
                << "rtpTimestamp=" << m_RtpTimestamp;
    }
}

void MediaTrackRelay::sendBufferedKeyframe()
{
    if (!m_HaveBufferedKeyframe) return;
    if (m_Stopping.load() || !m_VideoTrack || !m_VideoTrack->isOpen()) return;

    qInfo() << "[MediaTrackRelay] Sending buffered keyframe, size="
            << m_BufferedKeyframe.size();

    auto frameInfo = std::make_shared<rtc::FrameInfo>(m_RtpTimestamp);
    frameInfo->isKeyFrame = true;

    rtc::binary bin(static_cast<size_t>(m_BufferedKeyframe.size()));
    if (m_BufferedKeyframe.size() > 0) {
        std::memcpy(bin.data(), m_BufferedKeyframe.constData(),
                    static_cast<size_t>(m_BufferedKeyframe.size()));
    }

    try {
        m_VideoTrack->sendFrame(std::move(bin), *frameInfo);
    } catch (const std::exception& e) {
        qWarning() << "[MediaTrackRelay] sendBufferedKeyframe error:" << e.what();
    }

    m_RtpTimestamp += 1500;
    m_BufferedKeyframe.clear();
    m_HaveBufferedKeyframe = false;
}

// ── Audio forwarding (via DataChannel, same as DataChannelRelay) ────────────────

void MediaTrackRelay::onAudioSample(const QByteArray& data)
{
    if (m_Stopping.load()) {
        static int dropCount = 0;
        if (++dropCount <= 3)
            qInfo() << "[MediaTrackRelay] onAudioSample dropped — m_Stopping=true";
        return;
    }
    if (!m_AudioDc) {
        static int noDcCount = 0;
        if (++noDcCount <= 3)
            qInfo() << "[MediaTrackRelay] onAudioSample dropped — m_AudioDc is null";
        return;
    }
    if (!m_AudioDc->isOpen()) {
        static int notOpenCount = 0;
        if (++notOpenCount <= 3)
            qInfo() << "[MediaTrackRelay] onAudioSample dropped — Audio DC not yet open";
        return;
    }

    static int audioCount = 0;
    audioCount++;
    if (audioCount <= 3) {
        qInfo() << "[MediaTrackRelay] Audio sample #" << audioCount
                << "size=" << data.size();
    }

    sendAudioFragmented(data, m_AudioDc);
}

void MediaTrackRelay::onShimConnectionTerminated(int errorCode)
{
    qInfo() << "[MediaTrackRelay] Shim connection terminated, code=" << errorCode;
    if (!m_Stopping.exchange(true)) {
        m_Connected = false;
        emit sessionEnded();
    }
}

// ── Input handling (from libdatachannel callback, marshaled to main thread) ──

void MediaTrackRelay::onInputMessage(const std::string& message)
{
    if (m_Stopping.load() || !m_Connected) return;

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(message));
    if (!doc.isObject()) {
        qWarning() << "[MediaTrackRelay] Invalid input JSON";
        return;
    }

    QJsonObject msg = doc.object();
    QString type = msg["type"].toString();

    static QMap<QString, int> inputCounts;
    int& count = inputCounts[type];
    count++;
    if (count <= 2) {
        qInfo() << "[MediaTrackRelay] Input #" << count << "type=" << type;
    }

    if (type == "keydown" || type == "keyup") {
        bool down = (type == "keydown");
        int vk = msg["keyCode"].toInt(0);
        QString code = msg["code"].toString();
        char mods = 0;
        if (msg["ctrlKey"].toBool(false))  mods |= 0x02;
        if (msg["shiftKey"].toBool(false)) mods |= 0x01;
        if (msg["altKey"].toBool(false))   mods |= 0x04;
        if (msg["metaKey"].toBool(false))  mods |= 0x08;

        short keyCode;
        char flags = 0;

        // International keys without standard US VK equivalents:
        // IntlBackslash (ISO key next to left Shift) and IntlRo (JIS \ key)
        // need raw scancode mode so Sunshine interprets them by physical
        // position instead of VK mapping.
        if (code == "IntlBackslash") {
            keyCode = 0x56;  // Windows Set 1 scancode for IntlBackslash
            flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else if (code == "IntlRo") {
            keyCode = 0x73;  // Windows Set 1 scancode for IntlRo
            flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else {
            keyCode = static_cast<short>(vk);
            flags = 0;
        }
        m_Shim->sendKeyEvent(keyCode, down, mods, flags);
    }
    else if (type == "mousemove") {
        short dx = static_cast<short>(msg["dx"].toInt(0));
        short dy = static_cast<short>(msg["dy"].toInt(0));
        m_Shim->sendMouseMove(dx, dy);
    }
    else if (type == "mousedown" || type == "mouseup") {
        bool down = (type == "mousedown");
        int button = msg["button"].toInt(1);
        m_Shim->sendMouseButton(down, button);
    }
    else if (type == "mousewheel") {
        short delta = static_cast<short>(msg["delta"].toInt(0));
        m_Shim->sendMouseScroll(delta);
    }
    else if (type == "request_idr") {
        qInfo() << "[MediaTrackRelay] Requesting IDR frame via DataChannel (browser)";
        m_Shim->requestIdrFrame();
    }
    else if (type == "requestidr") {
        qInfo() << "[MediaTrackRelay] Requesting IDR frame from Sunshine (browser request)";
        m_Shim->requestIdrFrame();
    }
    else if (type == "ping") {
        // Respond with pong (mirror the browser's timestamp for RTT calculation).
        int seq = msg["seq"].toInt(0);
        double ts = msg["ts"].toDouble(0);
        QJsonObject pong;
        pong["type"] = "pong";
        pong["seq"] = seq;
        pong["ts"] = ts;
        QByteArray pongJson = QJsonDocument(pong).toJson(QJsonDocument::Compact);
        if (m_InputDc && !m_Stopping.load()) {
            try {
                m_InputDc->send(std::string(pongJson.constData(), pongJson.size()));
            } catch (const std::exception& e) {
                if (!m_Stopping.load()) {
                    qWarning() << "[MediaTrackRelay] Pong send error:" << e.what();
                }
            }
        }
    }
    else {
        qWarning() << "[MediaTrackRelay] Unknown input type:" << type;
    }
}

// ── Audio fragmented send (same format as DataChannelRelay) ─────────────────────

void MediaTrackRelay::sendAudioFragmented(const QByteArray& data,
                                           std::shared_ptr<rtc::DataChannel>& dc)
{
    if (m_Stopping.load() || !dc || !dc->isOpen()) return;
    if (data.isEmpty()) return;

    int totalSize = data.size();
    int totalChunks = (totalSize + kMaxPayloadSize - 1) / kMaxPayloadSize;
    // Audio uses a separate frame ID space (starts high to avoid collision with DataChannelRelay)
    // Since this class doesn't use video DCs, we use frameId=0 for audio consistently.
    const uint32_t frameId = 0;

    for (int chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        int offset = chunkIdx * kMaxPayloadSize;
        int payloadSize = std::min(kMaxPayloadSize, totalSize - offset);

        rtc::binary bin(kFragHeaderSize + payloadSize);

        // Frame ID (4 bytes, big endian) — constant 0 for audio
        bin[0] = static_cast<std::byte>((frameId >> 24) & 0xFF);
        bin[1] = static_cast<std::byte>((frameId >> 16) & 0xFF);
        bin[2] = static_cast<std::byte>((frameId >> 8) & 0xFF);
        bin[3] = static_cast<std::byte>(frameId & 0xFF);

        // Chunk index (2 bytes, big endian)
        uint16_t chunkIdx16 = static_cast<uint16_t>(chunkIdx);
        bin[4] = static_cast<std::byte>((chunkIdx16 >> 8) & 0xFF);
        bin[5] = static_cast<std::byte>(chunkIdx16 & 0xFF);

        // Total chunks (2 bytes, big endian)
        uint16_t totalChunks16 = static_cast<uint16_t>(totalChunks);
        bin[6] = static_cast<std::byte>((totalChunks16 >> 8) & 0xFF);
        bin[7] = static_cast<std::byte>(totalChunks16 & 0xFF);

        // Is keyframe (1 byte) — always 0 for audio
        bin[8] = static_cast<std::byte>(0x00);

        // Payload size (4 bytes, big endian)
        uint32_t payloadSize32 = static_cast<uint32_t>(payloadSize);
        bin[9] = static_cast<std::byte>((payloadSize32 >> 24) & 0xFF);
        bin[10] = static_cast<std::byte>((payloadSize32 >> 16) & 0xFF);
        bin[11] = static_cast<std::byte>((payloadSize32 >> 8) & 0xFF);
        bin[12] = static_cast<std::byte>(payloadSize32 & 0xFF);

        // Backend timestamp (4 bytes, big endian) — 0 for non-video
        bin[13] = static_cast<std::byte>(0);
        bin[14] = static_cast<std::byte>(0);
        bin[15] = static_cast<std::byte>(0);
        bin[16] = static_cast<std::byte>(0);

        // Payload
        std::memcpy(bin.data() + kFragHeaderSize, data.constData() + offset,
                    static_cast<size_t>(payloadSize));

        try {
            dc->send(bin);
        } catch (const std::exception& e) {
            if (!m_Stopping.load()) {
                qWarning() << "[MediaTrackRelay] Audio fragmented send error:" << e.what();
            }
            return;
        }
    }
}

// ── Stats timer (2s interval) ───────────────────────────────────────────────────

void MediaTrackRelay::onStatsTimerTick()
{
    if (m_Stopping.load() || !m_Connected) return;
    if (!m_InputDc || !m_InputDc->isOpen()) return;

    double hostRttMs = 0.0;
    if (m_Shim) {
        hostRttMs = m_Shim->hostRttMs();
    }

    QJsonObject stats;
    stats["type"] = "stats";
    stats["hostRttMs"] = hostRttMs;
    // Media track mode: decode happens natively in the browser via RTP,
    // so backend decode latency is not meaningful here.
    stats["decodeLatencyUs"] = 0;
    QByteArray statsJson = QJsonDocument(stats).toJson(QJsonDocument::Compact);

    try {
        m_InputDc->send(std::string(statsJson.constData(), statsJson.size()));
    } catch (const std::exception& e) {
        if (!m_Stopping.load()) {
            qWarning() << "[MediaTrackRelay] Stats send error:" << e.what();
        }
    }
}

// ── ICE timeout ────────────────────────────────────────────────────────────────

void MediaTrackRelay::onIceCheckTimeout()
{
    if (m_Stopping.load()) return;
    if (m_Connected) return;
    if (m_Pc) {
        auto state = m_Pc->state();
        if (state == rtc::PeerConnection::State::Connected) return;
    }

    qWarning() << "[MediaTrackRelay] ICE timeout — PC did not reach Connected within 3s."
               << "Emitting iceTimedOut().";

    // In auto mode: the relay tracking will catch this and trigger tryNext().
    // In explicit mode: SignalingServer will start WS fallback.
    emit iceTimedOut();
}

// ── Stop ────────────────────────────────────────────────────────────────────────

void MediaTrackRelay::stop()
{
    if (m_Stopping.exchange(true)) {
        qInfo() << "[MediaTrackRelay::stop] Already stopping, skip";
        return;
    }

    qInfo() << "[MediaTrackRelay::stop] ENTER, frameCount=" << m_FrameCount;

    // Stop timers
    if (m_IceCheckTimer) {
        m_IceCheckTimer->stop();
    }
    if (m_StatsTimer) {
        m_StatsTimer->stop();
    }

    m_Connected = false;
    m_BufferedKeyframe.clear();
    m_HaveBufferedKeyframe = false;

    // Close DataChannels
    auto closeDc = [](std::shared_ptr<rtc::DataChannel>& dc, const char* name) {
        if (dc) {
            qInfo() << "[MediaTrackRelay] Closing" << name << "DataChannel";
            try { dc->close(); } catch (...) {}
            dc.reset();
        }
    };

    closeDc(m_AudioDc, "audio");
    closeDc(m_InputDc, "input");

    // Close Video Track
    if (m_VideoTrack) {
        qInfo() << "[MediaTrackRelay] Closing video track";
        try { m_VideoTrack->close(); } catch (...) {}
        m_VideoTrack.reset();
    }

    // Close PeerConnection
    if (m_Pc) {
        qInfo() << "[MediaTrackRelay] Closing PeerConnection";
        try { m_Pc->close(); } catch (...) {}
        m_Pc.reset();
    }

    qInfo() << "[MediaTrackRelay::stop] EXIT";
}

void MediaTrackRelay::requestIdrFrame()
{
    if (m_Stopping.load() || !m_Shim) return;
    qInfo() << "[MediaTrackRelay] requestIdrFrame: forwarding to MoonlightShim";
    m_Shim->requestIdrFrame();
}

void MediaTrackRelay::setPublicAddress(const std::string& publicIP, uint16_t publicPort)
{
    m_PublicIP = publicIP;
    m_PublicPort = publicPort;
    qInfo() << "[MediaTrackRelay] UPnP public address set:"
            << QString::fromStdString(publicIP) << ":" << publicPort;
}
