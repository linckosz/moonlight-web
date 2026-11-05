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
#include <QHash>
#include <QString>

#include <optional>

/**
 * MW-PIN-v1 — the PIN proved, never sent, and bound to the channel it was typed on.
 *
 * Binding a PIN to the TLS key the client sees takes more than a field the client
 * fills in (whoever ends the TLS can rewrite it) or a hash keyed by the PIN (six
 * digits fall to an offline search in well under a second). It takes a
 * password-authenticated key exchange: SPAKE2, RFC 9382, ciphersuite
 * P256-SHA256-HKDF-HMAC. Each end mixes into the confirmations the channel it is
 * on — the TLS key the client sees, the TLS key this connection was served — so
 * a relay ends with two confirmations that do not match. A party in the middle
 * gets one guess per run, counted like a wrong PIN.
 *
 * What this protects is the pairing with a host the client has already seen
 * prove PINs: the client sends that host no PIN any other way. A first contact
 * can still be downgraded to the plain PIN route (POST /api/auth/validate): an
 * older host answers these routes 404, and so can whatever answers in its place.
 *
 * The client is A (it goes first, uses M), this host is B (uses N). The
 * identities are the client's MW-BIND key and this host's key, both SPKI DER.
 * docs/design/tls-key-proof.md has the protocol and its reasons.
 */
namespace PinBinding {

/// The protocol label. Anything that does not carry it is not this protocol.
inline constexpr char PROTOCOL[] = "MW-PIN-v1";

/// w for @p pin: SHA-512 of the framed PIN, reduced modulo the group order,
/// 32 bytes big-endian (RFC 9382 §3.2 asks for 64 bits over the order's size).
/// No memory-hard function: it would only protect a w read out of this
/// process's memory, where the PIN itself is.
QByteArray pinScalar(const QString& pin);

/// The channel of a direct TLS connection: ("tls", SHA-256 of the
/// SubjectPublicKeyInfo of the certificate it was served). Empty for no key.
QByteArray tlsChannel(const QByteArray& tlsSpki);

/// The channel of a rendezvous tunnel: ("line", SHA-256 of the host key whose
/// signature opened the line, this host's own).
QByteArray lineChannel(const QByteArray& hostSpki);

/// RFC 9382 §4's associated data for MW-PIN-v1: the protocol, then the channel.
QByteArray associatedData(const QByteArray& channel);

/// One party's run: the scalar it keeps, and the element it sends
/// (uncompressed SEC1, 65 bytes).
struct Share
{
    QByteArray scalar; ///< 32 bytes big-endian; never leaves the party
    QByteArray element;
};

/// What a finished run gives both parties alike (RFC 9382 §4).
struct Keys
{
    QByteArray transcript; ///< TT
    QByteArray ke;
    QByteArray ka;
    QByteArray kcA;
    QByteArray kcB;
    QByteArray confirmA; ///< MAC(KcA, TT): what A sends
    QByteArray confirmB; ///< MAC(KcB, TT): what B sends
};

/// A's share, pA = x·P + w·M: @p x random unless given (the RFC's vectors).
std::optional<Share> shareA(const QByteArray& w, const QByteArray& x = {});

/// B's share, pB = y·P + w·N: @p y random unless given.
std::optional<Share> shareB(const QByteArray& w, const QByteArray& y = {});

/// A's keys once pB is in. Nullopt for an element that is no point of the
/// group, or a K at the identity: RFC 9382 §7, the run is over.
std::optional<Keys> finishA(const QByteArray& w, const Share& a, const QByteArray& pB,
                            const QByteArray& idA, const QByteArray& idB, const QByteArray& aad);

/// B's keys once pA is in.
std::optional<Keys> finishB(const QByteArray& w, const Share& b, const QByteArray& pA,
                            const QByteArray& idA, const QByteArray& idB, const QByteArray& aad);

/// Whether @p element is one a peer may send: uncompressed, a point of the group,
/// not the identity. It does not depend on the PIN, so a caller may be told.
/// finishA() and finishB() also fail for a K at the identity, which happens for
/// pA = w·M with the right w: that failure must never show (POST
/// /api/auth/pin/start answers such a run like any other, and /finish refuses it).
bool isElement(const QByteArray& element);

/// Equal bytes, in a time that does not depend on their content. Different
/// lengths are never equal.
bool sameBytes(const QByteArray& a, const QByteArray& b);

/// The runs between POST /api/auth/pin/start and /finish: in memory, single
/// use, short-lived, capped per caller (its own oldest gives way) and in all (a
/// full table takes no new caller's run, and drops none it holds). A restart
/// forgets them, and the client starts again.
class Exchanges
{
public:
    struct Pending
    {
        QString bucket;       ///< the caller's rate-limit bucket (AuthManager::rateLimitKey)
        QString pin;          ///< the PIN the run was made with: it must still be the current one
        QByteArray clientKey; ///< the client's MW-BIND key, bound to the session it ends in
        QByteArray confirmA;  ///< what the client must send
        QByteArray confirmB;  ///< what this host answers once it has
        bool provable = true; ///< false for a run whose K fell on the identity: it never proves
        qint64 expiresAt = 0; ///< seconds since the epoch, set by add()
    };

    /// Whether a run of @p bucket would be kept now: the table has room, or the
    /// caller holds enough runs of its own that its oldest gives way. The route
    /// asks before it counts the attempt.
    bool hasRoom(const QString& bucket, qint64 nowSecs);

    /// Keeps @p run under a fresh random id, which it returns. Empty when there
    /// is no room for it (hasRoom), or the system has no randomness to give.
    QString add(Pending run, qint64 nowSecs);

    /// The run under @p id, removed, when @p bucket began it: a second /finish
    /// finds nothing, and a caller from elsewhere cannot spend another's run.
    /// Nullopt when unknown, expired or another caller's.
    std::optional<Pending> take(const QString& id, const QString& bucket, qint64 nowSecs);

    int size() const { return static_cast<int>(m_Runs.size()); }

    /// Room for many callers at once. Full, it takes no new caller's run until
    /// one ends or expires: no caller's run ever gives way to another caller's.
    static constexpr int MAX_RUNS = 256;
    static constexpr int MAX_RUNS_PER_BUCKET = 4;
    static constexpr qint64 RUN_TTL_SECS = 60;

private:
    struct Kept
    {
        Pending run;
        quint64 order = 0;
    };

    void dropExpired(qint64 nowSecs);
    int runsOf(const QString& bucket) const;
    /// The oldest run of @p bucket, and of no other.
    void dropOldestOf(const QString& bucket);

    QHash<QString, Kept> m_Runs;
    quint64 m_Next = 0;
};

/// A few calls per caller and per window: what GET /api/auth/tls-proof answers
/// before 429, so that nobody makes this host sign in a loop. A client asks it
/// once per key that changed.
class CallBudget
{
public:
    explicit CallBudget(int perWindow, qint64 windowSecs = 60)
        : m_PerWindow(perWindow)
        , m_WindowSecs(windowSecs)
    {}

    /// Whether @p bucket may make one more call now; counts it when it may.
    bool spend(const QString& bucket, qint64 nowSecs);

    int size() const { return static_cast<int>(m_Windows.size()); }

    /// The callers one window holds. Past it, a flood: nobody new until the
    /// window has passed, and the table never grows with the flood.
    static constexpr int MAX_CALLERS = 4096;

private:
    struct Window
    {
        qint64 start = 0;
        int used = 0;
    };

    QHash<QString, Window> m_Windows;
    int m_PerWindow;
    qint64 m_WindowSecs;
    qint64 m_SweptAt = 0;
};

} // namespace PinBinding
