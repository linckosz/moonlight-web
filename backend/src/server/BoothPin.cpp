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

#include "server/BoothPin.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>

namespace {

/// The session's bucket, under a digest: the token itself is not kept here.
QString sessionBucket(const QString& sessionKey)
{
    return QStringLiteral("s:") +
           QString::fromLatin1(
               QCryptographicHash::hash(sessionKey.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString addressBucket(const QString& addressKey)
{
    return QStringLiteral("a:") + addressKey;
}

} // namespace

BoothPin::BoothPin()
    : m_Clock([]() { return QDateTime::currentSecsSinceEpoch(); })
    , m_Generator([]() {
        const quint32 n = QRandomGenerator::system()->bounded(1000000u);
        return QStringLiteral("%1").arg(n, kDigits, 10, QLatin1Char('0'));
    })
{}

void BoothPin::setClockForTesting(std::function<qint64()> nowSecs)
{
    m_Clock = std::move(nowSecs);
}

void BoothPin::setGeneratorForTesting(std::function<QString()> next)
{
    m_Generator = std::move(next);
}

qint64 BoothPin::now() const
{
    return m_Clock();
}

QString BoothPin::fresh(const QString& unlike)
{
    // A change is a change: the new PIN never repeats the one it replaces, so
    // "the PIN changed" is always true for whoever read the old one.
    QString pin = m_Generator();
    for (int i = 0; i < 16 && pin == unlike; ++i)
        pin = m_Generator();
    return pin;
}

QString BoothPin::mint(const QString& key)
{
    Entry e;
    e.pin = fresh(m_Pins.value(key).pin);
    e.reason = Reason::Minted;
    m_Pins.insert(key, e);
    return e.pin;
}

void BoothPin::forget(const QString& key)
{
    m_Pins.remove(key);
}

bool BoothPin::has(const QString& key) const
{
    return m_Pins.contains(key);
}

QString BoothPin::read(const QString& key) const
{
    return m_Pins.value(key).pin;
}

QString BoothPin::rotate(const QString& key, Reason why)
{
    auto it = m_Pins.find(key);
    if (it == m_Pins.end()) return QString();
    it->pin = fresh(it->pin);
    it->failures = 0;
    it->generation++;
    it->reason = why;
    return it->pin;
}

quint64 BoothPin::generation(const QString& key) const
{
    return m_Pins.value(key).generation;
}

BoothPin::Reason BoothPin::lastReason(const QString& key) const
{
    return m_Pins.value(key).reason;
}

int BoothPin::lockout(const QString& bucket) const
{
    const auto it = m_Buckets.constFind(bucket);
    if (it == m_Buckets.constEnd()) return 0;
    const qint64 left = it->lockoutUntil - now();
    return left > 0 ? static_cast<int>(left) : 0;
}

int BoothPin::remaining(const QString& bucket) const
{
    const int failures = m_Buckets.value(bucket).failures;
    if (failures < kTier1) return kTier1 - failures;
    if (failures < kTier2) return kTier2 - failures;
    if (failures < kTier3) return kTier3 - failures;
    return 0;
}

void BoothPin::fail(const QString& bucket)
{
    Bucket& b = m_Buckets[bucket];
    const qint64 t = now();
    b.failures++;
    b.lastFailure = t;
    if (b.failures >= kTier3)
        b.lockoutUntil = t + kLockoutLongSecs;
    else if (b.failures >= kTier2)
        b.lockoutUntil = t + kLockoutMediumSecs;
    else if (b.failures >= kTier1)
        b.lockoutUntil = t + kLockoutShortSecs;
}

void BoothPin::purge()
{
    const qint64 t = now();
    for (auto it = m_Buckets.begin(); it != m_Buckets.end();) {
        if (it->lockoutUntil <= t && t - it->lastFailure > kBucketIdleSecs)
            it = m_Buckets.erase(it);
        else
            ++it;
    }
}

bool BoothPin::samePin(const QString& a, const QString& b)
{
    // In a time that depends only on the longer input.
    const QByteArray ba = a.toUtf8();
    const QByteArray bb = b.toUtf8();
    const int n = qMax(ba.size(), bb.size());
    quint8 diff = ba.size() == bb.size() ? 0 : 1;
    for (int i = 0; i < n; ++i) {
        const quint8 ca = i < ba.size() ? static_cast<quint8>(ba[i]) : 0;
        const quint8 cb = i < bb.size() ? static_cast<quint8>(bb[i]) : 0;
        diff |= static_cast<quint8>(ca ^ cb);
    }
    return diff == 0 && !a.isEmpty();
}

BoothPin::Outcome BoothPin::check(const QString& key, const QString& pin, const QString& sessionKey,
                                  const QString& addressKey)
{
    purge();
    Outcome out;
    const auto entry = m_Pins.find(key);
    if (entry == m_Pins.end()) {
        out.result = Result::NoStream;
        return out;
    }

    const QString s = sessionBucket(sessionKey);
    const QString a = addressBucket(addressKey);
    const int shut = qMax(lockout(s), lockout(a));
    if (shut > 0) {
        out.result = Result::RateLimited;
        out.lockoutSeconds = shut;
        return out;
    }

    QString typed = pin;
    typed.remove(QLatin1Char(' '));
    if (samePin(typed, entry->pin)) {
        // Both buckets stay as they are: they count tries at every stream, and
        // one stream's PIN, which a member reads, must not reopen them for
        // guesses at another's — nor one device's success open a neighbour's
        // address. They age out instead (purge()).
        out.result = Result::Ok;
        out.remainingAttempts = qMin(remaining(s), remaining(a));
        return out;
    }

    fail(s);
    fail(a);
    out.result = Result::Wrong;
    out.remainingAttempts = qMin(remaining(s), remaining(a));
    out.lockoutSeconds = qMax(lockout(s), lockout(a));
    if (++entry->failures >= kFailuresBeforeRotation) {
        rotate(key, Reason::Failures);
        out.rotated = true;
    }
    return out;
}
