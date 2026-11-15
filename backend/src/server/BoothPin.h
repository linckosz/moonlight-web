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
#include <QSet>
#include <QString>

#include <functional>

/**
 * A booth's stream PINs (booth_stream_pin, issue #68).
 *
 * Each stream the booth serves gets a PIN of its own while it lasts. Whoever
 * watches that stream may read it; a device the booth already knows enters it
 * to join the stream, or to start another one in its place. The caller names
 * a stream by an opaque key of its choosing (the host, and the app where a host
 * runs several side by side).
 *
 * A PIN is never destroyed for being guessed at: it changes. It changes after
 * each use the caller reports (a join, a take-over, a guest removed or gone),
 * on the owner's request, and after kFailuresBeforeRotation wrong tries on the
 * current PIN, whoever made them. Guessing therefore never ends anybody's
 * stream; it only makes the PIN move.
 *
 * Each try is also held to two buckets, both of which must be open: one for the
 * session asking, one for the address it asks from. The ladder is the one the
 * PIN login and the share PIN use (3 → 30 s, 5 → 2 min, 10 → 10 min), on
 * buckets of its own. The buckets hold for every stream at once, so a right
 * PIN empties neither: knowing one stream's PIN must never buy more tries at
 * another's. They only age out, kBucketIdleSecs after their last failure.
 *
 * Held in memory only, in clear — the owner reads it back, and a digest of a
 * million values hides nothing — and compared in constant time. Nothing here
 * writes a log: callers name the machine and the action, never the PIN.
 *
 * Pure Qt Core, no clock or randomness it cannot be handed, so the whole
 * behaviour is covered by tests/test_booth_pin.cpp.
 */
class BoothPin
{
public:
    static constexpr int kDigits = 6;
    static constexpr int kFailuresBeforeRotation = 5;

    /// Why a PIN changed, the last time it did. The owner's page says so when
    /// it was the wrong tries.
    enum class Reason
    {
        Minted,   ///< the stream started
        Used,     ///< a join or a take-over went through with it
        Departed, ///< a guest who joined with it was removed, or left
        Asked,    ///< the owner asked for a new one
        Failures, ///< kFailuresBeforeRotation wrong tries on it
    };

    enum class Result
    {
        Ok,
        Wrong,       ///< not this stream's PIN
        RateLimited, ///< the session's or the address's bucket is shut; not tried
        NoStream,    ///< no PIN for this key: nothing to join or take over
    };

    struct Outcome
    {
        Result result = Result::Wrong; // fail closed
        /// Tries left before the next lockout of the stricter bucket.
        int remainingAttempts = 0;
        /// Seconds until the stricter bucket opens again, 0 when open.
        int lockoutSeconds = 0;
        /// This try was the one that made the PIN change.
        bool rotated = false;
    };

    BoothPin();

    /// A fresh PIN for @p key, replacing any it had. Returns it.
    QString mint(const QString& key);
    /// The stream is gone: its PIN with it.
    void forget(const QString& key);
    bool has(const QString& key) const;
    /// The current PIN, empty when there is none.
    QString read(const QString& key) const;
    /// A new PIN for a stream that has one, different from the last. Empty
    /// when @p key has none.
    QString rotate(const QString& key, Reason why);
    /// Forget every PIN whose key is not in @p live: the streams that ended.
    void retainOnly(const QSet<QString>& live);
    /// Seconds until both of the caller's buckets are open, 0 when they are:
    /// asked before anything else is checked, so a caller held back is told so
    /// without its proof or its PIN being looked at.
    int heldFor(const QString& sessionKey, const QString& addressKey);
    /// How many times this stream's PIN has changed since it was minted, and
    /// why the last time. A page that saw generation n knows a change by n+1.
    quint64 generation(const QString& key) const;
    Reason lastReason(const QString& key) const;

    /// Try @p pin for @p key, on behalf of @p sessionKey from @p addressKey
    /// (AuthManager::rateLimitKey: an IPv6 address counts by its /64). Spaces
    /// in @p pin are ignored ("482 913"). A Wrong try counts in both buckets
    /// and towards the PIN's rotation; a RateLimited one counts nowhere, since
    /// it was never tried. Ok clears no bucket (see the class comment). The
    /// caller reports a use with rotate(key, Reason::Used) once the join or
    /// take-over is done.
    Outcome check(const QString& key, const QString& pin, const QString& sessionKey,
                  const QString& addressKey);

    /// For tests: the clock (unix seconds) and the PIN source.
    void setClockForTesting(std::function<qint64()> nowSecs);
    void setGeneratorForTesting(std::function<QString()> next);

private:
    struct Entry
    {
        QString pin;
        int failures = 0;
        quint64 generation = 0;
        Reason reason = Reason::Minted;
    };

    struct Bucket
    {
        int failures = 0;
        qint64 lastFailure = 0;
        qint64 lockoutUntil = 0;
    };

    QString fresh(const QString& unlike);
    qint64 now() const;
    int lockout(const QString& bucket) const;
    int remaining(const QString& bucket) const;
    void fail(const QString& bucket);
    void purge();
    static bool samePin(const QString& a, const QString& b);

    QHash<QString, Entry> m_Pins;
    QHash<QString, Bucket> m_Buckets;
    std::function<qint64()> m_Clock;
    std::function<QString()> m_Generator;

    static constexpr int kTier1 = 3;
    static constexpr int kTier2 = 5;
    static constexpr int kTier3 = 10;
    static constexpr int kLockoutShortSecs = 30;
    static constexpr int kLockoutMediumSecs = 120;
    static constexpr int kLockoutLongSecs = 600;
    /// A bucket nobody has failed in for this long is forgotten.
    static constexpr int kBucketIdleSecs = 3600;
};
