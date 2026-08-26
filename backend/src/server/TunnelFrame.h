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

#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

/**
 * The bytes that cross the rendezvous control channel.
 *
 * Split out of ControlTunnel and kept free of libdatachannel so it can be tested
 * as a unit — which matters more here than usual, because the other half of this
 * format lives in JavaScript (bootstrap/tunnel.js) on a machine we will never
 * see. Nothing catches a disagreement between the two at compile time; the tests
 * on both sides use the same vectors, and that is what does.
 *
 * One data channel, "mw-ctl", carrying binary messages. Every integer is
 * big-endian. SCTP already frames messages, so a frame carries no length of its
 * own — only the parts inside it are measured.
 *
 *   0x01 REQUEST   [u8][u32 id][u32 headLen][head JSON][body…]
 *                  head = {"m":method,"p":path,"h":{header:value,…}}
 *   0x02 RESPONSE  [u8][u32 id][u32 headLen][head JSON]
 *                  head = {"s":status,"h":{…}}
 *   0x03 BODY      [u8][u32 id][chunk…]
 *   0x04 END       [u8][u32 id]
 *   0x05 WS_OPEN   [u8][u32 id][u32 headLen][head JSON]
 *                  head = {"p":path,"h":{…}} — the cookies in it decide access
 *   0x06 WS_TEXT   [u8][u32 id][text UTF-8]
 *   0x07 WS_CLOSE  [u8][u32 id][reason UTF-8]
 *   0x08 WS_OPENED [u8][u32 id]
 *
 * Responses arrive as one RESPONSE, any number of BODY frames and one END.
 * Chunking is not an optimisation: SCTP has a negotiated message ceiling, and
 * the application's own script bundles sail past it.
 */
namespace TunnelFrame {

enum Kind : quint8
{
    Request = 0x01,
    Response = 0x02,
    Body = 0x03,
    End = 0x04,
    WsOpen = 0x05,
    WsText = 0x06,
    WsClose = 0x07,
    WsOpened = 0x08,
};

/// Bytes carried per BODY frame. Well under the 64 KiB every implementation
/// accepts, so no negotiated maximum has to be trusted.
inline constexpr int kChunkBytes = 16 * 1024;

/// Largest request a browser may push in one frame. The API takes JSON
/// documents, not uploads; past this it is a mistake or an attempt.
inline constexpr int kMaxRequestBytes = 256 * 1024;

/// Fixed cost of a frame: the kind and the identifier.
inline constexpr int kHeaderBytes = 5;

inline void appendU32(QByteArray& out, quint32 v)
{
    out.append(static_cast<char>((v >> 24) & 0xFF));
    out.append(static_cast<char>((v >> 16) & 0xFF));
    out.append(static_cast<char>((v >> 8) & 0xFF));
    out.append(static_cast<char>(v & 0xFF));
}

inline quint32 readU32(const QByteArray& b, int offset)
{
    return (static_cast<quint32>(static_cast<quint8>(b[offset])) << 24) |
           (static_cast<quint32>(static_cast<quint8>(b[offset + 1])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(b[offset + 2])) << 8) |
           static_cast<quint32>(static_cast<quint8>(b[offset + 3]));
}

inline QByteArray build(quint8 kind, quint32 id, const QByteArray& payload)
{
    QByteArray out;
    out.reserve(kHeaderBytes + payload.size());
    out.append(static_cast<char>(kind));
    appendU32(out, id);
    out.append(payload);
    return out;
}

/// Split a frame. False for anything too short to be one — a frame we cannot
/// read is dropped, never guessed at.
inline bool parse(const QByteArray& message, quint8* outKind, quint32* outId,
                  QByteArray* outPayload)
{
    if (message.size() < kHeaderBytes) return false;
    *outKind = static_cast<quint8>(message[0]);
    *outId = readU32(message, 1);
    *outPayload = message.mid(kHeaderBytes);
    return true;
}

inline QByteArray encodeHead(const QJsonObject& head, const QByteArray& body)
{
    const QByteArray json = QJsonDocument(head).toJson(QJsonDocument::Compact);
    QByteArray out;
    out.reserve(4 + json.size() + body.size());
    appendU32(out, static_cast<quint32>(json.size()));
    out.append(json);
    out.append(body);
    return out;
}

/// Split a `[u32 headLen][head JSON][rest]` payload. False for anything that
/// does not decode — including a length that runs past the end, which is the
/// shape a truncated or hostile frame takes.
inline bool decodeHead(const QByteArray& payload, QJsonObject* outHead, QByteArray* outBody)
{
    if (payload.size() < 4) return false;
    const quint32 headLen = readU32(payload, 0);
    if (headLen == 0 || static_cast<qsizetype>(headLen) > payload.size() - 4) return false;

    const QJsonDocument doc = QJsonDocument::fromJson(payload.mid(4, static_cast<int>(headLen)));
    if (!doc.isObject()) return false;

    *outHead = doc.object();
    if (outBody) *outBody = payload.mid(4 + static_cast<int>(headLen));
    return true;
}

} // namespace TunnelFrame
