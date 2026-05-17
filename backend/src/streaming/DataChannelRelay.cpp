#include "DataChannelRelay.h"
#include "MoonlightShim.h"

#include <rtc/rtc.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QDebug>
#include <QDateTime>
#include <QMap>
#include <mutex>

DataChannelRelay::DataChannelRelay(MoonlightShim* shim, QObject* parent)
    : QObject(parent)
    , m_Shim(shim)
{
    qInfo() << "[DataChannelRelay] Created";

    connect(m_Shim, &MoonlightShim::videoFrameReady,
            this, &DataChannelRelay::onVideoFrame);
    connect(m_Shim, &MoonlightShim::audioSampleReady,
            this, &DataChannelRelay::onAudioSample);
    connect(m_Shim, &MoonlightShim::connectionTerminated,
            this, &DataChannelRelay::onShimConnectionTerminated);
}

DataChannelRelay::~DataChannelRelay()
{
    qInfo() << "[DataChannelRelay] Destructor";
    stop();
}

bool DataChannelRelay::prepare(const rtc::Configuration& config, bool)
{
    if (m_Pc) {
        qWarning() << "[DataChannelRelay] already prepared";
        return false;
    }

    setupPeerConnection(config);
    return true;
}

bool DataChannelRelay::setRemoteDescription(const std::string& sdp)
{
    if (!m_Pc) {
        qWarning() << "[DataChannelRelay] No PeerConnection for setRemoteDescription";
        return false;
    }
    try {
        m_Pc->setRemoteDescription(rtc::Description(sdp));
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[DataChannelRelay] setRemoteDescription failed:" << e.what();
        return false;
    }
}

bool DataChannelRelay::addRemoteCandidate(const std::string& candidate, const std::string& mid)
{
    if (!m_Pc) return false;
    try {
        m_Pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[DataChannelRelay] addRemoteCandidate failed:" << e.what();
        return false;
    }
}

void DataChannelRelay::setupPeerConnection(const rtc::Configuration& config)
{
    qInfo() << "[DataChannelRelay] Creating PeerConnection";

    m_Pc = std::make_shared<rtc::PeerConnection>(config);

    // --- Local description callback ---
    m_Pc->onLocalDescription([this](const rtc::Description& sdp) {
        qInfo() << "[DataChannelRelay] Local SDP generated, type=" << sdp.typeString();
        emit signalingSdpReady(std::string(sdp));
    });

    // --- Local ICE candidate callback ---
    m_Pc->onLocalCandidate([this](const rtc::Candidate& candidate) {
        rtc::Candidate modCandidate = candidate;

        // If UPnP is active and this is a host candidate, rewrite it with the
        // public IP and mapped port. This gives the browser a reachable UDP
        // endpoint through the UPnP-opened router port.
        if (m_ForceHostPublic && !m_PublicIP.empty() && m_PublicPort > 0 &&
            candidate.type() == rtc::Candidate::Type::Host) {

            // Only rewrite IPv4 candidates — parsing the candidate string
            // to check the address field. IPv6 addresses contain ':' in the
            // address part; rewriting them with an IPv4 public IP produces
            // an invalid candidate that breaks ICE.
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
                    qInfo() << "[DataChannelRelay] Rewrote host candidate:"
                            << QString::fromStdString(candidate.candidate())
                            << "->" << QString::fromStdString(m_PublicIP)
                            << ":" << m_PublicPort;
                } catch (const std::exception& e) {
                    qWarning() << "[DataChannelRelay] Failed to rewrite candidate:"
                               << e.what();
                }
            } else {
                qInfo() << "[DataChannelRelay] Skipping IPv6 candidate (cannot rewrite to IPv4):"
                        << QString::fromStdString(candidate.candidate());
            }
        }

        // When UPnP is active, suppress IPv6 candidates entirely so the
        // browser's ICE agent is forced to use the IPv4 UPnP path.
        // Residential IPv6 often fails because the router firewall blocks
        // unsolicited inbound IPv6 traffic (DTLS/SCTP timeout).
        if (m_SuppressIPv6) {
            std::string candStr = std::string(modCandidate.candidate());
            size_t space = candStr.find(' ');
            if (space != std::string::npos &&
                candStr.find(':', space + 1) != std::string::npos) {
                qInfo() << "[DataChannelRelay] Suppressing IPv6 candidate (UPnP active):"
                        << QString::fromStdString(candStr).left(80);
                return;  // Skip — don't emit this candidate
            }
        }

        emit signalingIceCandidate(
            std::string(modCandidate.candidate()),
            std::string(modCandidate.mid()));
    });

    // --- State change callback ---
    m_Pc->onStateChange([this](rtc::PeerConnection::State state) {
        qInfo() << "[DataChannelRelay] PC state changed to" << static_cast<int>(state);
        if (state == rtc::PeerConnection::State::Connected) {
            qInfo() << "[DataChannelRelay] PeerConnection connected";
        } else if (state == rtc::PeerConnection::State::Disconnected ||
                   state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed) {
            if (!m_Stopping.exchange(true)) {
                m_Connected = false;
                qInfo() << "[DataChannelRelay] PC disconnected/failed/closed";
                emit sessionEnded();
            }
        }
    });

    // --- Gathering state ---
    m_Pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        qInfo() << "[DataChannelRelay] ICE gathering state:" << static_cast<int>(state);
    });

    // Create the 3 DataChannels
    createDataChannels();
}

void DataChannelRelay::createDataChannels()
{
    if (!m_Pc) return;

    qInfo() << "[DataChannelRelay] Creating DataChannels";

    // --- Video DataChannel (server->browser, H.264 NAL units) ---
    // Unordered + no retransmits: video frames are time-sensitive, stale data
    // is worse than lost data. The decoder can recover from the next keyframe.
    rtc::DataChannelInit videoConfig;
    videoConfig.reliability.unordered = true;
    videoConfig.reliability.maxRetransmits = 0;  // No retransmits for video
    videoConfig.negotiated = true;
    videoConfig.id = 0;

    m_VideoDc = m_Pc->createDataChannel("video", videoConfig);
    if (m_VideoDc) {
        m_VideoDc->onOpen([this]() {
            qInfo() << "[DataChannelRelay] Video DataChannel open";
            // If a keyframe arrived before the DC was ready, send it now.
            // Must marshal to main thread because sendFragmented() may access
            // Qt objects owned by the main thread.
            QMetaObject::invokeMethod(this, [this]() {
                sendBufferedKeyframe();
            }, Qt::QueuedConnection);
        });
        m_VideoDc->onClosed([this]() {
            qInfo() << "[DataChannelRelay] Video DataChannel closed";
        });
    }

    // --- Audio DataChannel (server->browser, PCM samples) ---
    rtc::DataChannelInit audioConfig;
    // Default reliability: ordered + reliable (no maxRetransmits, no maxPacketLifeTime)
    audioConfig.negotiated = true;
    audioConfig.id = 1;

    m_AudioDc = m_Pc->createDataChannel("audio", audioConfig);
    if (m_AudioDc) {
        m_AudioDc->onOpen([this]() {
            qInfo() << "[DataChannelRelay] Audio DataChannel open";
        });
        m_AudioDc->onClosed([this]() {
            qInfo() << "[DataChannelRelay] Audio DataChannel closed";
        });
    }

    // --- Input DataChannel (bidirectional, JSON text) ---
    rtc::DataChannelInit inputConfig;
    // Default reliability: ordered + reliable
    inputConfig.negotiated = true;
    inputConfig.id = 2;

    m_InputDc = m_Pc->createDataChannel("input", inputConfig);
    if (m_InputDc) {
        m_InputDc->onOpen([this]() {
            qInfo() << "[DataChannelRelay] Input DataChannel open";
            m_Connected = true;
            // All 3 DataChannels are open when we reach here
            // (they all open together as part of SCTP association)
            emit dataChannelsOpen();
        });
        m_InputDc->onClosed([this]() {
            qInfo() << "[DataChannelRelay] Input DataChannel closed";
        });

        // Input messages arrive from browser on this channel
        m_InputDc->onMessage([this](const std::variant<rtc::binary, rtc::string>& msg) {
            // Marshal to main thread for Qt signal safety
            if (std::holds_alternative<rtc::string>(msg)) {
                std::string text = std::get<rtc::string>(msg);
                QMetaObject::invokeMethod(this, [this, text]() {
                    onInputMessage(text);
                }, Qt::QueuedConnection);
            }
        });
    }

    qInfo() << "[DataChannelRelay] DataChannels created (video=0, audio=1, input=2)";
}

// --- Video/Audio forwarding (from MoonlightShim signals, on main thread) ---

void DataChannelRelay::onVideoFrame(const QByteArray& data, int frameType, int)
{
    if (m_Stopping.load()) {
        static int dropCount = 0;
        if (++dropCount <= 3)
            qInfo() << "[DataChannelRelay] onVideoFrame dropped — m_Stopping=true";
        return;
    }

    bool isKeyframe = (frameType == 1);

    // Buffer keyframes arriving before the Video DC is ready.
    // Sunshine starts sending the initial IDR immediately after launch,
    // before ICE negotiation and DataChannel creation complete (~1-2s).
    // Without this buffer, the keyframe (containing SPS/PPS) is lost, the
    // browser's VideoDecoder can never configure, and we get decoder=null.
    if (isKeyframe && (!m_VideoDc || !m_VideoDc->isOpen())) {
        m_BufferedKeyframe = data;
        m_HaveBufferedKeyframe = true;
        qInfo() << "[DataChannelRelay] Buffered keyframe size=" << data.size()
                << "(DC ready=" << (m_VideoDc && m_VideoDc->isOpen()) << ")";
        return;
    }

    // Drop non-keyframes before DC is ready
    if (!m_VideoDc) {
        static int noDcCount = 0;
        if (++noDcCount <= 5)
            qInfo() << "[DataChannelRelay] onVideoFrame dropped — m_VideoDc is null (DCs not created yet?)";
        return;
    }
    if (!m_VideoDc->isOpen()) {
        return;  // DC exists but not yet open
    }

    static int logCounter = 0;
    logCounter++;
    if (logCounter <= 5 || logCounter % 120 == 0) {
        qInfo() << "[DataChannelRelay] Video frame #" << logCounter
                << "size=" << data.size() << "type=" << frameType;
    }

    sendFragmented(data, isKeyframe, m_VideoDc);
}

// --- Buffered keyframe ---

void DataChannelRelay::sendBufferedKeyframe()
{
    if (!m_HaveBufferedKeyframe) return;
    if (m_Stopping.load() || !m_VideoDc || !m_VideoDc->isOpen()) return;

    qInfo() << "[DataChannelRelay] Sending buffered keyframe, size="
            << m_BufferedKeyframe.size();
    sendFragmented(m_BufferedKeyframe, true, m_VideoDc);
    m_BufferedKeyframe.clear();
    m_HaveBufferedKeyframe = false;
}

// --- Audio forwarding ---

void DataChannelRelay::onAudioSample(const QByteArray& data)
{
    if (m_Stopping.load()) {
        static int dropCount = 0;
        if (++dropCount <= 3)
            qInfo() << "[DataChannelRelay] onAudioSample dropped — m_Stopping=true";
        return;
    }
    if (!m_AudioDc) {
        static int noDcCount = 0;
        if (++noDcCount <= 3)
            qInfo() << "[DataChannelRelay] onAudioSample dropped — m_AudioDc is null";
        return;
    }
    if (!m_AudioDc->isOpen()) {
        static int notOpenCount = 0;
        if (++notOpenCount <= 3)
            qInfo() << "[DataChannelRelay] onAudioSample dropped — Audio DC not yet open";
        return;
    }

    static int audioCount = 0;
    audioCount++;
    if (audioCount <= 3) {
        qInfo() << "[DataChannelRelay] Audio sample #" << audioCount
                << "size=" << data.size();
    }

    // Audio uses the same fragmented format as video (isKeyframe=false).
    // Most audio packets are small (~KB), but PCM samples can also be large
    // (e.g., a full 16ms frame at 48kHz stereo = 3072 bytes). The header
    // is consistent and the receiver can demux by channel.
    sendFragmented(data, false, m_AudioDc);
}

void DataChannelRelay::onShimConnectionTerminated(int errorCode)
{
    qInfo() << "[DataChannelRelay] Shim connection terminated, code=" << errorCode;
    if (!m_Stopping.exchange(true)) {
        m_Connected = false;
        emit sessionEnded();
    }
}

// --- Input handling (from libdatachannel callback, marshaled to main thread) ---

void DataChannelRelay::onInputMessage(const std::string& message)
{
    if (m_Stopping.load() || !m_Connected) return;

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(message));
    if (!doc.isObject()) {
        qWarning() << "[DataChannelRelay] Invalid input JSON";
        return;
    }

    QJsonObject msg = doc.object();
    QString type = msg["type"].toString();

    static QMap<QString, int> inputCounts;
    int& count = inputCounts[type];
    count++;
    if (count <= 2) {
        qInfo() << "[DataChannelRelay] Input #" << count << "type=" << type;
    }

    if (type == "keydown" || type == "keyup") {
        bool down = (type == "keydown");
        int vk = msg["keyCode"].toInt(0);
        char mods = 0;
        if (msg["ctrlKey"].toBool(false))  mods |= 0x02;
        if (msg["shiftKey"].toBool(false)) mods |= 0x01;
        if (msg["altKey"].toBool(false))   mods |= 0x04;
        if (msg["metaKey"].toBool(false))  mods |= 0x08;
        m_Shim->sendKeyEvent(static_cast<short>(vk), down, mods, 0);
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
    else if (type == "requestidr") {
        qInfo() << "[DataChannelRelay] Requesting IDR frame from Sunshine (browser request)";
        m_Shim->requestIdrFrame();
    }
    else {
        qWarning() << "[DataChannelRelay] Unknown input type:" << type;
    }
}

// --- Fragmented send ---
// Splits data into chunks of up to kMaxPayloadSize bytes.
// Header format (17 bytes total):
//   [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
// backend_ts: QDateTime::currentMSecsSinceEpoch() modulo 2^32, written at send time.
// All multi-byte fields in network byte order (big endian).

void DataChannelRelay::sendFragmented(const QByteArray& data, bool isKeyframe,
                                      std::shared_ptr<rtc::DataChannel>& dc)
{
    if (m_Stopping.load() || !dc || !dc->isOpen()) return;
    if (data.isEmpty()) return;

    // Backpressure: drop delta frames when the SCTP send buffer is full.
    // Without this check, dc->send() blocks the main thread (Qt event loop)
    // until the buffer drains, causing micro-freezes in audio/video processing.
    // Keyframes are always sent because losing a keyframe would stall the
    // browser decoder until the next IDR request.
    if (!isKeyframe) {
        size_t bufAmt = dc->bufferedAmount();
        if (bufAmt > kHighWatermark) {
            m_DeltaDroppedCount++;
            if (m_DeltaDroppedCount <= 3 || m_DeltaDroppedCount % 120 == 0) {
                qInfo() << "[DataChannelRelay] Dropped delta frame (SCTP full)"
                        << "bufferedAmount=" << bufAmt
                        << "totalDropped=" << m_DeltaDroppedCount
                        << "kHighWatermark=" << kHighWatermark;
            }
            return;
        }
    } else {
        // Log a warning if a keyframe arrives while the buffer is above the
        // watermark — it may cause a brief main-thread stall.
        size_t bufAmt = dc->bufferedAmount();
        if (bufAmt > kHighWatermark) {
            m_KeyframeBackpressureWarnings++;
            if (m_KeyframeBackpressureWarnings <= 5) {
                qInfo() << "[DataChannelRelay] Keyframe with full SCTP buffer"
                        << "bufferedAmount=" << bufAmt
                        << "warnCount=" << m_KeyframeBackpressureWarnings;
            }
        }
    }

    int totalSize = data.size();
    int totalChunks = (totalSize + kMaxPayloadSize - 1) / kMaxPayloadSize;
    uint32_t frameId = m_FrameId++;

    // Capture a monotonic timestamp (ms since epoch, modulo 2^32) at send time.
    // The frontend uses this with Date.now() to estimate end-to-end latency.
    // Clocks must be roughly synchronized (NTP) for the estimate to be accurate.
    uint32_t backendTs = static_cast<uint32_t>(
        QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);

    for (int chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        int offset = chunkIdx * kMaxPayloadSize;
        int payloadSize = std::min(kMaxPayloadSize, totalSize - offset);

        rtc::binary bin(kFragHeaderSize + payloadSize);

        // Frame ID (4 bytes, big endian)
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

        // Is keyframe (1 byte)
        bin[8] = static_cast<std::byte>(isKeyframe ? 0x01 : 0x00);

        // Payload size (4 bytes, big endian)
        uint32_t payloadSize32 = static_cast<uint32_t>(payloadSize);
        bin[9] = static_cast<std::byte>((payloadSize32 >> 24) & 0xFF);
        bin[10] = static_cast<std::byte>((payloadSize32 >> 16) & 0xFF);
        bin[11] = static_cast<std::byte>((payloadSize32 >> 8) & 0xFF);
        bin[12] = static_cast<std::byte>(payloadSize32 & 0xFF);

        // Backend timestamp (4 bytes, big endian) — same value for all chunks
        bin[13] = static_cast<std::byte>((backendTs >> 24) & 0xFF);
        bin[14] = static_cast<std::byte>((backendTs >> 16) & 0xFF);
        bin[15] = static_cast<std::byte>((backendTs >> 8) & 0xFF);
        bin[16] = static_cast<std::byte>(backendTs & 0xFF);

        // Payload
        std::memcpy(bin.data() + kFragHeaderSize, data.constData() + offset,
                    static_cast<size_t>(payloadSize));

        try {
            dc->send(bin);
        } catch (const std::exception& e) {
            if (!m_Stopping.load()) {
                qWarning() << "[DataChannelRelay] Fragmented send error:" << e.what();
            }
            return;
        }
    }

    m_FrameCount++;
    if (m_FrameCount <= 3 || m_FrameCount % 300 == 0) {
        qInfo() << "[DataChannelRelay] Sent frame #" << m_FrameCount
                << "totalSize=" << totalSize << "chunks=" << totalChunks
                << "isKeyframe=" << isKeyframe << "frameId=" << frameId;
    }
}

// --- Stop ---

void DataChannelRelay::stop()
{
    if (m_Stopping.exchange(true)) {
        qInfo() << "[DataChannelRelay::stop] Already stopping, skip";
        return;
    }

    qInfo() << "[DataChannelRelay::stop] ENTER, frameCount=" << m_FrameCount;

    // Reset backpressure counters for next session
    m_DeltaDroppedCount = 0;
    m_KeyframeBackpressureWarnings = 0;

    // Clear buffered keyframe (if any)
    m_BufferedKeyframe.clear();
    m_HaveBufferedKeyframe = false;

    m_Connected = false;

    // Close DataChannels
    auto closeDc = [](std::shared_ptr<rtc::DataChannel>& dc, const char* name) {
        if (dc) {
            qInfo() << "[DataChannelRelay] Closing" << name << "DataChannel";
            try { dc->close(); } catch (...) {}
            dc.reset();
        }
    };

    closeDc(m_VideoDc, "video");
    closeDc(m_AudioDc, "audio");
    closeDc(m_InputDc, "input");

    // Close PeerConnection
    if (m_Pc) {
        qInfo() << "[DataChannelRelay] Closing PeerConnection";
        try { m_Pc->close(); } catch (...) {}
        m_Pc.reset();
    }

    qInfo() << "[DataChannelRelay::stop] EXIT";
}

void DataChannelRelay::requestIdrFrame()
{
    if (m_Stopping.load() || !m_Shim) return;
    qInfo() << "[DataChannelRelay] requestIdrFrame: forwarding to MoonlightShim";
    m_Shim->requestIdrFrame();
}

void DataChannelRelay::setPublicAddress(const std::string& publicIP, uint16_t publicPort)
{
    m_PublicIP = publicIP;
    m_PublicPort = publicPort;
    qInfo() << "[DataChannelRelay] UPnP public address set:"
            << QString::fromStdString(publicIP) << ":" << publicPort;
}
