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
#include "AudioPathLog.h"
#include "ClipboardBridge.h"
#include "InputMessageCodec.h"
#include "IMediaEngine.h"
#include "ExitNotice.h"
// Only for the cursor-mode message, which is meaningless to any other engine:
// no remote GameStream host can be told to stop drawing its own pointer.
#include "NativeMediaEngine.h"

extern "C" {
#include "Limelight.h"
}

#include "SctpCounters.h"
#include "SctpFlood.h"
#include "common/Logger.h"

#include <rtc/rtc.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QThread>
#include <QDebug>
#include <QDateTime>
#include <QMap>
#include <QVector>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <optional>
#include <variant>
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
// The host's steady clock in µs: the clock frames are stamped on (backendTs)
// and the pong's `host` field reads.
static int64_t steadyUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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

    // ── VPS: rebuild with max_sub_layers=0
    // ─────────────────────────────────
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

    // ── SPS: rebuild with max_sub_layers=0 + cap level_idc
    // ────────────────
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

DataChannelRelay::DataChannelRelay(IMediaEngine* engine, QObject* parent)
    : RelayBase(parent)
    , m_Shim(engine)
{
    qInfo() << "[DataChannelRelay] Created";
    // Empty until a frame has used the slot — an atomic's default value is not
    // guaranteed to be anything before C++20.
    for (auto& slot : m_FrameNumberById)
        slot.store(-1, std::memory_order_relaxed);

    // Dedicated sender thread: fragmentation + dc->send() run off the main
    // thread. For the native engine it runs as an MMCSS "Games" task like the
    // capture thread that feeds it, and holds at most ONE delta: a delta that
    // has not left when the next frame is ready is replaced (latency first;
    // the hole is repaired by intra-refresh or a keyframe). GameStream engines
    // hold two: the queue only ever grows when dc->send() is blocked on a full
    // SCTP buffer, i.e. when the link is saturated, and eight deltas waiting
    // there were 133 ms of picture the viewer would see late at 60 fps — the
    // historical depth, kept until the dedicated pass of September 2026
    // (docs/optimisations-existant.md). Two absorb a frame that lands while
    // the previous one is on the wire; a third evicts the oldest and asks for
    // a keyframe, as the SCTP watermark below does. Ordinary priority.
    {
        const bool nativeEngine = qobject_cast<NativeMediaEngine*>(engine) != nullptr;
        FrameSender::Options senderOptions;
        senderOptions.multimediaPriority = nativeEngine;
        senderOptions.maxQueuedDeltas = nativeEngine ? 1 : kGameStreamQueuedDeltas;
        m_Sender = std::make_unique<FrameSender>(senderOptions);
    }

    // Video path threading. The native engine finishes a frame on its own
    // capture thread; queueing it onto the relay thread costs a wake-up per
    // frame on a thread that also parses every input message, answers the
    // signaling WS and ticks the stats — and a QByteArray copy so the frame
    // survives the queue. So for that engine the relay takes the frame where
    // it is: it installs itself as the engine's direct sink, reads the
    // encoder's buffer through a borrowed QByteArray and cuts the wire chunks
    // right there (sendFragmented). The state it touches is serialized by
    // m_VideoMutex (MediaTrackRelay's P2-B model). A GameStream engine hands
    // over a QByteArray of its own through the signal, and the sender thread
    // cuts it — but the signal is connected DIRECT too (September 2026, the
    // dedicated pass on the other engines): the frame is handled on
    // moonlight-common-c's decode thread, where it was queued to the relay
    // thread before, one wake-up per frame behind the input parser and the
    // stats. Same lock, same code after it; the QByteArray is shared, not
    // copied, by the sender's job.
    auto* native = qobject_cast<NativeMediaEngine*>(engine);
    m_DirectVideoSend = native != nullptr;
    // Input is handled on the libdatachannel thread that received it for every
    // engine (see m_DirectInput).
    qInfo() << "[DataChannelRelay] Video send mode:"
            << (m_DirectVideoSend ? "direct (capture thread, zero-copy)"
                                  : "direct (engine thread, sender cuts)")
            << "— input: direct (libdatachannel thread)";
    if (native) {
        native->setDirectFrameSink([this](const NativeMediaEngine::FrameView& view) {
            handleVideoFrame(QByteArray::fromRawData(reinterpret_cast<const char*>(view.data),
                                                     static_cast<qsizetype>(view.size)),
                             view.keyframe, static_cast<int>(view.frameNumber),
                             view.presentationTimeUs);
        });
    } else {
        connect(m_Shim, &IMediaEngine::videoFrameReady, this, &DataChannelRelay::onVideoFrame,
                Qt::DirectConnection);
    }
    connect(m_Shim, &IMediaEngine::audioSampleReady, this, &DataChannelRelay::onAudioSample);
    connect(m_Shim, &IMediaEngine::connectionTerminated, this,
            &DataChannelRelay::onShimConnectionTerminated);

    // Forward host rumble requests to the browser over the input DC.
    // 'this' as context → runs on the relay thread (signal is emitted from the
    // moonlight worker thread).
    connect(m_Shim, &IMediaEngine::rumble, this, [this](int controller, int low, int high) {
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

    // What the host's OS asked of a recreated HID device, for the page to
    // pass to the real one (output reports, features). Same thread as rumble.
    connect(m_Shim, &IMediaEngine::hidRequest, this,
            [this](int slot, int kind, int reportId, QByteArray data) {
                if (m_Stopping.load() || !m_InputDc) return;
                static const char* const kKinds[] = {"output", "getfeature", "setfeature"};
                QJsonObject m;
                m["type"] = "hidrequest";
                m["slot"] = slot;
                m["kind"] = QLatin1String(kKinds[qBound(0, kind, 2)]);
                m["reportId"] = reportId;
                m["data"] = QString::fromLatin1(data.toBase64());
                QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
                try {
                    m_InputDc->send(std::string(j.constData(), j.size()));
                } catch (const std::exception&) {}
            });

    // Force feedback a game asked of a recreated wheel, for the page to play on
    // the real one. Reliable and ordered like the rest of this channel: an
    // effect's start must never overtake its parameters.
    connect(m_Shim, &IMediaEngine::hidFfb, this, [this](QJsonObject m) {
        if (m_Stopping.load() || !m_InputDc) return;
        const QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
        try {
            m_InputDc->send(std::string(j.constData(), j.size()));
        } catch (const std::exception&) {}
    });

    // Forward the mouse pointer's shape when the browser is the one drawing it.
    // Rare by construction — one message per shape change, never per frame —
    // so the base64 of a small PNG on the input channel costs nothing.
    connect(m_Shim, &IMediaEngine::cursorShapeChanged, this,
            [this](QByteArray png, int hotspotX, int hotspotY, bool visible, QString kind,
                   double scale) {
                if (m_Stopping.load() || !m_InputDc) return;
                QJsonObject m;
                m["type"] = "cursor";
                // Visible with no image means "there is a pointer, we have not
                // been shown it yet" — the browser draws its ordinary arrow.
                // Not visible means draw nothing.
                m["visible"] = visible;
                m["hotspotX"] = hotspotX;
                m["hotspotY"] = hotspotY;
                // Desktop pixels to frame pixels. Sent even at 1 so a client
                // that had a different one is corrected rather than left to
                // remember it.
                m["scale"] = scale;
                // Both are sent every time: the client picks which one to use,
                // and can be reconfigured without the host being told.
                if (!kind.isEmpty()) m["kind"] = kind;
                if (!png.isEmpty()) m["png"] = QString::fromLatin1(png.toBase64());
                QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
                try {
                    m_InputDc->send(std::string(j.constData(), j.size()));
                } catch (const std::exception&) {}
            });

    // And where that pointer is, for a touch screen that draws it from its own
    // finger and needs the host's position only to correct itself. Sparse by
    // construction (throttled at the source), so it rides the input channel
    // like the shape does.
    connect(m_Shim, &IMediaEngine::cursorMoved, this, [this](double x, double y, bool visible) {
        if (m_Stopping.load() || !m_InputDc) return;
        QJsonObject m;
        m["type"] = "cursorpos";
        m["x"] = x;
        m["y"] = y;
        m["visible"] = visible;
        QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
        try {
            m_InputDc->send(std::string(j.constData(), j.size()));
        } catch (const std::exception&) {}
    });

    // The input gate (native host): the viewer's presses are being dropped,
    // or no longer are. On change only, so it costs nothing in steady state.
    connect(m_Shim, &IMediaEngine::inputGateChanged, this,
            [this](bool blocked, QString reason, QString window) {
                if (m_Stopping.load() || !m_InputDc) return;
                QJsonObject m;
                m["type"] = "inputgate";
                m["blocked"] = blocked;
                m["reason"] = reason;
                m["window"] = window;
                QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
                try {
                    m_InputDc->send(std::string(j.constData(), j.size()));
                } catch (const std::exception&) {}
            });

    // The host's display changed mode, shape or dynamic range (native host).
    // On change only; the browser decides whether it is worth a new session.
    connect(m_Shim, &IMediaEngine::displayFormatChanged, this,
            [this](int displayWidth, int displayHeight, int frameWidth, int frameHeight,
                   bool displayHdr, bool hdr, bool hdrCapable) {
                if (m_Stopping.load() || !m_InputDc) return;
                QJsonObject m;
                m["type"] = "displayformat";
                m["displayWidth"] = displayWidth;
                m["displayHeight"] = displayHeight;
                m["frameWidth"] = frameWidth;
                m["frameHeight"] = frameHeight;
                m["displayHdr"] = displayHdr;
                m["hdr"] = hdr;
                m["hdrCapable"] = hdrCapable;
                QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
                try {
                    m_InputDc->send(std::string(j.constData(), j.size()));
                } catch (const std::exception&) {}
            });

    // The guests' shared feed changes codec (native host): the browser comes
    // back in it, as from its own codec fallback.
    connect(m_Shim, &IMediaEngine::sharedFeedCodecChanged, this, [this](QString codec) {
        if (m_Stopping.load() || !m_InputDc) return;
        QJsonObject m;
        m["type"] = "feedcodec";
        m["codec"] = codec;
        QByteArray j = QJsonDocument(m).toJson(QJsonDocument::Compact);
        try {
            m_InputDc->send(std::string(j.constData(), j.size()));
        } catch (const std::exception&) {}
    });

    // ICE connection timeout: emit iceTimedOut() if PC doesn't reach
    // Connected within m_IceTimeoutMs after setRemoteDescription().
    // Triggers WebSocket fallback when UDP is blocked (corporate firewall).
    m_IceCheckTimer = new QTimer(this);
    m_IceCheckTimer->setSingleShot(true);
    connect(m_IceCheckTimer, &QTimer::timeout, this, &DataChannelRelay::onIceCheckTimeout);

    // Stats timer: sends periodic stats (hostRtt, decodeLatency) to the browser.
    // Starts when Input DC opens, stops in stop().
    m_StatsTimer = new QTimer(this);
    m_StatsTimer->setInterval(1000); // 1s interval — matches moonlight-qt's stats window flip
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
    // What SCTP had to repair during the session. A T3 timeout is a repair
    // that waited out the retransmission timer (200 ms at least) instead of a
    // fast retransmit — on a LAN, the one way a single frame arrives that late.
    // Here and not in stop(), which a closing peer has usually entered first.
    if (m_SctpAtOpenSet.load()) {
        const mw::sctp::Counters d = mw::sctp::readCounters() - m_SctpAtOpen;
        qInfo() << "[DataChannelRelay] SCTP this session:" << d.sent << "data chunks sent,"
                << d.retrans << "retransmitted (" << d.fast << "fast)," << d.t3 << "T3 timeouts";
        // How the window took those repairs (plan Wi-Fi W1): a loss inside a
        // recovery already under way, a chunk lost twice, sends the window or
        // max burst held back.
        qInfo() << "[DataChannelRelay] SCTP window this session:" << d.fastInRtt
                << "losses inside a recovery," << d.multFast << "chunks fast-retransmitted twice,"
                << d.cwndHeld << "sends held by the window," << d.burstHeld
                << "windows trimmed to max burst," << d.sacks << "SACKs," << d.packetsOut
                << "packets out," << d.dupIn << "duplicate chunks in";
    }
    // What the bench's pacing cost: the frames that waited, and how long.
    if (m_Sender && m_PaceMultiple > 0) {
        const FrameSender::PacingStats p = m_Sender->pacingStats();
        qInfo().noquote() << QStringLiteral(
                                 "[DataChannelRelay] bench pacing this session: %1 frames, %2 "
                                 "waited at least once, %3 ms waited in all, the longest %4 ms "
                                 "inside one frame")
                                 .arg(p.frames)
                                 .arg(p.pacedFrames)
                                 .arg(p.waitedUs / 1000.0, 0, 'f', 1)
                                 .arg(p.maxFrameWaitUs / 1000.0, 0, 'f', 2);
    }
    // The bench's frame log (`relaylog=1`): next to this process's log.
    if (m_FrameLog) {
        const QString logFile = Logger::instance()->logFilePath();
        const QString dir =
            logFile.isEmpty() ? QDir::tempPath() : QFileInfo(logFile).absolutePath();
        const QString path = dir + QStringLiteral("/relay-frames-%1-%2.csv")
                                       .arg(QCoreApplication::applicationPid())
                                       .arg(QDateTime::currentMSecsSinceEpoch());
        const bool ok = m_FrameLog->writeCsv(path.toStdString());
        qInfo() << "[DataChannelRelay] frame log:" << m_FrameLog->summary().c_str()
                << (ok ? "— written to" : "— could NOT be written to") << path;
    }
    // The bench's audio log (`audiolog=1`): the same place, the same way.
    if (m_AudioLog) {
        AudioPathLog& audioLog = AudioPathLog::instance();
        audioLog.stop();
        const QString logFile = Logger::instance()->logFilePath();
        const QString dir =
            logFile.isEmpty() ? QDir::tempPath() : QFileInfo(logFile).absolutePath();
        const QString path = dir + QStringLiteral("/relay-audio-%1-%2.csv")
                                       .arg(QCoreApplication::applicationPid())
                                       .arg(QDateTime::currentMSecsSinceEpoch());
        const bool ok = audioLog.writeCsv(path.toStdString());
        qInfo() << "[DataChannelRelay] audio log:" << audioLog.summary().c_str()
                << (ok ? "— written to" : "— could NOT be written to") << path;
    }
    // Static call: dynamic dispatch is meaningless in a destructor.
    DataChannelRelay::stop();
}

/// Make `bufferedAmount` mean something.
///
/// usrsctp's send buffer defaults to 1 MiB, and `bufferedAmount` counts only
/// what libdatachannel keeps AFTER usrsctp refuses — so with the default,
/// nothing is visible until a megabyte is already queued and every backlog
/// measurement starts a megabyte late. Shrinking the buffer moves that backlog
/// from a place we cannot see into one we can; the bytes in flight are the
/// same, the blindness is not.
///
/// How much stays invisible is a matter of TIME, so the size follows the
/// stream's bitrate — see SendBacklog::sendBufferBytesFor for the figures and
/// the freeze that showed a fixed 256 KiB hiding a whole second at 2 Mbit/s.
///
/// ⚠️ Read on 03/10/2026 (plan Wi-Fi W2 C): below 256 KiB this never took.
/// libdatachannel's SctpTransport raises SO_SNDBUF to its largest message
/// (Configuration::maxMessageSize, 256 KiB unset) right after reading the
/// sysctl, so every session has had 256 KiB whatever the bitrate — a second
/// at 2 Mbit/s, ~100 ms at 20. Only the bench's `sctpbuf=` (@p bufferKb)
/// lowers it, by lowering the largest message along with it (setupPeerConnection).
///
/// A sysctl, read by each SCTP socket at creation: it must be set before the
/// peer connection, and setting it again for another session in the same
/// process is fine.
void applySctpSettings(int bitrateKbps, int congestionModule, int bufferKb, int maxBurst)
{
    const size_t bytes = bufferKb > 0 ? static_cast<size_t>(bufferKb) * 1024
                                      : SendBacklog::sendBufferBytesFor(bitrateKbps);
    rtc::SctpSettings settings;
    settings.sendBufferSize = bytes;
    // Bench knobs, never settings: unset, libdatachannel's defaults stand (a
    // 200 ms minimum RTO, a 20 ms delayed SACK). They exist for one A/B — the
    // click-to-photon tail of 25/09/2026, one click in ten ~205 ms late, has
    // the shape of a T3 timeout at that minimum RTO: a lone flag frame whose
    // last packet is lost has nothing behind it to trigger a fast retransmit.
    bool ok = false;
    const int rtoMinMs = qEnvironmentVariableIntValue("MW_SCTP_RTO_MIN_MS", &ok);
    if (ok && rtoMinMs > 0) settings.minRetransmitTimeout = std::chrono::milliseconds(rtoMinMs);
    const int sackMs = qEnvironmentVariableIntValue("MW_SCTP_SACK_DELAY_MS", &ok);
    if (ok && sackMs >= 0) settings.delayedSackTime = std::chrono::milliseconds(sackMs);
    // The bench's `sctpcc=` (plan Idées Punktfunk, A0.3): which loss-driven
    // window the link is held to. Unset, usrsctp's own (RFC 2581).
    if (congestionModule >= 0) {
        settings.congestionControlModule = static_cast<unsigned int>(congestionModule);
        static const char* const kModules[] = {"RFC 2581", "HSTCP", "H-TCP", "RTCC"};
        qWarning() << "[DataChannelRelay] SCTP bench override: congestion control"
                   << kModules[congestionModule & 3];
    }
    // `sctpburst=` (plan Wi-Fi W2.5), the Windows and Linux native hosts' own unless
    // said: how many packets usrsctp sends at one opportunity.
    // libdatachannel holds it at 10; a frame of 20 to 35 packets then waits 2
    // to 4 SACK round trips, and a round trip is 8-9 ms on a Mac in Wi-Fi
    // against 3-4 on Ethernet.
    if (maxBurst >= 0) {
        settings.maxBurst = static_cast<size_t>(maxBurst);
        qInfo() << "[DataChannelRelay] SCTP max burst:"
                << (maxBurst > 0 ? QString::number(maxBurst) + " packets"
                                 : QStringLiteral("no limit"));
    }
    rtc::SetSctpSettings(settings);
    if (bufferKb > 0)
        qWarning() << "[DataChannelRelay] SCTP bench override (sctpbuf=): send buffer" << bufferKb
                   << "KiB, the largest message with it";
    else
        qInfo() << "[DataChannelRelay] SCTP send buffer asked at" << (bytes / 1024) << "KiB for"
                << bitrateKbps << "kbps (libdatachannel keeps 256 KiB at least)";
    if (settings.minRetransmitTimeout || settings.delayedSackTime)
        qInfo() << "[DataChannelRelay] SCTP bench override: min RTO"
                << (settings.minRetransmitTimeout ? settings.minRetransmitTimeout->count() : -1)
                << "ms, delayed SACK"
                << (settings.delayedSackTime ? settings.delayedSackTime->count() : -1) << "ms";
}

void DataChannelRelay::setLinkBench(const mw::native::EncoderTuning& tuning)
{
    m_Loss.configure(tuning.lossPermille, tuning.lossBurst);
    m_SctpCongestion = tuning.sctpCongestion;
    m_FloodKbps = tuning.floodKbps;
    m_FloodBytes = tuning.floodBytes > 0 ? tuning.floodBytes : SctpFlood::kDefaultBytes;
    m_FloodLikeVideo = tuning.floodLikeVideo;
    m_UltraSynthKb = tuning.ultraSynthKb;
    m_UltraUnordered = tuning.ultraUnordered;
    m_AroadPace = tuning.aroadPace;
    m_AroadWindowKb = tuning.aroadWindowKb;
    // PyroWave sends its whole frame every time (~178 KB at 170 Mbit/s), and
    // the host spends ~12 us a packet whatever its size: its chunks are the
    // biggest a 1500-byte MTU holds, which Ultra's cabled LAN has (POC Ultra
    // P-B, design §6.41: -0.4 ms on the way down). aroadchunk= says otherwise.
    const bool pyrowave = tuning.enc12 == mw::native::EncoderTuning::Encoder12::Pyrowave;
    if (tuning.aroadChunk > 0 || pyrowave) {
        m_AroadChunk = static_cast<size_t>(tuning.aroadChunk > 0 ? tuning.aroadChunk : 1400);
        qInfo() << "[DataChannelRelay] audio road chunks of" << m_AroadChunk << "bytes"
                << (tuning.aroadChunk > 0 ? "(aroadchunk=)" : "(PyroWave's default)");
    }
    if (tuning.aroadBudgetPct > 0) {
        std::lock_guard<std::mutex> budget(m_AroadBudgetMutex);
        m_AroadBudget.setShare(tuning.aroadBudgetPct / 100.0);
        qInfo() << "[DataChannelRelay] audio road resends:" << tuning.aroadBudgetPct
                << "% of what it sent (aroadbudget=)";
    }
    m_PaceMultiple = tuning.paceMultiple;
    m_PaceBurstKb = tuning.paceBurstKb;
    m_SctpBufferKb = tuning.sctpBufferKb;
    m_LinkHoldMs = tuning.linkHoldMs;
    m_SctpMaxBurst = tuning.sctpMaxBurst >= 0 ? tuning.sctpMaxBurst : kNativeSctpMaxBurst;
    m_SctpScheduler = tuning.sctpScheduler;
    m_SctpMtu = tuning.sctpMtu;
    if (tuning.relayLog && !m_FrameLog) {
        m_FrameLog = std::make_unique<RelayFrameLog>();
        qWarning() << "[DataChannelRelay] bench frame log on (relaylog=1): each video frame's way "
                      "through the relay, written next to the log when the session ends";
    }
    if (tuning.clickTrace && !m_ClickTraceLog) {
        m_ClickTraceLog = true;
        qWarning() << "[DataChannelRelay] bench click trace on (clicktrace=1): a line for each "
                      "stamped input, when it came off the channel and when it was handled";
    }
    if (tuning.audioLog && !m_AudioLog) {
        m_AudioLog = true;
        AudioPathLog::instance().start();
        qWarning() << "[DataChannelRelay] bench audio log on (audiolog=1): each audio packet's "
                      "way through the host, written next to the log when the session ends";
    }
    if (m_Loss.active())
        qWarning() << "[DataChannelRelay] bench losses: video messages thrown away before SCTP,"
                   << tuning.lossPermille << "per thousand, bursts of"
                   << (tuning.lossBurst > 1 ? tuning.lossBurst : 1);
}

void DataChannelRelay::applyPacing()
{
    if (!m_Sender || m_PaceMultiple <= 0) return;
    if (m_StreamBitrateKbps <= 0) {
        qWarning() << "[DataChannelRelay] bench pacing asked for, but the stream's bitrate is "
                      "unknown: not paced";
        return;
    }
    const int64_t bytesPerSecond =
        static_cast<int64_t>(m_StreamBitrateKbps) * 1000 / 8 * m_PaceMultiple;
    const size_t burst = static_cast<size_t>(m_PaceBurstKb > 0 ? m_PaceBurstKb : 16) * 1024;
    // A run no longer than the burst: chunks of that size at most, header
    // included, so pacing has something to space.
    m_ChunkPayload = burst < 16000 + 17 ? burst - 17 : 16000;
    m_Sender->setPacing(bytesPerSecond, burst);
    qWarning().noquote() << QStringLiteral(
                                "[DataChannelRelay] bench pacing (pace=%1): a frame's chunks "
                                "handed to SCTP at %2 Mbit/s at most, %3 KB at a time, chunks "
                                "of %4 bytes")
                                .arg(m_PaceMultiple)
                                .arg(bytesPerSecond * 8 / 1e6, 0, 'f', 1)
                                .arg(burst / 1024)
                                .arg(m_ChunkPayload);
}

bool DataChannelRelay::prepare(const rtc::Configuration& config, bool isInternet)
{
    // Before the peer connection, since each SCTP socket reads this when it is
    // made.
    applySctpSettings(m_StreamBitrateKbps, m_SctpCongestion, m_SctpBufferKb, m_SctpMaxBurst);
    m_Backlog.setBitrateKbps(m_StreamBitrateKbps);
    applyPacing();

    if (m_Pc) {
        qWarning() << "[DataChannelRelay] already prepared";
        return false;
    }

    // A public peer needs a longer ICE deadline than a LAN one; see
    // RelayBase::kIceTimeoutInternetMs.
    m_IceTimeoutMs = isInternet ? kIceTimeoutInternetMs : kIceTimeoutLocalMs;

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
        const rtc::Description answer(sdp);
        m_Pc->setRemoteDescription(answer);
        // A browser without Encoded Transform answers the video track
        // inactive: the video then stays on the DataChannel (U1.4).
        if (m_VideoTrack || m_UltraTrack) {
            bool video = false, ultra = false;
            for (int i = 0; i < answer.mediaCount(); ++i) {
                const auto entry = answer.media(i);
                if (!std::holds_alternative<const rtc::Description::Media*>(entry)) continue;
                const auto* media = std::get<const rtc::Description::Media*>(entry);
                const bool taken = !media->isRemoved() &&
                                   media->direction() != rtc::Description::Direction::Inactive;
                if (media->mid() == "video" || media->mid() == "vaudio") video = taken;
                if (media->mid() == "ultra" || media->mid() == "uaudio") ultra = taken;
            }
            m_RtpVideoAccepted.store(m_VideoTrack && video);
            m_UltraRtpAccepted.store(m_UltraTrack && ultra);
            qInfo() << "[DataChannelRelay] RTP video track"
                    << (m_VideoTrack ? (video ? "accepted"
                                              : "REFUSED by the browser, video on the DataChannel")
                                     : "not offered")
                    << "| Ultra RTP track"
                    << (m_UltraTrack ? (ultra ? "accepted" : "refused") : "not offered");
        }
        qInfo() << "[DataChannelRelay] Remote description set — starting ICE timeout ("
                << m_IceTimeoutMs << "ms)";
        // Start ICE connection timer. The remote description is set, so ICE
        // negotiation begins now. If it doesn't reach Connected in time,
        // we emit iceTimedOut() for WS fallback.
        if (m_IceCheckTimer) {
            m_IceCheckTimer->start(m_IceTimeoutMs);
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

    // `sctpbuf=`: libdatachannel raises usrsctp's send buffer to the largest
    // message, so that comes down with it (see applySctpSettings). Both ways:
    // the page may send nothing bigger either, which only the clipboard ever
    // comes near — a bench has none.
    rtc::Configuration pcConfig = config;
    if (m_SctpBufferKb > 0) pcConfig.maxMessageSize = static_cast<size_t>(m_SctpBufferKb) * 1024;
    // `sctpmtu=` (plan Wi-Fi W2.6): usrsctp sizes its packets from the path
    // MTU (mtu - 108, path MTU discovery off), 1172 bytes at libdatachannel's
    // 1280, so a frame goes in ~16 % fewer packets at 1500. Only our sends:
    // the browser's SCTP keeps its own size. DTLS fragments its handshake
    // from it too, so a path that does not hold it never connects.
    if (m_SctpMtu > 0) {
        pcConfig.mtu = static_cast<size_t>(m_SctpMtu);
        qWarning() << "[DataChannelRelay] SCTP bench override (sctpmtu=): path MTU" << m_SctpMtu
                   << "bytes, SCTP packets of" << (m_SctpMtu - 108) << "bytes";
    }

    m_Pc = std::make_shared<rtc::PeerConnection>(pcConfig);

    // `sctpss=` (plan Wi-Fi W2.3): the host's small messages on the input
    // channel wait behind the video's 16 KB chunks inside usrsctp. Its stream
    // scheduler is a sysctl the SCTP socket copies when it is made, after
    // DTLS; usrsctp_init, which the peer connection just ran, resets it.
    if (m_SctpScheduler >= 0) {
        static const char* const kSchedulers[] = {
            "default",  "round robin",    "round robin by packet",
            "priority", "fair bandwidth", "first come"};
        const bool ok = mw::sctp::setStreamScheduler(m_SctpScheduler);
        qWarning() << "[DataChannelRelay] SCTP bench override (sctpss=): stream scheduler"
                   << kSchedulers[m_SctpScheduler % 6] << (ok ? "" : "— REFUSED by usrsctp");
    }

    // --- Local description callback ---
    m_Pc->onLocalDescription([this](const rtc::Description& sdp) {
        qInfo() << "[DataChannelRelay] Local SDP generated, type="
                << QString::fromStdString(sdp.typeString());
        emit signalingSdpReady(std::string(sdp));
    });

    // --- Local ICE candidate callback ---
    m_Pc->onLocalCandidate([this](const rtc::Candidate& candidate) {
        emitLocalCandidate(candidate, "[DataChannelRelay]");
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
            logSelectedCandidatePair(*m_Pc, "[DataChannelRelay]");
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

    // ORDER IS LOAD-BEARING: every addTrack() MUST come before the first
    // createDataChannel(). libdatachannel generates the offer on its own, and
    // createDataChannel() is what triggers it — synchronously, as soon as the
    // signaling state is Stable (peerconnection.cpp, createDataChannel);
    // addTrack() triggers nothing. Creating the video DC first therefore
    // published an offer with no m=audio section at all, and the audio track
    // could only reach the browser through a *second* offer, renegotiated once
    // the answer put the signaling state back to Stable. That offer raced the
    // frontend closing the signaling WS as soon as the DataChannels opened: it
    // won on a fast LAN (audio fine) and lost behind a slow/proxied WS (audio
    // silent for the whole session, with no way for the user to recover it).
    // See issue #11. MediaTrackRelay::createTracksAndChannels() has the same
    // constraint and already respects it.

    // --- Audio track (server->browser, Opus over RTP) ---
    // Native RTP Opus track on the SAME PeerConnection as the video DataChannel.
    // The browser decodes Opus with its own jitter buffer + in-band FEC + PLC, so
    // a lost UDP packet is concealed instead of head-of-line-blocking the audio
    // (the old ordered DataChannel caused periodic ~0.5s dropouts on packet loss).
    // useinbandfec=1 tells the decoder to use the FEC carried in the next packet.
    // stereo=1;sprop-stereo=1 is REQUIRED: without it libwebrtc instantiates a
    // MONO Opus decoder and downmixes the stereo Sunshine stream (L+R)/2, which
    // plays ~-6 dB quieter than moonlight-qt and loses the stereo image.
    {
        auto audioDesc = rtc::Description::Audio("audio", rtc::Description::Direction::SendOnly);
        audioDesc.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");

        // Declared in the description, before addTrack: the PeerConnection
        // routes incoming RTCP by the SSRCs it finds there, so an SSRC known
        // only to the packetizer leaves the NACK responder below deaf. See the
        // long note in MediaTrackRelay, where the same omission was costing the
        // video track every retransmission and every keyframe request.
        std::random_device rd;
        const uint32_t ssrc = static_cast<uint32_t>(rd());
        audioDesc.addSSRC(ssrc, "audio");

        m_AudioTrack = m_Pc->addTrack(audioDesc);
        if (m_AudioTrack) {
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

    // --- Video DataChannel (server->browser, H.264 NAL units) ---
    // NOTE: this call is what publishes the SDP offer (see the ordering note
    // above) — nothing that must appear in it may be added after this point.
    // Ordered + partial reliability: an HEVC keyframe is ~11 chunks ≈ 140 UDP
    // packets, so with no retransmission at all a single packet loss kills
    // the whole frame and forces an IDR recovery cycle.
    //
    // Ordered is required for video: frames reference their predecessor, so
    // delivery order IS decode order. With unordered delivery, a retransmitted
    // chunk made frame N complete AFTER frame N+1 — the frontend saw a frameId
    // gap (false loss), invalidated the reference and requested an IDR on every
    // reorder. Ordered lets SCTP hold N+1 the ~RTT the retransmit takes; a
    // message SCTP gives up on is skipped via FORWARD-TSN and surfaces as a
    // real gap.
    //
    // Given up on by AGE, not after a number of retransmits (16/09/2026). The
    // count was 3, and on a corporate Wi-Fi that froze the link for a second
    // every half minute it was the wrong measure: the retransmit timer expires
    // and doubles under a freeze — 200, 400, 800 ms — so everything in flight
    // when it began was still being retransmitted, in order, when the link
    // came back, ahead of the keyframe the receiver was waiting for, and at
    // the crawl of a window SCTP rebuilds from one segment after a timeout
    // (one more per round trip). The receiver spent seconds decoding frames a
    // second old and the pointer trailed the hand by as much. A message older
    // than the
    // lifetime is abandoned wherever it is, and the receiver jumps to what is
    // current. A lone loss on a healthy link still gets its fast retransmit
    // within a round trip, and one timer-driven retry inside the lifetime.
    // Before the first createDataChannel(), like the audio track (above).
    createRtpVideoTracks();

    rtc::DataChannelInit videoConfig;
    videoConfig.reliability.unordered = false;
    // Must match the frontend's negotiated channel config.
    videoConfig.reliability.maxPacketLifeTime = std::chrono::milliseconds(kVideoFrameLifetimeMs);
    videoConfig.negotiated = true;
    videoConfig.id = 0;

    m_VideoDc = m_Pc->createDataChannel("video", videoConfig);
    if (m_VideoDc && m_FrameLog) {
        m_FrameLogDc = m_VideoDc;
        m_FrameLog->setBufferedProbe(
            [](void* ctx) -> size_t {
                const auto dc = static_cast<DataChannelRelay*>(ctx)->m_FrameLogDc.lock();
                return dc ? dc->bufferedAmount() : 0;
            },
            this);
    }
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
    // `linkhold=` (plan Wi-Fi W2 C): the native session asks, at each picture,
    // whether video has waited outside usrsctp for longer than the key says.
    // Not whether it waits at all: on Ethernet a frame bigger than usrsctp's
    // room overflows for a millisecond or two and is gone, and holding on that
    // halved the frame rate (03/10/2026) — held pictures come out bigger and
    // overflow again. So the probe dates the start of each backlog, and the
    // channel's "buffered amount low" (at 0) closes it. `bufferedAmount` is an
    // atomic in libdatachannel, so the capture thread may read it; the
    // channel is held weakly and a closed one is never busy.
    if (m_VideoDc && m_LinkHoldMs > 0) {
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim)) {
            std::weak_ptr<rtc::DataChannel> weak = m_VideoDc;
            auto since = std::make_shared<std::atomic<int64_t>>(-1);
            m_VideoDc->setBufferedAmountLowThreshold(0);
            m_VideoDc->onBufferedAmountLow([since]() { since->store(-1); });
            const int64_t graceUs = static_cast<int64_t>(m_LinkHoldMs) * 1000;
            native->setLinkBusyProbe([weak, since, graceUs]() {
                const auto dc = weak.lock();
                if (!dc || !dc->isOpen() || dc->bufferedAmount() == 0) {
                    since->store(-1);
                    return false;
                }
                const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count();
                int64_t from = since->load();
                if (from < 0 && since->compare_exchange_strong(from, nowUs)) from = nowUs;
                return nowUs - from >= graceUs;
            });
            qWarning() << "[DataChannelRelay] bench link hold (linkhold=): the session holds "
                          "its pictures once video has waited outside usrsctp ("
                       << (m_SctpBufferKb > 0 ? m_SctpBufferKb : 256) << "KiB) for" << m_LinkHoldMs
                       << "ms";
        } else {
            qWarning() << "[DataChannelRelay] linkhold= asked for, but this engine is not the "
                          "native one: nothing held";
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
            if (!m_SctpAtOpenSet.load()) {
                m_SctpAtOpen = mw::sctp::readCounters();
                m_SctpAtOpenSet.store(true);
            }
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

            // Whether this host recreates WebHID devices: the page shows its
            // "Transmit a device" panel from this answer.
            {
                const QString why = !m_InputPolicy.hid
                                        ? QStringLiteral("not allowed for this player")
                                        : m_Shim->hidUnavailableReason();
                QJsonObject caps{
                    {QStringLiteral("type"), QStringLiteral("hidcaps")},
                    {QStringLiteral("available"), why.isEmpty()},
                    {QStringLiteral("ffb"), why.isEmpty() && m_Shim->hidForceFeedback()},
                    {QStringLiteral("hidpp"), why.isEmpty() && m_Shim->hidRelaysHidpp()},
                    {QStringLiteral("why"), why}};
                const QByteArray j = QJsonDocument(caps).toJson(QJsonDocument::Compact);
                try {
                    m_InputDc->send(std::string(j.constData(), j.size()));
                } catch (const std::exception&) {}
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
            if (!std::holds_alternative<rtc::string>(msg)) return;
            // Taken first, before any lock or parse: a stamped message reports
            // this as its arrival.
            const int64_t recvUs = steadyUs();
            if (m_DirectInput) {
                // Parse and inject here, on the receiving thread, whatever the
                // engine. A message that finds the relay stopping leaves; one
                // that got in before stop() took the lock finishes first.
                std::lock_guard<std::mutex> lk(m_InputMutex);
                if (m_Stopping.load()) return;
                onInputMessage(std::get<rtc::string>(msg), recvUs);
                return;
            }
            // The queued form, kept for a flag nothing clears today: the relay
            // thread handles the message on its next turn.
            std::string text = std::get<rtc::string>(msg);
            QMetaObject::invokeMethod(
                this, [this, text, recvUs]() { onInputMessage(text, recvUs); },
                Qt::QueuedConnection);
        });
    }

    // --- HID DataChannel (the HID passthrough's input reports, plan P2) ---
    // Unordered and never retransmitted: each report carries the device's
    // whole state, so the next one repairs a loss, and the page sends an
    // unchanged report again every 500 ms. Negotiated, so it adds nothing to
    // the offer; binary [slot][report id][seq u16 LE][report], applied on this
    // thread like the direct input, under the same lock and the same policy.
    {
        rtc::DataChannelInit hidConfig;
        hidConfig.negotiated = true;
        hidConfig.id = 4;
        hidConfig.reliability.unordered = true;
        hidConfig.reliability.maxRetransmits = 0;
        m_HidDc = m_Pc->createDataChannel("hid", hidConfig);
        if (m_HidDc) {
            m_HidDc->onMessage([this](const std::variant<rtc::binary, rtc::string>& msg) {
                if (!std::holds_alternative<rtc::binary>(msg)) return;
                const rtc::binary& frame = std::get<rtc::binary>(msg);
                std::lock_guard<std::mutex> lk(m_InputMutex);
                if (m_Stopping.load() || !m_InputPolicy.hid) return;
                m_Shim->hidInput(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
            });
        }
    }

    // --- Ultra DataChannel (bench only: `ultra=synthetic:<KiB>`, POC U1.1) ---
    // What an intra codec would ask of SCTP, without the codec: per video frame
    // sent, a train of incompressible chunks in the video's own format (header
    // with frameId and the frame's backendTs) on channel id 5 — id 4 is the
    // HID passthrough's. Negotiated, so it adds nothing to the offer; a client
    // without `mw_ultra_sink` drops what lands on the id. Its own FrameSender,
    // with the video's queue depth, so that a full link evicts Ultra frames,
    // never the video, and the drops are counted apart.
    if (m_UltraSynthKb > 0) {
        rtc::DataChannelInit ultraConfig;
        ultraConfig.negotiated = true;
        ultraConfig.id = 5;
        if (m_UltraUnordered) {
            ultraConfig.reliability.unordered = true;
            ultraConfig.reliability.maxRetransmits = 0;
        } else {
            ultraConfig.reliability.unordered = false;
            ultraConfig.reliability.maxPacketLifeTime =
                std::chrono::milliseconds(kVideoFrameLifetimeMs);
        }
        m_UltraDc = m_Pc->createDataChannel("ultra", ultraConfig);
        if (m_UltraDc) {
            FrameSender::Options ultraOptions;
            ultraOptions.multimediaPriority = true;
            ultraOptions.maxQueuedDeltas = 1;
            m_UltraSender = std::make_unique<FrameSender>(ultraOptions);
            // Incompressible bytes: SCTP compresses nothing, but a capture of
            // the wire or a middlebox might, and the bench must not lean on it.
            m_UltraPayload.resize(static_cast<size_t>(m_UltraSynthKb) * 1024);
            uint64_t x = 0x9E3779B97F4A7C15ull;
            for (auto& b : m_UltraPayload) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                b = static_cast<uint8_t>(x);
            }
            m_UltraDc->onOpen([this]() { m_UltraOpen.store(true); });
            m_UltraDc->onClosed([this]() { m_UltraOpen.store(false); });
            qWarning().noquote() << "[DataChannelRelay] bench Ultra synthetic on DC#5:"
                                 << m_UltraSynthKb << "KiB per video frame,"
                                 << (m_UltraUnordered ? "unordered, no retransmission"
                                                      : "ordered, 500 ms lifetime");
        }
    }

    // --- Flood DataChannel (bench only: `flood=`, plan Idées Punktfunk A0) ---
    // What SCTP carries to the real receiver, on the id the FEC channel is to
    // have (3) and with its reliability — unordered, never retransmitted — or
    // the video channel's, to compare the two the same way. Negotiated, so it
    // adds nothing to the offer; a client without `mw_flood` drops what lands
    // on the id. See SctpFlood.h.
    if (m_FloodKbps != 0) {
        rtc::DataChannelInit floodConfig;
        floodConfig.negotiated = true;
        floodConfig.id = 3;
        if (m_FloodLikeVideo) {
            floodConfig.reliability.unordered = false;
            floodConfig.reliability.maxPacketLifeTime =
                std::chrono::milliseconds(kVideoFrameLifetimeMs);
        } else {
            floodConfig.reliability.unordered = true;
            floodConfig.reliability.maxRetransmits = 0;
        }
        m_FloodDc = m_Pc->createDataChannel("flood", floodConfig);
        if (m_FloodDc) {
            m_Flood = std::make_unique<SctpFlood>(m_FloodDc, m_FloodKbps, m_FloodBytes);
            m_FloodDc->onOpen([this]() {
                qInfo() << "[DataChannelRelay] Flood DataChannel open (bench)";
                if (m_Flood && !m_Stopping.load()) m_Flood->start();
            });
            qWarning().noquote() << "[DataChannelRelay] bench flood on DC#3,"
                                 << (m_FloodLikeVideo ? "ordered, 500 ms lifetime"
                                                      : "unordered, no retransmission");
        }
    }

    // Guard on the ordering invariant above: everything we want negotiated is
    // created by now, so the published offer must already describe all of it.
    // A leftover pending negotiation means something was added after the offer
    // went out and will only reach the browser through a racy renegotiation —
    // exactly the issue #11 failure. Fail loudly in the log instead of shipping
    // an intermittent silence.
    if (m_Pc->negotiationNeeded()) {
        qWarning() << "[DataChannelRelay] BUG: negotiation still pending after setup —"
                   << "a track/channel was created after the offer was published;"
                   << "audio or video may be missing from the SDP (see issue #11)";
    }

    qInfo() << "[DataChannelRelay] Channels created (video=DC#0, audio=RTP, input=DC#2)";
}

bool DataChannelRelay::ridingOutLoss() const
{
    return m_RideOutLoss && m_Shim && m_Shim->intraRefreshActive();
}

// --- Video/Audio forwarding (from media engine signals) ---
// The engine's own thread in both cases: moonlight-common-c's decode thread
// for a GameStream engine (direct signal connection), the capture thread for
// the native one (m_DirectVideoSend, direct sink) — see the class comment.

void DataChannelRelay::onVideoFrame(const QByteArray& data, int frameType, int frameNumber,
                                    qint64 presentationTimeUs)
{
    handleVideoFrame(data, frameType == 1, frameNumber, presentationTimeUs);
}

void DataChannelRelay::handleVideoFrame(const QByteArray& data, bool isKeyframe, int frameNumber,
                                        qint64 presentationTimeUs)
{
    // Balance the worker→relay pending counter (incremented before each emit).
    // m_Shim lifetime is guaranteed: the shim is Qt-parented to this relay
    // (Session.cpp), so it cannot be destroyed while this slot runs.
    bool workerDroppedDelta = false;
    if (m_Shim) {
        m_Shim->videoFrameDelivered();
        workerDroppedDelta = m_Shim->takeWorkerDroppedDelta();
    }

    if (m_Stopping.load()) {
        static int dropCount = 0;
        if (++dropCount <= 3)
            qInfo() << "[DataChannelRelay] onVideoFrame dropped — m_Stopping=true";
        return;
    }

    // Serialize with stop(), sendBufferedKeyframe() and the requestidr paths.
    // In direct mode this runs on the capture thread; the lock is what keeps the
    // video DC and the buffered keyframe from being torn down mid-frame.
    std::lock_guard<std::mutex> lk(m_VideoMutex);

    // Re-check after acquiring the lock: stop() may have run while we waited.
    if (m_Stopping.load()) return;

    if (m_FrameLog) {
        // The capture on the host's clock, as sendFragmented stamps backendTs:
        // what the client's per-frame log carries, and how a pass joins the two.
        int64_t captureUs = -1;
        if (m_Shim) {
            const int64_t firstMs = m_Shim->firstFrameArrivalSteadyMs();
            const int64_t presUs =
                presentationTimeUs >= 0 ? presentationTimeUs : m_Shim->framePresentationTimeUs();
            if (firstMs > 0 && presUs >= 0) captureUs = firstMs * 1000 + presUs;
        }
        const mw::sctp::Counters c = mw::sctp::readCounters();
        m_FrameLog->begin(frameNumber, isKeyframe, static_cast<size_t>(data.size()), captureUs,
                          steadyUs(), RelayFrameLog::Sctp{c.retrans, c.fast, c.t3},
                          m_SrttMs.load());
    }

    // Worker dropped deltas due to relay-thread backlog — enter awaiting-IDR
    // recovery (guards inside are no-ops when stopping).
    //
    // …unless the stream refreshes by intra-refresh AND the client said it
    // will decode through the damage. Then the picture repairs itself over
    // one refresh cycle, and the keyframe this would ask for is exactly the
    // wrong thing to send: the drop happened because the link was already
    // saturated, and an IDR is the largest frame there is.
    if (workerDroppedDelta && !ridingOutLoss()) {
        m_AwaitingIdr = true;
        sendIdrRequestThrottled();
    }

    // HEVC VPS/SPS patch for Chrome Windows black screen.
    // Patch the first keyframe once per session: Chrome's HEVC decoder chokes
    // on some VPS/SPS parameters (high level_idc, temporal sublayers). H.264
    // needs no patch. `frameData` shares `data` until something writes to it:
    // the patch below is the only writer, and it happens once per session, so
    // this is the one frame that ever gets copied here.
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

    // POC Ultra U1.4: the video on its RTP track, when the answer took it.
    if (m_RtpVideoAccepted.load(std::memory_order_relaxed)) {
        if (!m_VideoTrack->isOpen()) {
            if (isKeyframe) {
                m_BufferedKeyframe = QByteArray(frameData.constData(), frameData.size());
                m_BufferedKeyframePresUs = presentationTimeUs;
                m_HaveBufferedKeyframe = true;
                m_NewKeyframeArrived = false;
            }
            return;
        }
        if (!isKeyframe && !m_RtpVideoSentKeyframe) {
            sendIdrRequestThrottled();
            return;
        }
        if (isKeyframe && m_HaveBufferedKeyframe) m_NewKeyframeArrived = true;
        sendRtpVideo(frameData, isKeyframe, presentationTimeUs, frameNumber);
        return;
    }

    // Buffer keyframes arriving before the Video DC is ready.
    // Without this buffer, the keyframe (containing SPS/PPS) is lost, the
    // browser's VideoDecoder can never configure, and we get decoder=null.
    if (isKeyframe && (!m_VideoDc || !m_VideoDc->isOpen())) {
        // A deep copy on purpose: in direct mode `frameData` may still be a
        // window onto the encoder's buffer, which is gone the moment this
        // returns, and this keyframe has to outlive the call.
        m_BufferedKeyframe = QByteArray(frameData.constData(), frameData.size());
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
        m_AwaitingIdrDropCount++;
        // The buffer is sampled here too, at frame rate: a link coming back is
        // seen on the first delta after it, not on the next keyframe the
        // backed-off cooldown lets through.
        const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
        const size_t gatedBuffered = m_VideoDc->bufferedAmount();
        const bool shedding = m_Backlog.note(gatedBuffered, nowMs);
        if (m_FrameLog)
            m_FrameLog->decide(frameNumber, RelayFrameLog::Outcome::Gated, gatedBuffered,
                               m_Backlog.ageMs(nowMs), steadyUs());
        if (shedding) m_Freezes.note(nowMs - m_Backlog.ageMs(nowMs), nowMs);
        if (m_IdrWaitsForDrain) {
            if (!m_Backlog.backedUp()) requestIdrOnDrain();
        } else if (shedding) {
            // A request now would only produce a keyframe for the gate to drop.
            m_IdrWaitsForDrain = true;
            m_IdrWaitSinceMs = nowMs;
        } else {
            // Throttle absorbs bursts; keeps requesting until the IDR arrives.
            sendIdrRequestThrottled();
        }
        return;
    }

    // If a new keyframe arrives directly while a buffered keyframe exists,
    // mark it as stale. Delta frames do NOT invalidate the buffer — they
    // are useless without a keyframe, so we must still send the buffered
    // one when sendBufferedKeyframe() fires.
    if (isKeyframe && m_HaveBufferedKeyframe) {
        m_NewKeyframeArrived = true;
    }

    sendFragmented(frameData, isKeyframe, m_VideoDc, presentationTimeUs, frameNumber);
}

// --- Buffered keyframe ---

void DataChannelRelay::sendBufferedKeyframe()
{
    // Same lock as onVideoFrame: in direct mode the buffered keyframe is
    // written from the capture thread while this runs on the relay thread.
    std::lock_guard<std::mutex> lk(m_VideoMutex);

    if (!m_HaveBufferedKeyframe) return;
    if (m_RtpVideoAccepted.load()) {
        // The RTP track's turn (U1.4): it calls back here when it opens.
        if (m_Stopping.load() || !m_VideoTrack->isOpen()) return;
        if (!m_NewKeyframeArrived) sendRtpVideo(m_BufferedKeyframe, true, m_BufferedKeyframePresUs);
        m_BufferedKeyframe.clear();
        m_HaveBufferedKeyframe = false;
        m_NewKeyframeArrived = false;
        return;
    }
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

    // Stale references — the same hazard MediaTrackRelay::sendBufferedKeyframe()
    // already guards. Deltas were dropped while the channel was closed, so the
    // live deltas that follow this keyframe ON THE WIRE are not its GOP
    // continuation: they reference frames the browser will never receive.
    // Nothing downstream can notice, because frameId is handed out at send time
    // (sendFragmented) — the browser sees a contiguous sequence, feeds the
    // orphan delta to a decoder that has no reference for it, and reports
    // "VideoDecoder error: Decoding error." one frame after the first decoded
    // one. It then tears the decoder down and recovers, once per session.
    //
    // Send the keyframe regardless: it is the freshest displayable image, and
    // Sunshine only encodes on damage, so on a static host screen the fresh IDR
    // we ask for below may be a long time coming — discarding this one would
    // mean showing nothing until the screen changes. Only the delta gate stays
    // closed, until a genuinely fresh keyframe lands.
    const bool staleReferences = m_AwaitingIdr;

    qInfo() << "[DataChannelRelay] Sending buffered keyframe, size=" << m_BufferedKeyframe.size()
            << (staleReferences ? "(stale references — delta gate stays closed)" : "");
    sendFragmented(m_BufferedKeyframe, true, m_VideoDc, m_BufferedKeyframePresUs);
    if (staleReferences) {
        // sendFragmented() opens the gate on every keyframe it sends; this one
        // has not earned it. Re-arm and keep asking — each gated delta
        // re-requests, so recovery costs at most one cooldown window.
        m_AwaitingIdr = true;
        sendIdrRequestThrottled();
    }
    m_BufferedKeyframe.clear();
    m_BufferedKeyframePresUs = -1;
    m_HaveBufferedKeyframe = false;
    m_NewKeyframeArrived = false;
}

// --- Audio forwarding ---

void DataChannelRelay::onAudioSample(const QByteArray& data)
{
    if (m_Stopping.load()) return;
    // `audiolog=1`: when the packet reached this thread, before the lock.
    const int64_t inUs = m_AudioLog ? steadyUs() : 0;

    // Serialize against track teardown in stop().
    std::lock_guard<std::mutex> lk(m_AudioMutex);
    if (m_Stopping.load() || !m_AudioTrack || !m_AudioTrack->isOpen()) {
        static int notReady = 0;
        if (++notReady <= 3)
            qInfo() << "[DataChannelRelay] onAudioSample dropped — audio track not ready";
        if (m_AudioLog)
            AudioPathLog::instance().relayed(static_cast<size_t>(data.size()), inUs, 0,
                                             AudioPathLog::Outcome::NotReady, m_AudioRtpTs);
        return;
    }

    // Send the Opus packet as one RTP frame; the OpusRtpPacketizer wraps it.
    const uint32_t rtpTs = m_AudioRtpTs;
    auto frameInfo = std::make_shared<rtc::FrameInfo>(m_AudioRtpTs);
    // Advance by the negotiated Opus frame size (48 kHz clock): one clean tick per
    // packet. A jittery arrival-time clock makes NetEq time-stretch → robotic audio.
    m_AudioRtpTs += static_cast<uint32_t>(m_Shim ? m_Shim->audioSamplesPerFrame() : 240);

    rtc::binary bin(static_cast<size_t>(data.size()));
    if (data.size() > 0)
        std::memcpy(bin.data(), data.constData(), static_cast<size_t>(data.size()));

    AudioPathLog::Outcome outcome = AudioPathLog::Outcome::Sent;
    try {
        m_AudioTrack->sendFrame(std::move(bin), *frameInfo);
    } catch (const std::exception& e) {
        outcome = AudioPathLog::Outcome::Error;
        if (!m_Stopping.load())
            qWarning() << "[DataChannelRelay] audio sendFrame error:" << e.what();
    }
    if (m_AudioLog)
        AudioPathLog::instance().relayed(static_cast<size_t>(data.size()), inUs, steadyUs(),
                                         outcome, rtpTs);
}

void DataChannelRelay::onShimConnectionTerminated(int errorCode)
{
    qInfo() << "[DataChannelRelay] Shim connection terminated, code=" << errorCode;
    // Graceful termination: the host ended the stream on purpose, typically
    // because the app was closed there. Say so before the channels drop, or
    // the browser reads the close as a connection error. Any other code is
    // the host side failing (Sunshine crashed, its encoder hung the GPU, the
    // native engine died): the browser link is fine, so say which side went.
    sendExitNotice(errorCode == ML_ERROR_GRACEFUL_TERMINATION ? "host-ended" : "host-lost");
    if (!m_Stopping.exchange(true)) {
        m_Connected = false;
        emit sessionEnded();
    }
}

// --- Input handling ---
// Runs on the relay thread for a GameStream engine (queued from the
// libdatachannel callback), on the libdatachannel thread itself under
// m_InputMutex for the native engine. Nothing in here may assume the relay
// thread: engine input calls are thread-safe by IMediaEngine's contract, the
// clipboard bridge hops to the main thread on its own, and the one piece of
// video state touched (requestidr) goes under m_VideoMutex.

void DataChannelRelay::onInputMessage(const std::string& message, int64_t recvUs)
{
    if (m_Stopping.load() || !m_Connected) return;

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(message));
    if (!doc.isObject()) {
        qWarning() << "[DataChannelRelay] Invalid input JSON";
        return;
    }

    QJsonObject msg = doc.object();
    QString type = msg["type"].toString();

    // Any message at all proves the input link is alive — feeds the shim's
    // dead-man switch, which releases held inputs when this goes silent. A
    // silence it fired on was the upstream half of a link freeze.
    if (m_Shim) {
        if (const qint64 silentMs = m_Shim->noteClientAlive(); silentMs > 0) {
            const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
            m_Freezes.note(nowMs - silentMs, nowMs);
        }
    }

    // An invited player only gets what the owner ticked. Dropped in silence.
    if (!InputMsg::allowed(type, m_InputPolicy)) return;

    // A message the client stamped (the click → flag probe, the uplink bench)
    // is answered once handled, whatever its type: when it arrived, and, by the
    // reply's own time, when its handling (the injection) ended. Measurement
    // only — nothing else sends a stamp, and the input itself is unchanged.
    //
    // Built in place and never copied: until 08/10/2026 it was emplaced from a
    // temporary, whose destructor answered at once — "handled" 40 µs after it
    // arrived, before the injection had even begun — and the client, keeping
    // the first answer, read ~0 ms of injection where SendInput took 8.
    struct StampReply
    {
        StampReply(DataChannelRelay* r, double i, int64_t u)
            : relay(r)
            , id(i)
            , recvUs(u)
        {}
        StampReply(const StampReply&) = delete;
        StampReply& operator=(const StampReply&) = delete;
        ~StampReply() { relay->sendInputStamp(id, recvUs); }
        DataChannelRelay* relay;
        double id;
        int64_t recvUs;
    };
    std::optional<StampReply> stampReply;
    if (const QJsonValue stamp = msg.value(QStringLiteral("stamp")); stamp.isDouble())
        stampReply.emplace(this, stamp.toDouble(), recvUs > 0 ? recvUs : steadyUs());

    if (type == "cursormode") {
        // Who draws the mouse pointer. In desktop mode the browser draws its
        // own — at its own refresh rate, independent of the stream — and the
        // host leaves the picture clean. In gaming mode pointer lock takes
        // the viewer's real pointer away, so the only one that can exist is the
        // one burned into the frame.
        //
        // A touch screen has no pointer device, so CSS `cursor` draws nothing
        // there: the client either has the host composite the pointer (then
        // `cursorPx` is how wide it should end up in frame pixels — a pointer
        // at its desktop size is a few screen pixels across on a phone) or
        // draws an image of it itself, moved from the finger and corrected by
        // the host's `cursorpos` reports.
        //
        // Native host only; every other backend ignores it, since no remote
        // GameStream host can be told to stop drawing its cursor.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setCompositeCursor(msg["composite"].toBool(true), msg["cursorPx"].toInt(0));
        return;
    }

    if (type == "unblockinput") {
        // The viewer is locked out — an elevated window holds the foreground
        // and Windows refuses every event, the pointer's included, so they
        // cannot click their way to another window. This asks the shell to
        // minimise the desktop, which is not input and so is not refused.
        //
        // Behind the keyboard/mouse policy above, deliberately: it moves the
        // host's windows about, which is exactly what a view-only guest may
        // not do. A guest who DOES have keyboard and mouse could reach the
        // shell's own Show Desktop with the pointer anyway, when the pointer
        // works — this is that same act, for when it does not.
        //
        // Native host only: no remote GameStream host exposes such a thing.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim)) native->releaseInputBlock();
        return;
    }

    if (type == "framefloor") {
        // How fast the client wants frames to keep coming while nothing on the
        // host's screen moves. It is the side that knows what its situation is
        // worth: a desktop being worked on wants a still picture to keep
        // settling, a phone on a metered link does not, and a locked pointer
        // over a paused game wants the stream to keep feeling alive. The host
        // clamps it to the frame rate the viewer chose — see
        // Session::setFrameFloorFps.
        //
        // Native host only: no remote GameStream host can be told how to pace
        // its own capture.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setFrameFloorFps(msg["fps"].toInt(0));
        return;
    }

    if (type == "clientrefresh") {
        // The client's screen changed under the stream — its window moved to
        // another monitor, or tearing was switched. Its refresh in millihertz
        // and whether it paints on vsync; the native engine re-chooses the
        // stream's cadence so frames land on the client's grid. See
        // Session::setClientRefresh.
        //
        // Native host only: a GameStream host's cadence is its own.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setClientRefresh(msg["mhz"].toInt(0), msg["vsync"].toBool(false));
        return;
    }

    if (type == "clientfpscap") {
        // The client's decoder keeps a queue it never empties: it asks for
        // fewer frames than the viewer set rather than drop them and pay a
        // keyframe each time. Zero lifts the cap. See Session::setClientFpsCap.
        //
        // Native host only: a GameStream host's frame rate is fixed at launch.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setClientFpsCap(msg["fps"].toInt(0));
        return;
    }

    if (type == "fpsstep") {
        // "Auto"'s detection (frontend CadenceStepper.js): the client asks
        // for the stream to run above its own rate while it measures whether
        // what it shows gets younger — or, with 0, to come back. The session
        // answers at once and the answer goes straight back, so a trial starts
        // and ends within one round trip. See Session::setClientFpsStep.
        //
        // Native host only: a GameStream host's frame rate is fixed at launch.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim)) {
            const mw::native::FpsStep step = native->setClientFpsStep(msg["fps"].toInt(0));
            QJsonObject reply;
            reply["type"] = "fpsstep";
            reply["fps"] = step.fps;
            reply["asked"] = step.askedFps;
            reply["verdict"] = step.verdict == mw::native::FpsStep::Verdict::Applied  ? "applied"
                               : step.verdict == mw::native::FpsStep::Verdict::Capped ? "capped"
                               : step.verdict == mw::native::FpsStep::Verdict::Base   ? "base"
                                                                                      : "refused";
            if (step.why && *step.why) reply["why"] = QString::fromUtf8(step.why);
            const QByteArray json = QJsonDocument(reply).toJson(QJsonDocument::Compact);
            if (m_InputDc && !m_Stopping.load()) {
                try {
                    m_InputDc->send(std::string(json.constData(), json.size()));
                } catch (const std::exception& e) {
                    if (!m_Stopping.load())
                        qWarning() << "[DataChannelRelay] fpsstep reply error:" << e.what();
                }
            }
        }
        return;
    }

    if (type == "decodequeue") {
        // Frames waiting at the client's decoder, said when a second one
        // does and when it is back to one. A host streaming at its own
        // display's rate skips presents on it rather than let them queue
        // there (cadence=host-guarded). See Session::setClientDecodeQueue.
        //
        // Native host only: a GameStream host sends what it captures.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setClientDecodeQueue(msg["depth"].toInt(0));
        return;
    }

    if (type == "vsyncgrid") {
        // When the client's screen refreshes, on this host's clock, and the
        // lead a frame needs to be ready there: the host takes one picture
        // per refresh that lead before it (cadence=deadline). See
        // Session::setClientVsyncGrid.
        //
        // Native host only: a GameStream host sends what it captures.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setClientVsyncGrid(
                msg["periodUs"].toDouble(0), static_cast<int64_t>(msg["phaseUs"].toDouble(0)),
                static_cast<int64_t>(msg["leadUs"].toDouble(0)), msg["tearing"].toBool(false),
                // A page from before the field aims.
                msg["steady"].toBool(true), msg["budgetFps"].toDouble(0));
        return;
    }

    if (type == "clientbitrate") {
        // The viewer's automatic bitrate follows the frame the host really
        // streams (its own display's size under Auto, a mode change): the
        // ceiling moves between two frames, no relaunch. Native host only —
        // a GameStream host takes a bitrate at launch and nowhere else.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setClientBitrate(msg["kbps"].toInt(0));
        return;
    }

    if (type == "linkstats") {
        // The receiver's view of the link, twice a second: how much later
        // frames arrive than at the best of the session (the queue building
        // in the transport, in milliseconds, before anything is lost) and the
        // frames it never got. The native engine's rate governor lowers the
        // encoder's target on it and raises it back through quiet — between
        // two frames, nothing renegotiated. See mw::native::LinkFeedback.
        //
        // Native host only: a GameStream host's rate is Sunshine's business.
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim)) {
            mw::native::LinkFeedback fb;
            // The receiver times frames from their capture, so a slower encode
            // reads there as a filling link: the host's own rise comes out
            // first (HostLagTracker.h).
            int64_t hostRiseMs = 0;
            {
                std::lock_guard<std::mutex> lk(m_VideoMutex);
                hostRiseMs =
                    m_HostLag.takeRise(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
            }
            fb.owdRiseMs = std::max(0, msg["owdRiseMs"].toInt(0) - static_cast<int>(hostRiseMs));
            m_LinkQueueMs.store(fb.owdRiseMs, std::memory_order_relaxed);
            fb.gaps = msg["gaps"].toInt(0);
            fb.receivedFps = msg["fps"].toInt(0);
            // What SCTP sent again since the last report, per thousand chunks:
            // the rate governor cuts on it (retrcut=, 3 by default on the
            // Windows host; plan Wi-Fi W2 B).
            {
                const mw::sctp::Counters now = mw::sctp::readCounters();
                if (m_LinkSctpSet) {
                    const mw::sctp::Counters d = now - m_LinkSctp;
                    fb.retransPermille =
                        d.sent > 0 ? static_cast<int>(int64_t(d.retrans) * 1000 / d.sent) : 0;
                }
                m_LinkSctp = now;
                m_LinkSctpSet = true;
                // The audio road's chunks the page asked again, per thousand
                // sent: the same signal, where SCTP has no say.
                const int64_t sent = m_AroadSent.load(std::memory_order_relaxed);
                const int64_t resent = m_AroadResent.load(std::memory_order_relaxed);
                const int64_t dSent = sent - m_LinkAroadSent;
                const int64_t dResent = resent - m_LinkAroadResent;
                m_LinkAroadSent = sent;
                m_LinkAroadResent = resent;
                if (dSent > 0)
                    fb.retransPermille =
                        std::max(fb.retransPermille, static_cast<int>(dResent * 1000 / dSent));
            }
            // Present, and true, only on the first report after the page came
            // back from the background (StreamView._resyncAfterHidden).
            fb.resumed = msg["resumed"].toBool(false);
            // The reports come on a timer, whatever the viewer is doing: two
            // of them missing is the link frozen upstream, held key or not —
            // the input watchdog only sees a silence while something is held.
            // The one report the browser itself delayed says so (`resumed`).
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (m_LastLinkstatsMs > 0 && !fb.resumed &&
                nowMs - m_LastLinkstatsMs >= kLinkstatsSilentMs) {
                m_Freezes.note(m_LastLinkstatsMs + kLinkstatsPeriodMs, nowMs);
            }
            m_LastLinkstatsMs = nowMs;
            if (!m_LinkReportsSeen) {
                // Once: the loop is closed. The governor logs its own moves.
                m_LinkReportsSeen = true;
                qInfo() << "[DataChannelRelay] link reports flowing from the receiver (first:"
                        << "owdRise" << fb.owdRiseMs << "ms, gaps" << fb.gaps << ", fps"
                        << fb.receivedFps << ")";
            }
            native->reportLink(fb);
        }
        return;
    }

    if (type == "keydown" || type == "keyup") {
        bool down = (type == "keydown");
        const InputMsg::KeyPlan plan = InputMsg::resolveKey(msg, m_Shim->keyboardMode());
        InputMsg::logKey(msg, m_Shim->keyboardMode(), plan, down);
        if (plan.isText()) {
            // The client's layout puts a character here that this key's US
            // position would not produce. The engine decides what it can do
            // with that: a real press/release on the native host, a one-shot
            // text injection on Sunshine (where the release has nothing to do).
            m_Shim->sendKeyChar(plan.text, down, InputMsg::modifierMask(msg));
        } else {
            // `hold`: a movement key the client wants kept down through a brief
            // stall instead of released at the short grace period (see the shim).
            m_Shim->sendKeyEvent(plan.keyCode, down, InputMsg::modifierMask(msg), plan.flags,
                                 msg["hold"].toBool(false));
        }
    } else if (type == "inputstate") {
        // Client heartbeat: its authoritative held-input state, sent while (and
        // only while) something is held. Re-presses whatever the watchdog
        // released during a stall but the user is genuinely still holding.
        const InputMsg::HeldState held =
            InputMsg::readHeldState(msg, m_Shim->keyboardMode(), m_InputPolicy);
        m_Shim->syncHeldInputs(held.keys, held.buttons, held.buttonsHold);
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
        m_Shim->sendMouseButton(down, button, msg["hold"].toBool(false));
    } else if (type == "mousewheel") {
        short delta = static_cast<short>(msg["delta"].toInt(0));
        m_Shim->sendMouseScroll(delta);
    } else if (type == "mousehwheel") {
        short delta = static_cast<short>(msg["delta"].toInt(0));
        m_Shim->sendMouseHScroll(delta);
    } else if (type == "secureattention") {
        // Ctrl+Alt+Suppr: the one combination a browser can never deliver — the
        // OS running the viewer eats it first — so it arrives as a message of
        // its own, from the touch keyboard bar (Ctrl and Alt down, then Del).
        // Whether it reaches the host's secure desktop
        // depends on the host (IMediaEngine::sendSecureAttention).
        m_Shim->sendSecureAttention();
    } else if (type == "textinput") {
        // Virtual/soft keyboard text (UTF-8) — forwarded as a text event.
        m_Shim->sendUtf8Text(msg["text"].toString());
    } else if (type == "locksync") {
        // Align the host's toggle locks (Num/Caps/Scroll) with the client's,
        // sent once per session on the browser's first real keyboard event.
        m_Shim->syncLockKeys(msg["numLock"].toBool(false), msg["capsLock"].toBool(false),
                             msg["scrollLock"].toBool(false));
    } else if (type == "clipboardpaste") {
        // Browser Ctrl/Cmd+V: commit the client text to the host clipboard,
        // then inject the paste chord (main-thread hop keeps that order).
        // Only meaningful when the streamed host is this machine.
        if (m_ClipboardEnabled) {
            ClipboardBridge::instance()->pasteFromClient(m_Shim, msg["text"].toString(),
                                                         msg["injectCtrl"].toBool(false));
        }
    } else if (type == "aroadnack") {
        resendAudioRoad(msg);
    } else if (type == "aroadack") {
        // {"type":"aroadack","t":"v"|"u","s":seq,"i":index}: the last chunk
        // the page's worker received, for the road's window (aroadwin=).
        if (m_AroadPacer) {
            m_AroadPacer->ack(static_cast<uint16_t>(msg["s"].toInt()),
                              static_cast<uint16_t>(msg["i"].toInt()));
            if (++m_AroadAcks % 3000 == 0)
                qInfo() << "[DataChannelRelay] audio road window:" << m_AroadPacer->inFlight()
                        << "bytes in flight, frames dropped" << m_AroadPacer->droppedFrames()
                        << "| resets" << m_AroadPacer->windowResets() << "| longest wait"
                        << m_AroadPacer->maxQueueUs() / 1000 << "ms";
        }
    } else if (type == "requestidr") {
        qInfo() << "[DataChannelRelay] Requesting IDR frame from Sunshine (browser request)";
        std::lock_guard<std::mutex> lk(m_VideoMutex);
        m_AwaitingIdr = true;
        sendIdrRequestThrottled();
    } else if (type == "invalidateref") {
        // The receiver names the wire ids it never got, `from`..`to`
        // inclusive. For the native engine each one becomes a reference
        // invalidation — the next delta predicts from older frames and the
        // picture heals without a keyframe (design §9.2). Anything the ring
        // no longer remembers, or a hole wider than a DPB could cover, is
        // answered the old way. Never for a GameStream engine, which has no
        // such call: the receiver only sends this when /start said it could.
        auto* native = qobject_cast<NativeMediaEngine*>(m_Shim);
        const qint64 from = msg["from"].toDouble(-1);
        const qint64 to = msg["to"].toDouble(-1);
        bool healed = false;
        if (native && from >= 0 && to >= from && to - from < 16) {
            healed = true;
            for (qint64 id = from; id <= to; ++id) {
                const int64_t slot =
                    m_FrameNumberById[static_cast<uint32_t>(id) % kFrameNumberRing].load(
                        std::memory_order_acquire);
                if (slot < 0 || (slot >> 32) != id) {
                    healed = false;
                    break;
                }
                native->invalidateReference(static_cast<uint32_t>(slot & 0xffffffff));
            }
        }
        if (!healed) {
            qInfo() << "[DataChannelRelay] invalidateref" << from << ".." << to
                    << "could not be mapped — requesting a keyframe instead";
            std::lock_guard<std::mutex> lk(m_VideoMutex);
            m_AwaitingIdr = true;
            sendIdrRequestThrottled();
        }
    } else if (type == "ping") {
        // Respond with pong on the input DataChannel.
        // The ts field mirrors the browser's timestamp so it can compute RTT.
        int seq = msg["seq"].toInt(0);
        double ts = msg["ts"].toDouble(0);
        QJsonObject pong;
        pong["type"] = "pong";
        pong["seq"] = seq;
        pong["ts"] = ts;
        // The host's steady clock as it answers, in µs: the clock frames are
        // stamped on (backendTs), for a client that puts its own on it — the
        // content-age probe (frontend ContentAgeProbe.js). Exact in a double.
        pong["host"] = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
        // cadence=deadline (native host): whether the host wants the client's
        // refresh grid, and whether it aims its pictures at it right now — the
        // client drops its render reserve and counts its misses only then
        // (frontend VsyncGrid.js).
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim)) {
            const auto grid = native->vsyncGridStatus();
            if (grid.wanted) pong["grid"] = true;
            if (grid.followed) {
                QJsonObject deadline;
                deadline["presentUs"] = grid.presentUs;
                deadline["aimed"] = grid.aimed;
                pong["deadline"] = deadline;
            }
        }
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
        // The pad is gone. What that means depends on the engine — a cleared
        // mask bit for a GameStream host, an actual unplug for the native one,
        // which owns the virtual device — so the intent is sent, not one
        // engine's encoding of it.
        m_Shim->sendControllerRemoval(static_cast<uint8_t>(msg["index"].toInt(0)),
                                      static_cast<uint16_t>(msg["mask"].toInt(0)));
    } else if (type == "hidattach") {
        // A device the page reads through WebHID, to recreate here. The answer
        // says whether it was, and why not: the page shows it by the device.
        const int slot = msg["slot"].toInt(-1);
        const QString why = (slot < 0 || slot > 7) ? QStringLiteral("slot out of range")
                                                   : m_Shim->hidAttach(slot, msg);
        QJsonObject r{{QStringLiteral("type"), QStringLiteral("hidattached")},
                      {QStringLiteral("slot"), slot},
                      {QStringLiteral("ok"), why.isEmpty()},
                      {QStringLiteral("why"), why}};
        const QByteArray j = QJsonDocument(r).toJson(QJsonDocument::Compact);
        try {
            if (m_InputDc) m_InputDc->send(std::string(j.constData(), j.size()));
        } catch (const std::exception&) {}
    } else if (type == "hiddetach") {
        m_Shim->hidDetach(msg["slot"].toInt(-1));
    } else if (type == "hidreply") {
        // The real device's answer to an hidrequest (a HID++ reply under
        // Linux): one report, at most the 63 bytes of HID++'s longest.
        const QByteArray data = QByteArray::fromBase64(msg["data"].toString().toLatin1()).left(64);
        m_Shim->hidReply(msg["slot"].toInt(-1), msg["reportId"].toInt(-1), data);
    } else if (type == "uprobe") {
        // The uplink bench's dated message: nothing to inject, only its stamp
        // to answer (above).
    } else {
        qWarning() << "[DataChannelRelay] Unknown input type:" << type;
    }
}

void DataChannelRelay::sendInputStamp(double id, int64_t recvUs)
{
    QJsonObject reply;
    reply["type"] = "inputstamp";
    reply["id"] = id;
    reply["recv"] = static_cast<double>(recvUs);
    const int64_t doneUs = steadyUs();
    reply["done"] = static_cast<double>(doneUs);
    // The bench's click trace: the same two stamps on this side, so the host's
    // own files can date a click's arrival without the client's.
    if (m_ClickTraceLog)
        qInfo().noquote() << QStringLiteral("[DataChannelRelay] click trace: input stamp %1 "
                                            "received at steady %2 us, handled at %3 us")
                                 .arg(static_cast<qint64>(id))
                                 .arg(recvUs)
                                 .arg(doneUs);
    const QByteArray json = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    if (!m_InputDc || m_Stopping.load()) return;
    try {
        m_InputDc->send(std::string(json.constData(), json.size()));
    } catch (const std::exception& e) {
        if (!m_Stopping.load())
            qWarning() << "[DataChannelRelay] inputstamp send failed:" << e.what();
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
                                      qint64 presentationTimeUs, int frameNumber)
{
    if (m_Stopping.load() || !dc || !dc->isOpen()) return;
    if (data.isEmpty()) return;

    // Track decode pipeline latency: time from frameSubmitUs (set in
    // the media engine's frame callback) to actual send over WebRTC.
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

    // Backpressure: drop delta frames when the transport has been unable to
    // drain for longer than SendBacklog::kToleranceMs. ⚠️ the old comment here
    // said this existed to stop dc->send() blocking the Qt event loop — it does
    // not block, the SCTP socket is non-blocking and libdatachannel queues.
    // What this actually bounds is LATENCY, which is why it is now measured in
    // time; see SendBacklog.h.
    const int64_t backlogNowMs = QDateTime::currentMSecsSinceEpoch();
    size_t bufferedSeen = 0; // for the bench's frame log
    if (!isKeyframe) {
        size_t bufAmt = dc->bufferedAmount();
        bufferedSeen = bufAmt;
        if (m_Backlog.note(bufAmt, backlogNowMs)) {
            m_DeltaDroppedCount++;
            m_BackpressureDropCount++;
            m_Freezes.note(backlogNowMs - m_Backlog.ageMs(backlogNowMs), backlogNowMs);

            // The engine that heals by reference invalidation can be told
            // which frame never left (namedrops, plan §9-25). No wire id was
            // spent on it, so the client sees no hole; the next delta predicts
            // from a frame the client has, and neither a keyframe nor the
            // refresh wave is waited for. The same gesture as the sender's
            // evictions (design §9.10.2): a stall longer than the encoder's
            // references reach ends in the keyframe the session asks for.
            auto* native = qobject_cast<NativeMediaEngine*>(m_Shim);
            const bool named = native && frameNumber >= 0 && native->nameLinkDrops() &&
                               native->referenceInvalidation();
            if (named) {
                native->invalidateReference(static_cast<uint32_t>(frameNumber));
                m_NamedDeltaDropCount++;
            }

            // Set sticky awaiting state. The keyframe is asked for when the
            // buffer drains (requestIdrOnDrain): asked for now, it would come
            // back a frame later into the same backed-up buffer and be dropped
            // by the keyframe gate below.
            //
            // Skipped entirely when the stream refreshes by intra-refresh and
            // the client rides out damage. This is the single place the two
            // strategies differ most: the buffer is full BECAUSE the link is
            // saturated, and the classic answer to that is to ask for the
            // largest frame the encoder can make. MediaTrackRelay documents
            // where that leads — "every stall became an IDR storm […] the
            // stream collapsed outright". Riding out drops one delta and lets
            // the refresh wave repair the picture instead. A named drop needs
            // neither.
            if (!named && !ridingOutLoss()) {
                m_AwaitingIdr = true;
                if (!m_IdrWaitsForDrain) {
                    m_IdrWaitsForDrain = true;
                    m_IdrWaitSinceMs = backlogNowMs;
                }
            }

            if (m_DeltaDroppedCount <= 3 || m_DeltaDroppedCount % 120 == 0) {
                qInfo() << "[DataChannelRelay] Dropped delta frame (link not draining)"
                        << "bufferedAmount=" << bufAmt
                        << "backlogMs=" << m_Backlog.ageMs(backlogNowMs)
                        << "totalDropped=" << m_DeltaDroppedCount
                        << (named ? "named to the encoder" : "");
            }
            if (m_FrameLog)
                m_FrameLog->decide(frameNumber,
                                   named ? RelayFrameLog::Outcome::NamedDrop
                                         : RelayFrameLog::Outcome::BacklogDrop,
                                   bufAmt, m_Backlog.ageMs(backlogNowMs), steadyUs());
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
        // m_AwaitingIdr stays sticky, and the next keyframe is asked for the
        // moment the buffer drains (the awaiting-IDR gate watches it at frame
        // rate), so it goes through fresh. This caps the buffer at ~watermark
        // + one keyframe, bounding latency to well under a second instead of
        // letting it run away.
        size_t bufAmt = dc->bufferedAmount();
        bufferedSeen = bufAmt;
        if (m_Backlog.note(bufAmt, backlogNowMs)) {
            m_KeyframeBackpressureWarnings++;
            if (m_FrameLog)
                m_FrameLog->decide(frameNumber, RelayFrameLog::Outcome::KeyframeDrop, bufAmt,
                                   m_Backlog.ageMs(backlogNowMs), steadyUs());
            m_Freezes.note(backlogNowMs - m_Backlog.ageMs(backlogNowMs), backlogNowMs);
            if (m_KeyframeBackpressureWarnings <= 5) {
                qInfo() << "[DataChannelRelay] Dropped keyframe (link not draining)"
                        << "bufferedAmount=" << bufAmt
                        << "backlogMs=" << m_Backlog.ageMs(backlogNowMs)
                        << "warnCount=" << m_KeyframeBackpressureWarnings;
            }
            m_AwaitingIdr = true;
            // The request was honoured — this is the keyframe it produced —
            // and the drop is ours, not the link's: nothing to back off for.
            m_IdrOutstanding = false;
            if (!m_IdrWaitsForDrain) {
                m_IdrWaitsForDrain = true;
                m_IdrWaitSinceMs = backlogNowMs;
            }
            return;
        }
        // Keyframe sent successfully: clear sticky state and backpressure counters,
        // and reset the IDR cooldown backoff (recovery completed).
        m_AwaitingIdr = false;
        m_IdrWaitsForDrain = false;
        m_BackpressureDropCount = 0;
        m_IdrOutstanding = false;
        m_IdrCooldownMs = kIdrCooldownBaseMs;
        // A keyframe that got through IS the fresh start: whatever backlog the
        // recovery built up stops counting against the next frames.
        m_Backlog.reset();
    }

    // Video-only path now (audio is a native RTP Opus track, not fragmented over
    // a DataChannel), so this always uses the video frameId sequence.
    uint32_t frameId = m_FrameId++;
    if (m_FrameLog) {
        m_FrameLog->decide(frameNumber, RelayFrameLog::Outcome::Sent, bufferedSeen,
                           m_Backlog.ageMs(backlogNowMs), steadyUs());
        m_FrameLog->sent(frameNumber, frameId);
    }
    // Remember which engine frame went out under this wire id, so a receiver
    // naming a lost id can be answered with a reference invalidation. The slot
    // packs both so a stale entry from 512 frames ago is never mistaken for
    // the current id.
    m_FrameNumberById[frameId % kFrameNumberRing].store(
        frameNumber >= 0
            ? (static_cast<int64_t>(frameId) << 32) | static_cast<uint32_t>(frameNumber)
            : -1,
        std::memory_order_release);

    // ── End-to-end latency timestamp
    // ───────────────────────────────────────
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
            // What the host itself added to the delay the receiver will see
            // on this frame — taken back out of its link report.
            const int64_t nowSteadyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count();
            m_HostLag.note(nowSteadyMs - captureSteadyMs, nowSteadyMs);
        }
    }
    // Fallback: use current wall time if steady_clock not yet initialized
    if (backendTs == 0) {
        backendTs = static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);
    }

    // dc->send() belongs to the dedicated sender thread: it can block on a full
    // SCTP buffer, and neither the relay thread (HTTP/REST/signaling) nor the
    // capture thread may ever wait on the wire. The job holds a shared_ptr
    // copy of the DC, so an in-flight send cannot outlive the channel during
    // stop().
    //
    // If the sender queue was full and a queued delta got evicted, that delta
    // already carries a frameId: the frontend will see a frameId gap, and every
    // delta still queued behind the hole references a frame that will never
    // arrive. Start IDR recovery now instead of waiting a full round-trip for
    // the frontend to detect the gap and ask.
    // The engine that stamps its frames gets told when each one left; the
    // others hand over a null sink and the sender reads no clock for them.
    FrameSentSink* sink = (frameNumber >= 0 && m_Shim) ? m_Shim->frameSentSink() : nullptr;
    // The bench's frame log stands between the sender and the engine's sink:
    // it keeps the stamps, then hands them on.
    if (m_FrameLog && frameNumber >= 0) {
        m_FrameLog->setInner(sink);
        sink = m_FrameLog.get();
    }
    const uint32_t reportedNumber = static_cast<uint32_t>(frameNumber < 0 ? 0 : frameNumber);
    // The engine that stamps its frames also wants to know WHICH ones the
    // sender threw away: those are the numbers it can invalidate.
    auto* native = qobject_cast<NativeMediaEngine*>(m_Shim);
    const bool nameEvictions = native && frameNumber >= 0 && native->referenceInvalidation();
    std::vector<uint32_t> evictedNumbers;
    std::vector<uint32_t>* evictedOut = (nameEvictions || m_FrameLog) ? &evictedNumbers : nullptr;
    bool evicted = false;
    if (m_DirectVideoSend) {
        // Direct mode: `data` may be borrowed from the encoder (valid only for
        // this call), so the chunks are cut here, once, straight from it —
        // the header and the slice written together, nothing copied before
        // and nothing left for the sender to copy after.
        auto fragments = FrameSender::buildFragments(
            reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size()),
            isKeyframe, frameId, backendTs, m_ChunkPayload);
        // The bench's losses (`loss=`, LinkLoss.h): messages that never reach
        // SCTP, so the receiver meets holes SCTP knows nothing of. A frame that
        // loses every message still spent its wire id: a gap, as on a link.
        if (m_Loss.active()) {
            const uint64_t before = m_Loss.dropped();
            fragments.erase(
                std::remove_if(fragments.begin(), fragments.end(),
                               [this](const FrameSender::Fragment&) { return m_Loss.dropNext(); }),
                fragments.end());
            const uint64_t dropped = m_Loss.dropped();
            if (dropped != before && (dropped <= 3 || dropped / 500 != before / 500))
                qInfo() << "[DataChannelRelay] bench losses:" << dropped << "messages in"
                        << m_Loss.losses() << "losses so far";
        }
        if (fragments.empty() && m_FrameLog)
            m_FrameLog->decide(frameNumber, RelayFrameLog::Outcome::BenchLoss, bufferedSeen,
                               m_Backlog.ageMs(backlogNowMs), steadyUs());
        evicted = fragments.empty()
                      ? false
                      : m_Sender->enqueueFragments(dc, std::move(fragments), isKeyframe,
                                                   reportedNumber, sink, evictedOut);
    } else {
        // Queued mode: the frame is already ours (copied out of the GameStream
        // engine), the sender cuts it on its own thread as it always has.
        evicted = m_Sender->enqueue(dc, data, isKeyframe, /*isAudio=*/false, frameId, backendTs,
                                    reportedNumber, sink, evictedOut);
    }
    // An eviction leaves a hole in the reference chain. The engine that heals
    // by reference invalidation is told the exact frames now — the sender is
    // the one party that knows them for certain, and this is a round trip
    // earlier than the receiver seeing the gap and naming it (design §9.10);
    // the session turns a refusal into a keyframe itself. Otherwise: a stream
    // that repairs itself by intra-refresh and a client that rides out the
    // damage need no keyframe for it (same bargain as the SCTP drop above);
    // every other stream asks for one. GameStream engines never ride out, so
    // this is exactly their previous behaviour.
    if (m_FrameLog) {
        for (uint32_t n : evictedNumbers)
            m_FrameLog->evicted(n);
    }
    if (evicted && nameEvictions && !evictedNumbers.empty()) {
        for (uint32_t n : evictedNumbers)
            native->invalidateReference(n);
    } else if (evicted && !ridingOutLoss()) {
        m_AwaitingIdr = true;
        sendIdrRequestThrottled();
    }
    // The native engine's rate governor counts evictions as the surest sign
    // the link is full — loss the host caused itself. GameStream engines have
    // no governor to tell.
    if (evicted && native) native->noteEviction();

    // The bench's synthetic Ultra stream rides on the frames the native
    // engine sends (direct mode), with the same stamp.
    if (m_DirectVideoSend && (m_UltraSender || m_UltraTrack)) sendUltraSynthetic(backendTs);

    m_FrameCount++;
}

void DataChannelRelay::sendUltraSynthetic(uint32_t backendTs)
{
    if (m_UltraRtpAccepted.load(std::memory_order_relaxed)) {
        // On its RTP track (U1.4): one frame = a VP8 key frame's 10-byte
        // header (so Chrome assembles every frame on its own, none depending
        // on another), then the DataChannel's 17-byte header for a single
        // chunk, then the payload. The client strips the 10 bytes.
        if (!m_UltraTrack->isOpen() || m_UltraPayload.empty()) return;
        static const uint8_t kVp8Key[10] = {0x10, 0x00, 0x00, 0x9d, 0x01,
                                            0x2a, 0x80, 0x07, 0x38, 0x04}; // 1920x1080
        // On the audio road (U1.4 ter) no VP8 header: the chunks carry it all.
        const size_t prefix = m_UltraAudioRoad ? 0 : sizeof(kVp8Key);
        const uint32_t size = static_cast<uint32_t>(m_UltraPayload.size());
        const uint32_t seq = m_UltraSeq++;
        rtc::binary frame(prefix + kFragHeaderSize + size);
        std::memcpy(frame.data(), kVp8Key, prefix);
        // [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
        auto* h = reinterpret_cast<uint8_t*>(frame.data()) + prefix;
        const auto put32 = [](uint8_t* at, uint32_t v) {
            for (int i = 0; i < 4; ++i)
                at[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
        };
        put32(h, seq);
        h[4] = h[5] = h[6] = 0;
        h[7] = 1;
        h[8] = 0;
        put32(h + 9, size);
        put32(h + 13, backendTs);
        std::memcpy(h + kFragHeaderSize, m_UltraPayload.data(), size);
        rtc::FrameInfo info(backendTs);
        info.isKeyFrame = true;
        try {
            if (m_UltraAudioRoad)
                sendAudioRoad(m_UltraTrack, m_UltraRoadHistory, m_UltraAudioRoadSeq++,
                              reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), true,
                              backendTs);
            else
                m_UltraTrack->sendFrame(std::move(frame), info);
        } catch (const std::exception& e) {
            qWarning() << "[DataChannelRelay] Ultra RTP send failed:" << e.what();
        }
        return;
    }
    if (!m_UltraOpen.load(std::memory_order_relaxed) || m_UltraPayload.empty()) return;
    auto fragments =
        FrameSender::buildFragments(m_UltraPayload.data(), m_UltraPayload.size(),
                                    /*isKeyframe=*/false, m_UltraSeq++, backendTs, m_ChunkPayload);
    m_UltraSender->enqueueFragments(m_UltraDc, std::move(fragments), /*isKeyframe=*/false);
}

// --- Stats timer (1s interval) ---

void DataChannelRelay::onStatsTimerTick()
{
    if (m_Stopping.load() || !m_Connected) return;
    if (!m_InputDc || !m_InputDc->isOpen()) return;
    if (m_FrameLog && m_Pc) {
        if (const auto rtt = m_Pc->rtt()) m_SrttMs.store(static_cast<int>(rtt->count()));
    }

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
    //
    // The counters are written on the video path — the capture thread in
    // direct mode — so the snapshot is taken under its lock. Once a second,
    // for a handful of loads: nothing the capture thread will notice.
    qint64 bpDrops = 0;
    {
        std::lock_guard<std::mutex> lk(m_VideoMutex);
        bpDrops = m_DeltaDroppedCount + m_KeyframeBackpressureWarnings + m_AwaitingIdrDropCount;
        const qint64 workerDrops = m_Shim ? m_Shim->workerDropCount() : 0;
        const qint64 senderDrops = m_Sender ? static_cast<qint64>(m_Sender->queueDropCount()) : 0;
        const qint64 snapshot = workerDrops + senderDrops + bpDrops;
        if (snapshot != m_LastDropSnapshot) {
            m_LastDropSnapshot = snapshot;
            // bufferedAmount rides along: it is what decides every sctp* drop
            // below, and until now it only appeared inside the drop lines
            // themselves — so the run-up to a stall was invisible, and a buffer
            // pinned just above the watermark read like a healthy one.
            qInfo() << "[DataChannelRelay] Drop counters — worker:" << workerDrops
                    << "senderQueue:" << senderDrops << "sctpDelta:" << m_DeltaDroppedCount
                    << "sctpDeltaNamed:" << m_NamedDeltaDropCount
                    << "sctpKeyframe:" << m_KeyframeBackpressureWarnings
                    << "gatedDelta:" << m_AwaitingIdrDropCount
                    << "pendingVideoFrames:" << (m_Shim ? m_Shim->pendingVideoFrames() : 0)
                    << "bufferedAmount:"
                    << (m_VideoDc ? static_cast<qint64>(m_VideoDc->bufferedAmount()) : -1);
        }
    }

    QJsonObject stats;
    stats["type"] = "stats";
    stats["hostRttMs"] = hostRttMs;
    stats["decodeLatencyUs"] = static_cast<qint64>(decodeLatUs);
    // Sunshine capture→encode latency (RTP extension), averaged over the frames
    // of this stats window. 0 when the host doesn't report it.
    stats["hostProcMs"] = m_Shim ? m_Shim->takeHostProcessingLatencyMs() : 0.0;
    // Cumulative frames dropped by SCTP backpressure (deltas + keyframes).
    // The frontend cannot see these drops (dropped frames never get a frameId),
    // so this is its only signal that the link is saturated backend-side. It
    // drives the frontend's congestion monitor (automatic bitrate degradation).
    stats["bpDrops"] = bpDrops;
    // The receiver's own link report with the host's share taken out (see the
    // linkstats handler): what its LINK QUEUE leg shows instead of the raw rise,
    // which counts a slower encode twice. Absent until the first report.
    if (const int linkQueueMs = m_LinkQueueMs.load(std::memory_order_relaxed); linkQueueMs >= 0)
        stats["linkQueueMs"] = linkQueueMs;
    // Link freezes, cumulative: how many, the longest, the latest (ms). Absent
    // until the first one, so a healthy session's message is unchanged.
    if (const LinkFreezeLog::Snapshot fz = m_Freezes.snapshot(); fz.count > 0) {
        QJsonObject freezes;
        freezes["n"] = fz.count;
        freezes["maxMs"] = static_cast<qint64>(fz.maxMs);
        freezes["lastMs"] = static_cast<qint64>(fz.lastMs);
        stats["freezes"] = freezes;
    }
    // Host-side stage latencies (present → acquire → convert → encode → queue
    // → send), mean and tail over this window. Only an engine that stamps its
    // own frames has any; the GameStream message is unchanged.
    if (m_Shim) {
        const QJsonObject stages = m_Shim->takeStageStats();
        if (!stages.isEmpty()) stats["stages"] = stages;
    }
    // What "Auto"'s detection reads of the native cadence (frontend
    // CadenceStepper.js): how fast the captured display presents, the
    // stream's own rate, the step in force, the display's refresh. Absent
    // from every other host, and from a guest on the shared feed.
    if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim)) {
        const mw::native::CadenceStatus c = native->cadenceStatus();
        if (c.baseFps > 0) {
            QJsonObject cadence;
            cadence["presents"] = c.presentsPerSecond;
            cadence["base"] = c.baseFps;
            cadence["step"] = c.stepFps;
            cadence["display"] = c.displayHz;
            stats["cadence"] = cadence;
        }
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

void DataChannelRelay::notifyClientSessionEnded()
{
    sendExitNotice("session-ended");
}

void DataChannelRelay::sendExitNotice(const char* type)
{
    // Control message on the input DC, sent before stop() closes it, so the
    // browser can show a graceful exit instead of a generic disconnect. The
    // channel being reliable/ordered is not enough on its own: send() only
    // queues into SCTP, and the close() that follows may discard it — hence the
    // synchronous flush (see ExitNotice.h).
    if (!m_InputDc || m_Stopping.load()) return;
    QByteArray json = QJsonDocument(QJsonObject{{"type", type}}).toJson(QJsonDocument::Compact);
    if (!exitnotice::sendAndFlush(m_InputDc, std::string(json.constData(), json.size()))) {
        qWarning() << "[DataChannelRelay] Exit notice" << type << "not flushed before teardown";
    }
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

    // Step out of the engine's frame path first, and before taking
    // m_VideoMutex: clearing the sink waits for a frame in flight, and that
    // frame is waiting for the mutex. Done even when we are already stopping
    // (a state-change callback may have set the flag without coming through
    // here), because from this point the engine must emit videoFrameReady
    // again — the WebSocket fallback listens to that signal. Idempotent.
    if (m_DirectVideoSend) {
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setDirectFrameSink(nullptr);
    }
    // linkhold=: a WebSocket fallback must not ask a channel that is going.
    if (m_LinkHoldMs > 0) {
        if (auto* native = qobject_cast<NativeMediaEngine*>(m_Shim))
            native->setLinkBusyProbe(nullptr);
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

    // Video state and the video DC go under the video lock: in direct mode a
    // frame may be halfway through onVideoFrame on the capture thread right
    // now, and it must find either an open channel or a null one — never a
    // channel being reset under it. m_Stopping is already true, so the next
    // frame to take the lock leaves at once.
    {
        std::lock_guard<std::mutex> lk(m_VideoMutex);

        // Reset backpressure counters and IDR state for next session
        m_DeltaDroppedCount = 0;
        m_NamedDeltaDropCount = 0;
        m_KeyframeBackpressureWarnings = 0;
        m_BackpressureDropCount = 0;
        m_AwaitingIdrDropCount = 0;
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

        closeDc(m_VideoDc, "video");
    }

    closeDc(m_InputDc, "input");
    closeDc(m_HidDc, "hid");

    // The bench's Ultra stream: its channel closed first, so a send blocked on
    // it returns, then its sender joined. What the queue threw away is the
    // number the transport lab wants.
    if (m_UltraSender) {
        m_UltraOpen.store(false);
        closeDc(m_UltraDc, "ultra");
        const uint64_t drops = m_UltraSender->queueDropCount();
        m_UltraSender->stop();
        qInfo() << "[DataChannelRelay] bench Ultra synthetic:" << m_UltraSeq << "frames of"
                << m_UltraSynthKb << "KiB queued," << drops << "dropped by the sender's queue";
        m_UltraSender.reset();
    }
    // The bench's flood: its channel closed first, so a send blocked on it
    // errors out and the thread joins at once.
    if (m_Flood) {
        closeDc(m_FloodDc, "flood");
        m_Flood->stop();
        m_Flood.reset();
    }
    if (m_Loss.active())
        qInfo() << "[DataChannelRelay] bench losses:" << m_Loss.dropped() << "video messages in"
                << m_Loss.losses() << "losses this session";

    // Direct input: a message may be halfway through injection on a
    // libdatachannel thread. Wait it out — m_Stopping is set, so the next one
    // to take the lock leaves at once — before the caller goes on to stop the
    // engine the message is injecting into. Taken with no other relay lock
    // held (see the ordering note on m_InputMutex).
    if (m_DirectInput) {
        std::lock_guard<std::mutex> lk(m_InputMutex);
    }

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
    std::lock_guard<std::mutex> lk(m_VideoMutex);
    m_AwaitingIdr = true;
    sendIdrRequestThrottled();
}

void DataChannelRelay::requestIdrOnDrain()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    qInfo() << "[DataChannelRelay] link draining again after" << (nowMs - m_IdrWaitSinceMs)
            << "ms — keyframe requested at once";
    m_IdrWaitsForDrain = false;
    m_IdrOutstanding = false;
    m_IdrCooldownMs = kIdrCooldownBaseMs;
    m_IdrCooldownTimer.invalidate();
    sendIdrRequestThrottled();
}

void DataChannelRelay::sendIdrRequestThrottled()
{
    if (m_Stopping.load() || !m_Shim) return;

    // The gate is dropping keyframes until the buffer moves; the drain asks.
    if (m_IdrWaitsForDrain) return;

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
    qInfo() << "[DataChannelRelay] IDR request → media engine (cooldown" << m_IdrCooldownMs
            << "ms)";
    m_Shim->requestIdrFrame();
}

void DataChannelRelay::onIceCheckTimeout()
{
    if (m_Stopping.load()) return;
    if (m_Connected) return; // Safety: should not happen since we cancel on Connected

    qWarning() << "[DataChannelRelay] ICE timeout — PC did not reach Connected within"
               << m_IceTimeoutMs << "ms."
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

// --- RTP video track (POC Ultra U1.4) ---

void DataChannelRelay::setRtpVideoPolicy(const QString& spec, bool nativeHost)
{
    m_RtpVideoSpec = spec.trimmed().toLower();
    m_RtpVideoNativeHost = nativeHost;
}

bool DataChannelRelay::rtpVideoActive() const
{
    return m_RtpVideoAccepted.load(std::memory_order_relaxed);
}

void DataChannelRelay::createRtpVideoTracks()
{
    if (m_RtpVideoSpec.isEmpty() || !m_Pc || !m_Shim) return;
    // `native:h264+hevc;other:h264`: the section of this host's type.
    QStringList named;
    const QString wanted =
        m_RtpVideoNativeHost ? QStringLiteral("native") : QStringLiteral("other");
    for (const QString& section : m_RtpVideoSpec.split(';', Qt::SkipEmptyParts)) {
        const int colon = section.indexOf(':');
        if (colon < 0 || section.left(colon).trimmed() != wanted) continue;
        for (QString item : section.mid(colon + 1).split('+', Qt::SkipEmptyParts))
            named << item.trimmed();
    }
    const int format = m_Shim->negotiatedVideoFormat();
    const QString codec = (format & VIDEO_FORMAT_MASK_AV1)    ? QStringLiteral("av1")
                          : (format & VIDEO_FORMAT_MASK_H265) ? QStringLiteral("hevc")
                                                              : QStringLiteral("h264");
    const bool ultra = named.contains(QStringLiteral("ultra")) && m_UltraSynthKb > 0;
    qInfo() << "[DataChannelRelay] RTP video policy" << m_RtpVideoSpec << "| host" << wanted
            << "| codec" << codec << "format" << format << "| video on"
            << (named.contains(codec) ? "RTP" : "the DataChannel") << "| ultra on"
            << (ultra ? "RTP" : "-");

    std::random_device rd;
    // The bench's other road (U1.4 ter, item `aroad`): the frames cut in
    // packets on an Opus track. Chrome holds each received video frame for
    // its 64 Hz metronome before the transform; an audio frame goes at once.
    m_RtpVideoAudioRoad = named.contains(codec) && named.contains(QStringLiteral("aroad"));
    if (m_RtpVideoAudioRoad) {
        auto desc = rtc::Description::Audio("vaudio", rtc::Description::Direction::SendOnly);
        const int pt = 110;
        desc.addOpusCodec(pt);
        const uint32_t ssrc = rd();
        desc.addSSRC(ssrc, "vaudio");
        m_VideoTrack = m_Pc->addTrack(desc);
        auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "vaudio", static_cast<uint8_t>(pt), rtc::OpusRtpPacketizer::DefaultClockRate);
        m_VideoTrack->setMediaHandler(std::make_shared<rtc::OpusRtpPacketizer>(cfg));
        m_VideoTrack->onOpen([this]() {
            qInfo() << "[DataChannelRelay] RTP video track open (audio road)";
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    sendBufferedKeyframe();
                    std::lock_guard<std::mutex> lk(m_VideoMutex);
                    if (!m_RtpVideoSentKeyframe) sendIdrRequestThrottled();
                },
                Qt::QueuedConnection);
        });
    } else if (named.contains(codec)) {
        auto desc = rtc::Description::Video("video", rtc::Description::Direction::SendOnly);
        const int pt = 96;
        if (codec == QLatin1String("av1"))
            desc.addAV1Codec(pt);
        else if (codec == QLatin1String("hevc"))
            desc.addH265Codec(pt);
        else
            desc.addH264Codec(pt);
        const uint32_t ssrc = rd();
        desc.addSSRC(ssrc, "video"); // before addTrack: else NACK / PLI never route to it
        m_VideoTrack = m_Pc->addTrack(desc);
        // The frame's backendTs (ms) as the RTP timestamp, as is: see the header.
        auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "video", static_cast<uint8_t>(pt), rtc::RtpPacketizer::VideoClockRate);
        std::shared_ptr<rtc::RtpPacketizer> packetizer;
        if (codec == QLatin1String("av1"))
            packetizer = std::make_shared<rtc::AV1RtpPacketizer>(
                rtc::AV1RtpPacketizer::Packetization::TemporalUnit, cfg);
        else if (codec == QLatin1String("hevc"))
            packetizer = std::make_shared<rtc::H265RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, cfg);
        else
            packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, cfg);
        // ~1 s of packets at 100 Mbit/s, for the browser's NACKs.
        auto nack = std::make_shared<rtc::RtcpNackResponder>(8192);
        packetizer->addToChain(nack);
        // Counted, never acted on: the browser's own decoder never gets a
        // frame (the transform keeps them all), so it asks for a keyframe
        // over and over. Ours asks on the input channel, as on SCTP.
        auto pli = std::make_shared<rtc::PliHandler>([this]() {
            const int n = ++m_RtpPliCount;
            if (n == 1 || n % 200 == 0)
                qInfo() << "[DataChannelRelay] RTP video: browser PLIs ignored so far:" << n;
        });
        nack->addToChain(pli);
        m_VideoTrack->setMediaHandler(packetizer);
        m_VideoTrack->onOpen([this]() {
            qInfo() << "[DataChannelRelay] RTP video track open";
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    sendBufferedKeyframe();
                    std::lock_guard<std::mutex> lk(m_VideoMutex);
                    if (!m_RtpVideoSentKeyframe) sendIdrRequestThrottled();
                },
                Qt::QueuedConnection);
        });
    }
    m_UltraAudioRoad = ultra && named.contains(QStringLiteral("aroad"));
    if (m_UltraAudioRoad) {
        auto desc = rtc::Description::Audio("uaudio", rtc::Description::Direction::SendOnly);
        const int pt = 109;
        desc.addOpusCodec(pt);
        const uint32_t ssrc = rd();
        desc.addSSRC(ssrc, "uaudio");
        m_UltraTrack = m_Pc->addTrack(desc);
        auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "uaudio", static_cast<uint8_t>(pt), rtc::OpusRtpPacketizer::DefaultClockRate);
        m_UltraTrack->setMediaHandler(std::make_shared<rtc::OpusRtpPacketizer>(cfg));
        m_UltraTrack->onOpen(
            []() { qInfo() << "[DataChannelRelay] Ultra RTP track open (audio road)"; });
    } else if (ultra) {
        auto desc = rtc::Description::Video("ultra", rtc::Description::Direction::SendOnly);
        const int pt = 97;
        desc.addVP8Codec(pt);
        const uint32_t ssrc = rd();
        desc.addSSRC(ssrc, "ultra");
        m_UltraTrack = m_Pc->addTrack(desc);
        auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "ultra", static_cast<uint8_t>(pt), rtc::RtpPacketizer::VideoClockRate);
        auto packetizer = std::make_shared<rtc::VP8RtpPacketizer>(cfg);
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(16384));
        m_UltraTrack->setMediaHandler(packetizer);
        m_UltraTrack->onOpen([]() { qInfo() << "[DataChannelRelay] Ultra RTP track open"; });
    }
    // The audio road's pacing (AroadPacer.h), off unless the bench asks
    // (aroadpace=<x>): on a Mac in Wi-Fi, 3 times its rate over a 50 Mbit/s
    // floor dropped more in the kernel than a frame in one run (06/10/2026,
    // W4 « after »), as pacing SCTP did in W2 A: Chrome reads late, the host's
    // bursts are not the cause. Its resends feed the rate governor either way.
    // Its window (aroadwin=<KiB>, AroadPacer.h), off unless the bench asks:
    // at most that much sent past the last chunk the page acknowledged. One
    // road a session, so one window for either track.
    if ((m_RtpVideoAudioRoad || m_UltraAudioRoad) && !m_AroadPacer) {
        const double multiple = m_AroadPace < 0 ? 0.0 : m_AroadPace;
        const int64_t window = static_cast<int64_t>(m_AroadWindowKb) * 1024;
        if (multiple > 0 || window > 0) {
            m_AroadPacer =
                std::make_unique<AroadPacer>(multiple, 50'000'000 / 8, 16 * 1024, window);
            qInfo() << "[DataChannelRelay] audio road paced at" << multiple
                    << "x what it carries, 50 Mbit/s at least (aroadpace=); window"
                    << m_AroadWindowKb << "KiB (aroadwin=, 0 = none)";
        }
    }
}

void DataChannelRelay::sendAudioRoad(const std::shared_ptr<rtc::Track>& track,
                                     AudioRoadHistory& history, uint16_t seq, const uint8_t* data,
                                     size_t size, bool isKeyframe, uint32_t timestamp,
                                     uint32_t frameId)
{
    rtc::FrameInfo info(timestamp);
    info.isKeyFrame = isKeyframe;
    // One Opus packet per chunk: 'M', flags (1 = key), frame seq, index,
    // count (u16, big endian), the wire frame id (u32, big endian), then up
    // to kChunk bytes of the frame (1100, or the bench's aroadchunk=).
    const size_t kChunk = m_AroadChunk;
    constexpr size_t kHead = 12;
    const size_t count = std::max<size_t>(1, (size + kChunk - 1) / kChunk);
    AudioRoadHistory::Frame kept;
    kept.seq = seq;
    kept.timestamp = timestamp;
    kept.chunks.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t off = i * kChunk;
        const size_t len = std::min(kChunk, size - off);
        std::vector<rtc::byte> chunk(kHead + len);
        const uint8_t head[kHead] = {'M',
                                     static_cast<uint8_t>(isKeyframe ? 1 : 0),
                                     static_cast<uint8_t>(seq >> 8),
                                     static_cast<uint8_t>(seq),
                                     static_cast<uint8_t>(i >> 8),
                                     static_cast<uint8_t>(i),
                                     static_cast<uint8_t>(count >> 8),
                                     static_cast<uint8_t>(count),
                                     static_cast<uint8_t>(frameId >> 24),
                                     static_cast<uint8_t>(frameId >> 16),
                                     static_cast<uint8_t>(frameId >> 8),
                                     static_cast<uint8_t>(frameId)};
        std::memcpy(chunk.data(), head, kHead);
        std::memcpy(chunk.data() + kHead, data + off, len);
        // The bench's loss (MW_AROAD_DROP, per mille): first sends only, so
        // the page's NACK path is what brings the chunk back.
        static const int dropPerMille = qEnvironmentVariableIntValue("MW_AROAD_DROP");
        if (dropPerMille <= 0 ||
            static_cast<int>(QRandomGenerator::global()->bounded(1000)) >= dropPerMille) {
            if (m_AroadPacer)
                m_AroadPacer->send(track, chunk, timestamp, false);
            else
                track->sendFrame(chunk.data(), chunk.size(), info);
        }
        kept.chunks.push_back(std::move(chunk));
    }
    m_AroadSent.fetch_add(static_cast<int64_t>(count), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> budget(m_AroadBudgetMutex);
        m_AroadBudget.sent(size + count * kHead, steadyUs());
    }
    std::lock_guard<std::mutex> lk(history.mutex);
    history.frames.push_back(std::move(kept));
    while (history.frames.size() > history.maxFrames)
        history.frames.pop_front();
}

void DataChannelRelay::resendAudioRoad(const QJsonObject& msg)
{
    // {"type":"aroadnack","t":"v"|"u","s":seq,"i":[indexes]} from the page's
    // transform worker: the chunks it saw missing, sent again as they went.
    // "all":1 names a frame none of whose chunks came (a hole in the seqs: a
    // small delta is one or two chunks, one loss on the air takes it whole),
    // so the page knows neither its indexes nor how many: all of them go.
    const bool ultra = msg["t"].toString() == QLatin1String("u");
    const std::shared_ptr<rtc::Track> track = ultra ? m_UltraTrack : m_VideoTrack;
    AudioRoadHistory& history = ultra ? m_UltraRoadHistory : m_VideoRoadHistory;
    if (!track || !track->isOpen()) return;
    const uint16_t seq = static_cast<uint16_t>(msg["s"].toInt());
    const bool all = msg["all"].toInt() != 0;
    QJsonArray indexes = msg["i"].toArray();
    std::lock_guard<std::mutex> lk(history.mutex);
    for (const auto& f : history.frames) {
        if (f.seq != seq) continue;
        rtc::FrameInfo info(f.timestamp);
        if (all) {
            indexes = QJsonArray();
            for (size_t i = 0; i < f.chunks.size(); ++i)
                indexes.append(static_cast<int>(i));
        }
        int n = 0;
        for (const QJsonValue& v : indexes) {
            const int i = v.toInt(-1);
            if (i < 0 || i >= static_cast<int>(f.chunks.size())) continue;
            {
                std::lock_guard<std::mutex> budget(m_AroadBudgetMutex);
                if (!m_AroadBudget.mayResend(f.chunks[static_cast<size_t>(i)].size(), steadyUs()))
                    continue;
            }
            try {
                if (m_AroadPacer)
                    m_AroadPacer->send(track, f.chunks[static_cast<size_t>(i)], f.timestamp, true);
                else
                    track->sendFrame(f.chunks[static_cast<size_t>(i)].data(),
                                     f.chunks[static_cast<size_t>(i)].size(), info);
                ++n;
            } catch (const std::exception& e) {
                qWarning() << "[DataChannelRelay] audio road resend failed:" << e.what();
                return;
            }
        }
        history.resent += n;
        m_AroadResent.fetch_add(n, std::memory_order_relaxed);
        if (history.resent == n || history.resent / 200 != (history.resent - n) / 200)
            qInfo() << "[DataChannelRelay] audio road" << (ultra ? "Ultra" : "video")
                    << "chunks resent so far:" << history.resent
                    << "| refused by the budget:" << [this]() {
                           std::lock_guard<std::mutex> budget(m_AroadBudgetMutex);
                           return m_AroadBudget.refused();
                       }();
        return;
    }
}

void DataChannelRelay::sendRtpVideo(const QByteArray& frameData, bool isKeyframe,
                                    int64_t presentationTimeUs, int frameNumber)
{
    // The frame's capture on the host's steady clock, as sendFragmented stamps it.
    uint32_t backendTs = 0;
    if (m_Shim) {
        const int64_t firstMs = m_Shim->firstFrameArrivalSteadyMs();
        const int64_t presUs =
            presentationTimeUs >= 0 ? presentationTimeUs : m_Shim->framePresentationTimeUs();
        if (firstMs > 0 && presUs >= 0)
            backendTs = static_cast<uint32_t>((firstMs + presUs / 1000) & 0xFFFFFFFF);
    }
    if (backendTs == 0)
        backendTs = static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);
    rtc::FrameInfo info(backendTs);
    info.isKeyFrame = isKeyframe;
    // Stamped now: how long after its capture the frame leaves (POC U1.4).
    const int64_t sendStartUs = steadyUs();
    int64_t wireId = -1;
    try {
        if (m_RtpVideoAudioRoad) {
            // The DataChannel's frame id, and its map to the engine's frame
            // number (sendFragmented's): a frame the page gives up on is
            // named back by id, and a host that heals by invalidation answers
            // with deltas instead of a keyframe.
            const uint32_t frameId = m_FrameId++;
            wireId = frameId;
            m_FrameNumberById[frameId % kFrameNumberRing].store(
                frameNumber >= 0
                    ? (static_cast<int64_t>(frameId) << 32) | static_cast<uint32_t>(frameNumber)
                    : -1,
                std::memory_order_release);
            sendAudioRoad(m_VideoTrack, m_VideoRoadHistory, m_RtpAudioRoadSeq++,
                          reinterpret_cast<const uint8_t*>(frameData.constData()),
                          static_cast<size_t>(frameData.size()), isKeyframe, backendTs, frameId);
        } else {
            m_VideoTrack->sendFrame(reinterpret_cast<const rtc::byte*>(frameData.constData()),
                                    static_cast<size_t>(frameData.size()), info);
        }
        const int64_t endUs = steadyUs();
        // The bench's frame log (relaylog=1): the first and the last packet
        // handed to libdatachannel, as the sender stamps a DataChannel frame.
        if (m_FrameLog && frameNumber >= 0) {
            if (wireId >= 0) m_FrameLog->sent(frameNumber, static_cast<uint32_t>(wireId));
            m_FrameLog->frameSent(static_cast<uint32_t>(frameNumber), sendStartUs, endUs);
        }
        m_RtpSendUs.push_back(static_cast<int>(endUs - sendStartUs));
        // backendTs is the steady clock in ms, mod 2^32: the difference wraps alike.
        const uint32_t nowMs32 = static_cast<uint32_t>((endUs / 1000) & 0xFFFFFFFF);
        m_RtpLateUs.push_back(static_cast<int>(static_cast<int32_t>(nowMs32 - backendTs)) * 1000);
        if (m_RtpSendUs.size() >= 600) {
            std::sort(m_RtpSendUs.begin(), m_RtpSendUs.end());
            std::sort(m_RtpLateUs.begin(), m_RtpLateUs.end());
            qInfo() << "[DataChannelRelay] RTP video: sendFrame p50" << m_RtpSendUs[300] << "us p99"
                    << m_RtpSendUs[594] << "| capture to sent p50" << m_RtpLateUs[300] << "us p99"
                    << m_RtpLateUs[594];
            m_RtpSendUs.clear();
            m_RtpLateUs.clear();
        }
        if (isKeyframe) m_RtpVideoSentKeyframe = true;
        m_AwaitingIdr = false;
    } catch (const std::exception& e) {
        qWarning() << "[DataChannelRelay] RTP video send failed:" << e.what();
    }
    if (m_DirectVideoSend && (m_UltraSender || m_UltraTrack)) sendUltraSynthetic(backendTs);
}
