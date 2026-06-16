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
