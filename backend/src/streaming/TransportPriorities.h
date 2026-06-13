#pragma once

#include <QString>
#include <QStringList>
#include <QMap>

struct TransportPriorities {
    struct Entry {
        int priority = 100;
        bool available = true;
    };

    // Lower priority = tried first. available=false = hidden from auto dropdown.
    static QMap<QString, Entry> defaults() {
        return {
            {"webrtc-media-udp", {10, true}},
            {"webrtc-dc-udp",    {20, true}},
            {"webrtc-media-tcp", {30, true}},
            {"webrtc-dc-tcp",    {40, true}},
            {"wss",              {50, true}}
        };
    }

    // Ordered transports sorted by priority (lowest first).
    // Unavailable transports excluded unless they match forcedExplicit.
    static QStringList orderedTransports(const QString& forcedExplicit = {}) {
        auto map = defaults();
        QList<QPair<int, QString>> sorted;
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (!it.value().available && it.key() != forcedExplicit)
                continue;
            sorted.append({it.value().priority, it.key()});
        }
        std::sort(sorted.begin(), sorted.end());
        QStringList result;
        for (const auto& pair : sorted)
            result.append(pair.second);
        return result;
    }

    // Priority of a given transport mode (100 = unknown).
    static int priority(const QString& mode) {
        return defaults().value(mode).priority;
    }

    // Whether a transport is available (visible in auto dropdown).
    static bool available(const QString& mode) {
        return defaults().value(mode).available;
    }
};
