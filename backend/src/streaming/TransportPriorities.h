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

#include <QString>
#include <QStringList>

struct TransportPriorities {
    // Auto-mode fallback order (lowest index = tried first).
    //
    //   Video Enhancement OFF:
    //     1. webrtc-dc-udp     DataChannel (UDP)  → <canvas>
    //     2. webrtc-dc-tcp     DataChannel (TCP)  → <canvas>
    //     3. webrtc-media-udp  MediaTrack  (UDP)  → <video>
    //     4. webrtc-media-tcp  MediaTrack  (TCP)  → <video>
    //     5. wss               WebSocket Secure   → <canvas>
    //
    //   Video Enhancement ON (canvas transports first so WebGPU upscaling
    //   applies; MediaTrack is kept only as a last resort and streams
    //   WITHOUT enhancement since <video> cannot be processed by WebGPU):
    //     1. webrtc-dc-udp     DataChannel (UDP)  → <canvas>
    //     2. webrtc-dc-tcp     DataChannel (TCP)  → <canvas>
    //     3. wss               WebSocket Secure   → <canvas>
    //     4. webrtc-media-udp  MediaTrack  (UDP)  → <video> (no enhancement)
    //     5. webrtc-media-tcp  MediaTrack  (TCP)  → <video> (no enhancement)
    static QStringList orderedTransports(bool videoEnhancement = false) {
        if (videoEnhancement) {
            return {QStringLiteral("webrtc-dc-udp"),
                    QStringLiteral("webrtc-dc-tcp"),
                    QStringLiteral("wss"),
                    QStringLiteral("webrtc-media-udp"),
                    QStringLiteral("webrtc-media-tcp")};
        }
        return {QStringLiteral("webrtc-dc-udp"),
                QStringLiteral("webrtc-dc-tcp"),
                QStringLiteral("webrtc-media-udp"),
                QStringLiteral("webrtc-media-tcp"),
                QStringLiteral("wss")};
    }
};
