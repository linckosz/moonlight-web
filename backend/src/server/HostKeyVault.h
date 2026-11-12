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

#include <functional>

/**
 * The keys this machine's own entry points open a page with.
 *
 * The tray icon, a launch and the installer's last page each open the browser
 * on a link that ends in `#k=<key>`; the page spends the key at
 * POST /api/auth/host-key for a session that says "this browser runs on the
 * host machine" (server/routes/HostKeyRoute.h).
 *
 * A key is made when a link is asked for and lives here, in memory, until it is
 * spent or ten minutes have passed: it is never written to a file, and a
 * restart forgets them all. It opens one session. No more than eight wait at
 * once; a ninth takes the place of the oldest, so asking for links over and
 * over leaves nothing behind.
 *
 * Only a hash of each key is kept. Free of the server around it, so the rules
 * are tested as a unit (tests/test_host_key_vault.cpp).
 *
 * It has no lock: everything that reaches it runs on the server's main thread
 * (the routes, the server's own tray icon, the local channel, which posts its
 * requests there). It must stay so.
 */
class HostKeyVault
{
public:
    /// The most keys that wait to be spent.
    static constexpr int kMaxPending = 8;
    /// How long one waits.
    static constexpr qint64 kLifetimeMs = 10 * 60 * 1000;

    /// @p nowMs is a clock that only goes forward; the default is the
    /// process's own. @p random gives the bytes of a key; the default is the
    /// system's generator. Both are for the tests.
    explicit HostKeyVault(std::function<qint64()> nowMs = {},
                          std::function<QByteArray(int bytes)> random = {});

    /// A new key: 32 random bytes, in the URL-safe base64 a link carries.
    QString mint();

    /// Spends @p key, in one step: true when it was made here, is not too old
    /// and had not been spent, and from then on it opens nothing. There is no
    /// asking without spending: a key that was recognised is used up, whatever
    /// becomes of the request that brought it.
    bool spend(const QString& key);

    /// How many wait now.
    int pending();

private:
    struct Entry
    {
        QByteArray hash;
        qint64 mintedAt = 0;
    };

    void dropExpired();
    int indexOf(const QString& key);

    std::function<qint64()> m_NowMs;
    std::function<QByteArray(int)> m_Random;
    QList<Entry> m_Entries; // oldest first
};
