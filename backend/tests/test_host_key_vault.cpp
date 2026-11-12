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

// The keys this machine's own entry points open a page with
// (server/HostKeyVault.h): made on demand, good once, for ten minutes, eight at
// most.

#include "server/HostKeyVault.h"
#include "test_framework.h"

#include <QSet>
#include <QStringList>

void run_host_key_vault_tests()
{
    SECTION("Host key vault — a key is 32 random bytes, in the text a link carries");
    {
        HostKeyVault vault;
        const QString key = vault.mint();
        // 32 bytes in URL-safe base64, without padding: 43 characters.
        CHECK_EQ(static_cast<int>(key.size()), 43);
        const QByteArray raw =
            QByteArray::fromBase64(key.toLatin1(), QByteArray::Base64UrlEncoding);
        CHECK_EQ(static_cast<int>(raw.size()), 32);
        for (const QChar c : key)
            CHECK(c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'));

        // No two alike.
        QSet<QString> seen{key};
        for (int i = 0; i < 50; ++i) {
            const QString next = vault.mint();
            CHECK(!seen.contains(next));
            seen.insert(next);
        }
    }

    SECTION("Host key vault — a key opens one session");
    {
        HostKeyVault vault;
        const QString key = vault.mint();
        CHECK_EQ(vault.pending(), 1);
        CHECK(vault.spend(key));
        CHECK_EQ(vault.pending(), 0);
        CHECK(!vault.spend(key)); // once
        CHECK(!vault.spend(key));
    }

    SECTION("Host key vault — what was not made here opens nothing, and costs a real key nothing");
    {
        HostKeyVault vault;
        const QString key = vault.mint();
        CHECK(!vault.spend(QString()));
        CHECK(!vault.spend(QStringLiteral("x")));
        CHECK(!vault.spend(key + QLatin1Char('A')));
        CHECK(!vault.spend(key.left(key.size() - 1)));
        CHECK(!vault.spend(key.toUpper() == key ? key.toLower() : key.toUpper()));
        // A key of another vault, as one of before a restart would be.
        HostKeyVault other;
        CHECK(!vault.spend(other.mint()));
        // None of that cost the real one its place.
        CHECK_EQ(vault.pending(), 1);
        CHECK(vault.spend(key));

        // A vault that never made a key holds none, the empty one least of all.
        HostKeyVault empty;
        CHECK(!empty.spend(QString()));
        CHECK(!empty.spend(key));
        CHECK_EQ(empty.pending(), 0);
    }

    SECTION("Host key vault — a key waits ten minutes and no longer");
    {
        qint64 now = 1000;
        HostKeyVault vault([&now]() { return now; });
        const QString first = vault.mint();
        const QString second = vault.mint();
        now += HostKeyVault::kLifetimeMs - 1;
        CHECK_EQ(vault.pending(), 2);
        CHECK(vault.spend(first)); // a millisecond before its time
        const QString third = vault.mint();
        now += 1; // ten minutes after the first two, to the millisecond
        CHECK(!vault.spend(second));
        CHECK_EQ(vault.pending(), 1);
        now += HostKeyVault::kLifetimeMs - 2;
        CHECK_EQ(vault.pending(), 1);
        now += 1;
        CHECK(!vault.spend(third));
        CHECK_EQ(vault.pending(), 0);
    }

    SECTION("Host key vault — eight wait at most: a ninth takes the oldest one's place");
    {
        qint64 now = 0;
        HostKeyVault vault([&now]() { return now; });
        QStringList keys;
        for (int i = 0; i < HostKeyVault::kMaxPending; ++i) {
            keys.append(vault.mint());
            now += 10;
        }
        CHECK_EQ(vault.pending(), HostKeyVault::kMaxPending);

        const QString ninth = vault.mint();
        CHECK_EQ(vault.pending(), HostKeyVault::kMaxPending);
        CHECK(!vault.spend(keys.first())); // the oldest gave way
        CHECK(vault.spend(keys.at(1)));    // the next oldest did not
        CHECK(vault.spend(ninth));
        CHECK_EQ(vault.pending(), HostKeyVault::kMaxPending - 2);

        // A hundred links asked for in a row leave eight keys, the last eight.
        QStringList many;
        for (int i = 0; i < 100; ++i)
            many.append(vault.mint());
        CHECK_EQ(vault.pending(), HostKeyVault::kMaxPending);
        CHECK(!vault.spend(keys.at(2)));
        CHECK(!vault.spend(many.at(91)));
        CHECK(vault.spend(many.at(92)));
        CHECK(vault.spend(many.last()));
        CHECK_EQ(vault.pending(), HostKeyVault::kMaxPending - 2);
    }

    SECTION("Host key vault — the bytes come from the generator it was given, whole");
    {
        int asked = 0;
        HostKeyVault vault({}, [&asked](int bytes) {
            asked = bytes;
            return QByteArray(bytes, '\x01');
        });
        const QString key = vault.mint();
        CHECK_EQ(asked, 32);
        CHECK(QByteArray::fromBase64(key.toLatin1(), QByteArray::Base64UrlEncoding) ==
              QByteArray(32, '\x01'));
        CHECK(vault.spend(key));
    }
}
