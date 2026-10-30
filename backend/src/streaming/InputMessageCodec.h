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
#include "mw/native/HostKeyboardLayout.h"

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
//    InputStream.c:947), an OS we could not identify — and Sunshine on macOS,
//    for the reason given below.
//  - Native — the MoonlightWeb native host: we own the injection, so a
//    character is resolved in the host's real layout and pressed as a real key.
//  - SunshineWindows — SS_KBE_FLAG_NON_NORMALIZED makes Sunshine inject the VK
//    itself instead of a US scancode (libvirtualhid windows_backend.cpp:1430),
//    and Windows resolves a VK through the ACTIVE layout, so a letter VK types
//    that letter on any layout AND stays a real key press. Only letters: digits
//    and punctuation sit behind different shift states from one layout to the
//    next, so their VK says nothing about the character.
//
// Sunshine on macOS gets no mode of its own, and that is a decision, not an
// omission. Its keyboard maps the VK through a fixed table and ignores the
// non-normalized flag (#ifdef _WIN32 — virtualhid_input.cpp:314), so the one
// exact channel there would be text. And text is exactly what a Sunshine older
// than 2026.824 cannot type on a Mac: its unicode() logged "Unicode input not
// yet implemented for MacOS" and dropped the packet on the floor (libvirtualhid
// brought the real thing in 687e12d0, 2026-08-16). A letter sent as text to
// such a host is not mistyped, it is LOST — an AZERTY viewer on a QWERTY Mac
// watched "q" and "a" vanish while "p" typed, which is how this was found. And
// nothing on the wire says which Sunshine is listening: serverinfo's appversion
// is the GameStream-compat constant "7.1.431.-1" on every Sunshine, and the
// feature flags carry pen/touch bits only. The position gives a swapped letter,
// which is the pre-existing behaviour and at least a keystroke; text gives
// either the right letter or nothing, and no way to know which. The day
// Sunshine advertises text support, a mode goes back here.
//
// The rule on every Sunshine host: NEVER trade a real key press for text. Text
// has no key state, and a divergence is measured against the US layout rather
// than the host's — so on a host that already matched the client, the position
// was producing the right character and text would only take the key away.
// Correcting what can be corrected for free, and leaving the rest exactly as it
// was, is what makes this change cost nothing to anyone.

/// Name for the log line the session writes once per stream.
inline const char* keyboardModeName(KeyboardMode mode)
{
    switch (mode) {
    case KeyboardMode::Native: return "native host, resolved in the host's own layout";
    case KeyboardMode::SunshineWindows: return "Sunshine/Windows, letters as non-normalized VKs";
    case KeyboardMode::Positional: break;
    }
    return "key positions only, host layout decides";
}

/// The owner's choice, for the same log line.
inline const char* layoutFidelityName(LayoutFidelity fidelity)
{
    switch (fidelity) {
    case LayoutFidelity::Auto: return "auto — the client's characters, positions in gaming mode";
    case LayoutFidelity::Always: return "always — the client's characters";
    case LayoutFidelity::Never: break;
    }
    return "never — key positions";
}

/// The owner's choice as settings.json spells it (AppSettings::
/// keyboardLayoutFidelity); anything unknown is the default.
inline LayoutFidelity parseLayoutFidelity(const QString& text)
{
    if (text == QLatin1String("always")) return LayoutFidelity::Always;
    if (text == QLatin1String("never")) return LayoutFidelity::Never;
    return LayoutFidelity::Auto;
}

/// A virtual key no layout assigns, tapped in place of a key the native host
/// must not type (a dead key, see sendKey): Windows then sees a key between
/// an Alt's press and its release, and does not take the Alt for a tap. The
/// Linux and macOS hosts have no key for it and drop it.
constexpr short kMaskVk = 0xE8;

/// VK_MENU, and VK_RMENU: the client's right Alt, AltGr on a European layout.
constexpr short kVkAlt = 0x12;
constexpr short kVkRightAlt = 0xA5;

/// The left Ctrl Windows puts under an AltGr, as the client marks it: a
/// release it sends as the AltGr goes down, and the key in its heartbeats.
/// A positional host — Wolf, Sunshine on Linux or macOS — must not hold it:
/// Ctrl with the right Alt is no AltGr there, and AltGr+E typed no € on a
/// French Wolf (issue #44, 29/10/2026). Windows reads Ctrl and Alt as AltGr,
/// and the native host keeps it down for its own (WinTextKeys).
inline bool altGrCtrl(const QJsonObject& msg)
{
    return msg["altGrCtrl"].toBool(false);
}

/// What to do with one key message: press a key, or type a character.
struct KeyPlan
{
    short keyCode = 0;
    char flags = 0;
    /// Non-empty: type this instead of pressing keyCode. The host resolves it
    /// in its own layout (native) or injects it as Unicode (Sunshine).
    QString text;
    /// Send nothing at all: a dead key on the native host (see resolveKey).
    bool skip = false;
    /// The text is a toolbar chord's character: the host presses the key that
    /// carries it under the held modifiers, and keyCode is its US position for
    /// a layout that has none.
    bool chord = false;

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

/// Whether a key message goes by its position: the owner's choice (never), a
/// host that can do nothing else — or gaming mode under "auto", which the
/// client marks on the press (`gaming`) and replays on its release and in the
/// heartbeat, so a key pressed at the desktop and let go in gaming mode lets go
/// of what it pressed. In gaming mode a key plays by its place: the movement
/// keys and the weapon slots stay where they are whatever the two keyboards,
/// as a keyboard plugged into the host would (Bruno's rule of 22/10/2026).
inline bool byPosition(const QJsonObject& msg, KeyboardMode mode, LayoutFidelity fidelity)
{
    return mode == KeyboardMode::Positional || fidelity == LayoutFidelity::Never ||
           (fidelity == LayoutFidelity::Auto && msg["gaming"].toBool(false));
}

/// A key message's `char`: what one key produces, a character or a short
/// grapheme (an emoji, a letter and its accent). Anything longer is no key's,
/// and is dropped here, the key going by its position: Utf8Text is the one way
/// to type a string, with its own bounds.
inline QString keyChar(const QJsonObject& msg)
{
    constexpr qsizetype kMaxKeyChar = 8; // UTF-16 units
    const QString ch = msg["char"].toString();
    return ch.size() <= kMaxKeyChar ? ch : QString();
}

/// Resolve one keyboard message to what the host should actually receive.
///
/// `code` still carries the physical key, because two international keys have
/// no US VK at all and go as raw VKs wherever a position is sent: IntlBackslash
/// (the ISO key beside left Shift) and IntlRo (the JIS \ key). The native host
/// at the desktop types their character instead, as it does every other key's.
///
/// `char` carries the character the client's layout produced, for every
/// printable key; `nonUs` says whether the US layout puts something else at
/// that position (StreamView.clientChar) — read by the diagnostic line alone
/// now that Sunshine for Windows gets every letter.
///
/// `gaming` marks a key pressed in the client's gaming mouse mode, which under
/// "auto" goes by its position (below).
///
/// `chord` marks a chord of the touch keyboard's bar: a letter or a digit
/// typed while one of its modifiers is latched (Ctrl, then z). It names a
/// shortcut, so every host that can resolves it by its character, whatever
/// the owner chose for the physical keyboard: sent as a US position, Ctrl+z
/// was Ctrl+W on an AZERTY host, which closed the tab (22/10/2026). On the
/// native host the key carrying it is pressed under the held modifiers; on
/// Sunshine for Windows a letter goes as its virtual key, non-normalized; a
/// digit, and every other host, keep the position.
inline KeyPlan resolveKey(const QJsonObject& msg, KeyboardMode mode, LayoutFidelity fidelity)
{
    KeyPlan plan;
    const QString code = msg["code"].toString();

    const bool iso = code == QLatin1String("IntlBackslash");
    if (iso || code == QLatin1String("IntlRo")) {
        // VK_OEM_102 (ISO <> key) or VK_ABNT_C1 (JIS Ro key).
        plan.keyCode = iso ? 0xE2 : 0xC1;
        plan.flags = SS_KBE_FLAG_NON_NORMALIZED;
        // Their character on the native host at the desktop, like any other
        // key's. A Mac's ISO keyboard reports its top-left key as IntlBackslash:
        // the @ and # of a French Mac went as that position, and a French host
        // typed "<" and ">" for them (24/10/2026).
        const QString ch = keyChar(msg);
        if (mode == KeyboardMode::Native && !ch.isEmpty() && !msg["dead"].toBool(false) &&
            !byPosition(msg, mode, fidelity))
            plan.text = ch;
        return plan;
    }

    plan.keyCode = static_cast<short>(msg["keyCode"].toInt(0));
    plan.flags = 0;
    // The right Alt comes as VK_RMENU, AltGr to a Linux host. The native host
    // keeps it as VK_MENU: it lets the viewer's Alt go for its own AltGr and
    // puts it back as that Alt (WinTextKeys). Held as the right one, it went
    // up with the symbol, and the Alt put back stayed down for good.
    if (mode == KeyboardMode::Native && plan.keyCode == kVkRightAlt) plan.keyCode = kVkAlt;

    // The numeric keypad is the same on every layout, and a key of its own:
    // its 1 typed as a character came out as the 1 of the main row — Shift+&
    // on a French host — which Alt+0233 codes and Blender's views never see.
    if (code.startsWith(QLatin1String("Numpad"))) return plan;

    const QString ch = keyChar(msg);
    if (msg["chord"].toBool(false) && !ch.isEmpty()) {
        switch (mode) {
        case KeyboardMode::Native:
            plan.text = ch;
            plan.chord = true;
            break;
        case KeyboardMode::SunshineWindows:
            if (const short vk = letterVk(ch)) {
                plan.keyCode = vk;
                plan.flags = SS_KBE_FLAG_NON_NORMALIZED;
            }
            break;
        case KeyboardMode::Positional: break;
        }
        return plan;
    }

    const bool positional = byPosition(msg, mode, fidelity);

    // A dead key (the client's layout waits for the next key to compose: the
    // tilde of AltGr+2 on a French keyboard). Its composed character arrives
    // with that next key, and the native host types it in its own layout — so
    // the dead key itself must not reach it. Pressed as a position, it typed
    // what the host's layout has there without the client's AltGr: "é" on a
    // French host, then "~" with the space, "é~" for one tilde (12/10/2026).
    // A Sunshine host composes nothing from text and keeps the position, and
    // so does a key sent by position: the host composes it in its layout. So
    // does gaming mode under "always": the client does not hold the next key
    // back there to read what the two compose to, so the host has to.
    if (mode == KeyboardMode::Native && !positional && msg["dead"].toBool(false) &&
        !msg["gaming"].toBool(false)) {
        plan.skip = true;
        return plan;
    }

    if (ch.isEmpty() || positional) return plan;

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
        // EVERY letter, agreeing ones included (Bruno's decision of
        // 22/10/2026). Until then only a letter that differs from the US one,
        // to keep the fixed scancode Sunshine derives for a normalized VK —
        // the non-normalized one comes from ITS OWN thread's layout
        // (MapVirtualKeyW, windows_backend.cpp:1433). That left the mirror case
        // wrong: a QWERTY viewer on an AZERTY Sunshine presses the key marked
        // A, means "a", agrees with US, and the position typed "q". The one
        // loss is a Sunshine thread whose layout differs from the foreground
        // window's, which is rare. In gaming mode the position stays (above).
        if (const short vk = letterVk(ch)) {
            plan.keyCode = vk;
            plan.flags = SS_KBE_FLAG_NON_NORMALIZED;
        }
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

/// The modifier byte of a key sent by its position. A character typed under
/// AltGr carries Ctrl and Alt in its DOM flags (Chrome on Windows reports
/// AltGraph as both), and a positional host — Wolf, Sunshine on Linux —
/// presses whatever the byte names around the key: Ctrl and the LEFT Alt went
/// down with the E, the host's AltGr no longer applied, and AltGr+E typed no €
/// on a French Wolf (issue #44, 29/10/2026). The right Alt is held as a key of
/// its own, which is all the host's layout needs. Other hosts keep the byte.
inline char positionModifiers(const QJsonObject& msg, KeyboardMode mode)
{
    char mods = modifierMask(msg);
    if (mode == KeyboardMode::Positional && msg["altGraph"].toBool(false))
        mods &= ~(MODIFIER_CTRL | MODIFIER_ALT);
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
inline QVector<IMediaEngine::HeldKey> parseHeldKeys(const QJsonObject& msg, KeyboardMode mode,
                                                    LayoutFidelity fidelity)
{
    QVector<IMediaEngine::HeldKey> keys;
    const QJsonArray arr = msg["keys"].toArray();
    keys.reserve(arr.size());
    // A positional host holds no AltGr's Ctrl, as a key or in a key's byte,
    // unless the viewer also holds a Ctrl of their own.
    bool ghostCtrl = false, ownCtrl = false;
    if (mode == KeyboardMode::Positional) {
        for (const QJsonValue& v : arr) {
            const QJsonObject k = v.toObject();
            const QString code = k["code"].toString();
            if (altGrCtrl(k))
                ghostCtrl = true;
            else if (code == QLatin1String("ControlLeft") || code == QLatin1String("ControlRight"))
                ownCtrl = true;
        }
    }
    for (const QJsonValue& v : arr) {
        const QJsonObject k = v.toObject();
        if (ghostCtrl && altGrCtrl(k)) continue;
        const KeyPlan plan = resolveKey(k, mode, fidelity);
        if (plan.isText() || plan.skip) continue;
        IMediaEngine::HeldKey held;
        held.keyCode = plan.keyCode;
        held.flags = plan.flags;
        held.modifiers = modifierMask(k);
        if (ghostCtrl && !ownCtrl) held.modifiers &= ~MODIFIER_CTRL;
        held.hold = k["hold"].toBool(false);
        keys.append(held);
    }
    return keys;
}

/// An 'inputstate' heartbeat as the host may act on it.
struct HeldState
{
    QVector<IMediaEngine::HeldKey> keys;
    quint32 buttons = 0;
    bool buttonsHold = false;
};

/// Read a heartbeat and strip from it what @p p forbids — the one way every
/// transport reads it. The watchdog presses whatever key or button a heartbeat
/// lists that it does not already hold down (InputWatchdog::sync), so an
/// unfiltered one lets a guest who may only watch press keys and click on the
/// host. Only DataChannelRelay filtered it until 01/10/2026; webrtc-media and
/// WSS passed it straight through.
inline HeldState readHeldState(const QJsonObject& msg, KeyboardMode mode, LayoutFidelity fidelity,
                               const Policy& p)
{
    HeldState s;
    s.keys = parseHeldKeys(msg, mode, fidelity);
    s.buttons = static_cast<quint32>(msg["buttons"].toInt(0));
    s.buttonsHold = msg["buttonsHold"].toBool(false);
    filterHeldState(p, s.keys, s.buttons);
    return s;
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

/// While this answers true, logKey() writes nothing, whatever the switch says.
/// Whoever turns the diagnostics on sets it to the native host's "the secure
/// desktop has the input" (NativeHost::secureDesktopHasInput): what is typed on
/// a lock screen or in a UAC prompt is a PIN or a password, and never belongs
/// in a log. Null = never quiet, for a process with no such desktop to ask.
using QuietProbe = bool (*)();
inline std::atomic<QuietProbe>& quietProbeSlot()
{
    static std::atomic<QuietProbe> probe{nullptr};
    return probe;
}
inline void setQuietProbe(QuietProbe probe)
{
    quietProbeSlot().store(probe, std::memory_order_relaxed);
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
    const QString ch = keyChar(msg);
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
        sent = plan.chord ? QStringLiteral("chord, host presses the key carrying it")
                          : QStringLiteral("character, host resolves");
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
    // The client's mouse mode, which decides positions or characters under
    // "auto". Last, so a reader of the verdicts above is not thrown.
    if (msg.contains(QLatin1String("gaming")))
        diag.line += msg["gaming"].toBool(false) ? QStringLiteral(" | mode: gaming")
                                                 : QStringLiteral(" | mode: desktop");
    return diag;
}

/// Write @p msg's diagnostic, if diagnostics are on and the key has one. Called
/// by each transport on a key DOWN only: a release resolves identically by
/// construction (the client replays the press's character), so logging it again
/// would double every line for nothing.
inline void logKey(const QJsonObject& msg, KeyboardMode mode, const KeyPlan& plan, bool down)
{
    if (!down || !debugEnabled()) return;
    const QuietProbe quiet = quietProbeSlot().load(std::memory_order_relaxed);
    if (quiet && quiet()) return;
    const KeyDiag diag = describeKey(msg, mode, plan);
    if (diag.line.isEmpty()) return;
    if (diag.warn)
        qWarning().noquote() << diag.line;
    else
        qInfo().noquote() << diag.line;
}

/// One key message, resolved, logged and handed to the engine — the way every
/// transport does it, so the four of them cannot drift apart on what a key
/// becomes. @p withHold passes the client's `hold` (a movement key kept down
/// through a brief stall in gaming mode); the WSS fallback has never honoured
/// it, and keeps not doing so.
inline void sendKey(IMediaEngine& engine, const QJsonObject& msg, bool down, bool withHold = true)
{
    const KeyboardMode mode = engine.keyboardMode();
    const LayoutFidelity fidelity = engine.layoutFidelity();
    // The release of an AltGr's Ctrl is for a positional host only: Windows
    // keeps it down under the right Alt, as a keyboard plugged into it does.
    if (altGrCtrl(msg) && mode != KeyboardMode::Positional) return;
    // One line when the client's keys move between its two mouse modes: under
    // "auto" that is when they go from characters to positions, or back.
    if (down && msg.contains(QLatin1String("gaming"))) {
        const bool gaming = msg["gaming"].toBool(false);
        if (engine.noteGamingKeys(gaming))
            qInfo().noquote() << "[Input] keys from the client's"
                              << (gaming ? "gaming mode" : "desktop mouse mode") << "—"
                              << (mode == KeyboardMode::Positional ||
                                          fidelity == LayoutFidelity::Never ||
                                          (gaming && fidelity == LayoutFidelity::Auto)
                                      ? "by position"
                                      : "by the client's characters");
    }
    const KeyPlan plan = resolveKey(msg, mode, fidelity);
    logKey(msg, mode, plan, down);
    // A dead key on the native host: its character comes with the next key.
    // In its place, a tap of a key that types nothing: under a Mac's Option
    // (Option+n for "~") the host saw an Alt go down and would see it come
    // up with nothing between, which Windows takes for a tap of Alt — the
    // menu bar would get the keys.
    if (plan.skip) {
        if (down) {
            engine.sendKeyEvent(kMaskVk, true, modifierMask(msg), 0);
            engine.sendKeyEvent(kMaskVk, false, modifierMask(msg), 0);
        }
        return;
    }
    if (plan.isText()) {
        // A character the host must honour instead of its position: a real
        // press and release on the native host, a one-shot text injection on
        // Sunshine (where the release has nothing to do).
        IMediaEngine::CharKeyHints hints;
        hints.chord = plan.chord;
        hints.positionVk = plan.keyCode;
        hints.altGraph = msg["altGraph"].toBool(false);
        engine.sendKeyChar(plan.text, down, modifierMask(msg), hints);
        return;
    }
    // `hold`: a movement key the client wants kept down through a brief stall
    // instead of released at the short grace period (see the shim).
    engine.sendKeyEvent(plan.keyCode, down, positionModifiers(msg, mode), plan.flags,
                        withHold && msg["hold"].toBool(false));
}

/// The key a US keyboard types @p c with, and whether it takes Shift. False for
/// a character no US key carries (an accent, an emoji, a control character).
inline bool usKeyOf(QChar c, short& vk, bool& shift)
{
    const char16_t u = c.unicode();
    shift = false;
    if (u >= 'a' && u <= 'z') {
        vk = static_cast<short>(u - 'a' + 'A');
        return true;
    }
    if (u >= 'A' && u <= 'Z') {
        vk = static_cast<short>(u);
        shift = true;
        return true;
    }
    if (u >= '0' && u <= '9') {
        vk = static_cast<short>(u);
        return true;
    }
    static constexpr struct
    {
        char16_t plain, shifted;
        short vk;
    } kKeys[] = {
        {' ', 0, 0x20},   {'\n', 0, 0x0D},   {'\t', 0, 0x09},  {';', ':', 0xBA},  {'=', '+', 0xBB},
        {',', '<', 0xBC}, {'-', '_', 0xBD},  {'.', '>', 0xBE}, {'/', '?', 0xBF},  {'`', '~', 0xC0},
        {'[', '{', 0xDB}, {'\\', '|', 0xDC}, {']', '}', 0xDD}, {'\'', '"', 0xDE}, {0, ')', '0'},
        {0, '!', '1'},    {0, '@', '2'},     {0, '#', '3'},    {0, '$', '4'},     {0, '%', '5'},
        {0, '^', '6'},    {0, '&', '7'},     {0, '*', '8'},    {0, '(', '9'},
    };
    for (const auto& k : kKeys) {
        if (u == k.plain || (k.shifted && u == k.shifted)) {
            vk = k.vk;
            shift = u == k.shifted;
            return true;
        }
    }
    return false;
}

/// One step of typing a string: a key pressed and released (its US position,
/// Shift in the modifier byte, AltGr as the right Alt held around it), or a run
/// of text.
struct TextStep
{
    short keyCode = 0;
    bool shift = false;
    QString text;
    bool altGr = false;

    bool isText() const { return !text.isEmpty(); }
};

/// How the touch keyboard's text reaches a host running @p mode.
///
/// Everywhere but a positional host it goes as text, which those hosts inject
/// exactly. A positional host — Wolf, Sunshine on Linux — types text through
/// the GTK/IBus sequence (Ctrl+Shift+U, hex, space), and a session with no
/// input method running gets nothing, or the hex digits: a game in a Wolf
/// container took no letter at all from a phone, while its arrows worked and
/// the official app typed fine (issue #44, 28/10/2026). So a character a US
/// key carries goes as that key, Shift riding in the modifier byte, which
/// Wolf and Sunshine both press around the key — what Moonlight for Android
/// does with its soft keyboard (Game.java, handleKeyDown). The host's layout
/// decides what the key types, as it does for the physical keyboard on these
/// hosts. Only what no US key carries is left to text.
///
/// With @p layout — this machine's, for a host that runs on it, when the owner
/// ticked the box — every character goes on the key that layout puts it on,
/// dead keys included: `a` is the US Q on an AZERTY, `é` its 2, `€` AltGr+E,
/// where the text sequence typed nothing in a game (SDL, GLFW) and closed the
/// XFCE session for `€` (29/10/2026). Only what the layout cannot type is left
/// to text.
inline QVector<TextStep> planText(const QString& text, KeyboardMode mode,
                                  const mw::native::HostKeyboardLayout* layout = nullptr)
{
    QVector<TextStep> steps;
    if (text.isEmpty()) return steps;
    if (mode != KeyboardMode::Positional) {
        steps.append({0, false, text});
        return steps;
    }
    QString run;
    for (qsizetype i = 0; i < text.size(); ++i) {
        // A whole code point: an emoji is two UTF-16 units, and the layout
        // speaks code points.
        QString unit(text[i]);
        char32_t cp = text[i].unicode();
        if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(text[i], text[i + 1]);
            unit += text[i + 1];
            ++i;
        }

        QVector<TextStep> keys;
        mw::native::LayoutChar typed;
        short vk = 0;
        bool shift = false;
        if (layout) {
            if (layout->find(cp, typed)) {
                for (int k = 0; k < typed.count; ++k) {
                    const mw::native::LayoutKey& key = typed.keys[k];
                    keys.append({static_cast<short>(key.vk), key.shift, QString(), key.altGr});
                }
            }
        } else if (unit.size() == 1 && usKeyOf(unit[0], vk, shift)) {
            keys.append({vk, shift, QString()});
        }

        if (keys.isEmpty()) {
            run += unit;
            continue;
        }
        if (!run.isEmpty()) {
            steps.append({0, false, run});
            run.clear();
        }
        steps += keys;
    }
    if (!run.isEmpty()) steps.append({0, false, run});
    return steps;
}

/// The characters of @p steps a host running @p mode can only type through its
/// input method: the text a positional host enters with Ctrl+Shift+U, which
/// nothing types without IBus in the host's session (issue #44). Empty when
/// every character went as a key, or when the host injects text itself.
inline QString inputMethodText(const QVector<TextStep>& steps, KeyboardMode mode)
{
    QString left;
    if (mode != KeyboardMode::Positional) return left;
    for (const TextStep& step : steps)
        left += step.text;
    return left;
}

/// Type the touch keyboard's @p text on the host (see planText).
///
/// @returns the characters left to the host's input method (inputMethodText),
///          for the page to say why they may not arrive.
inline QString sendText(IMediaEngine& engine, const QString& text)
{
    const std::shared_ptr<const mw::native::HostKeyboardLayout> layout = engine.hostTextLayout();
    const KeyboardMode mode = engine.keyboardMode();
    const QVector<TextStep> steps = planText(text, mode, layout.get());
    for (const TextStep& step : steps) {
        if (step.isText()) {
            engine.sendUtf8Text(step.text);
            continue;
        }
        const char mods = step.shift ? MODIFIER_SHIFT : 0;
        if (step.altGr) engine.sendKeyEvent(kVkRightAlt, true, 0, 0);
        engine.sendKeyEvent(step.keyCode, true, mods, 0);
        engine.sendKeyEvent(step.keyCode, false, mods, 0);
        if (step.altGr) engine.sendKeyEvent(kVkRightAlt, false, 0, 0);
    }
    return inputMethodText(steps, mode);
}

} // namespace InputMsg
