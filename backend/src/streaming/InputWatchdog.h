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

#include "IMediaEngine.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QTimer;

/// The input dead-man switch, shared by both media engines.
///
/// The input channel is ordered+reliable: a link stall does not lose events,
/// it DELAYS them. A press that already landed therefore stays down on the
/// host for the whole stall while its release waits in the SCTP queue, and
/// the guest OS typematic turns one keystroke into dozens ("rrrrrrr..." for
/// a 2 s freeze). A stuck key also outlives a link that never comes back.
///
/// Nothing the client sends during the stall can fix this — its correction
/// would ride the same blocked queue. So the host side watches for silence:
/// every message from the client refreshes a liveness timestamp, and when it
/// goes stale we release whatever is still held. The client heartbeats its
/// full held-input state (only while something IS held), so the next message
/// after the stall both refreshes liveness and re-presses anything the user
/// is genuinely still holding — see sync().
///
/// Two grace periods: `hold` inputs (movement keys/buttons in gaming mode)
/// get the long one, so a network hiccup does not stop a player mid-stride;
/// everything else gets the short one, chosen to fire BEFORE the typematic
/// delay (~500 ms on Windows) so no repeat is ever generated. Gamepads are
/// gaming input by definition: only the long period neutralizes them.
///
/// Shift, Ctrl, Alt and the Windows key get the long one too: a repeat of
/// theirs types nothing, while letting go of one the viewer still holds and
/// pressing it again when the link wakes is a tap of it the viewer never
/// made — a Windows key alone opens the Start menu, an Alt alone the menu
/// bar. A Mac on Wi-Fi went silent for 293 ms under a held right Option,
/// and the host saw the Option let go of, then pressed again (bench of
/// 09/11/2026).
///
/// ── Why it is its own class ─────────────────────────────────────────────────
///
/// A key held through a link stall sticks down just the same whether the host
/// is a remote GameStream machine or this one. The watchdog therefore owns
/// the state — what is held, since when the client was last heard — and the
/// engine only supplies what letting go MEANS on its wire, through the Sink.
/// It used to live inside MoonlightShim, which left the native engine with
/// no watchdog at all.
///
/// ── Threading ───────────────────────────────────────────────────────────────
///
/// The note*() feeds and release() may be called from any thread: the relay
/// input path runs on the relay thread while the clipboard paste path injects
/// its chord from the main thread. The state is mutex-guarded, and only the
/// QTimer (thread-affine) is hopped back onto the thread this object lives
/// on — which is the engine's, since the engine owns it.
class InputWatchdog : public QObject
{
    Q_OBJECT

public:
    using HeldKey = IMediaEngine::HeldKey;

    /// What letting go means on the engine's wire. Each is called once per
    /// input to release, from the tick (this object's thread) or from
    /// release() (the caller's). An engine that is no longer connected simply
    /// ignores the call.
    struct Sink
    {
        std::function<void(const HeldKey& key)> releaseKey;
        std::function<void(int button)> releaseButton;
        std::function<void(short controller, short mask)> neutralizePad;
        /// The release of a character noteChar() saw go down, with the hints
        /// of its press: the native host lets go of the key that typed it.
        std::function<void(const QString& ch, const CharKeyHints& hints)> releaseChar;
    };

    static constexpr int kTickMs = 100;
    static constexpr int kStaleMs = 250;
    static constexpr int kStaleHoldMs = 3000;

    explicit InputWatchdog(Sink sink, QObject* parent = nullptr);

    // ── Feeds: what the engine just applied ──────────────────────────────────

    /// Returns true for a press of a key already held: the client's auto-repeat
    /// (or a press the watchdog has not released yet), which an engine whose
    /// host repeats on its own does not forward.
    bool noteKey(const HeldKey& key, bool down);
    /// `button` is the browser's 1..32 numbering; `hold` marks a press to keep
    /// through a brief stall (aim/fire in gaming mode).
    void noteButton(int button, bool down, bool hold);
    /// A pad state was sent. `atRest` (see padAtRest) drops the pad out of the
    /// set — a removal reads as at-rest and does the same.
    void notePad(short controller, short mask, bool atRest);
    /// A character the native host holds down on a key of its own layout
    /// (IMediaEngine::sendKeyChar). Released like a key, on a silent link and
    /// by release() — a guest whose keyboard is taken away can no longer send
    /// its key-up, and the key stayed down on the host, a game walking on,
    /// until the guest had a keyboard again (bench of 05/11/2026). Never
    /// pressed again: the heartbeat leaves characters out, as a re-press
    /// would type the letter once more.
    ///
    /// Returns false for the client's key-up of a character the watchdog let
    /// go of already, which the engine does not forward: a second release
    /// would be a key-up of more on macOS, and on Windows a chord's key let go
    /// of twice, under a Ctrl the viewer really holds. A new press of it
    /// forwards again.
    bool noteChar(const QString& ch, const CharKeyHints& hints, bool down);

    /// A pad only sends on change, so a stick pushed and left there goes
    /// silent exactly like a held key. The threshold is deliberately loose:
    /// neutralizing a barely-drifted stick on a dead link costs nothing,
    /// missing a shoved one costs a runaway.
    static bool padAtRest(int buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
                          short leftStickX, short leftStickY, short rightStickX, short rightStickY);

    /// Refresh the client-liveness timestamp. Call on every inbound client
    /// message, whatever its type. Returns how long the client had been
    /// silent when that silence was long enough to fire the watchdog, 0
    /// otherwise — a freeze of the link's upstream, for the stats.
    qint64 noteClientAlive();

    // ── Heartbeat ────────────────────────────────────────────────────────────

    /// What the engine has to send to match the client's authoritative held
    /// state: press what the watchdog released but the user still holds,
    /// release what drifted. Apply releases before presses.
    struct SyncDiff
    {
        QVector<HeldKey> press;
        QVector<HeldKey> release;
        QVector<int> buttonsDown;
        QVector<int> buttonsUp;
    };
    /// Reconcile the held state with the client's heartbeat. The first call is
    /// also the handshake that arms the timer: an older cached frontend that
    /// never heartbeats goes silent while a key is legitimately held, and must
    /// not have it released out from under it. `buttonMask` is 1 << (button - 1).
    SyncDiff sync(const QVector<HeldKey>& keys, quint32 buttonMask, bool buttonsHold);

    // ── Release ──────────────────────────────────────────────────────────────

    /// Let go of everything held, through the sink. `includeHold` also
    /// releases the inputs flagged `hold`, and the gamepads; without it,
    /// `keepModifiers` keeps Shift, Ctrl, Alt and the Windows key as well
    /// (the short grace period's own release).
    void release(bool includeHold, bool keepModifiers = false);

    /// Shift, Ctrl, Alt or the Windows key, either side, as a Windows
    /// virtual-key code.
    static bool isModifierVk(short vk);

    bool anythingHeld() const;

    // ── Test hooks ───────────────────────────────────────────────────────────

    /// Replace the clock, so a stall can be simulated without waiting it out.
    void setClock(std::function<qint64()> nowMs);
    /// One watchdog tick, as the timer would run it.
    void tick();

private:
    /// Run the timer only while something is actually held and the client has
    /// proved it heartbeats.
    void arm();

    Sink m_Sink;
    QTimer* m_Timer = nullptr;
    QElapsedTimer m_Clock;
    std::function<qint64()> m_Now;

    mutable QMutex m_Mutex;
    qint64 m_LastAliveMs = -1; // -1 until the first client message
    QHash<short, HeldKey> m_HeldKeys;
    quint32 m_HeldButtons = 0; // 1 << (button - 1)
    bool m_HeldButtonsHold = false;
    QHash<short, short> m_ActivePads; // controller → active mask
    QHash<QString, CharKeyHints> m_HeldChars;
    QSet<QString> m_ReleasedChars; // let go of here, their key-up still to come
    bool m_ShortFired = false;     // don't re-log/re-release each tick
    bool m_LongFired = false;
    bool m_HeartbeatSeen = false;

    bool anythingHeldLocked() const
    {
        return !m_HeldKeys.isEmpty() || m_HeldButtons != 0 || !m_ActivePads.isEmpty() ||
               !m_HeldChars.isEmpty();
    }
};
