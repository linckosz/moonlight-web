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

#include "common/Types.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

/**
 * The page a link of this machine opens first: /owner-handoff, on this
 * machine's own loopback (http://127.0.0.1:<port>/owner-handoff#n=<nonce>).
 *
 * The tray icon, a second launch, the first launch and --open-link start a
 * browser on this page, with a nonce: 128 random bits, good once and for a
 * minute, kept in memory only, made for one page (the target, chosen when it
 * is made) and for one account, the one that browser runs as.
 *
 * The page trades it at POST /api/auth/owner-handoff, on the loopback
 * connection it was served on, for the owner's link: the target with a key
 * made at that moment (HostKeyVault), which it then opens in its own place.
 * The trade is made only when that connection is held by the account the
 * nonce was made for, as the system's own tables of connections say
 * (SocketTables::connectionOwner). Anything else is refused, and the nonce
 * the refused request carried is spent by it.
 *
 * No more than eight wait; a ninth takes the place of the oldest. Only a hash
 * of each nonce is kept. Free of the server around it, so the rules are tested
 * as a unit (tests/test_owner_handoff.cpp). It has no lock: everything that
 * reaches it runs on the server's main thread, as for HostKeyVault.
 */
class OwnerHandoff
{
public:
    /// The most nonces that wait to be traded.
    static constexpr int kMaxPending = 8;
    /// How long one waits.
    static constexpr qint64 kLifetimeMs = 60 * 1000;
    /// 128 bits: 22 characters of URL-safe base64.
    static constexpr int kNonceBytes = 16;

    /// The page, and the route its script trades the nonce at.
    static constexpr char kPagePath[] = "/owner-handoff";
    static constexpr char kPageFile[] = "/owner-handoff.html";
    static constexpr char kTradePath[] = "/api/auth/owner-handoff";
    /// The page's own policy: its files and nothing else, in no frame, sending
    /// no referrer.
    static constexpr char kPagePolicy[] =
        "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'";

    /// Where a link leads once its key is made: `base`, "#k=" and the key,
    /// then `suffix` (the page to land on, "&p=/admin", on the internet link).
    struct Target
    {
        QString base;
        QString suffix;
    };

    /// The link to @p target with @p key: what the tray opened before there
    /// was this page, and what --print-link still prints.
    static QString linkWith(const Target& target, const QString& key);

    /// The page's address for @p nonce: this machine's plain HTTP port on
    /// loopback, or its HTTPS port when no plain one listens. Empty with
    /// neither, or no nonce.
    static QString pageUrl(quint16 httpPort, quint16 httpsPort, const QString& nonce);

    /// Whether a page may be opened for @p account: a known account that is a
    /// person's. Never root, never one of the system's own (LocalSystem,
    /// LocalService, NetworkService).
    static bool mayOpenFor(const MachineAccount& account);

    /// The account of a program that asked over the local channel, as the
    /// channel's server end read it: the SID of its token on Windows, its
    /// socket's uid elsewhere. Nobody when the channel did not read it, and
    /// nobody is given no nonce (mayOpenFor).
    static MachineAccount accountOf(const LocalCaller& caller);

    /// Whether two accounts are one: EqualSid on Windows, the same uid
    /// elsewhere. Nobody is never anybody.
    static bool sameAccount(const MachineAccount& a, const MachineAccount& b);

    /// The account this process runs as: what a browser it starts runs as.
    static MachineAccount thisProcess();

    /// The account that holds the browser's end of the connection @p req came
    /// on, read from the system's tables. Nothing for a request that came on
    /// no socket, or when the tables do not say (SocketTables::connectionOwner).
    static std::optional<MachineAccount> connectionAccount(const HttpRequest& req);

    /// @p nowMs is a clock that only goes forward; the default is the
    /// process's own. @p random gives the bytes of a nonce; the default is the
    /// system's generator. Both are for the tests.
    explicit OwnerHandoff(std::function<qint64()> nowMs = {},
                          std::function<QByteArray(int bytes)> random = {});

    /// A nonce for @p target, for @p account alone. Empty, and nothing kept,
    /// when mayOpenFor() refuses that account.
    QString issue(const Target& target, const MachineAccount& account);

    /// Why a trade was refused: what the log says of it, and all it says.
    enum class Refusal
    {
        None,
        Body,        ///< no nonce in the body
        Nonce,       ///< not one made here, too old, or already spent
        Method,      ///< not a POST
        ContentType, ///< not JSON
        NotDirect,   ///< not asked on one of this server's own sockets
        NotLoopback, ///< the peer is not 127.0.0.1 or ::1
        Proxied,     ///< a proxy says it forwarded it
        Host,        ///< the Host is not this loopback and this socket's port
        Origin,      ///< another page asked, or a browser said so
        Owner,       ///< the connection is not held by the nonce's account
    };
    static QString categoryOf(Refusal refusal);

    struct Trade
    {
        Refusal refusal = Refusal::Body;
        Target target; ///< with Refusal::None
    };

    /// Trades the nonce @p req carries ({"n": "<nonce>"}), in one step. It is
    /// spent as soon as it is recognised, whatever happens next. The trade then
    /// needs a POST of JSON, asked on one of this server's sockets (@p direct)
    /// by a loopback peer, 127.0.0.1 or ::1, with no proxy on the way; a Host
    /// that names that loopback and that socket's port (the port left out only
    /// when it is its scheme's own, as a browser leaves it out); an Origin that
    /// is absent or this very page's; and a Sec-Fetch-Site, when given, of
    /// same-origin. Last, and only then, @p browserAccount is asked who holds
    /// the connection: the nonce's own account, or nothing is given.
    Trade redeem(const HttpRequest& req, bool direct,
                 const std::function<std::optional<MachineAccount>()>& browserAccount);

    /// The answer to POST /api/auth/owner-handoff: the owner's link,
    /// {"url": …}, with a key from @p mintKey, made only for a trade that holds;
    /// 403 {"error": "reopen"} otherwise. A refused nonce that was made here,
    /// and is spent by the refusal, is one warning in the log that names the
    /// category and nothing else: no nonce, no key, no account. A request
    /// with no nonce made here spends nothing and is only counted: one line a
    /// minute at most says how many, and nothing of who. Never kept in a cache.
    HttpResponse answer(const HttpRequest& req, bool direct,
                        const std::function<std::optional<MachineAccount>()>& browserAccount,
                        const std::function<QString()>& mintKey);

    /// How many wait now.
    int pending();

private:
    struct Entry
    {
        QByteArray hash;
        qint64 madeAt = 0;
        Target target;
        MachineAccount account;
    };

    void dropExpired();

    /// How often the requests with no nonce made here are said (answer()).
    static constexpr qint64 kUnknownLineMs = 60 * 1000;

    std::function<qint64()> m_NowMs;
    std::function<QByteArray(int)> m_Random;
    QList<Entry> m_Entries; // oldest first
    qint64 m_UnknownSaidAt = -1;
    int m_UnknownSince = 0;
};
