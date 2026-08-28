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
 * Session sharing — the owner invites up to three Players into a stream, each
 * as viewer, gamepad-only, desktop or full control.
 *
 * Every share is an *activation*: opening a Player row mints a fresh link token
 * AND a fresh 6-digit PIN, both valid for that row's chosen lifetime and for
 * that opening only. Opening again mints a new pair and kills the old one. The
 * link is expected to travel over Discord, SMS, anything — so it is never a
 * credential on its own: joining needs the token (in the URL) *and* the PIN
 * (sent separately). What the player's browser keeps afterwards is a cookie
 * bound to that activation, so a reload does not ask for the PIN again.
 *
 * One activation binds exactly one device (kMaxCookiesPerActivation). A correct
 * PIN offered from a second device is not a failure — it is the interesting
 * event, the one that tells an owner their invitation was forwarded — so it is
 * answered AlreadyBound, recorded for the board to show, and costs the
 * legitimate holder none of their PIN attempts.
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

    /// Default lifetime of an activation, and the choices the board offers.
    /// 0 means unlimited: only an owner action ends it. Anything not on this
    /// list is refused — the lifetime arrives from a browser.
    static constexpr qint64 kTtlSecs = 8 * 3600;
    static bool isValidTtl(qint64 secs);

    /// Wrong PINs before the activation is destroyed outright. Whoever holds a
    /// leaked link can therefore kill the share, never brute-force into it.
    static constexpr int kMaxPinFailures = 10;

    /// Devices per activation. One, so that "Binded" means what it says on the
    /// board: this invitation answers to a single machine. The cost is that a
    /// guest changing browser needs a fresh link; the gain is that forwarding
    /// the link *and* the PIN no longer buys a second seat.
    static constexpr int kMaxCookiesPerActivation = 1;

    /// Longest a name may be. It is typed by the owner and shown back to them.
    static constexpr int kMaxNameLength = 32;

    enum class State
    {
        Off,    ///< no live activation
        Shared, ///< link alive, PIN not consumed yet
        Binded  ///< the PIN was consumed: this link answers to one device now
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

        /// The badge the board shows. A real table over both flags: keyboard
        /// and mouse without a gamepad is "desktop", not "full" — the two are
        /// different promises, and calling them the same misled the owner.
        ///
        ///   ✗ ✗ → "viewer"   ✓ ✗ → "gamer"
        ///   ✗ ✓ → "desktop"  ✓ ✓ → "full"
        QString accessLevel() const;
        QJsonObject toJson() const;
        static Permissions fromJson(const QJsonObject& obj);
    };

    /// A device that spent the PIN. The owner sees these on the board, which is
    /// the whole point: an invitation that was forwarded stops being invisible.
    struct BoundDevice
    {
        qint64 boundAt = 0;
        QString userAgent; ///< raw UA, trimmed; the board turns it into "Chrome · Windows"
    };

    struct SlotStatus
    {
        int slot = 0;
        State state = State::Off;
        QString name; ///< owner-given label, empty means "Player N"
        Permissions permissions;
        bool streaming = false; ///< orthogonal to state: a Binded row may be idle
        qint64 ttlSecs = kTtlSecs;
        qint64 expiresAt = 0; ///< unix secs; 0 when Off or unlimited
        QList<BoundDevice> devices;

        /// Last time a correct PIN arrived from a device that was not the bound
        /// one. 0 when it never happened. The board surfaces it verbatim.
        qint64 lastRefusedAt = 0;
        QString lastRefusedAgent;
    };

    explicit ShareManager(QObject* parent = nullptr);
    ~ShareManager() override;

    static bool isPlayerSlot(int slot) { return slot >= kFirstSlot && slot <= kLastSlot; }

    // ── Owner side ──────────────────────────────────────────────────────────

    /// Every player slot, always kSlotCount entries, lowest slot first.
    QList<SlotStatus> status();

    /// Mint a new activation on @p slot, revoking any previous one. @p hostUuid
    /// (and @p appId) is the host this invitation is bound to — either the one
    /// the owner is streaming right now, or the one they picked when opening the
    /// row cold. A player is only ever routed there, never to whatever other host
    /// happens to be up when they join. @p ttlSecs must be one of the offered
    /// lifetimes (0 = unlimited). Returns false for a slot outside the player
    /// range, an empty @p hostUuid (nothing to bind to), or an unlisted lifetime.
    bool activate(int slot, const QString& hostUuid, int appId, qint64 ttlSecs, QString& outToken,
                  QString& outPin, SlotStatus& outStatus);

    /// The raw link token and PIN of a live activation, so the owner reopening
    /// the popin can read what they already sent out. False when the slot is
    /// Off, or when the process restarted since: the clear values live in
    /// memory only, share.json keeps digests.
    bool secrets(int slot, QString& outToken, QString& outPin);

    /// Update the input permissions. Accepted at any time, including while the
    /// player is streaming: permissionsChanged() carries the new policy down to
    /// the live worker, so a guest is promoted or demoted without a reconnect.
    /// On a slot with nothing shared it records the choice the next activation
    /// will be minted with, so the board's poll cannot undo a fresh tick.
    /// Returns false only for a slot outside the player range.
    bool setPermissions(int slot, const Permissions& perms);

    /// Change how long this invitation still has to live, counted from *now*.
    ///
    /// It can only ever shorten: the new deadline is min(now + @p ttlSecs, the
    /// deadline already in force). Picking 8 h on a 24 h invitation with 20 h
    /// left leaves 8 h; picking it with 3 h left leaves 3 h, not 8. Measuring
    /// from the opening instead would make the same gesture mean "expire now"
    /// on an old invitation and "add five hours" on a fresh one. Unlimited is
    /// therefore only reachable at START — nothing here hands a live link more
    /// time than it already had.
    ///
    /// On a slot with nothing shared there is no deadline to cap, so this just
    /// records the lifetime START will use. False for an unlisted lifetime.
    bool setTtl(int slot, qint64 ttlSecs);

    /// Rename a player row. An empty name restores the default "Player N".
    /// False for a slot outside the player range.
    bool setName(int slot, const QString& name);
    QString name(int slot);

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
        Invalid,      ///< wrong PIN, unknown token, or expired activation
        RateLimited,  ///< too many attempts from this caller, try later
        AlreadyBound, ///< right PIN, wrong device: this link already has one
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
    /// user; @p userAgent is recorded so the owner can see which machine took
    /// the invitation. An unknown token and a wrong PIN are answered
    /// identically. A correct PIN on an invitation that is already bound is
    /// answered AlreadyBound and charges nobody's attempt counter.
    PinOutcome redeemPin(const QString& token, const QString& pin, const QString& rateKey,
                         const QString& userAgent = QString());

    /// The slot a raw mw_player cookie unlocks, -1 when it matches no live
    /// activation. This is the only thing that opens /ws2../ws4 and the player
    /// routes — a player is never a MoonlightWeb user.
    int slotForCookie(const QString& cookie);

    /// True when @p token names a live activation. Used only to decide whether
    /// the join page should ask for a PIN; it reveals nothing else.
    bool tokenIsLive(const QString& token);

    /// The slot @p token belongs to, -1 when it names no live activation. The
    /// player routes resolve the slot from the *link* and then require the
    /// cookie to agree: a browser holding player 1's cookie must not be read as
    /// player 1 when it opens player 2's link.
    int slotForToken(const QString& token);

    // ── Stream lifecycle ────────────────────────────────────────────────────

    /// Mark a player's stream as up or down. Dropping to false keeps the
    /// activation alive (an automatic disconnect must never end the share).
    void setStreaming(int slot, bool streaming);

    Permissions permissions(int slot);
    State state(int slot);
    /// Whether a worker is up on this slot. Orthogonal to state(): a Binded row
    /// whose guest closed their tab is bound but idle.
    bool isStreaming(int slot);
    /// Whether this slot's activation has already spent its PIN on a device.
    /// The join page asks so it can stop offering a PIN form nobody can pass:
    /// past this point the only correct answer is AlreadyBound.
    bool isBound(int slot);
    qint64 expiresAt(int slot);
    /// This activation's chosen lifetime, 0 when unlimited or the slot is Off.
    qint64 ttlSecs(int slot);

    /// The host uuid this slot's live activation is bound to, empty when Off.
    /// The player join uses this — and only this — to decide which host to reach,
    /// so a leaked link can never be pointed at a different machine.
    QString hostForSlot(int slot);
    /// The app id captured when the slot was shared, -1 when Off.
    int appForSlot(int slot);

signals:
    /// A slot's observable state changed — the owner's board should refresh.
    void slotChanged(int slot);

    /// A live player stream must be torn down. Carries EndReason as an int so
    /// the signal crosses to main.cpp without dragging the enum along.
    void playerMustDisconnect(int slot, int reason);

    /// The input policy of a slot changed. main.cpp forwards it to the slot's
    /// worker, which swaps it under the running stream — no reconnect, and the
    /// keys held under the old policy are released on the way.
    void permissionsChanged(int slot, bool gamepad, bool keyboardMouse);

private:
    struct Activation
    {
        QString id;
        QString tokenHash;
        QString pinHash;
        QStringList cookieHashes;
        QList<BoundDevice> devices; ///< parallel to cookieHashes, same order
        Permissions permissions;
        /// The lifetime the owner picked, purely so the board can put the
        /// slider back where they left it. The deadline below is the truth.
        qint64 ttlSecs = kTtlSecs; ///< 0 = unlimited
        /// When this dies, unix seconds; 0 = never. Stored rather than derived
        /// from activatedAt + ttlSecs, because setTtl() moves it to a value
        /// that is not activatedAt plus any offered lifetime (see setTtl).
        qint64 expiresAtSecs = 0;
        qint64 activatedAt = 0;
        int pinFailures = 0;
        bool streaming = false; ///< runtime only, never persisted

        /// A correct PIN from a device this activation had no room for. Kept so
        /// the board can say "someone else opened this link at 21:37" — the
        /// answer to not knowing an invitation had been passed along.
        qint64 lastRefusedAt = 0;
        QString lastRefusedAgent;

        /// The host the owner was streaming when this share was minted. A player
        /// on this activation is only ever routed here — the share is per-host, so
        /// a link leaked from one host can never reach another (see activate()).
        QString hostUuid;
        int appId = -1;

        /// The clear link token and PIN, so the owner can reopen the popin and
        /// read what they already handed out. Runtime only — share.json holds
        /// digests, so the file on disk is never a usable invitation and a
        /// restart simply forgets the pair while the share stays valid.
        QString tokenClear;
        QString pinClear;

        bool live() const { return !tokenHash.isEmpty(); }
        qint64 expiresAt() const { return expiresAtSecs; }
    };

    struct Slot
    {
        QString name;                ///< owner label, empty means "Player N"
        Permissions lastPermissions; ///< prefills the next opening
        qint64 lastTtlSecs = kTtlSecs;
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

    /// Trim and clamp a name the owner typed. Plain text; the board escapes it.
    static QString sanitizeName(const QString& name);

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
