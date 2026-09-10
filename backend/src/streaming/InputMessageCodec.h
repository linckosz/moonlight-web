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

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <atomic>

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
//    injection is the only exact path. Letters only, same as above.
//
// The rule the two Sunshine modes share: NEVER trade a real key press for text.
// Text has no key state, and a divergence is measured against the US layout
// rather than the host's — so on a host that already matched the client, the
// position was producing the right character and text would only take the key
// away. Correcting what can be corrected for free, and leaving the rest exactly
// as it was, is what makes this change cost nothing to anyone.

/// Name for the log line the session writes once per stream.
inline const char* keyboardModeName(KeyboardMode mode)
{
    switch (mode) {
    case KeyboardMode::Native: return "native host, resolved in the host's own layout";
    case KeyboardMode::SunshineWindows: return "Sunshine/Windows, letters as non-normalized VKs";
    case KeyboardMode::SunshineMacos: return "Sunshine/macOS, letters as text";
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
    // Whether the client's character differs from the one the US layout puts at
    // this position. Only the Sunshine modes care: see below.
    const bool nonUs = msg["nonUs"].toBool(false);

    switch (mode) {
    case KeyboardMode::Native:
        // EVERY printable character, agreeing ones included. The native host
        // resolves it in its own real layout and presses the key that carries
        // it, so when the two layouts already match the resolution lands back on
        // the very key the viewer pressed — nothing changes — and when they do
        // not, the right key is pressed instead. Gating this on "differs from
        // US" was wrong in the mirror case: a US-layout viewer on a French host
        // presses the key marked A, means `q`, and diverges from nothing at all
        // — yet the position types `a`. The comparison that matters is against
        // the HOST's layout, which only the host can make.
        plan.text = ch;
        break;
    case KeyboardMode::SunshineWindows:
        // Letters, and only letters. Their VK is exact here AND stays a real key
        // press, so the correction costs nothing.
        //
        // A digit or a punctuation mark is left POSITIONAL on purpose, even
        // though text would type it exactly. Text carries no key state, and the
        // divergence that got us here is measured against the US layout, not
        // against the HOST's — which we cannot know. So on a host whose layout
        // already matches the client's, the position was ALREADY producing the
        // right character, and swapping it for text would trade a working key
        // for a stateless one: weapon slots 1-5 in a shooter stop answering,
        // for a character that was never wrong. Being mistyped on a mismatched
        // host is the pre-existing behaviour; breaking a key that worked is not.
        //
        // And only when the character differs from the US one. Sunshine derives
        // the scancode for a non-normalized VK from ITS OWN thread's layout
        // (MapVirtualKeyW, windows_backend.cpp:1433), where the normalized path
        // is a fixed table — so asking for it when the client is already US
        // would trade a deterministic scancode for one we cannot predict, and
        // buy nothing.
        if (nonUs) {
            if (const short vk = letterVk(ch)) {
                plan.keyCode = vk;
                plan.flags = SS_KBE_FLAG_NON_NORMALIZED;
            }
        }
        break;
    case KeyboardMode::SunshineMacos:
        // macOS ignores the non-normalized flag, so a letter cannot be both
        // exact and a real key here — text is the only exact channel. Taken for
        // letters, because a macOS GameStream host is a machine people type on;
        // refused for the rest, for the reason spelled out just above.
        if (nonUs && letterVk(ch)) plan.text = ch;
        break;
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

// ── Diagnostics ─────────────────────────────────────────────────────────────
//
// A keystroke that comes out wrong looks the same from the browser whatever
// went wrong: the character on screen is simply not the one that was typed.
// This says which of the three links broke — what the client meant, what was
// put on the wire, and what the host will make of it — as one line per press.
//
// The verdicts answer the only two questions a user has, and they are NOT the
// same question:
//
//   Notepad — the character that appears in a text field. What a typist cares
//             about, and the one the protocol makes hard.
//   Game    — the physical key a title reading raw scancodes sees. Text
//             injection scores OK on the first and KO on the second: it types
//             the character perfectly and no game ever knows a key was pressed.
//
// OK means guaranteed. KO means "cannot be guaranteed from here", which on a
// remote host covers "depends on a layout we have no way to read" as well as
// "definitely lost" — both are the cases worth looking at, and the line says
// which one it is. Warning level for either, so failures stand out.
//
// On the MoonlightWeb native host the verdict is deliberately NOT given here:
// the host resolves the character in its own real layout and can say what it
// actually did, which no prediction from this side can match. Its own line
// follows this one (Win32Input::injectChar and the other two platforms).

/// Process-wide, set once at startup from AppSettings::keyboardDebug(). Same
/// shape as LatencyFlag: an instrument, off unless someone asked for it.
inline std::atomic<bool>& debugFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}
inline void setDebug(bool on)
{
    debugFlag().store(on, std::memory_order_relaxed);
}
inline bool debugEnabled()
{
    return debugFlag().load(std::memory_order_relaxed);
}

/// How a game will name the key carrying this US virtual key — its label on a
/// US keyboard, which is the vocabulary key bindings are written in. Only
/// printable keys reach the diagnostic, so the table stops there.
inline QString usKeyLabel(short vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return QString(QChar(static_cast<char>(vk)));
    switch (static_cast<int>(vk) & 0xFF) {
    case 0x20: return QStringLiteral("Space");
    case 0xBA: return QStringLiteral(";");
    case 0xBB: return QStringLiteral("=");
    case 0xBC: return QStringLiteral(",");
    case 0xBD: return QStringLiteral("-");
    case 0xBE: return QStringLiteral(".");
    case 0xBF: return QStringLiteral("/");
    case 0xC0: return QStringLiteral("`");
    case 0xDB: return QStringLiteral("[");
    case 0xDC: return QStringLiteral("\\");
    case 0xDD: return QStringLiteral("]");
    case 0xDE: return QStringLiteral("'");
    default: break;
    }
    return QStringLiteral("VK 0x%1").arg(static_cast<int>(vk) & 0xFF, 2, 16, QLatin1Char('0'));
}

/// One keystroke's diagnostic: the line, and whether anything in it is a KO.
struct KeyDiag
{
    QString line;
    bool warn = false;
};

/// Describe what @p plan will do to @p msg's key on a host running @p mode.
/// Returns an empty line for a key with no character — an arrow, an F-key, a
/// modifier: nothing about them depends on a layout, so there is nothing to
/// diagnose and every one of them would be noise.
inline KeyDiag describeKey(const QJsonObject& msg, KeyboardMode mode, const KeyPlan& plan)
{
    KeyDiag diag;
    const QString ch = msg["char"].toString();
    if (ch.isEmpty()) return diag;

    const bool nonUs = msg["nonUs"].toBool(false);
    const short posVk = static_cast<short>(msg["keyCode"].toInt(0));

    QString sent, notepad, game;
    if (plan.keyCode != 0 && plan.flags != 0 && plan.keyCode != letterVk(ch)) {
        // An international key — the ISO one beside left Shift, the JIS Ro —
        // sent as its own raw virtual key because no US position names it. That
        // is not a character correction and never was: whether the host has such
        // a key at all is its layout's business, and the ordinary US-layout
        // board this protocol assumes does not have one.
        sent = QStringLiteral("VK 0x%1 non-normalized, international key")
                   .arg(static_cast<int>(plan.keyCode) & 0xFF, 2, 16, QLatin1Char('0'));
        notepad = QStringLiteral("KO '%1' only if the host layout has this key").arg(ch);
        game = QStringLiteral("OK real key");
        diag.warn = true;
    } else if (plan.isText() && mode == KeyboardMode::Native) {
        // The host is us. It reads the character in its own layout and presses
        // the key that carries it, then says so on the next line — a verdict
        // here would only be a guess competing with an answer.
        sent = QStringLiteral("character, host resolves");
        notepad = QStringLiteral("-- host verdict below");
        game = QStringLiteral("-- host verdict below");
    } else if (plan.isText()) {
        // Unicode injection on a remote host: exact, and invisible to anything
        // reading the keyboard rather than the text field.
        sent = QStringLiteral("text");
        notepad = QStringLiteral("OK '%1'").arg(ch);
        game = QStringLiteral("KO no key state at all");
        diag.warn = true;
    } else if (plan.flags != 0) {
        // A non-normalized virtual key: Windows resolves it through the host's
        // ACTIVE layout, so the character is exact, and it stays a real press.
        sent = QStringLiteral("VK 0x%1 non-normalized")
                   .arg(static_cast<int>(plan.keyCode) & 0xFF, 2, 16, QLatin1Char('0'));
        notepad = QStringLiteral("OK '%1'").arg(ch);
        game = QStringLiteral("OK real key, at the host layout's own position");
    } else {
        // The position, untouched. What the host types depends on ITS layout,
        // which nothing in the protocol lets us read — so neither verdict here
        // is ever a certainty. The warning is spent where it earns its keep:
        // on a key whose character the client's layout does NOT put at its US
        // position, because that is the one this whole feature exists to fix
        // and did not. A key that agrees with US is the assumption the protocol
        // has always run on; flagging it would put a warning under every
        // keystroke of a US viewer and drown the lines that matter.
        sent = QStringLiteral("position VK 0x%1")
                   .arg(static_cast<int>(plan.keyCode) & 0xFF, 2, 16, QLatin1Char('0'));
        notepad = nonUs
                      ? QStringLiteral("KO '%1' only if the host runs the client's layout").arg(ch)
                      : QStringLiteral("OK '%1' unless the host layout is not US").arg(ch);
        game = QStringLiteral("OK real key, US '%1'").arg(usKeyLabel(posVk));
        diag.warn = nonUs;
    }

    diag.line = QStringLiteral("[KBD] %1 client '%2'%3 -> %4 | Notepad: %5 | Game: %6")
                    .arg(msg["code"].toString(), ch,
                         nonUs ? QStringLiteral(" (non-US)") : QString(), sent, notepad, game);
    return diag;
}

/// Write @p msg's diagnostic, if diagnostics are on and the key has one. Called
/// by each transport on a key DOWN only: a release resolves identically by
/// construction (the client replays the press's character), so logging it again
/// would double every line for nothing.
inline void logKey(const QJsonObject& msg, KeyboardMode mode, const KeyPlan& plan, bool down)
{
    if (!down || !debugEnabled()) return;
    const KeyDiag diag = describeKey(msg, mode, plan);
    if (diag.line.isEmpty()) return;
    if (diag.warn)
        qWarning().noquote() << diag.line;
    else
        qInfo().noquote() << diag.line;
}

} // namespace InputMsg
