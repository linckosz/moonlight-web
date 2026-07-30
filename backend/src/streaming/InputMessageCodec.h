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

// Shared decoding of the browser's keyboard input messages.
//
// The three transports (DataChannelRelay, MediaTrackRelay, StreamRelay) each
// own their input switch, and the pieces that must agree across all of them —
// how a (keyCode, code) pair maps to a host VK, and how the held-input
// heartbeat is read — live here rather than in three copies that drift.

#include "MoonlightShim.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

extern "C" {
#include "Limelight.h"
}

namespace InputMsg {

/// Resolve the browser's (keyCode, code) pair to the VK + flags sent to the
/// host. International keys without standard US VK equivalents —
/// IntlBackslash (the ISO key next to left Shift) and IntlRo (the JIS \ key) —
/// need NON_NORMALIZED mode: Sunshine injects the keyCode as a raw VK rather
/// than a US-layout scancode, so the host's active layout resolves it.
inline void resolveHostKey(int vk, const QString& code, short& outVk, char& outFlags)
{
    if (code == QLatin1String("IntlBackslash")) {
        outVk = 0xE2; // VK_OEM_102 (ISO <> key)
        outFlags = SS_KBE_FLAG_NON_NORMALIZED;
    } else if (code == QLatin1String("IntlRo")) {
        outVk = 0xC1; // VK_ABNT_C1 (JIS Ro key)
        outFlags = SS_KBE_FLAG_NON_NORMALIZED;
    } else {
        outVk = static_cast<short>(vk);
        outFlags = 0;
    }
}

/// Limelight modifier bitmask from a message's DOM-style modifier flags.
inline char modifierMask(const QJsonObject& msg)
{
    char mods = 0;
    if (msg["ctrlKey"].toBool(false)) mods |= 0x02;
    if (msg["shiftKey"].toBool(false)) mods |= 0x01;
    if (msg["altKey"].toBool(false)) mods |= 0x04;
    if (msg["metaKey"].toBool(false)) mods |= 0x08;
    return mods;
}

/// Read an 'inputstate' heartbeat's key list — the client's authoritative set
/// of keys it is still holding (see MoonlightShim's input watchdog).
inline QVector<MoonlightShim::HeldKey> parseHeldKeys(const QJsonObject& msg)
{
    QVector<MoonlightShim::HeldKey> keys;
    const QJsonArray arr = msg["keys"].toArray();
    keys.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject k = v.toObject();
        MoonlightShim::HeldKey held;
        resolveHostKey(k["keyCode"].toInt(0), k["code"].toString(), held.keyCode, held.flags);
        held.modifiers = modifierMask(k);
        held.hold = k["hold"].toBool(false);
        keys.append(held);
    }
    return keys;
}

} // namespace InputMsg
