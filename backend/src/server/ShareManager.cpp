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

#include "server/ShareManager.h"

#include "common/Logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace {

QString sharePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/share.json");
}

const char* reasonName(ShareManager::EndReason reason)
{
    switch (reason) {
    case ShareManager::EndReason::OwnerToggle: return "owner-toggle";
    case ShareManager::EndReason::OwnerStop: return "owner-stop";
    case ShareManager::EndReason::Expired: return "expired";
    case ShareManager::EndReason::PinAbuse: return "pin-abuse";
    }
    return "unknown";
}

} // namespace

// ── Permissions ─────────────────────────────────────────────────────────────

QString ShareManager::Permissions::accessLevel() const
{
    if (keyboardMouse) return QStringLiteral("full");
    if (gamepad) return QStringLiteral("gamer");
    return QStringLiteral("viewer");
}

QJsonObject ShareManager::Permissions::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("gamepad")] = gamepad;
    obj[QStringLiteral("keyboardMouse")] = keyboardMouse;
    return obj;
}

ShareManager::Permissions ShareManager::Permissions::fromJson(const QJsonObject& obj)
{
    Permissions p;
    p.gamepad = obj.value(QStringLiteral("gamepad")).toBool(false);
    p.keyboardMouse = obj.value(QStringLiteral("keyboardMouse")).toBool(false);
    return p;
}

// ── Construction ────────────────────────────────────────────────────────────

ShareManager::ShareManager(QObject* parent)
    : QObject(parent)
{
    for (int slot = kFirstSlot; slot <= kLastSlot; ++slot)
        m_Slots.insert(slot, Slot{});

    load();

    // The TTL has to bite even when nobody asks: a player streaming at the
    // eight-hour mark must be disconnected, not merely refused on their next
    // join. One minute of granularity is plenty for an eight-hour lease.
    m_SweepTimer = new QTimer(this);
    m_SweepTimer->setInterval(60 * 1000);
    connect(m_SweepTimer, &QTimer::timeout, this, [this]() {
        pruneExpired();
        ratePurge();
    });
    m_SweepTimer->start();
}

ShareManager::~ShareManager() = default;

// ── Internals ───────────────────────────────────────────────────────────────

ShareManager::Slot* ShareManager::slotFor(int slot)
{
    auto it = m_Slots.find(slot);
    return it == m_Slots.end() ? nullptr : &it.value();
}

ShareManager::Slot* ShareManager::slotForTokenHash(const QString& tokenHash, int* outSlot)
{
    if (tokenHash.isEmpty()) return nullptr;
    Slot* found = nullptr;
    int foundSlot = -1;
    // Every slot is visited whatever happens: an early return would leak which
    // slot a token belongs to through the response time.
    for (auto it = m_Slots.begin(); it != m_Slots.end(); ++it) {
        Activation& act = it.value().activation;
        if (act.live() && constantTimeEquals(act.tokenHash, tokenHash)) {
            found = &it.value();
            foundSlot = it.key();
        }
    }
    if (outSlot) *outSlot = foundSlot;
    return found;
}

void ShareManager::pruneExpired()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QList<int> expired;
    for (auto it = m_Slots.constBegin(); it != m_Slots.constEnd(); ++it) {
        const Activation& act = it.value().activation;
        if (act.live() && now - act.activatedAt >= kTtlSecs) expired.append(it.key());
    }
    for (int slot : expired) {
        Logger::info(QStringLiteral("[Share] Slot %1 activation expired (8h)").arg(slot));
        clearActivation(slot, EndReason::Expired);
    }
    if (!expired.isEmpty()) save();
}

void ShareManager::clearActivation(int slot, EndReason reason, bool notify)
{
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return;

    const bool wasStreaming = s->activation.streaming;
    s->activation = Activation{};

    if (notify) {
        if (wasStreaming) emit playerMustDisconnect(slot, static_cast<int>(reason));
        emit slotChanged(slot);
    }
}

// ── Owner side ──────────────────────────────────────────────────────────────

QList<ShareManager::SlotStatus> ShareManager::status()
{
    pruneExpired();

    QList<SlotStatus> out;
    for (int slot = kFirstSlot; slot <= kLastSlot; ++slot) {
        const Slot* s = &m_Slots[slot];
        SlotStatus st;
        st.slot = slot;
        if (!s->activation.live()) {
            st.state = State::Off;
            // Off rows still show a choice: the one this player had last time.
            st.permissions = s->lastPermissions;
            st.locked = false;
            st.expiresAt = 0;
        } else {
            st.state = s->activation.streaming ? State::Streaming : State::Shared;
            st.permissions = s->activation.permissions;
            st.locked = s->activation.locked;
            st.expiresAt = s->activation.activatedAt + kTtlSecs;
        }
        out.append(st);
    }
    return out;
}

bool ShareManager::activate(int slot, QString& outToken, QString& outPin, SlotStatus& outStatus)
{
    if (!isPlayerSlot(slot)) return false;
    pruneExpired();

    Slot* s = slotFor(slot);
    if (!s) return false;

    // Re-sharing a slot invalidates whatever was out there: the old link and
    // PIN stop working the moment a new pair exists.
    if (s->activation.live()) clearActivation(slot, EndReason::OwnerToggle);

    outToken = randomToken();
    outPin = randomPin();

    Activation act;
    act.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    act.tokenHash = hash(outToken);
    act.pinHash = hash(outPin);
    act.tokenClear = outToken;
    act.pinClear = outPin;
    act.permissions = s->lastPermissions; // viewer on a fresh install
    act.locked = false;
    act.activatedAt = QDateTime::currentSecsSinceEpoch();
    s->activation = act;

    save();
    emit slotChanged(slot);

    outStatus.slot = slot;
    outStatus.state = State::Shared;
    outStatus.permissions = act.permissions;
    outStatus.locked = false;
    outStatus.expiresAt = act.activatedAt + kTtlSecs;

    Logger::info(QStringLiteral("[Share] Slot %1 shared (access=%2, 8h)")
                     .arg(slot)
                     .arg(act.permissions.accessLevel()));
    return true;
}

bool ShareManager::secrets(int slot, QString& outToken, QString& outPin)
{
    pruneExpired();
    const Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return false;
    // Empty after a restart: the digests were reloaded, the clear pair was not.
    if (s->activation.tokenClear.isEmpty() || s->activation.pinClear.isEmpty()) return false;

    outToken = s->activation.tokenClear;
    outPin = s->activation.pinClear;
    return true;
}

bool ShareManager::setPermissions(int slot, const Permissions& perms)
{
    pruneExpired();
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return false;
    // Once frozen the answer is no: the player was told what they were getting,
    // and a worker already carries this policy.
    if (s->activation.locked) return false;

    s->activation.permissions = perms;
    s->lastPermissions = perms; // remembered for the next popin
    save();
    emit slotChanged(slot);
    return true;
}

bool ShareManager::lockPermissions(int slot)
{
    pruneExpired();
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return false;
    if (s->activation.locked) return true;

    s->activation.locked = true;
    save();
    Logger::info(QStringLiteral("[Share] Slot %1 permissions locked (access=%2)")
                     .arg(slot)
                     .arg(s->activation.permissions.accessLevel()));
    return true;
}

bool ShareManager::deactivate(int slot, EndReason reason)
{
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return false;

    Logger::info(QStringLiteral("[Share] Slot %1 revoked (%2)")
                     .arg(slot)
                     .arg(QLatin1String(reasonName(reason))));
    clearActivation(slot, reason);
    save();
    return true;
}

void ShareManager::deactivateAll(EndReason reason)
{
    bool any = false;
    for (int slot = kFirstSlot; slot <= kLastSlot; ++slot) {
        if (!m_Slots[slot].activation.live()) continue;
        clearActivation(slot, reason);
        any = true;
    }
    if (any) {
        Logger::info(QStringLiteral("[Share] All slots revoked (%1)")
                         .arg(QLatin1String(reasonName(reason))));
        save();
    }
}

int ShareManager::streamingCount()
{
    pruneExpired();
    int n = 0;
    for (auto it = m_Slots.constBegin(); it != m_Slots.constEnd(); ++it)
        if (it.value().activation.live() && it.value().activation.streaming) ++n;
    return n;
}

// ── Player side ─────────────────────────────────────────────────────────────

ShareManager::PinOutcome ShareManager::redeemPin(const QString& token, const QString& pin,
                                                 const QString& rateKey)
{
    pruneExpired();

    PinOutcome out;

    // One bucket per caller. Keying it on the token as well would let an
    // attacker with several links dodge the ladder, and keying it ONLY on the
    // token would let one attacker lock out the legitimate player.
    const QString bucket = QStringLiteral("share-pin:") + rateKey;
    if (const int locked = rateLockout(bucket); locked > 0) {
        out.result = PinResult::RateLimited;
        out.lockoutSeconds = locked;
        return out;
    }

    int slot = -1;
    Slot* s = slotForTokenHash(hash(token), &slot);
    const bool matched =
        s && !pin.isEmpty() && constantTimeEquals(s->activation.pinHash, hash(pin));

    rateRecord(bucket, matched, out.remainingAttempts, out.lockoutSeconds);

    if (!matched) {
        // An unknown token and a wrong PIN are the same answer. The per-
        // activation counter only runs when the token is real, otherwise
        // hammering random tokens could kill a share that exists.
        if (s) {
            s->activation.pinFailures++;
            if (s->activation.pinFailures >= kMaxPinFailures) {
                Logger::warning(QStringLiteral("[Share] Slot %1 revoked — %2 wrong PINs on this "
                                               "activation")
                                    .arg(slot)
                                    .arg(s->activation.pinFailures));
                clearActivation(slot, EndReason::PinAbuse);
            }
            save();
        }
        out.result = PinResult::Invalid;
        return out;
    }

    Activation& act = s->activation;
    act.pinFailures = 0;
    out.cookie = randomToken();
    act.cookieHashes.append(hash(out.cookie));
    while (act.cookieHashes.size() > kMaxCookiesPerActivation)
        act.cookieHashes.removeFirst();
    save();

    out.result = PinResult::Ok;
    out.slot = slot;
    Logger::info(
        QStringLiteral("[Share] Slot %1 PIN accepted — player device authorized").arg(slot));
    return out;
}

int ShareManager::slotForCookie(const QString& cookie)
{
    if (cookie.isEmpty()) return -1;
    pruneExpired();

    const QString h = hash(cookie);
    int found = -1;
    for (auto it = m_Slots.constBegin(); it != m_Slots.constEnd(); ++it) {
        const Activation& act = it.value().activation;
        if (!act.live()) continue;
        for (const QString& stored : act.cookieHashes)
            if (constantTimeEquals(stored, h)) found = it.key();
    }
    return found;
}

bool ShareManager::tokenIsLive(const QString& token)
{
    pruneExpired();
    return slotForTokenHash(hash(token)) != nullptr;
}

// ── Stream lifecycle ────────────────────────────────────────────────────────

void ShareManager::setStreaming(int slot, bool streaming)
{
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return;
    if (s->activation.streaming == streaming) return;

    s->activation.streaming = streaming;
    // A stream that drops — tab closed, ICE dead, host hiccup — leaves the
    // activation alive on purpose: only an owner action or the TTL ends a share.
    if (streaming && !s->activation.locked) lockPermissions(slot);
    emit slotChanged(slot);
}

ShareManager::Permissions ShareManager::permissions(int slot)
{
    Slot* s = slotFor(slot);
    if (!s) return {};
    return s->activation.live() ? s->activation.permissions : s->lastPermissions;
}

ShareManager::State ShareManager::state(int slot)
{
    pruneExpired();
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return State::Off;
    return s->activation.streaming ? State::Streaming : State::Shared;
}

qint64 ShareManager::expiresAt(int slot)
{
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return 0;
    return s->activation.activatedAt + kTtlSecs;
}

// ── Rate limiting ───────────────────────────────────────────────────────────

int ShareManager::rateLockout(const QString& bucket) const
{
    auto it = m_RateLimits.find(bucket);
    if (it == m_RateLimits.end()) return 0;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (it->lockoutUntil > now) return static_cast<int>(it->lockoutUntil - now);
    return 0;
}

void ShareManager::rateRecord(const QString& bucket, bool matched, int& outRemaining,
                              int& outLockout)
{
    RateEntry& e = m_RateLimits[bucket];
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    if (matched) {
        e.failures = 0;
        e.lockoutUntil = 0;
        outRemaining = RATE_TIER1;
        outLockout = 0;
        return;
    }

    e.failures++;
    e.lastFailure = now;
    if (e.failures >= RATE_TIER3)
        e.lockoutUntil = now + LOCKOUT_LONG_SECS;
    else if (e.failures >= RATE_TIER2)
        e.lockoutUntil = now + LOCKOUT_MEDIUM_SECS;
    else if (e.failures >= RATE_TIER1)
        e.lockoutUntil = now + LOCKOUT_SHORT_SECS;

    if (e.failures < RATE_TIER1)
        outRemaining = RATE_TIER1 - e.failures;
    else if (e.failures < RATE_TIER2)
        outRemaining = RATE_TIER2 - e.failures;
    else if (e.failures < RATE_TIER3)
        outRemaining = RATE_TIER3 - e.failures;
    else
        outRemaining = 0;

    outLockout = rateLockout(bucket);
}

void ShareManager::ratePurge()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto it = m_RateLimits.begin(); it != m_RateLimits.end();) {
        // Same decay as AuthManager: one failure forgiven per quiet 10 minutes.
        if (it->lockoutUntil <= now && now - it->lastFailure >= LOCKOUT_LONG_SECS) {
            it->failures = qMax(0, it->failures - 1);
            if (it->failures == 0) {
                it = m_RateLimits.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// ── Persistence ─────────────────────────────────────────────────────────────

void ShareManager::save()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);

    // Not named `slots`: Qt's moc keyword macro would eat it.
    QJsonArray slotArray;
    for (int slot = kFirstSlot; slot <= kLastSlot; ++slot) {
        const Slot& s = m_Slots[slot];
        QJsonObject obj;
        obj[QStringLiteral("slot")] = slot;
        obj[QStringLiteral("lastPermissions")] = s.lastPermissions.toJson();
        if (s.activation.live()) {
            QJsonObject act;
            act[QStringLiteral("id")] = s.activation.id;
            act[QStringLiteral("tokenHash")] = s.activation.tokenHash;
            act[QStringLiteral("pinHash")] = s.activation.pinHash;
            act[QStringLiteral("cookieHashes")] =
                QJsonArray::fromStringList(s.activation.cookieHashes);
            act[QStringLiteral("permissions")] = s.activation.permissions.toJson();
            act[QStringLiteral("locked")] = s.activation.locked;
            act[QStringLiteral("activatedAt")] = s.activation.activatedAt;
            act[QStringLiteral("pinFailures")] = s.activation.pinFailures;
            obj[QStringLiteral("activation")] = act;
        }
        slotArray.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("slots")] = slotArray;

    QFile file(sharePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::warning(QStringLiteral("[Share] Failed to write ") + sharePath());
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void ShareManager::load()
{
    QFile file(sharePath());
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) return;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QJsonArray slotArray = doc.object().value(QStringLiteral("slots")).toArray();
    for (const QJsonValue& v : slotArray) {
        const QJsonObject obj = v.toObject();
        const int slot = obj.value(QStringLiteral("slot")).toInt(-1);
        if (!isPlayerSlot(slot)) continue;

        Slot& s = m_Slots[slot];
        s.lastPermissions =
            Permissions::fromJson(obj.value(QStringLiteral("lastPermissions")).toObject());

        const QJsonObject act = obj.value(QStringLiteral("activation")).toObject();
        if (act.isEmpty()) continue;

        const qint64 activatedAt =
            static_cast<qint64>(act.value(QStringLiteral("activatedAt")).toDouble());
        // A link handed out before a restart keeps working: only an owner action
        // or the eight hours end a share, and a restart is neither.
        if (activatedAt <= 0 || now - activatedAt >= kTtlSecs) continue;

        Activation a;
        a.id = act.value(QStringLiteral("id")).toString();
        a.tokenHash = act.value(QStringLiteral("tokenHash")).toString();
        a.pinHash = act.value(QStringLiteral("pinHash")).toString();
        for (const QJsonValue& c : act.value(QStringLiteral("cookieHashes")).toArray())
            a.cookieHashes.append(c.toString());
        a.permissions = Permissions::fromJson(act.value(QStringLiteral("permissions")).toObject());
        a.locked = act.value(QStringLiteral("locked")).toBool(false);
        a.activatedAt = activatedAt;
        a.pinFailures = act.value(QStringLiteral("pinFailures")).toInt(0);
        a.streaming = false; // the worker died with the process
        if (a.tokenHash.isEmpty() || a.pinHash.isEmpty()) continue;
        s.activation = a;

        Logger::info(QStringLiteral("[Share] Slot %1 activation restored (%2s left)")
                         .arg(slot)
                         .arg(kTtlSecs - (now - activatedAt)));
    }
}

// ── Crypto helpers ──────────────────────────────────────────────────────────

QString ShareManager::hash(const QString& value)
{
    const QByteArray h = QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(
        h.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool ShareManager::constantTimeEquals(const QString& a, const QString& b)
{
    const QByteArray ba = a.toUtf8();
    const QByteArray bb = b.toUtf8();
    const int n = qMax(ba.size(), bb.size());
    quint8 diff = static_cast<quint8>(ba.size() ^ bb.size());
    for (int i = 0; i < n; ++i) {
        const quint8 ca = i < ba.size() ? static_cast<quint8>(ba[i]) : 0;
        const quint8 cb = i < bb.size() ? static_cast<quint8>(bb[i]) : 0;
        diff |= static_cast<quint8>(ca ^ cb);
    }
    return diff == 0;
}

QString ShareManager::randomToken()
{
    // 32 bytes from the system CSPRNG, base64url — safe in a URL and in a
    // Set-Cookie value alike.
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words);
    const QByteArray raw(reinterpret_cast<const char*>(words), sizeof(words));
    return QString::fromLatin1(
        raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString ShareManager::randomPin()
{
    // Six digits, uniform. Short on purpose — it is typed by hand and it is the
    // second factor, never the only one; kMaxPinFailures is what makes it safe.
    const quint32 n = QRandomGenerator::system()->bounded(1000000u);
    return QStringLiteral("%1").arg(n, 6, 10, QLatin1Char('0'));
}
