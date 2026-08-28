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
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace {

QString sharePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/share.json");
}

/// The lifetimes the board offers, in seconds. 0 is unlimited and last on
/// purpose: it is a deliberate choice, not somewhere a slider lands by accident.
constexpr qint64 kTtlChoices[] = {3600, 4 * 3600, 8 * 3600, 24 * 3600, 48 * 3600, 0};

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

bool ShareManager::isValidTtl(qint64 secs)
{
    for (const qint64 choice : kTtlChoices)
        if (secs == choice) return true;
    return false;
}

QString ShareManager::sanitizeName(const QString& name)
{
    QString out = name.simplified();
    // Control characters would travel straight into the board's markup; the
    // view escapes what it prints, but there is no reason to store them.
    out.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1F\\x7F]")));
    return out.left(kMaxNameLength);
}

// ── Permissions ─────────────────────────────────────────────────────────────

QString ShareManager::Permissions::accessLevel() const
{
    // Both flags matter. Keyboard and mouse alone is "desktop" — someone who can
    // drive the machine but not play; it used to report "full", which promised
    // the owner a gamepad they had not granted.
    if (gamepad && keyboardMouse) return QStringLiteral("full");
    if (keyboardMouse) return QStringLiteral("desktop");
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
        // A deadline of 0 is unlimited: it outlives the sweep, and only an
        // owner action ends it.
        if (act.live() && act.expiresAtSecs > 0 && now >= act.expiresAtSecs)
            expired.append(it.key());
    }
    for (int slot : expired) {
        Logger::info(QStringLiteral("[Share] Slot %1 activation expired").arg(slot));
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
        st.name = s->name;
        if (!s->activation.live()) {
            st.state = State::Off;
            // Off rows still show a choice: the one this player had last time.
            st.permissions = s->lastPermissions;
            st.ttlSecs = s->lastTtlSecs;
            st.expiresAt = 0;
        } else {
            // Binded is about the invitation, not the picture: a guest who
            // spent the PIN and then closed their tab is still bound to it.
            st.state = s->activation.devices.isEmpty() ? State::Shared : State::Binded;
            st.permissions = s->activation.permissions;
            st.streaming = s->activation.streaming;
            st.ttlSecs = s->activation.ttlSecs;
            st.expiresAt = s->activation.expiresAt();
            st.devices = s->activation.devices;
            st.lastRefusedAt = s->activation.lastRefusedAt;
            st.lastRefusedAgent = s->activation.lastRefusedAgent;
        }
        out.append(st);
    }
    return out;
}

bool ShareManager::activate(int slot, const QString& hostUuid, int appId, qint64 ttlSecs,
                            QString& outToken, QString& outPin, SlotStatus& outStatus)
{
    if (!isPlayerSlot(slot)) return false;
    // A share is an invitation onto a *specific* host. With no host there is
    // nothing to bind the link to, and an unbound link is exactly the cross-host
    // hole we are closing.
    if (hostUuid.isEmpty()) return false;
    if (!isValidTtl(ttlSecs)) return false;
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
    act.ttlSecs = ttlSecs;
    act.activatedAt = QDateTime::currentSecsSinceEpoch();
    act.expiresAtSecs = ttlSecs > 0 ? act.activatedAt + ttlSecs : 0;
    act.hostUuid = hostUuid;
    act.appId = appId;
    s->activation = act;
    s->lastTtlSecs = ttlSecs;

    save();
    emit slotChanged(slot);

    outStatus.slot = slot;
    outStatus.state = State::Shared;
    outStatus.name = s->name;
    outStatus.permissions = act.permissions;
    outStatus.ttlSecs = ttlSecs;
    outStatus.expiresAt = act.expiresAt();

    Logger::info(
        QStringLiteral("[Share] Slot %1 shared on host %2 (access=%3, ttl=%4)")
            .arg(slot)
            .arg(hostUuid)
            .arg(act.permissions.accessLevel())
            .arg(ttlSecs > 0 ? QStringLiteral("%1s").arg(ttlSecs) : QStringLiteral("unlimited")));
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
    if (!s) return false;

    // Nothing shared yet: this is the choice the next START will be minted
    // with, so it is remembered rather than refused. Refusing it left the
    // board's own poll free to repaint the row from a server that had never
    // been told — the tick came back off a second after the owner made it.
    if (!s->activation.live()) {
        if (s->lastPermissions.gamepad == perms.gamepad &&
            s->lastPermissions.keyboardMouse == perms.keyboardMouse)
            return true;
        s->lastPermissions = perms;
        save();
        emit slotChanged(slot);
        return true;
    }

    if (s->activation.permissions.gamepad == perms.gamepad &&
        s->activation.permissions.keyboardMouse == perms.keyboardMouse)
        return true;

    s->activation.permissions = perms;
    s->lastPermissions = perms; // remembered for the next opening
    save();

    Logger::info(
        QStringLiteral("[Share] Slot %1 access now %2").arg(slot).arg(perms.accessLevel()));
    // Down to the worker before the board hears about it: a demotion that shows
    // in the UI before it takes effect on the host is a lie for as long as the
    // round trip lasts.
    emit permissionsChanged(slot, perms.gamepad, perms.keyboardMouse);
    emit slotChanged(slot);
    return true;
}

bool ShareManager::setTtl(int slot, qint64 ttlSecs)
{
    if (!isValidTtl(ttlSecs)) return false;
    pruneExpired();
    Slot* s = slotFor(slot);
    if (!s) return false;
    s->lastTtlSecs = ttlSecs;
    if (!s->activation.live()) {
        save();
        emit slotChanged(slot);
        return true;
    }

    Activation& act = s->activation;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // Counted from now, and capped by the deadline already in force. See the
    // header: the same gesture has to mean the same thing on a fresh invitation
    // and on one that has nearly run out, and it may never buy a live link
    // more time than it already had.
    const qint64 candidate = ttlSecs > 0 ? now + ttlSecs : 0;
    if (act.expiresAtSecs == 0)
        act.expiresAtSecs = candidate; // unlimited can still be cut short
    else if (candidate > 0)
        act.expiresAtSecs = qMin(candidate, act.expiresAtSecs);
    // candidate == 0 on a dated invitation: unlimited would be an extension.

    act.ttlSecs = ttlSecs;
    save();
    emit slotChanged(slot);
    return true;
}

bool ShareManager::setName(int slot, const QString& name)
{
    Slot* s = slotFor(slot);
    if (!s) return false;
    const QString clean = sanitizeName(name);
    if (s->name == clean) return true;
    s->name = clean;
    save();
    emit slotChanged(slot);
    return true;
}

QString ShareManager::name(int slot)
{
    Slot* s = slotFor(slot);
    return s ? s->name : QString();
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
                                                 const QString& rateKey, const QString& userAgent)
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
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // Long enough to name a browser and an OS, short enough that nobody can use
    // the header as storage.
    const QString agent = userAgent.simplified().left(256);

    // One device per invitation. A correct PIN offered by a second one is
    // neither an attack nor a typo: the link was passed along. Refuse it, keep
    // the legitimate holder's attempts intact, and put it where the owner will
    // see it — not knowing this had happened was the whole complaint.
    if (act.cookieHashes.size() >= kMaxCookiesPerActivation) {
        act.lastRefusedAt = now;
        act.lastRefusedAgent = agent;
        save();
        emit slotChanged(slot);
        Logger::warning(QStringLiteral("[Share] Slot %1 refused a second device — this "
                                       "invitation is already bound")
                            .arg(slot));
        out.result = PinResult::AlreadyBound;
        out.slot = slot;
        return out;
    }

    out.cookie = randomToken();
    act.cookieHashes.append(hash(out.cookie));
    act.devices.append(BoundDevice{now, agent});
    save();
    // Shared → Binded: the board has a new row to draw, and the owner has a
    // device to recognise or disown.
    emit slotChanged(slot);

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
    return slotForToken(token) >= 0;
}

int ShareManager::slotForToken(const QString& token)
{
    pruneExpired();
    int slot = -1;
    return slotForTokenHash(hash(token), &slot) ? slot : -1;
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
    // Nothing freezes here any more: the policy follows the owner's board for
    // the whole life of the stream (see permissionsChanged).
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
    return s->activation.devices.isEmpty() ? State::Shared : State::Binded;
}

bool ShareManager::isStreaming(int slot)
{
    Slot* s = slotFor(slot);
    return s && s->activation.live() && s->activation.streaming;
}

qint64 ShareManager::expiresAt(int slot)
{
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return 0;
    return s->activation.expiresAt();
}

qint64 ShareManager::ttlSecs(int slot)
{
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return 0;
    return s->activation.ttlSecs;
}

QString ShareManager::hostForSlot(int slot)
{
    pruneExpired();
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return {};
    return s->activation.hostUuid;
}

int ShareManager::appForSlot(int slot)
{
    pruneExpired();
    Slot* s = slotFor(slot);
    if (!s || !s->activation.live()) return -1;
    return s->activation.appId;
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
        obj[QStringLiteral("name")] = s.name;
        obj[QStringLiteral("lastPermissions")] = s.lastPermissions.toJson();
        obj[QStringLiteral("lastTtlSecs")] = s.lastTtlSecs;
        if (s.activation.live()) {
            QJsonObject act;
            act[QStringLiteral("id")] = s.activation.id;
            act[QStringLiteral("tokenHash")] = s.activation.tokenHash;
            act[QStringLiteral("pinHash")] = s.activation.pinHash;
            act[QStringLiteral("cookieHashes")] =
                QJsonArray::fromStringList(s.activation.cookieHashes);
            QJsonArray devices;
            for (const BoundDevice& d : s.activation.devices) {
                QJsonObject dev;
                dev[QStringLiteral("boundAt")] = d.boundAt;
                dev[QStringLiteral("userAgent")] = d.userAgent;
                devices.append(dev);
            }
            act[QStringLiteral("devices")] = devices;
            act[QStringLiteral("permissions")] = s.activation.permissions.toJson();
            act[QStringLiteral("ttlSecs")] = s.activation.ttlSecs;
            act[QStringLiteral("expiresAt")] = s.activation.expiresAtSecs;
            act[QStringLiteral("activatedAt")] = s.activation.activatedAt;
            act[QStringLiteral("pinFailures")] = s.activation.pinFailures;
            act[QStringLiteral("hostUuid")] = s.activation.hostUuid;
            act[QStringLiteral("appId")] = s.activation.appId;
            act[QStringLiteral("lastRefusedAt")] = s.activation.lastRefusedAt;
            act[QStringLiteral("lastRefusedAgent")] = s.activation.lastRefusedAgent;
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
    const QJsonObject root = doc.object();
    const QJsonArray slotArray = root.value(QStringLiteral("slots")).toArray();
    for (const QJsonValue& v : slotArray) {
        const QJsonObject obj = v.toObject();
        const int slot = obj.value(QStringLiteral("slot")).toInt(-1);
        if (!isPlayerSlot(slot)) continue;

        Slot& s = m_Slots[slot];
        s.name = sanitizeName(obj.value(QStringLiteral("name")).toString());
        s.lastPermissions =
            Permissions::fromJson(obj.value(QStringLiteral("lastPermissions")).toObject());
        const qint64 lastTtl =
            static_cast<qint64>(obj.value(QStringLiteral("lastTtlSecs")).toDouble(kTtlSecs));
        s.lastTtlSecs = isValidTtl(lastTtl) ? lastTtl : kTtlSecs;

        const QJsonObject act = obj.value(QStringLiteral("activation")).toObject();
        if (act.isEmpty()) continue;

        const qint64 activatedAt =
            static_cast<qint64>(act.value(QStringLiteral("activatedAt")).toDouble());
        if (activatedAt <= 0) continue;
        // A lifetime that is not on the list any more (hand-edited file, older
        // build) falls back to the default rather than becoming unlimited by
        // accident — unlimited has to be asked for.
        const qint64 ttl =
            static_cast<qint64>(act.value(QStringLiteral("ttlSecs")).toDouble(kTtlSecs));
        const qint64 ttlSecs = isValidTtl(ttl) ? ttl : kTtlSecs;
        // The deadline is stored, not derived. A file written before it was
        // (or hand-edited) falls back to the old rule so nothing that was alive
        // becomes immortal on upgrade.
        const qint64 expiresAt =
            act.contains(QStringLiteral("expiresAt"))
                ? static_cast<qint64>(act.value(QStringLiteral("expiresAt")).toDouble())
                : (ttlSecs > 0 ? activatedAt + ttlSecs : 0);
        // A link handed out before a restart keeps working: only an owner action
        // or its own deadline ends a share, and a restart is neither.
        if (expiresAt > 0 && now >= expiresAt) continue;

        Activation a;
        a.id = act.value(QStringLiteral("id")).toString();
        a.tokenHash = act.value(QStringLiteral("tokenHash")).toString();
        a.pinHash = act.value(QStringLiteral("pinHash")).toString();
        for (const QJsonValue& c : act.value(QStringLiteral("cookieHashes")).toArray())
            a.cookieHashes.append(c.toString());
        for (const QJsonValue& d : act.value(QStringLiteral("devices")).toArray()) {
            const QJsonObject dev = d.toObject();
            a.devices.append(
                BoundDevice{static_cast<qint64>(dev.value(QStringLiteral("boundAt")).toDouble()),
                            dev.value(QStringLiteral("userAgent")).toString()});
        }
        // Cookies are what actually open a session; the device list is only what
        // the board draws. Keep them the same length so a Binded row can never
        // be shown as Shared (or the reverse) after a hand-edit or an upgrade
        // from a build that had no device list.
        while (a.devices.size() < a.cookieHashes.size())
            a.devices.append(BoundDevice{});
        a.devices = a.devices.mid(0, a.cookieHashes.size());
        a.permissions = Permissions::fromJson(act.value(QStringLiteral("permissions")).toObject());
        a.ttlSecs = ttlSecs;
        a.expiresAtSecs = expiresAt;
        a.activatedAt = activatedAt;
        a.pinFailures = act.value(QStringLiteral("pinFailures")).toInt(0);
        a.streaming = false; // the worker died with the process
        a.hostUuid = act.value(QStringLiteral("hostUuid")).toString();
        a.appId = act.value(QStringLiteral("appId")).toInt(-1);
        a.lastRefusedAt =
            static_cast<qint64>(act.value(QStringLiteral("lastRefusedAt")).toDouble());
        a.lastRefusedAgent = act.value(QStringLiteral("lastRefusedAgent")).toString();
        if (a.tokenHash.isEmpty() || a.pinHash.isEmpty()) continue;
        // An activation with no bound host predates the per-host binding (or was
        // hand-edited): drop it rather than let it resolve to an arbitrary host.
        if (a.hostUuid.isEmpty()) continue;
        s.activation = a;

        Logger::info(QStringLiteral("[Share] Slot %1 activation restored (%2)")
                         .arg(slot)
                         .arg(expiresAt > 0 ? QStringLiteral("%1s left").arg(expiresAt - now)
                                            : QStringLiteral("unlimited")));
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
