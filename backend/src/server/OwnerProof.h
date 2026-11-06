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

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

/**
 * The sharing board's word that it is the browser its session was paired with.
 *
 * A stream is only ever opened by the browser holding the session's pairing key
 * (MW-BIND-v1). The rows of the board that hand out a guest's way in — a link and
 * its PIN, the rights it carries, how long it lives — are held to the same: each
 * such request carries a signature, by that key, over a single-use challenge the
 * host drew for this session a moment before.
 *
 * Through the rendezvous tunnel the line itself already proved the key on every
 * request (HttpServer::tunnelKeyRefused), so the proof is asked of the other
 * paths only, and of sessions that have a key at all: one paired before MW-BIND-v1
 * existed has nothing to sign with, and streams unchecked the same way.
 *
 * Main thread only, like the routes that use it.
 */
class OwnerProof
{
public:
    /// How long a challenge waits for its signature.
    static constexpr qint64 kLifetimeMs = 120 * 1000;
    /// Challenges one session may hold at once; a new one retires its oldest.
    static constexpr int kPerSession = 4;
    /// Challenges held in all. Past it, no session's challenge is retired for
    /// another's: the new one is refused instead.
    static constexpr int kTotal = 256;

    /// Its own label, so a signature made for a board request can never stand
    /// for one of MW-BIND-v1's, nor the other way round.
    static constexpr char kLabel[] = "MW-BIND-v1|share-owner";

    enum class Result
    {
        Ok,
        Missing, ///< no proof in the request
        Refused, ///< a proof, and it does not hold
    };

    /// A fresh challenge for @p session, or empty when none can be held (no
    /// session, or kTotal reached).
    QByteArray issue(const QString& session, qint64 nowMs);

    /// The bytes the browser signs: frontend/js/util/pairingCrypto.js builds
    /// the same, field for field.
    static QByteArray message(const QString& hostId, const QByteArray& nonce, const QString& action,
                              int slot);

    /// Check @p proof ({nonce, sig}, both base64) for @p action on @p slot
    /// against @p spki, the session's pairing key. The challenge is spent
    /// whatever the outcome: it never verifies twice.
    Result check(const QString& session, const QByteArray& spki, const QString& hostId,
                 const QString& action, int slot, const QJsonObject& proof, qint64 nowMs);

    int pendingCount() const { return m_Pending.size(); }

private:
    struct Pending
    {
        QString session;
        QByteArray nonce;
        qint64 expiresMs = 0;
    };

    void prune(qint64 nowMs);

    QVector<Pending> m_Pending; ///< oldest first
};
