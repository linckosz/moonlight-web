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

#pragma once

#include <QByteArray>
#include <QJsonObject>

// Encodes JSON input events into GameStream wire-format binary packets.
// Each packet includes the 4-byte BE length prefix required by the TCP
// control channel (port 35043).
class InputEncoder
{
public:
    // Returns the encoded packet, or empty QByteArray for unknown event types.
    static QByteArray encodeFromJson(const QJsonObject& msg);

private:
    static QByteArray encodeKeyEvent(const QJsonObject& msg, bool down);
    static QByteArray encodeMouseMove(const QJsonObject& msg);
    static QByteArray encodeMouseButton(const QJsonObject& msg, bool down);
    static QByteArray encodeMouseScroll(const QJsonObject& msg);
};
