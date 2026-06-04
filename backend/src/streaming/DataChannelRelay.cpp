#include "DataChannelRelay.h"
#include "MoonlightShim.h"

extern "C" {
#include "Limelight.h"
}

#include <rtc/rtc.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QDebug>
#include <QDateTime>
#include <QMap>
#include <QVector>
#include <mutex>
#include <chrono>
#include <cstring>

// ============================================================================
// HEVC VPS/SPS patching helpers
// Chrome Windows HEVC decoder may produce a black frame from the initial
// VPS/SPS/IDR if the encoder uses a high level_idc (> 5.1/153) or multi-layer
// temporal sublayers.  We patch these parameters to safe defaults before
// forwarding the keyframe to the browser.
//
// NAL data layout (Annex B byte stream):
//   [00 00 00 01][NAL header + RBSP][00 00 00 01][NAL header + RBSP]...
// ============================================================================

/// Strip HEVC emulation prevention bytes (00 00 03) from RBSP data.
/// Returns cleaned data with 0x03 removal bytes omitted.
static QByteArray removeHvcEp(const QByteArray& data) {
    QByteArray result;
    result.reserve(data.size());
    for (int i = 0; i < data.size(); i++) {
        if (i + 2 < data.size() &&
            static_cast<unsigned char>(data[i]) == 0x00 &&
            static_cast<unsigned char>(data[i+1]) == 0x00 &&
            static_cast<unsigned char>(data[i+2]) == 0x03) {
            result.append('\x00');
            result.append('\x00');
            i += 2;  // skip the 0x03 — for loop increments past it
        } else {
            result.append(data[i]);
        }
    }
    return result;
}

/// Re-insert HEVC emulation prevention bytes (00 00 03) after modifying RBSP.
/// Inserts 0x03 after any 00 00 pair followed by 00, 01, 02, or 03.
static QByteArray addHvcEp(const QByteArray& rbsp) {
    QByteArray result;
    result.reserve(rbsp.size() + 16);
    int zeroRun = 0;
    for (int i = 0; i < rbsp.size(); i++) {
        unsigned char b = static_cast<unsigned char>(rbsp[i]);
        if (zeroRun >= 2 && b <= 0x03) {
            result.append('\x03');
            zeroRun = 0;
        }
        result.append(rbsp[i]);
        if (b == 0x00)
            zeroRun++;
        else
            zeroRun = 0;
    }
    return result;
}

/// Represents one NAL unit found in an Annex B byte stream.
struct NalLocation {
    int startOffset;  // offset of start code in the data
    int startLen;     // start code length (3 or 4)
    int nalOffset;    // offset of NAL data (after start code)
    int nalLen;       // length of NAL data
};

/// Scan an Annex B byte stream and return all NAL unit locations.
/// Assumes valid Annex B structure (may be corrupt for invalid streams).
static QVector<NalLocation> scanNals(const QByteArray& data) {
    QVector<NalLocation> nals;
    int i = 0;
    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    while (i < data.size() - 3) {
        if (d[i] == 0 && d[i+1] == 0) {
            int scLen = 0;
            if (d[i+2] == 1)
                scLen = 3;
            else if (i + 3 < data.size() && d[i+2] == 0 && d[i+3] == 1)
                scLen = 4;
            if (scLen) {
                NalLocation loc;
                loc.startOffset = i;
                loc.startLen = scLen;
                loc.nalOffset = i + scLen;
                // End of this NAL = next start code or end of buffer
                int end = data.size();
                for (int j = i + scLen; j < data.size() - 3; j++) {
                    if (d[j] == 0 && d[j+1] == 0 &&
                        (d[j+2] == 1 || (j + 3 < data.size() && d[j+2] == 0 && d[j+3] == 1))) {
                        end = j;
                        break;
                    }
                }
                loc.nalLen = end - loc.nalOffset;
                nals.append(loc);
                i = end;
                continue;
            }
        }
        i++;
    }
    return nals;
}

/// Extract the HEVC NAL unit type from a NAL unit (after start code, without EP removal).
static int hevcNalType(const QByteArray& nal) {
    if (nal.size() < 2) return -1;
    return (static_cast<unsigned char>(nal[0]) >> 1) & 0x3F;
}

/// Log HEVC VPS/SPS/PPS parameters for debugging.
static void logHevcParameters(const QByteArray& vps, const QByteArray& sps,
                               const QByteArray& pps, const QString& label)
{
    if (!sps.isEmpty()) {
        QByteArray cleanSps = removeHvcEp(sps);
        if (cleanSps.size() >= 15) {
            const unsigned char* s = reinterpret_cast<const unsigned char*>(cleanSps.constData());
            int subLayers = (s[2] >> 1) & 0x07;
            int profileIdc = s[3] & 0x1F;
            int tierFlag = (s[3] >> 5) & 0x01;
            int levelIdc = s[14];
            uint32_t compatFlags = (static_cast<uint32_t>(s[4]) << 24) |
                                   (static_cast<uint32_t>(s[5]) << 16) |
                                   (static_cast<uint32_t>(s[6]) << 8) |
                                   static_cast<uint32_t>(s[7]);

            qInfo().noquote()
                << QString("[HEVC-PARAM %1] SPS: subLayers=%2 profile=%3 tier=%4 level=%5 compat=0x%6")
                       .arg(label)
                       .arg(subLayers)
                       .arg(profileIdc)
                       .arg(tierFlag)
                       .arg(levelIdc)
                       .arg(compatFlags, 8, 16, QChar('0'));
        }
    }
    if (!vps.isEmpty()) {
        QByteArray cleanVps = removeHvcEp(vps);
        if (cleanVps.size() >= 4) {
            const unsigned char* v = reinterpret_cast<const unsigned char*>(cleanVps.constData());
            // vps_max_sub_layers_minus1 at RBSP byte 1 bits 3-1 = clean[3] bits 3-1
            int subLayers = (v[3] >> 1) & 0x07;
            qInfo().noquote()
                << QString("[HEVC-PARAM %1] VPS: subLayers=%2").arg(label).arg(subLayers);
        }
    }
}

/// Rebuild HEVC VPS NAL unit with vps_max_sub_layers_minus1=0.
///
/// Takes the EP-removed VPS NAL unit (including 2-byte NAL header) and returns
/// a rebuilt minimal VPS with:
///   - vps_max_sub_layers_minus1 set to 0
///   - Only the general profile/tier/level fields preserved
///   - All sub-layer specific data removed
///   - Minimal safe values for remaining VPS fields
///
/// Returns empty QByteArray if the original already has vps_max_sub_layers=0.
static QByteArray rebuildVpsNoSubLayers(const QByteArray& cleanVps)
{
    // Minimum size: NAL header(2) + VPS fixed header(4) + PTL general(12) = 18
    if (cleanVps.size() < 18) return {};

    const unsigned char* vps = reinterpret_cast<const unsigned char*>(cleanVps.constData());

    // RBSP byte 1 (= clean[3]) bits 3-1 = vps_max_sub_layers_minus1
    int origSubLayers = (vps[3] >> 1) & 0x07;
    if (origSubLayers == 0) return {};

    qInfo() << "[HEVC-Rebuild] VPS: sub_layers" << origSubLayers << "→ 0 (rebuild)";

    QByteArray result;
    result.reserve(24);

    // 1. NAL header (2 bytes) — copy verbatim
    result.append(cleanVps.constData(), 2);

    // 2. VPS fixed header (RBSP bytes 0-3 = clean[2..5])
    //    RBSP byte 0: vps_id(4) | base_internal(1) | base_available(1) | max_layers_hi(2)
    result.append(cleanVps[2]);
    //    RBSP byte 1: max_layers_lo(4) | max_sub_layers(3)=000 | temporal_nest(1)
    result.append(static_cast<char>(vps[3] & 0xF1));
    //    RBSP bytes 2-3: vps_reserved_0xffff — copy verbatim
    result.append(cleanVps[4]);
    result.append(cleanVps[5]);

    // 3. profile_tier_level(1, 0) — general fields only (12 bytes: clean[6..17])
    //    PTL layout: profile_space/tier/idc(1B) + compatibility_flags(4B)
    //    + constraint_indicators(6B) + level_idc(1B) = 12 bytes
    if (cleanVps.size() < 18) return {};
    result.append(cleanVps.constData() + 6, 12);

    // 4. Reserved zeros for maxSubLayersMinus1=0
    //    For maxSubLayersMinus1=0, no sub-layer present flags,
    //    8 iterations of reserved_zero_2bits = 16 bits = 2 bytes
    result.append(static_cast<char>(0x00));
    result.append(static_cast<char>(0x00));

    // 5. VPS tail (after PTL) — minimal safe values
    //    Bit layout (MSB first, 2 bytes total):
    //    Byte 0: vps_sub_layer_ordering_info_present_flag=0
    //            vps_max_dec_pic_buffering_minus1[0]=0 (ue(v)="1")
    //            vps_max_num_reorder_pics[0]=0 (ue(v)="1")
    //            vps_max_latency_increase_plus1[0]=0 (ue(v)="1")
    //            vps_max_layer_id hi=0 (2 bits)
    //    Byte 1: vps_max_layer_id lo=0 (4 bits)
    //            vps_num_layer_sets_minus1=0 (ue(v)="1")
    //            vps_timing_info_present_flag=0
    //            vps_extension_flag=0
    //            rbsp_stop_one_bit=1 + alignment zeros
    //
    //    Assembled: [0][1][1][1][0000][1][0][0][1][00]
    //    Byte 0: 0111 0000 = 0x70
    //    Byte 1: 0010 0100 = 0x24
    result.append(static_cast<char>(0x70));
    result.append(static_cast<char>(0x24));

    qInfo() << "[HEVC-Rebuild] VPS:" << cleanVps.size() << "→" << result.size() << "bytes";
    return result;
}

/// Rebuild HEVC SPS NAL unit with sps_max_sub_layers_minus1=0.
///
/// Takes the EP-removed SPS NAL unit (including 2-byte NAL header) and returns
/// a rebuilt SPS with:
///   - sps_max_sub_layers_minus1 set to 0
///   - profile_tier_level truncated to remove sub-layer data
///   - All SPS-specific fields after PTL preserved verbatim
///
/// Returns empty QByteArray if original already has sps_max_sub_layers=0.
static QByteArray rebuildSpsNoSubLayers(const QByteArray& cleanSps)
{
    // Minimum: NAL header(2) + SPS header(1) + PTL general(12) + flags(2) = 17
    if (cleanSps.size() < 17) return {};

    const unsigned char* sps = reinterpret_cast<const unsigned char*>(cleanSps.constData());

    // RBSP byte 0 (= clean[2]) bits 3-1 = sps_max_sub_layers_minus1
    int origSubLayers = (sps[2] >> 1) & 0x07;
    if (origSubLayers == 0) return {};

    // PTL general fields: clean[3..14] (12 bytes).
    // After level_idc (clean[14]) comes:
    //   - present flags: origSubLayers * 2 bits
    //   - reserved: (8 - origSubLayers) * 2 bits
    //   Total flags+reserved = 16 bits = 2 bytes regardless of origSubLayers
    //
    // After flags+reserved: sub-layer data (variable length):
    //   For each j where profile_present[j]=1: 12 bytes (full profile struct)
    //   For each j where level_present[j]=1: 1 byte (level_idc)

    // Parse present flags from clean[15..16]
    unsigned char flagsByte0 = static_cast<unsigned char>(sps[15]);
    unsigned char flagsByte1 = static_cast<unsigned char>(sps[16]);

    int subLayerDataBytes = 0;
    for (int j = 0; j < origSubLayers; j++) {
        bool profilePresent, levelPresent;
        if (j < 4) {
            // Byte 0: bit 7=p[0],6=l[0],5=p[1],4=l[1],3=p[2],2=l[2],1=p[3],0=l[3]
            profilePresent = (flagsByte0 >> (7 - j * 2)) & 1;
            levelPresent   = (flagsByte0 >> (6 - j * 2)) & 1;
        } else {
            // Byte 1: bit 7=p[4],6=l[4],5=p[5],4=l[5],3..0=reserved
            profilePresent = (flagsByte1 >> (15 - j * 2)) & 1;
            levelPresent   = (flagsByte1 >> (14 - j * 2)) & 1;
        }
        if (profilePresent)
            subLayerDataBytes += 12;
        if (levelPresent)
            subLayerDataBytes += 1;
    }

    // Original PTL end offset in cleanSps (index past the PTL)
    int ptlEnd = 15 + 2 + subLayerDataBytes;
    if (ptlEnd > cleanSps.size()) ptlEnd = cleanSps.size();

    qInfo() << "[HEVC-Rebuild] SPS: sub_layers" << origSubLayers
            << "→ 0 (subLayerData=" << subLayerDataBytes << "bytes)";

    QByteArray result;
    result.reserve(cleanSps.size() - subLayerDataBytes);

    // 1. NAL header (2 bytes)
    result.append(cleanSps.constData(), 2);

    // 2. SPS header with sub_layers=0
    result.append(static_cast<char>(sps[2] & 0xF1));

    // 3. PTL general fields (12 bytes: clean[3..14])
    result.append(cleanSps.constData() + 3, 12);

    // 4. Reserved zeros (2 bytes)
    result.append(static_cast<char>(0x00));
    result.append(static_cast<char>(0x00));

    // 5. SPS fields after original PTL (preserve verbatim)
    if (ptlEnd < cleanSps.size())
        result.append(cleanSps.constData() + ptlEnd, cleanSps.size() - ptlEnd);

    qInfo() << "[HEVC-Rebuild] SPS:" << cleanSps.size() << "→" << result.size() << "bytes";
    return result;
}

/// Patch VPS/SPS in the first HEVC keyframe to fix Chrome Windows black screen.
///
/// Modifications applied (only if beneficial):
///   1. general_level_idc capped to 153 (Level 5.1)
///   2. sps_max_sub_layers_minus1 → 0 (with PTL truncation)
///   3. vps_max_sub_layers_minus1 → 0 (with full VPS rebuild)
///
/// Returns true if any byte was modified.
static bool patchHevcKeyframe(QByteArray& data)
{
    auto nals = scanNals(data);

    int vpsIdx = -1, spsIdx = -1;
    for (int i = 0; i < nals.size(); i++) {
        QByteArray nal = data.mid(nals[i].nalOffset, nals[i].nalLen);
        int type = hevcNalType(nal);
        if (type == 32) vpsIdx = i;       // HEVC_VPS
        else if (type == 33) spsIdx = i;  // HEVC_SPS
    }

    bool patched = false;

    // ── VPS: rebuild with max_sub_layers=0 ─────────────────────────────────
    if (vpsIdx >= 0) {
        NalLocation& vpsLoc = nals[vpsIdx];
        QByteArray vpsNal = data.mid(vpsLoc.nalOffset, vpsLoc.nalLen);
        QByteArray clean = removeHvcEp(vpsNal);
        QByteArray rebuilt = rebuildVpsNoSubLayers(clean);
        if (!rebuilt.isEmpty()) {
            QByteArray patchedVps = addHvcEp(rebuilt);
            data.replace(vpsLoc.nalOffset, vpsLoc.nalLen, patchedVps);
            patched = true;
        }
    }

    // ── SPS: rebuild with max_sub_layers=0 + cap level_idc ────────────────
    if (spsIdx >= 0) {
        NalLocation& spsLoc = nals[spsIdx];
        QByteArray spsNal = data.mid(spsLoc.nalOffset, spsLoc.nalLen);
        QByteArray clean = removeHvcEp(spsNal);
        QByteArray rebuilt = rebuildSpsNoSubLayers(clean);
        if (!rebuilt.isEmpty()) {
            // general_level_idc at byte 14 (same offset as in the original)
            if (rebuilt.size() > 14) {
                unsigned char* spsRebuilt = reinterpret_cast<unsigned char*>(rebuilt.data());
                int levelIdc = spsRebuilt[14];
                int cappedLevel = levelIdc;
                if (levelIdc > 153) {
                    cappedLevel = 153;
                    qInfo() << "[HEVC-Patch] SPS: general_level_idc" << levelIdc << "→ 153";
                } else if (levelIdc > 0 && levelIdc < 30) {
                    cappedLevel = 153;
                    qInfo() << "[HEVC-Patch] SPS: general_level_idc too low" << levelIdc << "→ 153";
                }
                spsRebuilt[14] = static_cast<unsigned char>(cappedLevel);
            }

            QByteArray patchedSps = addHvcEp(rebuilt);
            data.replace(spsLoc.nalOffset, spsLoc.nalLen, patchedSps);
            patched = true;
        }
    }

    return patched;
}

/// Extract VPS/SPS/PPS from a HEVC keyframe for logging.
static void extractHevcParameterSets(const QByteArray& data,
                                      QByteArray& vps, QByteArray& sps, QByteArray& pps)
{
    auto nals = scanNals(data);
    for (const auto& loc : nals) {
        QByteArray nal = data.mid(loc.nalOffset, loc.nalLen);
        int type = hevcNalType(nal);
        if (type == 32 && vps.isEmpty()) vps = nal;
        else if (type == 33 && sps.isEmpty()) sps = nal;
        else if (type == 34 && pps.isEmpty()) pps = nal;
    }
}

// ============================================================================

DataChannelRelay::DataChannelRelay(MoonlightShim* shim, QObject* parent)
    : RelayBase(parent)
    , m_Shim(shim)
{
    qInfo() << "[DataChannelRelay] Created";

    connect(m_Shim, &MoonlightShim::videoFrameReady,
            this, &DataChannelRelay::onVideoFrame);
    connect(m_Shim, &MoonlightShim::audioSampleReady,
            this, &DataChannelRelay::onAudioSample);
    connect(m_Shim, &MoonlightShim::connectionTerminated,
            this, &DataChannelRelay::onShimConnectionTerminated);

    // ICE connection timeout: emit iceTimedOut() if PC doesn't reach
    // Connected within 3s after setRemoteDescription().
    // Triggers WebSocket fallback when UDP is blocked (corporate firewall).
    m_IceCheckTimer = new QTimer(this);
    m_IceCheckTimer->setSingleShot(true);
    connect(m_IceCheckTimer, &QTimer::timeout, this, &DataChannelRelay::onIceCheckTimeout);

    // Stats timer: sends periodic stats (hostRtt, decodeLatency) to the browser.
    // Starts when Input DC opens, stops in stop().
    m_StatsTimer = new QTimer(this);
    m_StatsTimer->setInterval(2000); // 2s interval
    connect(m_StatsTimer, &QTimer::timeout, this, &DataChannelRelay::onStatsTimerTick);
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
        qInfo() << "[DataChannelRelay] Remote description set — starting ICE timeout (3s)";
        // Start ICE connection timer. The remote description is set, so ICE
        // negotiation begins now. If it doesn't reach Connected within 3s,
        // we emit iceTimedOut() for WS fallback.
        if (m_IceCheckTimer) {
            m_IceCheckTimer->start(3000);
        }
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
            qInfo() << "[DataChannelRelay] PeerConnection connected — canceling ICE timeout";
            // Cancel ICE timeout timer — connection established successfully
            if (m_IceCheckTimer) {
                m_IceCheckTimer->stop();
            }
        } else if (state == rtc::PeerConnection::State::Disconnected ||
                   state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed) {
            if (!m_Stopping.exchange(true)) {
                m_Connected = false;
                // Cancel ICE timeout timer — PC already disconnected/failed
                if (m_IceCheckTimer) {
                    m_IceCheckTimer->stop();
                }
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

            // Start periodic stats timer
            if (m_StatsTimer) {
                m_StatsTimer->start();
                qInfo() << "[DataChannelRelay] Stats timer started (2s interval)";
            }
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

    // ── DEBUG LOG: First frames detailed hex dump ──────────────────────────
    static int debugFrameNumber = 0;
    debugFrameNumber++;
    if (debugFrameNumber <= 3 || debugFrameNumber % 120 == 0) {
        qInfo() << "[DataChannelRelay] onVideoFrame #" << debugFrameNumber
                << "type=" << frameType << "size=" << data.size()
                << "dcOpen=" << (m_VideoDc && m_VideoDc->isOpen())
                << "haveBuffered=" << m_HaveBufferedKeyframe
                << "framesSent=" << m_FramesSentCount;
        if (data.size() > 0) {
            int dumpLen = qMin(48, data.size());
            QByteArray hexDump;
            for (int i = 0; i < dumpLen; i++) {
                hexDump += QString::asprintf("%02x ", (unsigned char)data[i]).toUtf8();
            }
            qInfo() << "[DataChannelRelay]   HEX:" << hexDump;
            // Log NAL unit types for debugging
            QByteArray nalTypes;
            for (int i = 0; i < qMin(data.size() - 3, 4096); i++) {
                if (data[i] == 0 && data[i+1] == 0 &&
                    (data[i+2] == 1 || (i+3 < data.size() && data[i+2] == 0 && data[i+3] == 1))) {
                    int startCodeLen = (data[i+2] == 1) ? 3 : 4;
                    if (i + startCodeLen < data.size()) {
                        unsigned char nalByte = (unsigned char)data[i + startCodeLen];
                        int nalType = (nalByte & 0x1F);
                        int hevcType = (nalByte >> 1) & 0x3F;
                        if (!nalTypes.isEmpty()) nalTypes += " ";
                        nalTypes += QString::asprintf("H264:%d/HEVC:%d@%d", nalType, hevcType, i).toUtf8();
                    }
                }
            }
            if (!nalTypes.isEmpty())
                qInfo() << "[DataChannelRelay]   NALs:" << nalTypes;
        }
    }

    // ── HEVC VPS/SPS patch for Chrome Windows black screen ────────────────
    // Only apply to the first keyframe, once per session.
    // Makes a mutable copy so we can patch VPS/SPS parameters that Chrome's
    // decoder has trouble with (high level_idc, temporal sublayers).
    QByteArray frameData = data;
    if (isKeyframe && !m_HevcPatched) {
        // Detect if this is HEVC by checking the first NAL type
        auto nals = scanNals(frameData);
        bool isHevc = false;
        for (const auto& loc : nals) {
            QByteArray nal = frameData.mid(loc.nalOffset, loc.nalLen);
            int type = hevcNalType(nal);
            if (type == 32) { // HEVC_VPS
                isHevc = true;
                break;
            }
        }

        if (isHevc) {
            // Log original (unpatched) parameter sets
            QByteArray origVps, origSps, origPps;
            extractHevcParameterSets(frameData, origVps, origSps, origPps);
            logHevcParameters(origVps, origSps, origPps, "ORIG");

            // Apply patch
            if (patchHevcKeyframe(frameData)) {
                qInfo() << "[DataChannelRelay] HEVC VPS/SPS patch applied to first keyframe";
                m_HevcPatched = true;

                // Log patched parameter sets for comparison
                QByteArray patchedVps, patchedSps, patchedPps;
                extractHevcParameterSets(frameData, patchedVps, patchedSps, patchedPps);
                logHevcParameters(patchedVps, patchedSps, patchedPps, "PATCHED");
            } else {
                qInfo() << "[DataChannelRelay] HEVC VPS/SPS: parameters already safe, no patch needed";
                m_HevcPatched = true;  // Don't re-check every keyframe
            }
        } else {
            qInfo() << "[DataChannelRelay] HEVC VPS/SPS: not HEVC, skipping patch";
            m_HevcPatched = true;  // No need to check again for H.264
        }
    }

    // Buffer keyframes arriving before the Video DC is ready.
    // Sunshine starts sending the initial IDR immediately after launch,
    // before ICE negotiation and DataChannel creation complete (~1-2s).
    // Without this buffer, the keyframe (containing SPS/PPS) is lost, the
    // browser's VideoDecoder can never configure, and we get decoder=null.
    if (isKeyframe && (!m_VideoDc || !m_VideoDc->isOpen())) {
        m_BufferedKeyframe = frameData;
        m_HaveBufferedKeyframe = true;
        m_NewKeyframeArrived = false;  // Reset — we have the latest buffer
        qInfo() << "[DataChannelRelay] BUFFERED keyframe size=" << frameData.size()
                << "(DC ready=" << (m_VideoDc && m_VideoDc->isOpen())
                << " newKeyframeArrived=false)";
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

    // If a new keyframe arrives directly while a buffered keyframe exists,
    // mark it as stale. Delta frames do NOT invalidate the buffer — they
    // are useless without a keyframe, so we must still send the buffered
    // one when sendBufferedKeyframe() fires.
    if (isKeyframe && m_HaveBufferedKeyframe) {
        m_NewKeyframeArrived = true;
    }

    sendFragmented(frameData, isKeyframe, m_VideoDc);
}

// --- Buffered keyframe ---

void DataChannelRelay::sendBufferedKeyframe()
{
    if (!m_HaveBufferedKeyframe) return;
    if (m_Stopping.load() || !m_VideoDc || !m_VideoDc->isOpen()) return;

    // Stale buffer guard: if a NEW keyframe was already sent directly
    // (from onVideoFrame on the open DC) since the buffer was stored,
    // the buffered keyframe is stale and must be dropped.
    // Delta frames alone do NOT make the buffer stale — they need a
    // keyframe to be useful.
    //
    // This race happens when Sunshine sends a second IDR before the
    // sendBufferedKeyframe() event (invokeMethod, Qt::QueuedConnection)
    // is processed by the main loop. The stale keyframe carries outdated
    // SPS/VUI parameters; configuring the browser decoder with them while
    // stripping VPS/SPS/PSS from newer frames via toAvcc() causes wrong
    // color interpretation (green image).
    if (m_NewKeyframeArrived) {
        qInfo() << "[DataChannelRelay] STALE: buffered keyframe DROPPED —"
                << "new keyframe arrived while buffer was held, discarding"
                << m_BufferedKeyframe.size() << "bytes"
                << "(framesSentCount=" << m_FramesSentCount << ")";
        // HEX dump the stale keyframe for debugging
        if (m_BufferedKeyframe.size() > 0) {
            int dumpLen = qMin(48, m_BufferedKeyframe.size());
            QByteArray hexDump;
            for (int i = 0; i < dumpLen; i++) {
                hexDump += QString::asprintf("%02x ", (unsigned char)m_BufferedKeyframe[i]).toUtf8();
            }
            qInfo() << "[DataChannelRelay]   STALE keyframe HEX:" << hexDump;
        }
        m_BufferedKeyframe.clear();
        m_HaveBufferedKeyframe = false;
        m_NewKeyframeArrived = false;
        return;
    }

    qInfo() << "[DataChannelRelay] Sending buffered keyframe, size="
            << m_BufferedKeyframe.size();
    // HEX dump the buffered keyframe before sending
    if (m_BufferedKeyframe.size() > 0) {
        int dumpLen = qMin(48, m_BufferedKeyframe.size());
        QByteArray hexDump;
        for (int i = 0; i < dumpLen; i++) {
            hexDump += QString::asprintf("%02x ", (unsigned char)m_BufferedKeyframe[i]).toUtf8();
        }
        qInfo() << "[DataChannelRelay]   BUFFERED keyframe HEX:" << hexDump;
    }
    sendFragmented(m_BufferedKeyframe, true, m_VideoDc);
    m_BufferedKeyframe.clear();
    m_HaveBufferedKeyframe = false;
    m_NewKeyframeArrived = false;
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
        // Absolute mouse position (non-gaming mode)
        if (msg.contains("x") && msg.contains("y") &&
            msg.contains("referenceWidth") && msg.contains("referenceHeight")) {
            short x = static_cast<short>(msg["x"].toInt(0));
            short y = static_cast<short>(msg["y"].toInt(0));
            short refW = static_cast<short>(msg["referenceWidth"].toInt(0));
            short refH = static_cast<short>(msg["referenceHeight"].toInt(0));
            m_Shim->sendMousePosition(x, y, refW, refH);
        } else {
            // Legacy / gaming mode: relative mouse movement
            short dx = static_cast<short>(msg["dx"].toInt(0));
            short dy = static_cast<short>(msg["dy"].toInt(0));
            m_Shim->sendMouseMove(dx, dy);
        }
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
    else if (type == "ping") {
        // Respond with pong on the input DataChannel.
        // The ts field mirrors the browser's timestamp so it can compute RTT.
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
                    qWarning() << "[DataChannelRelay] Pong send error:" << e.what();
                }
            }
        }
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

    // Track decode pipeline latency: time from frameSubmitUs (set in
    // MoonlightShim::drSubmitDecodeUnit) to actual send over WebRTC.
    // This captures buffer concatenation + signal queuing + fragmentation overhead.
    if (m_Shim) {
        int64_t submitTs = m_Shim->frameSubmitTimeUs();
        if (submitTs > 0) {
            int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t latency = nowUs - submitTs;
            if (latency > 0 && latency < 1'000'000) { // Cap at 1s
                m_LastDecodeLatencyUs = latency;
            }
        }
    }

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

    // Log data being sent for first keyframes (debug)
    if (isKeyframe && frameId <= 2) {
        QByteArray hexDump;
        int dumpLen = qMin(48, totalSize);
        for (int i = 0; i < dumpLen; i++) {
            hexDump += QString::asprintf("%02x ", (unsigned char)data[i]).toUtf8();
        }
        qInfo() << "[DataChannelRelay] sendFragmented keyframe frameId=" << frameId
                << "totalSize=" << totalSize << "chunks=" << totalChunks
                << "firstBytes:" << hexDump;
    }

    // ── End-to-end latency timestamp ───────────────────────────────────────
    // Send the frame's capture time in steady_clock domain (monotonic ms).
    //   captureSteadyMs = (firstFrameArrivalTimeUs + presentationTimeUs) / 1000
    // The frontend estimates current steady_clock time from periodic stats
    // (streamSteadyMs + performance.now() delta) and subtracts captureSteadyMs.
    // Both steady_clock and performance.now() are monotonic — the delta works.
    uint32_t backendTs = 0;
    if (m_Shim) {
        int64_t firstArrivalSteadyMs = m_Shim->firstFrameArrivalSteadyMs();
        int64_t presTimeUs = m_Shim->framePresentationTimeUs();
        if (firstArrivalSteadyMs > 0 && presTimeUs >= 0) {
            int64_t captureSteadyMs = firstArrivalSteadyMs + (presTimeUs / 1000);
            backendTs = static_cast<uint32_t>(captureSteadyMs & 0xFFFFFFFF);
        }
    }
    // Fallback: use current wall time if steady_clock not yet initialized
    if (backendTs == 0) {
        backendTs = static_cast<uint32_t>(
            QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);
    }

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
    m_FramesSentCount++;
    if (m_FrameCount <= 3 || m_FrameCount % 300 == 0) {
        qInfo() << "[DataChannelRelay] Sent frame #" << m_FrameCount
                << "totalSize=" << totalSize << "chunks=" << totalChunks
                << "isKeyframe=" << isKeyframe << "frameId=" << frameId;
    }
}

// --- Stats timer (2s interval) ---

void DataChannelRelay::onStatsTimerTick()
{
    if (m_Stopping.load() || !m_Connected) return;
    if (!m_InputDc || !m_InputDc->isOpen()) return;

    double hostRttMs = 0.0;
    int64_t decodeLatUs = m_LastDecodeLatencyUs.load(std::memory_order_acquire);

    if (m_Shim) {
        hostRttMs = m_Shim->hostRttMs();
    }

    QJsonObject stats;
    stats["type"] = "stats";
    stats["hostRttMs"] = hostRttMs;
    stats["decodeLatencyUs"] = decodeLatUs;

    // Send steady_clock reference for end-to-end latency calculation.
    // The frontend uses this + performance.now() delta to estimate current
    // steady time, then subtracts the frame's captureSteadyMs to get e2e latency.
    {
        using namespace std::chrono;
        int64_t nowMs = duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        stats["streamTimeMs"] = nowMs;
    }

    QByteArray statsJson = QJsonDocument(stats).toJson(QJsonDocument::Compact);

    try {
        m_InputDc->send(std::string(statsJson.constData(), statsJson.size()));
    } catch (const std::exception& e) {
        if (!m_Stopping.load()) {
            qWarning() << "[DataChannelRelay] Stats send error:" << e.what();
        }
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

    // Stop ICE timeout timer
    if (m_IceCheckTimer) {
        m_IceCheckTimer->stop();
    }

    // Stop stats timer
    if (m_StatsTimer) {
        m_StatsTimer->stop();
    }

    // Reset backpressure counters for next session
    m_DeltaDroppedCount = 0;
    m_KeyframeBackpressureWarnings = 0;
    m_LastDecodeLatencyUs.store(0, std::memory_order_release);

    // Clear buffered keyframe (if any)
    m_BufferedKeyframe.clear();
    m_HaveBufferedKeyframe = false;
    m_NewKeyframeArrived = false;

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

void DataChannelRelay::onIceCheckTimeout()
{
    if (m_Stopping.load()) return;
    if (m_Connected) return; // Safety: should not happen since we cancel on Connected

    qWarning() << "[DataChannelRelay] ICE timeout — PC did not reach Connected within 3s."
               << "This likely indicates UDP is blocked (corporate firewall)."
               << "Emitting iceTimedOut() for WebSocket fallback.";

    emit iceTimedOut();

    // Also emit sessionEnded() so the auto fallback chain can progress to the
    // next transport (webrtc-dc-tcp, then wss).
    // Guard: only emit if m_Stopping wasn't already set by SignalingServer's
    // onRelayIceTimedOut -> startWsFallback() in non-auto mode.
    if (!m_Stopping.exchange(true)) {
        emit sessionEnded();
    }
}

void DataChannelRelay::setPublicAddress(const std::string& publicIP, uint16_t publicPort)
{
    m_PublicIP = publicIP;
    m_PublicPort = publicPort;
    qInfo() << "[DataChannelRelay] UPnP public address set:"
            << QString::fromStdString(publicIP) << ":" << publicPort;
}
