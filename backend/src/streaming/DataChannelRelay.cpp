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

#include "DataChannelRelay.h"
#include "ClipboardBridge.h"
#include "MoonlightShim.h"

extern "C" {
#include "Limelight.h"
}

#include <rtc/rtc.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QThread>
#include <QDebug>
#include <QDateTime>
#include <QMap>
#include <QVector>
#include <mutex>
#include <chrono>
#include <random>
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
static QByteArray removeHvcEp(const QByteArray& data)
{
    QByteArray result;
    result.reserve(data.size());
    for (int i = 0; i < data.size(); i++) {
        if (i + 2 < data.size() && static_cast<unsigned char>(data[i]) == 0x00 &&
            static_cast<unsigned char>(data[i + 1]) == 0x00 &&
            static_cast<unsigned char>(data[i + 2]) == 0x03) {
            result.append('\x00');
            result.append('\x00');
            i += 2; // skip the 0x03 — for loop increments past it
        } else {
            result.append(data[i]);
        }
    }
    return result;
}

/// Re-insert HEVC emulation prevention bytes (00 00 03) after modifying RBSP.
/// Inserts 0x03 after any 00 00 pair followed by 00, 01, 02, or 03.
static QByteArray addHvcEp(const QByteArray& rbsp)
{
    QByteArray result;
    result.reserve(rbsp.size() + 16);
    int zeroRun = 0;
    for (char i : rbsp) {
        unsigned char b = static_cast<unsigned char>(i);
        if (zeroRun >= 2 && b <= 0x03) {
            result.append('\x03');
            zeroRun = 0;
        }
        result.append(i);
        if (b == 0x00)
            zeroRun++;
        else
            zeroRun = 0;
    }
    return result;
}

/// Represents one NAL unit found in an Annex B byte stream.
struct NalLocation
{
    int startOffset; // offset of start code in the data
    int startLen;    // start code length (3 or 4)
    int nalOffset;   // offset of NAL data (after start code)
    int nalLen;      // length of NAL data
};

/// Scan an Annex B byte stream and return all NAL unit locations.
/// Assumes valid Annex B structure (may be corrupt for invalid streams).
static QVector<NalLocation> scanNals(const QByteArray& data)
{
    QVector<NalLocation> nals;
    int i = 0;
    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    while (i < data.size() - 3) {
        if (d[i] == 0 && d[i + 1] == 0) {
            int scLen = 0;
            if (d[i + 2] == 1)
                scLen = 3;
            else if (i + 3 < data.size() && d[i + 2] == 0 && d[i + 3] == 1)
                scLen = 4;
            if (scLen) {
                NalLocation loc;
                loc.startOffset = i;
                loc.startLen = scLen;
                loc.nalOffset = i + scLen;
                // End of this NAL = next start code or end of buffer
                int end = data.size();
                for (int j = i + scLen; j < data.size() - 3; j++) {
                    if (d[j] == 0 && d[j + 1] == 0 &&
                        (d[j + 2] == 1 ||
                         (j + 3 < data.size() && d[j + 2] == 0 && d[j + 3] == 1))) {
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
static int hevcNalType(const QByteArray& nal)
{
    if (nal.size() < 2) return -1;
    return (static_cast<unsigned char>(nal[0]) >> 1) & 0x3F;
}

/// Format a hex dump string from up to maxLen bytes of a QByteArray.
static QString hevcHexDump(const QByteArray& data, int maxLen = 64)
{
    QByteArray hex;
    int len = qMin(maxLen, data.size());
    for (int i = 0; i < len; i++) {
        hex += QString::asprintf("%02x ", (unsigned char)data[i]).toUtf8();
    }
    return hex;
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
    if (cleanVps.size() < 18) {
        qWarning() << "[HEVC-VPS-REBUILD] VPS too small:" << cleanVps.size() << "bytes (need 18)";
        return {};
    }

    const unsigned char* vps = reinterpret_cast<const unsigned char*>(cleanVps.constData());

    // RBSP byte 1 (= clean[3]) bits 3-1 = vps_max_sub_layers_minus1
    int origSubLayers = (vps[3] >> 1) & 0x07;
    if (origSubLayers == 0) {
        qInfo() << "[HEVC-VPS-REBUILD] VPS already has max_sub_layers=0, skipping";
        return {};
    }

    qInfo() << "[HEVC-VPS-REBUILD] Input VPS:" << cleanVps.size() << "bytes"
            << "RBSP[0..8]=" << hevcHexDump(cleanVps, 9) << "subLayers=" << origSubLayers << "→ 0";

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

    qInfo() << "[HEVC-VPS-REBUILD] Output VPS:" << cleanVps.size() << "→" << result.size()
            << "bytes"
            << "RBSP[0..8]=" << hevcHexDump(result, 9) << "(sub_layers forced to 0)";
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
    if (cleanSps.size() < 17) {
        qWarning() << "[HEVC-SPS-REBUILD] SPS too small:" << cleanSps.size() << "bytes (need 17)";
        return {};
    }

    const unsigned char* sps = reinterpret_cast<const unsigned char*>(cleanSps.constData());

    // RBSP byte 0 (= clean[2]) bits 3-1 = sps_max_sub_layers_minus1
    int origSubLayers = (sps[2] >> 1) & 0x07;
    if (origSubLayers == 0) {
        qInfo() << "[HEVC-SPS-REBUILD] SPS already has max_sub_layers=0, skipping";
        return {};
    }

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
            levelPresent = (flagsByte0 >> (6 - j * 2)) & 1;
        } else {
            // Byte 1: bit 7=p[4],6=l[4],5=p[5],4=l[5],3..0=reserved
            profilePresent = (flagsByte1 >> (15 - j * 2)) & 1;
            levelPresent = (flagsByte1 >> (14 - j * 2)) & 1;
        }
        if (profilePresent) subLayerDataBytes += 12;
        if (levelPresent) subLayerDataBytes += 1;
    }

    // Original PTL end offset in cleanSps (index past the PTL)
    int ptlEnd = 15 + 2 + subLayerDataBytes;
    if (ptlEnd > cleanSps.size()) ptlEnd = cleanSps.size();

    qInfo() << "[HEVC-SPS-REBUILD] Input SPS:" << cleanSps.size() << "bytes"
            << "RBSP[0..8]=" << hevcHexDump(cleanSps, 9) << "subLayers=" << origSubLayers << "→ 0"
            << "subLayerDataBytes=" << subLayerDataBytes << "ptlEnd=" << ptlEnd << "flagsB0=0x"
            << QString::number(flagsByte0, 16) << "flagsB1=0x" << QString::number(flagsByte1, 16);

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

    qInfo() << "[HEVC-SPS-REBUILD] Output SPS:" << cleanSps.size() << "→" << result.size()
            << "bytes"
            << "RBSP[0..8]=" << hevcHexDump(result, 9) << "(sub_layers forced to 0)";
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

    qInfo() << "[HEVC-PATCH] NAL scan found" << nals.size() << "NALs in keyframe"
            << "(total frame size=" << data.size() << "bytes)";

    // Log NAL types and locations
    for (int i = 0; i < nals.size() && i < 20; i++) {
        const auto& loc = nals[i];
        QByteArray nal = data.mid(loc.nalOffset, qMin(loc.nalLen, 8));
        int type = hevcNalType(nal);
        qInfo() << "[HEVC-PATCH]   NAL[" << i << "]"
                << "type=" << type << "offset=" << loc.nalOffset << "len=" << loc.nalLen
                << "startCode=" << loc.startLen << "headerHex=" << hevcHexDump(nal, 4);
    }

    int vpsIdx = -1, spsIdx = -1;
    for (int i = 0; i < nals.size(); i++) {
        QByteArray nal = data.mid(nals[i].nalOffset, nals[i].nalLen);
        int type = hevcNalType(nal);
        if (type == 32)
            vpsIdx = i; // HEVC_VPS
        else if (type == 33)
            spsIdx = i; // HEVC_SPS
    }

    qInfo() << "[HEVC-PATCH] VPS at NAL[" << vpsIdx << "], SPS at NAL[" << spsIdx << "]";

    bool patched = false;

    // ── VPS: rebuild with max_sub_layers=0 ─────────────────────────────────
    if (vpsIdx >= 0) {
        NalLocation& vpsLoc = nals[vpsIdx];
        QByteArray vpsNal = data.mid(vpsLoc.nalOffset, vpsLoc.nalLen);
        QByteArray clean = removeHvcEp(vpsNal);
        qInfo() << "[HEVC-PATCH] VPS: original NAL size=" << vpsLoc.nalLen
                << "clean size=" << clean.size() << "clean[0..8]=" << hevcHexDump(clean, 9);
        QByteArray rebuilt = rebuildVpsNoSubLayers(clean);
        if (!rebuilt.isEmpty()) {
            QByteArray patchedVps = addHvcEp(rebuilt);
            qInfo() << "[HEVC-PATCH] VPS: REPLACED" << vpsLoc.nalLen << "→" << patchedVps.size()
                    << "bytes"
                    << "patched[0..8]=" << hevcHexDump(patchedVps, 9);
            data.replace(vpsLoc.nalOffset, vpsLoc.nalLen, patchedVps);
            patched = true;
        } else {
            qInfo() << "[HEVC-PATCH] VPS: no rebuild needed (sub_layers already 0 or error)";
        }
    } else {
        qWarning() << "[HEVC-PATCH] VPS NOT FOUND in keyframe — NAL type 32 missing!";
    }

    // ── SPS: rebuild with max_sub_layers=0 + cap level_idc ────────────────
    if (spsIdx >= 0) {
        NalLocation& spsLoc = nals[spsIdx];
        QByteArray spsNal = data.mid(spsLoc.nalOffset, spsLoc.nalLen);
        QByteArray clean = removeHvcEp(spsNal);
        qInfo() << "[HEVC-PATCH] SPS: original NAL size=" << spsLoc.nalLen
                << "clean size=" << clean.size() << "clean[0..8]=" << hevcHexDump(clean, 9);
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
            qInfo() << "[HEVC-PATCH] SPS: REPLACED" << spsLoc.nalLen << "→" << patchedSps.size()
                    << "bytes"
                    << "patched[0..8]=" << hevcHexDump(patchedSps, 9);
            data.replace(spsLoc.nalOffset, spsLoc.nalLen, patchedSps);
            patched = true;
        } else {
            qInfo() << "[HEVC-PATCH] SPS: no rebuild needed (sub_layers already 0 or error)";
        }
    } else {
        qWarning() << "[HEVC-PATCH] SPS NOT FOUND in keyframe — NAL type 33 missing!";
    }

    if (patched) {
        qInfo() << "[HEVC-PATCH] Final frame size:" << data.size() << "bytes";
    }

    return patched;
}

// ============================================================================

DataChannelRelay::DataChannelRelay(MoonlightShim* shim, QObject* parent)
    : RelayBase(parent)
    , m_Shim(shim)
{
    qInfo() << "[DataChannelRelay] Created";

    // Dedicated sender thread: fragmentation + dc->send() run off the main thread.
    m_Sender = std::make_unique<FrameSender>();

    connect(m_Shim, &MoonlightShim::videoFrameReady, this, &DataChannelRelay::onVideoFrame);
    connect(m_Shim, &MoonlightShim::audioSampleReady, this, &DataChannelRelay::onAudioSample);
    connect(m_Shim, &MoonlightShim::connectionTerminated, this,
            &DataChannelRelay::onShimConnectionTerminated);

    // Forward host rumble requests to the browser over the input DC.
    // 'this' as context → runs on the relay thread (signal is emitted from the
    // moonlight worker thread).
    connect(m_Shim, &MoonlightShim::rumble, this, [this](int controller, int low, int high) {
        if (m_Stopping.load() || !m_InputDc) return;
        QJsonObject m;
        m["type"] = "rumble";
        m["index"] = controller;
        m["low"] = low;
        m["high"] = high;
        QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
        try {
            m_InputDc->send(std::string(j.constData(), j.size()));
        } catch (const std::exception&) {}
    });

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

void DataChannelRelay::setClipboardEnabled(bool enabled)
{
    m_ClipboardEnabled = enabled;
    if (!enabled) return;
    // Host clipboard changes → push to the browser over the input DC.
    // Context 'this': the slot runs on the relay's (future) thread; the DC
    // send itself is thread-safe. Auto-disconnected when the relay dies.
    connect(ClipboardBridge::instance(), &ClipboardBridge::hostTextChanged, this,
            [this](const QString& text) {
                if (m_Stopping.load() || !m_Connected || !m_InputDc) return;
                QJsonObject m;
                m["type"] = "clipboard";
                m["text"] = text;
                QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
                try {
                    m_InputDc->send(std::string(j.constData(), j.size()));
                } catch (const std::exception&) {}
            });
}

DataChannelRelay::~DataChannelRelay()
{
    qInfo() << "[DataChannelRelay] Destructor";
    // Static call: dynamic dispatch is meaningless in a destructor.
    DataChannelRelay::stop();
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
                // For a client on our own LAN (incl. one reaching us through the
                // public URL via NAT hairpin), also advertise the original
                // private host candidate so it can connect directly to
                // 192.168.x.x — many routers don't hairpin UDP, so a public-only
                // candidate never becomes reachable locally. Gated on
                // m_EmitLanCandidate (false for internet clients) so the private
                // IP is never leaked outside the LAN.
                if (m_EmitLanCandidate)
                    emit signalingIceCandidate(candStr, std::string(candidate.mid()));
                try {
                    modCandidate.changeAddress(m_PublicIP, m_PublicPort);
                    qInfo() << "[DataChannelRelay] Host candidate ->"
                            << QString::fromStdString(m_PublicIP) << ":" << m_PublicPort
                            << (m_EmitLanCandidate ? "(+ LAN)" : "");
                } catch (const std::exception& e) {
                    qWarning() << "[DataChannelRelay] Failed to rewrite candidate:" << e.what();
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
            if (space != std::string::npos && candStr.find(':', space + 1) != std::string::npos) {
                qInfo() << "[DataChannelRelay] Suppressing IPv6 candidate (UPnP active):"
                        << QString::fromStdString(candStr).left(80);
                return; // Skip — don't emit this candidate
            }
        }

        emit signalingIceCandidate(std::string(modCandidate.candidate()),
                                   std::string(modCandidate.mid()));
    });

    // --- State change callback ---
    m_Pc->onStateChange([this](rtc::PeerConnection::State state) {
        // This callback runs on a libdatachannel thread. QTimer is thread-affine
        // and signal-driven teardown must happen on the Qt main thread, so
        // marshal everything that touches Qt objects (avoids the
        // "Timers cannot be stopped from another thread" UB that corrupts the
        // event dispatcher and crashes later during timer processing).
        qInfo() << "[DataChannelRelay] PC state changed to" << static_cast<int>(state);
        if (state == rtc::PeerConnection::State::Connected) {
            qInfo() << "[DataChannelRelay] PeerConnection connected — canceling ICE timeout";
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    if (m_IceCheckTimer) m_IceCheckTimer->stop();
                },
                Qt::QueuedConnection);
        } else if (state == rtc::PeerConnection::State::Disconnected ||
                   state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed) {
            if (!m_Stopping.exchange(true)) {
                m_Connected = false;
                qInfo() << "[DataChannelRelay] PC disconnected/failed/closed";
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        if (m_IceCheckTimer) m_IceCheckTimer->stop();
                        emit sessionEnded();
                    },
                    Qt::QueuedConnection);
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
    // Ordered + partial reliability (3 retransmits): an HEVC keyframe is
    // ~11 chunks ≈ 140 UDP packets, so with 0 retransmits a single packet
    // loss kills the whole frame and forces an IDR recovery cycle.
    //
    // Ordered is required for video: frames reference their predecessor, so
    // delivery order IS decode order. With unordered delivery, a retransmitted
    // chunk made frame N complete AFTER frame N+1 — the frontend saw a frameId
    // gap (false loss), invalidated the reference and requested an IDR on every
    // reorder. Ordered lets SCTP hold N+1 the ~RTT the retransmit takes; frames
    // abandoned after 3 retransmits are skipped via FORWARD-TSN and surface as
    // a real gap.
    rtc::DataChannelInit videoConfig;
    videoConfig.reliability.unordered = false;
    videoConfig.reliability.maxRetransmits = 3; // Must match frontend negotiated channel config
    videoConfig.negotiated = true;
    videoConfig.id = 0;

    m_VideoDc = m_Pc->createDataChannel("video", videoConfig);
    if (m_VideoDc) {
        m_VideoDc->onOpen([this]() {
            qInfo() << "[DataChannelRelay] Video DataChannel open";
            // If a keyframe arrived before the DC was ready, send it now.
            // Must marshal to main thread because sendFragmented() may access
            // Qt objects owned by the main thread.
            QMetaObject::invokeMethod(
                this, [this]() { sendBufferedKeyframe(); }, Qt::QueuedConnection);
        });
        m_VideoDc->onClosed([this]() { qInfo() << "[DataChannelRelay] Video DataChannel closed"; });
    }

    // --- Audio track (server->browser, Opus over RTP) ---
    // Native RTP Opus track on the SAME PeerConnection as the video DataChannel.
    // The browser decodes Opus with its own jitter buffer + in-band FEC + PLC, so
    // a lost UDP packet is concealed instead of head-of-line-blocking the audio
    // (the old ordered DataChannel caused periodic ~0.5s dropouts on packet loss).
    // useinbandfec=1 tells the decoder to use the FEC carried in the next packet.
    {
        auto audioDesc = rtc::Description::Audio("audio", rtc::Description::Direction::SendOnly);
        audioDesc.addOpusCodec(111, "minptime=10;useinbandfec=1");

        m_AudioTrack = m_Pc->addTrack(audioDesc);
        if (m_AudioTrack) {
            std::random_device rd;
            uint32_t ssrc = static_cast<uint32_t>(rd());

            auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc, "audio", 111, rtc::OpusRtpPacketizer::DefaultClockRate);
            auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(64));
            m_AudioTrack->setMediaHandler(packetizer);

            m_AudioTrack->onOpen([this]() { qInfo() << "[DataChannelRelay] Audio Track open"; });
            m_AudioTrack->onClosed(
                [this]() { qInfo() << "[DataChannelRelay] Audio Track closed"; });
            qInfo() << "[DataChannelRelay] Audio track created (Opus, PT=111)";
        } else {
            qWarning() << "[DataChannelRelay] Failed to create audio track";
        }
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

            if (m_ClipboardEnabled) {
                // Advertise clipboard sync, then push the current host
                // clipboard once so copy-before-connect pastes locally.
                static const char kCaps[] = "{\"type\":\"clipboardcaps\",\"available\":true}";
                try {
                    m_InputDc->send(std::string(kCaps));
                } catch (const std::exception&) {}
                ClipboardBridge::instance()->requestAnnounce();
            }

            // Start periodic stats timer — marshal to the Qt main thread:
            // this callback runs on a libdatachannel thread and QTimer::start()
            // is thread-affine (silently fails otherwise).
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    if (m_StatsTimer && !m_Stopping.load()) {
                        m_StatsTimer->start();
                        qInfo() << "[DataChannelRelay] Stats timer started (2s interval)";
                    }
                },
                Qt::QueuedConnection);
        });
        m_InputDc->onClosed([this]() { qInfo() << "[DataChannelRelay] Input DataChannel closed"; });

        // Input messages arrive from browser on this channel
        m_InputDc->onMessage([this](const std::variant<rtc::binary, rtc::string>& msg) {
            // Marshal to main thread for Qt signal safety
            if (std::holds_alternative<rtc::string>(msg)) {
                std::string text = std::get<rtc::string>(msg);
                QMetaObject::invokeMethod(
                    this, [this, text]() { onInputMessage(text); }, Qt::QueuedConnection);
            }
        });
    }

    qInfo() << "[DataChannelRelay] DataChannels created (video=0, audio=1, input=2)";
}

// --- Video/Audio forwarding (from MoonlightShim signals, on main thread) ---

void DataChannelRelay::onVideoFrame(const QByteArray& data, int frameType, int /*frameNumber*/,
                                    qint64 presentationTimeUs)
{
    // Balance the worker→main pending counter (incremented before each emit).
    // m_Shim lifetime is guaranteed: the shim is Qt-parented to this relay
    // (Session.cpp), so it cannot be destroyed while this slot runs.
    if (m_Shim) {
        m_Shim->videoFrameDelivered();
        // Worker dropped deltas due to main-thread backlog — enter awaiting-IDR
        // recovery (guards inside are no-ops when stopping).
        if (m_Shim->takeWorkerDroppedDelta()) {
            m_AwaitingIdr = true;
            sendIdrRequestThrottled();
        }
    }

    if (m_Stopping.load()) {
        static int dropCount = 0;
        if (++dropCount <= 3)
            qInfo() << "[DataChannelRelay] onVideoFrame dropped — m_Stopping=true";
        return;
    }

    bool isKeyframe = (frameType == 1);

    // HEVC VPS/SPS patch for Chrome Windows black screen.
    // Patch the first keyframe once per session: Chrome's HEVC decoder chokes
    // on some VPS/SPS parameters (high level_idc, temporal sublayers). H.264
    // needs no patch. Work on a mutable copy.
    QByteArray frameData = data;
    if (isKeyframe && !m_HevcPatched) {
        // Detect HEVC via the presence of a VPS NAL (type 32).
        bool isHevc = false;
        for (const auto& loc : scanNals(frameData)) {
            if (hevcNalType(frameData.mid(loc.nalOffset, qMin(loc.nalLen, 16))) == 32) {
                isHevc = true;
                break;
            }
        }
        if (isHevc) {
            patchHevcKeyframe(frameData);
        }
        m_HevcPatched = true; // Only the first keyframe is checked
    }

    // Buffer keyframes arriving before the Video DC is ready.
    // Without this buffer, the keyframe (containing SPS/PPS) is lost, the
    // browser's VideoDecoder can never configure, and we get decoder=null.
    if (isKeyframe && (!m_VideoDc || !m_VideoDc->isOpen())) {
        m_BufferedKeyframe = frameData;
        m_BufferedKeyframePresUs = presentationTimeUs;
        m_HaveBufferedKeyframe = true;
        m_NewKeyframeArrived = false; // Reset — we have the latest buffer
        return;
    }

    // Drop non-keyframes before DC is ready; flag awaiting IDR so we skip
    // deltas until the decoder has a reference frame.
    if (!m_VideoDc) {
        if (!isKeyframe) {
            m_AwaitingIdr = true;
            sendIdrRequestThrottled();
        }
        static int noDcCount = 0;
        if (++noDcCount <= 5)
            qInfo() << "[DataChannelRelay] onVideoFrame dropped — m_VideoDc is null (DCs not "
                       "created yet?)";
        return;
    }
    if (!m_VideoDc->isOpen()) {
        if (!isKeyframe) {
            m_AwaitingIdr = true;
            sendIdrRequestThrottled();
        }
        return; // DC exists but not yet open
    }

    // Awaiting IDR: drop all deltas until a keyframe resets the decoder reference.
    if (m_AwaitingIdr && !isKeyframe) {
        sendIdrRequestThrottled(); // Throttle absorbs bursts; keeps requesting until IDR arrives
        return;
    }

    // If a new keyframe arrives directly while a buffered keyframe exists,
    // mark it as stale. Delta frames do NOT invalidate the buffer — they
    // are useless without a keyframe, so we must still send the buffered
    // one when sendBufferedKeyframe() fires.
    if (isKeyframe && m_HaveBufferedKeyframe) {
        m_NewKeyframeArrived = true;
    }

    sendFragmented(frameData, isKeyframe, m_VideoDc, presentationTimeUs);
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
                << m_BufferedKeyframe.size() << "bytes";
        m_BufferedKeyframe.clear();
        m_HaveBufferedKeyframe = false;
        m_NewKeyframeArrived = false;
        return;
    }

    qInfo() << "[DataChannelRelay] Sending buffered keyframe, size=" << m_BufferedKeyframe.size();
    sendFragmented(m_BufferedKeyframe, true, m_VideoDc, m_BufferedKeyframePresUs);
    m_BufferedKeyframe.clear();
    m_BufferedKeyframePresUs = -1;
    m_HaveBufferedKeyframe = false;
    m_NewKeyframeArrived = false;
}

// --- Audio forwarding ---

void DataChannelRelay::onAudioSample(const QByteArray& data)
{
    if (m_Stopping.load()) return;

    // Serialize against track teardown in stop().
    std::lock_guard<std::mutex> lk(m_AudioMutex);
    if (m_Stopping.load() || !m_AudioTrack || !m_AudioTrack->isOpen()) {
        static int notReady = 0;
        if (++notReady <= 3)
            qInfo() << "[DataChannelRelay] onAudioSample dropped — audio track not ready";
        return;
    }

    // Send the Opus packet as one RTP frame; the OpusRtpPacketizer wraps it.
    auto frameInfo = std::make_shared<rtc::FrameInfo>(m_AudioRtpTs);
    // Advance by the negotiated Opus frame size (48 kHz clock): one clean tick per
    // packet. A jittery arrival-time clock makes NetEq time-stretch → robotic audio.
    m_AudioRtpTs += static_cast<uint32_t>(m_Shim ? m_Shim->audioSamplesPerFrame() : 240);

    rtc::binary bin(static_cast<size_t>(data.size()));
    if (data.size() > 0)
        std::memcpy(bin.data(), data.constData(), static_cast<size_t>(data.size()));

    try {
        m_AudioTrack->sendFrame(std::move(bin), *frameInfo);
    } catch (const std::exception& e) {
        if (!m_Stopping.load())
            qWarning() << "[DataChannelRelay] audio sendFrame error:" << e.what();
    }
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

    if (type == "keydown" || type == "keyup") {
        bool down = (type == "keydown");
        int vk = msg["keyCode"].toInt(0);
        QString code = msg["code"].toString();
        char mods = 0;
        if (msg["ctrlKey"].toBool(false)) mods |= 0x02;
        if (msg["shiftKey"].toBool(false)) mods |= 0x01;
        if (msg["altKey"].toBool(false)) mods |= 0x04;
        if (msg["metaKey"].toBool(false)) mods |= 0x08;

        short keyCode;
        char flags = 0;

        // International keys without standard US VK equivalents:
        // IntlBackslash (ISO key next to left Shift) and IntlRo (JIS \ key)
        // need NON_NORMALIZED mode: Sunshine injects the keyCode as a raw VK
        // (not a US-layout scancode), so the host's active layout resolves it.
        if (code == "IntlBackslash") {
            keyCode = 0xE2; // VK_OEM_102 (ISO <> key)
            flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else if (code == "IntlRo") {
            keyCode = 0xC1; // VK_ABNT_C1 (JIS Ro key)
            flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else {
            keyCode = static_cast<short>(vk);
            flags = 0;
        }
        m_Shim->sendKeyEvent(keyCode, down, mods, flags);
    } else if (type == "mousemove") {
        // Absolute mouse position (non-gaming mode)
        if (msg.contains("x") && msg.contains("y") && msg.contains("referenceWidth") &&
            msg.contains("referenceHeight")) {
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
    } else if (type == "mousedown" || type == "mouseup") {
        bool down = (type == "mousedown");
        int button = msg["button"].toInt(1);
        m_Shim->sendMouseButton(down, button);
    } else if (type == "mousewheel") {
        short delta = static_cast<short>(msg["delta"].toInt(0));
        m_Shim->sendMouseScroll(delta);
    } else if (type == "textinput") {
        // Virtual/soft keyboard text (UTF-8) — forwarded as a text event.
        m_Shim->sendUtf8Text(msg["text"].toString());
    } else if (type == "clipboardpaste") {
        // Browser Ctrl/Cmd+V: commit the client text to the host clipboard,
        // then inject the paste chord (main-thread hop keeps that order).
        // Only meaningful when the streamed host is this machine.
        if (m_ClipboardEnabled) {
            ClipboardBridge::instance()->pasteFromClient(m_Shim, msg["text"].toString(),
                                                         msg["injectCtrl"].toBool(false));
        }
    } else if (type == "requestidr") {
        qInfo() << "[DataChannelRelay] Requesting IDR frame from Sunshine (browser request)";
        m_AwaitingIdr = true;
        sendIdrRequestThrottled();
    } else if (type == "ping") {
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
    } else if (type == "gamepad") {
        // Full controller state snapshot from the browser Gamepad API.
        m_Shim->sendControllerState(
            static_cast<short>(msg["index"].toInt(0)), static_cast<short>(msg["mask"].toInt(0)),
            msg["buttons"].toInt(0), static_cast<unsigned char>(msg["lt"].toInt(0)),
            static_cast<unsigned char>(msg["rt"].toInt(0)), static_cast<short>(msg["lx"].toInt(0)),
            static_cast<short>(msg["ly"].toInt(0)), static_cast<short>(msg["rx"].toInt(0)),
            static_cast<short>(msg["ry"].toInt(0)));
    } else if (type == "gamepadconnect") {
        m_Shim->sendControllerArrival(static_cast<uint8_t>(msg["index"].toInt(0)),
                                      static_cast<uint16_t>(msg["mask"].toInt(0)),
                                      static_cast<uint8_t>(msg["ctype"].toInt(0)),
                                      msg["rumble"].toBool(false));
    } else if (type == "gamepaddisconnect") {
        // Empty state with this controller's mask bit cleared = removal.
        m_Shim->sendControllerState(static_cast<short>(msg["index"].toInt(0)),
                                    static_cast<short>(msg["mask"].toInt(0)), 0, 0, 0, 0, 0, 0, 0);
    } else {
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
                                      std::shared_ptr<rtc::DataChannel>& dc,
                                      qint64 presentationTimeUs)
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
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
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
            m_BackpressureDropCount++;

            // Set sticky awaiting state and request IDR (throttled — absorbs bursts).
            m_AwaitingIdr = true;
            sendIdrRequestThrottled();

            if (m_DeltaDroppedCount <= 3 || m_DeltaDroppedCount % 120 == 0) {
                qInfo() << "[DataChannelRelay] Dropped delta frame (SCTP full)"
                        << "bufferedAmount=" << bufAmt << "totalDropped=" << m_DeltaDroppedCount;
            }
            return;
        }
    } else {
        // Keyframe backpressure ceiling. Keyframes used to bypass backpressure
        // entirely, but on a slow link the IDR-recovery loop (deltas dropped →
        // IDR requested → keyframe → always sent) stacks keyframes faster than
        // the link drains them, growing the SCTP buffer to MBs and adding many
        // seconds of latency (the "keyframe slideshow 10s behind" symptom).
        //
        // If the buffer is still above the watermark, a previous keyframe/frames
        // have not drained yet: drop this keyframe too and keep awaiting IDR.
        // m_AwaitingIdr stays sticky and the throttled IDR request keeps firing,
        // so the moment the buffer drains below the watermark the NEXT keyframe
        // goes through fresh. This caps the buffer at ~watermark + one keyframe,
        // bounding latency to well under a second instead of letting it run away.
        size_t bufAmt = dc->bufferedAmount();
        if (bufAmt > kHighWatermark) {
            m_KeyframeBackpressureWarnings++;
            if (m_KeyframeBackpressureWarnings <= 5) {
                qInfo() << "[DataChannelRelay] Dropped keyframe (SCTP buffer not drained)"
                        << "bufferedAmount=" << bufAmt
                        << "warnCount=" << m_KeyframeBackpressureWarnings;
            }
            m_AwaitingIdr = true;
            sendIdrRequestThrottled();
            return;
        }
        // Keyframe sent successfully: clear sticky state and backpressure counters,
        // and reset the IDR cooldown backoff (recovery completed).
        m_AwaitingIdr = false;
        m_BackpressureDropCount = 0;
        m_IdrOutstanding = false;
        m_IdrCooldownMs = kIdrCooldownBaseMs;
    }

    // Video-only path now (audio is a native RTP Opus track, not fragmented over
    // a DataChannel), so this always uses the video frameId sequence.
    uint32_t frameId = m_FrameId++;

    // ── End-to-end latency timestamp ───────────────────────────────────────
    // Send the frame's capture time in steady_clock domain (monotonic ms).
    //   captureSteadyMs = (firstFrameArrivalTimeUs + presentationTimeUs) / 1000
    // The frontend estimates current steady_clock time from periodic stats
    // (streamSteadyMs + performance.now() delta) and subtracts captureSteadyMs.
    // Both steady_clock and performance.now() are monotonic — the delta works.
    uint32_t backendTs = 0;
    if (m_Shim) {
        int64_t firstArrivalSteadyMs = m_Shim->firstFrameArrivalSteadyMs();
        // Use the frame's OWN presentation time (carried through the queued
        // signal). Re-reading the shim's latest-frame atomic here stamps every
        // frame of a drained event-queue burst with one shared backendTs, which
        // disables the frontend's out-of-order filter ("equal timestamps pass")
        // and shows SCTP-reordered frames as back-and-forth jumps.
        int64_t presTimeUs =
            presentationTimeUs >= 0 ? presentationTimeUs : m_Shim->framePresentationTimeUs();
        if (firstArrivalSteadyMs > 0 && presTimeUs >= 0) {
            int64_t captureSteadyMs = firstArrivalSteadyMs + (presTimeUs / 1000);
            backendTs = static_cast<uint32_t>(captureSteadyMs & 0xFFFFFFFF);
        }
    }
    // Fallback: use current wall time if steady_clock not yet initialized
    if (backendTs == 0) {
        backendTs = static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);
    }

    // Offload fragmentation + dc->send() to the dedicated sender thread so the
    // Qt main thread (HTTP/REST/signaling) is never spent building chunks or
    // blocking on a full SCTP buffer. The job holds a shared_ptr copy of the DC,
    // so an in-flight send cannot outlive the channel during stop().
    //
    // If the sender queue was full and a queued delta got evicted, that delta
    // already carries a frameId: the frontend will see a frameId gap, and every
    // delta still queued behind the hole references a frame that will never
    // arrive. Start IDR recovery now instead of waiting a full round-trip for
    // the frontend to detect the gap and ask.
    if (m_Sender->enqueue(dc, data, isKeyframe, /*isAudio=*/false, frameId, backendTs)) {
        m_AwaitingIdr = true;
        sendIdrRequestThrottled();
    }

    m_FrameCount++;
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

    // Drop-source counters: one log line per tick, only when a counter moved
    // (a healthy session stays silent). The drop paths themselves are hot and
    // mostly quiet; this is the log-file view of WHY IDR churn happens —
    // worker = relay-thread backlog (scheduling), senderQueue = sender-thread
    // backlog, sctp* = link saturation.
    {
        const qint64 workerDrops = m_Shim ? m_Shim->workerDropCount() : 0;
        const qint64 senderDrops = m_Sender ? static_cast<qint64>(m_Sender->queueDropCount()) : 0;
        const qint64 snapshot =
            workerDrops + senderDrops + m_DeltaDroppedCount + m_KeyframeBackpressureWarnings;
        if (snapshot != m_LastDropSnapshot) {
            m_LastDropSnapshot = snapshot;
            qInfo() << "[DataChannelRelay] Drop counters — worker:" << workerDrops
                    << "senderQueue:" << senderDrops << "sctpDelta:" << m_DeltaDroppedCount
                    << "sctpKeyframe:" << m_KeyframeBackpressureWarnings
                    << "pendingVideoFrames:" << (m_Shim ? m_Shim->pendingVideoFrames() : 0);
        }
    }

    QJsonObject stats;
    stats["type"] = "stats";
    stats["hostRttMs"] = hostRttMs;
    stats["decodeLatencyUs"] = static_cast<qint64>(decodeLatUs);
    // Cumulative frames dropped by SCTP backpressure (deltas + keyframes).
    // The frontend cannot see these drops (dropped frames never get a frameId),
    // so this is its only signal that the link is saturated backend-side. It
    // drives the frontend's congestion monitor (automatic bitrate degradation).
    stats["bpDrops"] = m_DeltaDroppedCount + m_KeyframeBackpressureWarnings;

    // Send steady_clock reference for end-to-end latency calculation.
    // The frontend uses this + performance.now() delta to estimate current
    // steady time, then subtracts the frame's captureSteadyMs to get e2e latency.
    {
        using namespace std::chrono;
        int64_t nowMs = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        stats["streamTimeMs"] = static_cast<qint64>(nowMs);
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

void DataChannelRelay::notifyClientTakenOver()
{
    sendExitNotice("takeover");
}

void DataChannelRelay::notifyClientRevoked()
{
    sendExitNotice("revoked");
}

void DataChannelRelay::sendExitNotice(const char* type)
{
    // Best-effort control message on the input DC, sent before stop() closes it,
    // so the browser can show a graceful exit instead of a generic disconnect.
    // Reliable/ordered channel → flushed ahead of the close.
    if (!m_InputDc || m_Stopping.load()) return;
    QByteArray json = QJsonDocument(QJsonObject{{"type", type}}).toJson(QJsonDocument::Compact);
    try {
        m_InputDc->send(std::string(json.constData(), json.size()));
    } catch (...) {}
}

void DataChannelRelay::stop()
{
    // The relay lives on its own session thread. If stop() is called from another
    // thread (main: /quit, Session::quit, auto-fallback), marshal it onto the
    // relay thread so timers/DC/PC teardown happen on the owning thread. Queued
    // (non-blocking) avoids any deadlock; a following deleteLater() posts after.
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
        return;
    }

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

    // Reset backpressure counters and IDR state for next session
    m_DeltaDroppedCount = 0;
    m_KeyframeBackpressureWarnings = 0;
    m_BackpressureDropCount = 0;
    m_LastDecodeLatencyUs.store(0, std::memory_order_release);
    m_AwaitingIdr = false;
    m_IdrCooldownTimer.invalidate(); // Reset throttle state
    m_IdrCooldownMs = kIdrCooldownBaseMs;
    m_IdrOutstanding = false;

    // Clear buffered keyframe (if any)
    m_BufferedKeyframe.clear();
    m_BufferedKeyframePresUs = -1;
    m_HaveBufferedKeyframe = false;
    m_NewKeyframeArrived = false;

    m_Connected = false;

    // Close DataChannels
    auto closeDc = [](std::shared_ptr<rtc::DataChannel>& dc, const char* name) {
        if (dc) {
            qInfo() << "[DataChannelRelay] Closing" << name << "DataChannel";
            try {
                dc->close();
            } catch (...) {}
            dc.reset();
        }
    };

    closeDc(m_VideoDc, "video");
    closeDc(m_InputDc, "input");

    // Close the audio track under the audio send lock, so an in-flight audio
    // sendFrame finishes before the track is destroyed.
    {
        std::lock_guard<std::mutex> lk(m_AudioMutex);
        if (m_AudioTrack) {
            qInfo() << "[DataChannelRelay] Closing audio track";
            try {
                m_AudioTrack->close();
            } catch (...) {}
            m_AudioTrack.reset();
        }
    }

    // Stop the sender thread AFTER closing the DataChannels: a send that is
    // blocked on a full SCTP buffer errors out promptly once the channel closes,
    // so join() can't stall the /quit path. The worker holds its own shared_ptr
    // to the (now closed) channel, so the in-flight send is exception-safe.
    if (m_Sender) {
        m_Sender->stop();
    }

    // Close PeerConnection
    if (m_Pc) {
        qInfo() << "[DataChannelRelay] Closing PeerConnection";
        try {
            m_Pc->close();
        } catch (...) {}
        m_Pc.reset();
    }

    qInfo() << "[DataChannelRelay::stop] EXIT";
}

void DataChannelRelay::requestIdrFrame()
{
    if (m_Stopping.load() || !m_Shim) return;
    m_AwaitingIdr = true;
    sendIdrRequestThrottled();
}

void DataChannelRelay::sendIdrRequestThrottled()
{
    if (m_Stopping.load() || !m_Shim) return;

    // Cooldown: absorb requests arriving within the adaptive cooldown window.
    if (m_IdrCooldownTimer.isValid() && m_IdrCooldownTimer.elapsed() < m_IdrCooldownMs) {
        return; // Absorbed — cooldown not elapsed yet
    }

    // Backoff: the previous effective request never produced a delivered
    // keyframe — the link is likely saturated. Double the cooldown (up to 5 s)
    // so recovery IDRs stop feeding the congestion. Reset when a keyframe
    // finally gets through (see the keyframe-sent path in sendFragmented).
    if (m_IdrOutstanding) {
        m_IdrCooldownMs = qMin(m_IdrCooldownMs * 2, kIdrCooldownMaxMs);
    }
    m_IdrOutstanding = true;

    m_IdrCooldownTimer.restart();
    qInfo() << "[DataChannelRelay] IDR request → MoonlightShim (cooldown" << m_IdrCooldownMs
            << "ms)";
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
    qInfo() << "[DataChannelRelay] UPnP public address set:" << QString::fromStdString(publicIP)
            << ":" << publicPort;
}
