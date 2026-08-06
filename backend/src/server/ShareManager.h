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

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

/**
 * Session sharing — the owner invites up to three Players into the stream that
 * is already running, each as viewer, gamepad-only, or full control.
 *
 * Every share is an *activation*: one click on a Player row mints a fresh
 * link token AND a fresh 6-digit PIN, both valid for eight hours and for that
 * click only. Clicking again mints a new pair and kills the old one. The link
 * is expected to travel over Discord, SMS, anything — so it is never a
 * credential on its own: joining needs the token (in the URL) *and* the PIN
 * (sent separately). What the player's browser keeps afterwards is a cookie
 * bound to that activation, so a reload does not ask for the PIN again.
 *
 * Nothing here is stored in the clear: token, PIN and cookies live as SHA-256
 * digests and are compared in constant time, the same policy AuthManager
 * applies to session cookies.
 *
 * Deliberately free of AuthManager, HttpServer and Sunshine: the whole state
 * machine is pure logic over Qt Core, so tests/test_share_manager.cpp covers it
 * on every platform. The rate-limit tiers below are a copy of AuthManager's on
 * purpose — a burst against a share PIN must never lock the owner out of their
 * own PIN login, so the two cannot share a bucket.
 */
class ShareManager : public QObject
{
    Q_OBJECT

public:
    /// Feature flag. OFF makes every share route answer 404 and hides the UI —
    /// the code stays shipped, the feature disappears. Developer switch, not a
    /// user setting.
    static constexpr bool kSessionSharingEnabled = true;

    /// Player slots map 1:1 onto stream worker slots: 0 and 1 belong to the
    /// owner (primary + standby), 2..4 to the players.
    static constexpr int kFirstSlot = 2;
    static constexpr int kSlotCount = 3;
    static constexpr int kLastSlot = kFirstSlot + kSlotCount - 1;

    /// How long an activation lives. Only an owner action cuts it short.
    static constexpr qint64 kTtlSecs = 8 * 3600;

    /// Wrong PINs before the activation is destroyed outright. Whoever holds a
    /// leaked link can therefore kill the share, never brute-force into it.
    static constexpr int kMaxPinFailures = 10;

    /// Cookies kept per activation (phone + laptop is legitimate; a crowd is
    /// not). Each one still costs a correct PIN to obtain.
    static constexpr int kMaxCookiesPerActivation = 4;

    enum class State
    {
        Off,      ///< no live activation
        Shared,   ///< link alive, nobody streaming
        Streaming ///< the player is streaming
    };

    /// Why a player's session is being ended — drives the message they see.
    enum class EndReason
    {
        OwnerToggle, ///< owner clicked the player row
        OwnerStop,   ///< owner stopped the stream / the app went away
        Expired,     ///< the eight hours ran out
        PinAbuse     ///< too many wrong PINs on this activation
    };
    Q_ENUM(EndReason)

    struct Permissions
    {
        bool gamepad = false;
        bool keyboardMouse = false;

        /// "viewer" | "gamer" | "full" — what the popin warns about.
        QString accessLevel() const;
        QJsonObject toJson() const;
        static Permissions fromJson(const QJsonObject& obj);
    };

    struct SlotStatus
    {
        int slot = 0;
        State state = State::Off;
        Permissions permissions;
        bool locked = false;  ///< permissions frozen (popin closed or joined)
        qint64 expiresAt = 0; ///< unix secs, 0 when Off
    };

    explicit ShareManager(QObject* parent = nullptr);
    ~ShareManager() override;

    static bool isPlayerSlot(int slot) { return slot >= kFirstSlot && slot <= kLastSlot; }

    // ── Owner side ──────────────────────────────────────────────────────────

    /// Every player slot, always kSlotCount entries, lowest slot first.
    QList<SlotStatus> status();

    /// Mint a new activation on @p slot, revoking any previous one. Returns
    /// false for a slot outside the player range.
    bool activate(int slot, QString& outToken, QString& outPin, SlotStatus& outStatus);

    /// The raw link token and PIN of a live activation, so the owner reopening
    /// the popin can read what they already sent out. False when the slot is
    /// Off, or when the process restarted since: the clear values live in
    /// memory only, share.json keeps digests.
    bool secrets(int slot, QString& outToken, QString& outPin);

    /// Update the input permissions while the popin is still open. Returns
    /// false when the slot is Off or already locked.
    bool setPermissions(int slot, const Permissions& perms);

    /// Freeze the permissions for the rest of this activation (popin closed).
    /// Idempotent; false only for a slot with no activation.
    bool lockPermissions(int slot);

    /// Revoke the activation: token, PIN and cookies die, and any live stream
    /// is asked to disconnect. Returns false when the slot was already Off.
    bool deactivate(int slot, EndReason reason);

    /// Revoke every slot at once (owner pressed Stop, or the host app is gone).
    void deactivateAll(EndReason reason);

    /// Players currently streaming — the number shown as "Share (n)".
    int streamingCount();

    // ── Player side ─────────────────────────────────────────────────────────

    enum class PinResult
    {
        Ok,
        Invalid,     ///< wrong PIN, unknown token, or expired activation
        RateLimited, ///< too many attempts from this caller, try later
    };

    struct PinOutcome
    {
        PinResult result = PinResult::Invalid; // fail closed
        int slot = -1;
        QString cookie; ///< raw mw_player value, only on Ok
        int lockoutSeconds = 0;
        int remainingAttempts = 0;
    };

    /// Trade a (link token, PIN) pair for a player cookie. @p rateKey is the
    /// caller's rate-limit bucket key (an address prefix), never shown to the
    /// user. An unknown token and a wrong PIN are answered identically.
    PinOutcome redeemPin(const QString& token, const QString& pin, const QString& rateKey);

    /// The slot a raw mw_player cookie unlocks, -1 when it matches no live
    /// activation. This is the only thing that opens /ws2../ws4 and the player
    /// routes — a player is never a MoonlightWeb user.
    int slotForCookie(const QString& cookie);

    /// True when @p token names a live activation. Used only to decide whether
    /// the join page should ask for a PIN; it reveals nothing else.
    bool tokenIsLive(const QString& token);

    // ── Stream lifecycle ────────────────────────────────────────────────────

    /// Mark a player's stream as up or down. Dropping to false keeps the
    /// activation alive (an automatic disconnect must never end the share).
    void setStreaming(int slot, bool streaming);

    Permissions permissions(int slot);
    State state(int slot);
    qint64 expiresAt(int slot);

signals:
    /// A slot's observable state changed — the owner's dropdown should refresh.
    void slotChanged(int slot);

    /// A live player stream must be torn down. Carries EndReason as an int so
    /// the signal crosses to main.cpp without dragging the enum along.
    void playerMustDisconnect(int slot, int reason);

private:
    struct Activation
    {
        QString id;
        QString tokenHash;
        QString pinHash;
        QStringList cookieHashes;
        Permissions permissions;
        bool locked = false;
        qint64 activatedAt = 0;
        int pinFailures = 0;
        bool streaming = false; ///< runtime only, never persisted

        /// The clear link token and PIN, so the owner can reopen the popin and
        /// read what they already handed out. Runtime only — share.json holds
        /// digests, so the file on disk is never a usable invitation and a
        /// restart simply forgets the pair while the share stays valid.
        QString tokenClear;
        QString pinClear;

        bool live() const { return !tokenHash.isEmpty(); }
    };

    struct Slot
    {
        Permissions lastPermissions; ///< prefills the next popin
        Activation activation;
    };

    /// Tiered lockout, mirroring AuthManager's ladder (3 → 30 s, 5 → 2 min,
    /// 10 → 10 min) on buckets of our own.
    struct RateEntry
    {
        int failures = 0;
        qint64 lastFailure = 0;
        qint64 lockoutUntil = 0;
    };

    Slot* slotFor(int slot);
    /// The slot whose live activation matches @p tokenHash, or nullptr.
    Slot* slotForTokenHash(const QString& tokenHash, int* outSlot = nullptr);

    /// Drop activations past their TTL, disconnecting whoever is streaming.
    /// Called at the top of every public accessor and by the sweep timer.
    void pruneExpired();

    /// Wipe an activation and notify. @p notify false during load/shutdown.
    void clearActivation(int slot, EndReason reason, bool notify = true);

    void save();
    void load();

    int rateLockout(const QString& bucket) const;
    /// Record an attempt on @p bucket, returning the seconds of lockout it
    /// earned (0 when none) and updating the remaining-attempts count.
    void rateRecord(const QString& bucket, bool matched, int& outRemaining, int& outLockout);
    void ratePurge();

    static QString hash(const QString& value);
    static bool constantTimeEquals(const QString& a, const QString& b);
    static QString randomToken();
    static QString randomPin();

    QHash<int, Slot> m_Slots;
    QHash<QString, RateEntry> m_RateLimits;
    QTimer* m_SweepTimer = nullptr;

    static constexpr int RATE_TIER1 = 3;
    static constexpr int RATE_TIER2 = 5;
    static constexpr int RATE_TIER3 = 10;
    static constexpr int LOCKOUT_SHORT_SECS = 30;
    static constexpr int LOCKOUT_MEDIUM_SECS = 120;
    static constexpr int LOCKOUT_LONG_SECS = 600;
};
