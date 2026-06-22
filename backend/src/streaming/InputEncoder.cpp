/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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

#include "InputEncoder.h"

#include <QDebug>
#include <QtEndian>

// --- Helpers ---------------------------------------------------------------

static QByteArray packPacket(uint32_t magic, const QByteArray& payload)
{
    // NV_INPUT_HEADER.size excludes the size field itself (matches moonlight-common-c).
    uint32_t headerSize = 4 + static_cast<uint32_t>(payload.size()); // magic + payload
    uint32_t bufferSize = 4 + headerSize;                            // size field + magic + payload

    QByteArray pkt;
    pkt.resize(static_cast<int>(bufferSize));

    // 4-byte big-endian size (excludes the size field itself)
    qToBigEndian(headerSize, pkt.data());

    // 4-byte little-endian magic
    qToLittleEndian(magic, pkt.data() + 4);

    // Payload
    if (!payload.isEmpty()) std::memcpy(pkt.data() + 8, payload.constData(), payload.size());

    return pkt;
}

// --- Public ----------------------------------------------------------------

QByteArray InputEncoder::encodeFromJson(const QJsonObject& msg)
{
    QString type = msg["type"].toString();

    if (type == "keydown") return encodeKeyEvent(msg, true);
    if (type == "keyup") return encodeKeyEvent(msg, false);
    if (type == "mousemove") return encodeMouseMove(msg);
    if (type == "mousedown") return encodeMouseButton(msg, true);
    if (type == "mouseup") return encodeMouseButton(msg, false);
    if (type == "mousewheel") return encodeMouseScroll(msg);

    qWarning() << "[InputEncoder] Unknown event type:" << type;
    return {};
}

// --- Keyboard --------------------------------------------------------------
// NV_KEYBOARD_PACKET:
//   header: size=BE(12), magic=LE(0x03 down / 0x04 up)
//   payload: flags(char) + keyCode(short LE) + modifiers(char) + zero2(short LE)

QByteArray InputEncoder::encodeKeyEvent(const QJsonObject& msg, bool down)
{
    QByteArray payload(6, '\0'); // 1 + 2 + 1 + 2 = 6 bytes

    // flags — 0 = standard (Sunshine), SS_KBE_FLAG_NON_NORMALIZED (0x01)
    payload[0] = 0;

    // keyCode (Windows VK) | 0x8000
    int vk = msg["keyCode"].toInt(0);
    qToLittleEndian(static_cast<short>(vk | 0x8000), payload.data() + 1);

    // modifiers
    char mods = 0;
    if (msg["ctrlKey"].toBool(false)) mods |= 0x02;
    if (msg["shiftKey"].toBool(false)) mods |= 0x01;
    if (msg["altKey"].toBool(false)) mods |= 0x04;
    if (msg["metaKey"].toBool(false)) mods |= 0x08;
    payload[3] = mods;

    // zero2 (padding) — already '\0'

    uint32_t magic = down ? 0x03u : 0x04u;
    return packPacket(magic, payload);
}

// --- Mouse move (relative) -------------------------------------------------
// NV_REL_MOUSE_MOVE_PACKET (Gen5):
//   header: size=BE(8), magic=LE(0x07)
//   payload: deltaX(short LE) + deltaY(short LE)

QByteArray InputEncoder::encodeMouseMove(const QJsonObject& msg)
{
    QByteArray payload(4, '\0');

    qToLittleEndian(static_cast<short>(msg["dx"].toInt(0)), payload.data());
    qToLittleEndian(static_cast<short>(msg["dy"].toInt(0)), payload.data() + 2);

    return packPacket(0x07u, payload);
}

// --- Mouse button ----------------------------------------------------------
// NV_MOUSE_BUTTON_PACKET (Gen5):
//   header: size=BE(9), magic=LE(0x08 down / 0x09 up)
//   payload: button(byte)

QByteArray InputEncoder::encodeMouseButton(const QJsonObject& msg, bool down)
{
    QByteArray payload(1, static_cast<char>(msg["button"].toInt(0)));
    uint32_t magic = down ? 0x08u : 0x09u;
    return packPacket(magic, payload);
}

// --- Mouse scroll (vertical) -----------------------------------------------
// NV_SCROLL_PACKET (Gen5):
//   header: size=BE(14), magic=LE(0x0A)
//   payload: scrollAmt1(short BE) + scrollAmt2(short BE) + zero3(short BE)
//   Each scroll amount is in WHEEL_DELTA units (120 per notch)

QByteArray InputEncoder::encodeMouseScroll(const QJsonObject& msg)
{
    QByteArray payload(6, '\0');

    short amount = static_cast<short>(msg["delta"].toInt(0));
    qToBigEndian(amount, payload.data());     // scrollAmt1
    qToBigEndian(amount, payload.data() + 2); // scrollAmt2 (mirror)

    return packPacket(0x0Au, payload);
}
