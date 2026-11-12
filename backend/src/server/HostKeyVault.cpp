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

#include "server/HostKeyVault.h"

#include "server/RequestGuard.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QRandomGenerator>

#include <utility>

namespace {

QByteArray hashOf(const QString& key)
{
    return QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256);
}

} // namespace

HostKeyVault::HostKeyVault(std::function<qint64()> nowMs, std::function<QByteArray(int)> random)
    : m_NowMs(std::move(nowMs))
    , m_Random(std::move(random))
{
    if (!m_NowMs) {
        // Since this vault was made: a clock no change of the system's date
        // moves.
        QElapsedTimer clock;
        clock.start();
        m_NowMs = [clock]() { return clock.elapsed(); };
    }
    if (!m_Random) {
        m_Random = [](int bytes) {
            QByteArray raw(bytes, '\0');
            for (int i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
            return raw;
        };
    }
}

void HostKeyVault::dropExpired()
{
    const qint64 now = m_NowMs();
    while (!m_Entries.isEmpty() && now - m_Entries.first().mintedAt >= kLifetimeMs)
        m_Entries.removeFirst();
}

QString HostKeyVault::mint()
{
    dropExpired();
    const QString key = QString::fromLatin1(
        m_Random(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    while (m_Entries.size() >= kMaxPending)
        m_Entries.removeFirst();
    m_Entries.append({hashOf(key), m_NowMs()});
    return key;
}

int HostKeyVault::indexOf(const QString& key)
{
    dropExpired();
    if (key.isEmpty()) return -1;
    // Every entry is compared, each in a time that says nothing of where two
    // hashes part: which key came close is not this function's to tell.
    const QByteArray given = hashOf(key);
    int found = -1;
    for (int i = 0; i < m_Entries.size(); ++i)
        if (RequestGuard::secretMatches(given, m_Entries[i].hash)) found = i;
    return found;
}

bool HostKeyVault::spend(const QString& key)
{
    const int i = indexOf(key);
    if (i < 0) return false;
    m_Entries.removeAt(i);
    return true;
}

int HostKeyVault::pending()
{
    dropExpired();
    return static_cast<int>(m_Entries.size());
}
