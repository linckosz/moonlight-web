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

/**
 * MW-PAIR-v1 — a device paired without its PIN, approved by the administrator.
 *
 * A TV is a poor place to type: the Android TV app shows a QR code and a short
 * code instead. The administrator scans the QR code, and types the short code
 * the device shows into the page it opens. The device then gets the session a
 * PIN would have opened, bound to its own MW-BIND-v1 key.
 *
 * What each side may do (routes/PairRequestRoutes.cpp):
 *
 *   - A device asks from this LAN: on a socket by its own address, under a name
 *     of this machine's; on a line by the address ICE settled on. From
 *     anywhere else only while the administrator allows it
 *     (AppSettings::pairRequestInternet, off by default, never under
 *     --noadmin). Each request keeps where it came from; those from elsewhere
 *     take MAX_FROM_ELSEWHERE places at most, and are dropped once the setting
 *     is off again.
 *   - Only the administrator answers, with the right that mints a PIN
 *     (HttpRequest::isLocal). A session that answers signs the code with its
 *     own pairing key, and the session it lets in is recorded as its own:
 *     revoking the one revokes the other (AuthManager::destroySession).
 *   - The administrator types the code the device shows, and this host
 *     compares it with the request's: CODE_TRIES wrong codes refuse it.
 *   - The request is bound to the device's key. Every claim is signed by it and
 *     comes the way the request did: on a line, with the line's own key.
 *   - Single use, five minutes, a few per address, and failures counted on the
 *     PIN's own counter (AuthManager::countPairingFailure).
 *
 * The short code is computed on both sides from both keys, the request and the
 * device's nonce, which the device commits to before it learns this host's key
 * and the request's id, and reveals only after. A party in the middle of a
 * first contact (on the LAN, or a hostile introduction server) that swaps a key
 * makes the device show another code than the one this host expects, and the
 * code typed from the device's screen does not match: one chance in 2^20 per
 * attempt, and it cannot try keys until the codes agree.
 *
 * The code vouches for the two keys, not for the connection: the claim proves
 * the device's key, and the session comes back on the connection the device
 * made, as private as that connection is, as with a PIN.
 *
 * Every message uses MW-BIND-v1's framing (PairingCrypto::buildSignedMessage)
 * under labels of its own, so no signature here is good for anything else. The
 * TV app computes the same bytes (top.moonlightweb.tv.net.PhonePair), and the
 * page the approval's (pairingCrypto.js); tests/test_phone_pairing.cpp holds
 * the vectors their tests share.
 *
 * The book itself is pure: in memory, the time passed in. A restart forgets
 * every request, and the device shows a new code.
 */
class PhonePairing
{
public:
    static constexpr char PROTOCOL[] = "MW-PAIR-v1";

    /// How long a request waits for the administrator, then for the device's claim.
    static constexpr qint64 TTL_SECS = 300;
    /// An expired request still answers "expired" this long, so a device asking
    /// late shows a new code rather than reporting an unknown one. A paired one
    /// is kept as long after its session was made, for a device whose answer
    /// was lost on the way.
    static constexpr qint64 GRACE_SECS = 60;
    /// Live requests one address may hold at once; a new one pushes the oldest out.
    static constexpr int MAX_PER_ADDRESS = 3;
    /// Live requests the host holds at once; beyond that a new one is refused.
    static constexpr int MAX_PENDING = 32;
    /// Of those, the most that may come from outside this LAN: the others are
    /// kept for its devices. Beyond it, one more from elsewhere is refused as
    /// when the book is full.
    static constexpr int MAX_FROM_ELSEWHERE = 24;
    /// Wrong codes the administrator may type before the request is refused.
    static constexpr int CODE_TRIES = 3;
    static constexpr int CODE_LENGTH = 4;
    static constexpr int NONCE_BYTES = 32;
    static constexpr int COMMITMENT_BYTES = 32;
    /// The longest device name kept, the session's name once paired.
    static constexpr int MAX_NAME = 64;

    enum class State
    {
        Waiting,
        Approved,
        Refused,
        Paired
    };

    struct Request
    {
        QString id;             ///< 16 characters of base64url, in the QR code
        QByteArray deviceKey;   ///< the device's MW-BIND-v1 key (SPKI DER)
        QString machineName;    ///< what the session will be listed as
        QByteArray commitment;  ///< SHA-256 of the device's framed nonce
        QByteArray nonce;       ///< revealed by the device's first claim
        QString code;           ///< known once the nonce is
        QString address;        ///< who asked: its failures count against it
        QString bucket;         ///< its address's group (AuthManager::rateLimitKey)
        bool viaTunnel = false; ///< asked on a line: claimed on a line
        bool fromLan = true;    ///< asked from this LAN, not from elsewhere
        qint64 createdAt = 0;
        quint64 sequence = 0; ///< its rank of birth in this book, for those born in one second
        State state = State::Waiting;
        int wrongCodes = 0;  ///< codes typed for it that were not its own
        QString approver;    ///< the session that approved it, empty for this machine itself
        QString sessionId;   ///< the session made for it, once paired
        qint64 pairedAt = 0; ///< when that session was made
    };

    // ── The messages ────────────────────────────────────────────────────────

    /// What the device sends first: SHA-256 of its framed nonce.
    static QByteArray commitment(const QByteArray& nonce);

    /// The short code the device shows: the first 20 bits of a hash over the
    /// host key, the device key, the request and the device's nonce, in
    /// Crockford's base32 (no I, L, O or U), upper case.
    static QString code(const QByteArray& hostKey, const QByteArray& deviceKey, const QString& id,
                        const QByteArray& nonce);

    /// What the device signs on each claim.
    static QByteArray claimMessage(const QString& hostId, const QString& id,
                                   const QByteArray& nonce);

    /// What the approving page signs with its session's key: the code it sends.
    static QByteArray approveMessage(const QString& hostId, const QString& id, const QString& code);
    /// What a session signs to turn a device away: approveMessage's framing, without a code.
    static QByteArray refuseMessage(const QString& hostId, const QString& id);

    /// A code as a person types it: spaces and dashes dropped, upper case, and
    /// the letters Crockford's base32 reads as digits (O as 0, I and L as 1).
    static QString normalizeCode(const QString& typed);

    /// The shape of a request id this host draws.
    static bool isRequestId(const QString& id);

    /// A device's name as a session keeps it: printable, single-spaced, short.
    static QString cleanName(const QString& name);

    // ── The device's side ───────────────────────────────────────────────────

    enum class Open
    {
        Opened,
        Crowded, ///< opened, after pushing out the oldest request of its address
        Full,    ///< MAX_PENDING live requests already, or MAX_FROM_ELSEWHERE from
                 ///< elsewhere for a request from elsewhere: nothing opened
    };

    struct OpenResult
    {
        Open outcome = Open::Full;
        Request request;
    };

    /// A new request for @p deviceKey, asked by @p address (in @p bucket), from
    /// this LAN or, @p fromLan false, from elsewhere. An older request of the
    /// same key from the same bucket goes (a new code on the same screen); a
    /// key named from elsewhere leaves it alone. Beyond MAX_PER_ADDRESS live
    /// requests in the bucket, its oldest goes too, and the answer says so.
    /// Paired requests and expired ones still answering count toward no limit.
    OpenResult open(const QByteArray& deviceKey, const QString& machineName,
                    const QByteArray& commitment, const QString& address, const QString& bucket,
                    bool viaTunnel, bool fromLan, qint64 now);

    /// Whether any request held came from elsewhere.
    bool hasFromElsewhere() const;

    /// Drop every request that came from elsewhere, in any state, once the
    /// administrator no longer allows them: their places are free again, and
    /// none of them can be approved or claimed. Those of this LAN stay.
    /// Returns how many went.
    int dropFromElsewhere();

    enum class Claim
    {
        Waiting,  ///< the administrator has not answered yet
        Paired,   ///< approved: the session is the caller's to make, then markPaired()
        Refused,  ///< the administrator said no: the request is spent
        Expired,  ///< too late: a new request
        Unknown,  ///< no such request, or not any more
        BadProof, ///< not signed by the device's key, or not the way it asked
    };

    struct ClaimResult
    {
        Claim outcome = Claim::Unknown;
        /// The request; for Paired, its sessionId is the session made at an
        /// earlier claim whose answer was lost, empty the first time.
        Request request;
    };

    /// The device asks where its request stands, proving its key: a signature
    /// of claimMessage(@p hostId, @p id, @p nonce). The first claim reveals the
    /// nonce, which must match the commitment; the code is then known. A request
    /// made on a line is claimed on a line, whose key (@p tunnelKey) must be the
    /// request's; one made on a socket, on a socket.
    ///
    /// A bad signature leaves the request in place: anyone who saw the QR code
    /// knows its id, and must not be able to cancel it. A nonce the device's
    /// own key signed that is not the one it committed to spends it.
    ClaimResult claim(const QString& id, const QByteArray& nonce, const QByteArray& signature,
                      const QString& hostId, const QByteArray& hostKey, bool viaTunnel,
                      const QByteArray& tunnelKey, qint64 now);

    /// The session made for a Paired claim. The request is kept GRACE_SECS
    /// more: the device that claims it again, its answer lost, gets a new
    /// session in place of this one.
    void markPaired(const QString& id, const QString& sessionId, qint64 now);

    /// Spend a request at once: its approval no longer stands.
    void drop(const QString& id);

    // ── The page's side ─────────────────────────────────────────────────────

    enum class Lookup
    {
        Found,
        Expired,
        Unknown
    };

    /// The request behind a QR code, as the page shows it.
    Lookup find(const QString& id, qint64 now, Request* out);

    enum class Answer
    {
        Done,
        Unknown,
        Expired,
        NotReady,     ///< the device has not revealed its nonce: no code yet
        CodeMismatch, ///< not the request's code; tries are left
        TooManyTries, ///< not its code, CODE_TRIES times: the request is refused
        Decided,      ///< already approved, refused or paired
    };

    struct AnswerResult
    {
        Answer outcome = Answer::Unknown;
        int triesLeft = 0; ///< for CodeMismatch
        QString address;   ///< for a refusal: the address it counts against
    };

    /// The administrator says yes, with @p typedCode read from the device's
    /// screen, as the session @p approver (empty for this machine itself).
    AnswerResult approve(const QString& id, const QString& typedCode, const QString& approver,
                         qint64 now);

    /// The administrator says no. The address the request came from comes
    /// back: a refusal counts against it like a wrong PIN.
    AnswerResult refuse(const QString& id, qint64 now);

    /// Requests held, in any state.
    int pending() const { return static_cast<int>(m_requests.size()); }

private:
    void purge(qint64 now);
    static bool expired(const Request& r, qint64 now)
    {
        return r.state != State::Paired && now - r.createdAt > TTL_SECS;
    }
    /// Still waiting on someone: what the limits count.
    static bool live(const Request& r, qint64 now)
    {
        return r.state != State::Paired && !expired(r, now);
    }
    /// Whether @p a was born before @p b: by time, then, within one second, in
    /// the order they came.
    static bool olderThan(const Request& a, const Request& b)
    {
        return a.createdAt != b.createdAt ? a.createdAt < b.createdAt : a.sequence < b.sequence;
    }

    QHash<QString, Request> m_requests; // id -> request
    quint64 m_births = 0;               // requests born so far; each takes the next number
};
