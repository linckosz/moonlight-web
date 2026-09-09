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

#include "InputPolicy.h"
#include "IMediaEngine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

extern "C" {
#include "Limelight.h"
}

namespace InputMsg {

/// Strip from an 'inputstate' heartbeat whatever @p p forbids, so a viewer's
/// heartbeat cannot re-press keys the policy just dropped.
inline void filterHeldState(const Policy& p, QVector<IMediaEngine::HeldKey>& keys, quint32& buttons)
{
    if (!p.keyboardMouse) {
        keys.clear();
        buttons = 0;
    }
}

// KeyboardMode is declared in IMediaEngine.h, where the session sets it. What
// each value means for the wire:
//
//  - Positional — send the position and let the host's layout decide, exactly
//    as every Moonlight client has always done. For hosts that can do no
//    better: Sunshine on Linux and Wolf (both type Unicode through the GTK/IBus
//    Ctrl+Shift+U sequence, which arrives as garbage with no input method
//    listening), GeForce Experience (drops the flags byte outright —
//    InputStream.c:947), and an OS we could not identify.
//  - Native — the MoonlightWeb native host: we own the injection, so a
//    character is resolved in the host's real layout and pressed as a real key.
//  - SunshineWindows — SS_KBE_FLAG_NON_NORMALIZED makes Sunshine inject the VK
//    itself instead of a US scancode (libvirtualhid windows_backend.cpp:1430),
//    and Windows resolves a VK through the ACTIVE layout, so a letter VK types
//    that letter on any layout AND stays a real key press. Only letters: digits
//    and punctuation sit behind different shift states from one layout to the
//    next, so their VK says nothing about the character.
//  - SunshineMacos — maps the VK through a fixed table and ignores the flag
//    entirely (it is #ifdef _WIN32 — virtualhid_input.cpp:314), so text
//    injection is the only exact path.

/// Name for the log line the session writes once per stream.
inline const char* keyboardModeName(KeyboardMode mode)
{
    switch (mode) {
    case KeyboardMode::Native: return "native host, resolved in the host's own layout";
    case KeyboardMode::SunshineWindows: return "Sunshine/Windows, letters as non-normalized VKs";
    case KeyboardMode::SunshineMacos: return "Sunshine/macOS, characters as text";
    case KeyboardMode::Positional: break;
    }
    return "off — key positions, host layout decides";
}

/// What to do with one key message: press a key, or type a character.
struct KeyPlan
{
    short keyCode = 0;
    char flags = 0;
    /// Non-empty: type this instead of pressing keyCode. The host resolves it
    /// in its own layout (native) or injects it as Unicode (Sunshine).
    QString text;

    bool isText() const { return !text.isEmpty(); }
};

/// The US VK of a letter, or 0. Letters are the one class whose Windows VK is
/// the same on every layout AND unshifted on every layout, which is what makes
/// the NON_NORMALIZED trick exact for them and only them.
inline short letterVk(const QString& ch)
{
    if (ch.size() != 1) return 0;
    const QChar up = ch.at(0).toUpper();
    if (up < QLatin1Char('A') || up > QLatin1Char('Z')) return 0;
    return static_cast<short>(up.unicode());
}

/// Resolve one keyboard message to what the host should actually receive.
///
/// `code` still carries the physical key, because two international keys have
/// no US VK at all and must be sent as raw VKs whatever the mode: IntlBackslash
/// (the ISO key beside left Shift) and IntlRo (the JIS \ key).
///
/// `char` carries the character the client's layout produced, and is present
/// ONLY when it differs from what the US layout puts at that position — so a
/// US client, and every agreeing key of any other client, takes the untouched
/// positional path below.
inline KeyPlan resolveKey(const QJsonObject& msg, KeyboardMode mode)
{
    KeyPlan plan;
    const QString code = msg["code"].toString();

    if (code == QLatin1String("IntlBackslash")) {
        plan.keyCode = 0xE2; // VK_OEM_102 (ISO <> key)
        plan.flags = SS_KBE_FLAG_NON_NORMALIZED;
        return plan;
    }
    if (code == QLatin1String("IntlRo")) {
        plan.keyCode = 0xC1; // VK_ABNT_C1 (JIS Ro key)
        plan.flags = SS_KBE_FLAG_NON_NORMALIZED;
        return plan;
    }

    plan.keyCode = static_cast<short>(msg["keyCode"].toInt(0));
    plan.flags = 0;

    const QString ch = msg["char"].toString();
    if (ch.isEmpty() || mode == KeyboardMode::Positional) return plan;

    switch (mode) {
    case KeyboardMode::Native:
        // Every character, because the native host turns each one back into a
        // real key of the host's layout — so nothing is lost by asking.
        plan.text = ch;
        break;
    case KeyboardMode::SunshineWindows:
        if (const short vk = letterVk(ch)) {
            // A real key press that still types the right letter: strictly
            // better than injecting text, which would have no key state.
            plan.keyCode = vk;
            plan.flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else {
            plan.text = ch;
        }
        break;
    case KeyboardMode::SunshineMacos: plan.text = ch; break;
    case KeyboardMode::Positional: break; // handled above
    }
    return plan;
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
/// of keys it is still holding (see the media engine's input watchdog).
///
/// A key that resolves to a CHARACTER is deliberately left out: the watchdog
/// re-presses whatever it sees, and re-pressing a character ten times a second
/// would not re-assert a held key, it would type the letter ten times. Their
/// release comes from the real key-up, and from the client's own
/// release-everything on focus loss, which sends one too.
inline QVector<IMediaEngine::HeldKey> parseHeldKeys(const QJsonObject& msg, KeyboardMode mode)
{
    QVector<IMediaEngine::HeldKey> keys;
    const QJsonArray arr = msg["keys"].toArray();
    keys.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject k = v.toObject();
        const KeyPlan plan = resolveKey(k, mode);
        if (plan.isText()) continue;
        IMediaEngine::HeldKey held;
        held.keyCode = plan.keyCode;
        held.flags = plan.flags;
        held.modifiers = modifierMask(k);
        held.hold = k["hold"].toBool(false);
        keys.append(held);
    }
    return keys;
}

} // namespace InputMsg
