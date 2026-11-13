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
#include <QList>
#include <QString>

#include <optional>

/**
 * How a TV on plain HTTP is signed in (issue #42): by a code it shows, that an
 * administrator types on a device already signed in over HTTPS.
 *
 * The TV cannot take part in MW-PAIR-v1 (server/PhonePairing.h): a page on
 * http:// has no crypto.subtle, so no key to sign with. What it gets instead:
 *   - the server draws a request id, a 6-character code and a 32-byte claim,
 *     all from the system's random source;
 *   - the TV shows the code and a QR code to the approval page, and asks with
 *     the claim, from its address, where its request stands;
 *   - an administrator (an admin session or the host machine, never another
 *     TV) opens that page, types the code the TV shows, and approves;
 *   - the TV's next ask is answered "approved", once: the caller then makes
 *     the TV's session, bound to its address (AuthManager).
 * The claim is kept hashed and compared in constant time; so is the code.
 *
 * A book of its own, apart from PhonePairing's on purpose: its places, its
 * quota and its answers are separate, so that neither can fill or read the
 * other, and what it hands out is only ever a TV's session. Pure: the clock
 * and the random source are the caller's, so the tests own both
 * (tests/test_lan_http_pairing.cpp).
 */
class LanHttpPairing
{
public:
    /// How long a request waits for its approval, as MW-PAIR-v1's.
    static constexpr qint64 TTL_SECS = 300;
    /// Wrong codes typed before the request is dropped.
    static constexpr int CODE_TRIES = 3;
    /// Characters of the code, of Crockford's base32 (no I, L, O, U).
    static constexpr int CODE_LENGTH = 6;
    /// Requests waiting at once, all addresses together: a handful of TVs.
    static constexpr int MAX_PENDING = 4;
    /// Bytes of the claim the TV asks with.
    static constexpr int CLAIM_BYTES = 32;

    /// What the TV is given when it asks to be let in.
    struct Opened
    {
        QString id;    ///< 96 random bits, base64url: the approval page's path
        QString code;  ///< what the administrator types
        QString claim; ///< what the TV asks with, base64url; never kept as it is
    };

    enum class Open
    {
        Opened,   ///< a new request; any earlier one of this address is gone
        Full,     ///< MAX_PENDING requests of other addresses are waiting
        BadSource ///< no random bytes to be had: nothing is opened
    };

    struct OpenResult
    {
        Open outcome = Open::Full;
        Opened request;
    };

    /// A random source: @p n bytes, or fewer on failure. The default draws
    /// from QRandomGenerator::system().
    using RandomBytes = QByteArray (*)(int n);

    explicit LanHttpPairing(RandomBytes random = nullptr);

    /// The TV at @p address (already normalized) asks to be let in. One
    /// request per address: asking again, a page reloaded, replaces its own.
    OpenResult open(const QString& address, qint64 now);

    enum class Poll
    {
        Pending,  ///< still waiting
        Approved, ///< approved: answered once, the request is spent
        Refused,  ///< refused: answered once, the request is spent
        Gone      ///< unknown, expired, another address, or a wrong claim
    };

    struct PollResult
    {
        Poll state = Poll::Gone;
        QString approverId; ///< with Approved: the approving session's id
    };

    /// Where the request stands, asked by the TV with its claim, from the
    /// address that opened it.
    PollResult poll(const QString& id, const QString& claim, const QString& address, qint64 now);

    /// What the approval page shows: the TV's address and the time left.
    /// Never the code: whoever approves must read it on the TV.
    struct Shown
    {
        QString address;
        qint64 expiresIn = 0;
    };
    std::optional<Shown> find(const QString& id, qint64 now) const;

    enum class Answer
    {
        Approved,
        WrongCode, ///< tries remain
        Gone       ///< unknown, expired, answered, or its last try spent
    };

    /// An administrator approves, typing the code the TV shows. @p approverId
    /// is that session's opaque id, empty for the host machine.
    Answer approve(const QString& id, const QString& typedCode, const QString& approverId,
                   qint64 now);
    /// An administrator refuses: the TV's next ask says so.
    bool refuse(const QString& id, qint64 now);

    /// Requests still waiting.
    int pending(qint64 now) const;

private:
    enum class State
    {
        Pending,
        Approved,
        Refused
    };

    struct Request
    {
        QString id;
        QString address;
        QString code;
        QByteArray claimHash;
        qint64 createdAt = 0;
        int tries = 0;
        State state = State::Pending;
        QString approverId;
    };

    static bool expired(const Request& r, qint64 now) { return now - r.createdAt > TTL_SECS; }
    void dropExpired(qint64 now);
    int indexOf(const QString& id) const;

    RandomBytes m_Random;
    QList<Request> m_Requests;
};
