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

#include "AuthManager.h"
#include "AppSettings.h"
#include "common/Logger.h"
#include "common/PairingCrypto.h"

#include <QRandomGenerator>
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPasswordDigestor>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <algorithm>

AuthManager::AuthManager(AppSettings* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_currentPin(QStringLiteral("------"))
{
    // Load or generate persistent HMAC key
    if (settings) {
        m_hmacKey = settings->hmacKey();
        if (m_hmacKey.isEmpty()) {
            m_hmacKey = generateRandomKey();
            settings->setHmacKey(m_hmacKey);
        }
    } else {
        // Fallback: generate a random key each startup (pre-5b behaviour)
        m_hmacKey = generateRandomKey();
    }
    Logger::info("[AuthManager] HMAC key initialized, no PIN set");

    // Reload sessions from disk if AppSettings is available
    loadSessions();

    // Periodically purge sessions that have been inactive past the TTL.
    m_purgeTimer = new QTimer(this);
    m_purgeTimer->setInterval(60 * 60 * 1000); // hourly
    connect(m_purgeTimer, &QTimer::timeout, this, &AuthManager::purgeExpiredSessions);
    m_purgeTimer->start();
}

QString AuthManager::hashToken(const QString& token)
{
    QByteArray h = QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(
        h.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool AuthManager::constantTimeEquals(const QString& a, const QString& b)
{
    // Compare over the UTF-8 bytes. We fold the length difference into the
    // accumulator so the running time depends only on the longer input, not on
    // where (or whether) the first mismatch occurs.
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

QByteArray AuthManager::generateRandomKey()
{
    QByteArray key(32, '\0');
    for (int i = 0; i < 32; ++i)
        key[i] = static_cast<char>(QRandomGenerator::securelySeeded().bounded(256));
    return key;
}

QString AuthManager::generatePinInternal()
{
    // 6-digit PIN in range [100000, 999999]
    quint64 value = QRandomGenerator::securelySeeded().bounded(100000ULL, 1000000ULL);
    return QString::number(value);
}

QByteArray AuthManager::generateHmac(const QString& data) const
{
    return QMessageAuthenticationCode::hash(data.toUtf8(), m_hmacKey, QCryptographicHash::Sha256);
}

void AuthManager::saveSessions()
{
    if (!m_settings) return;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QString path = dir + QStringLiteral("/sessions.json");

    QJsonArray arr;
    for (const auto& s : m_sessions) {
        // Sessions the visitor asked not to remember never touch the disk, so a
        // restart ends them too — that is half of what "do not remember me" buys.
        if (s.ephemeral) continue;
        arr.append(s.toJson());
    }

    QJsonObject root;
    root["sessions"] = arr;

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    } else {
        Logger::warning("[AuthManager] Failed to write sessions file: " + path);
    }
}

void AuthManager::loadSessions()
{
    if (!m_settings) return;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString path = dir + QStringLiteral("/sessions.json");

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return; // No sessions file yet

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject()) return;

    QJsonObject root = doc.object();
    QJsonArray arr = root["sessions"].toArray();

    int loaded = 0;
    int skipped = 0;
    m_sessions.clear();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // With the door closed — remote admin off, or no password ever set — there
    // is nothing a session could have come through, so a persisted admin flag is
    // stale (or hand-edited). Drop it on the way in.
    const bool keepAdminFlags = remoteAdminEnabled() && adminPasswordSet();

    for (const auto& val : arr) {
        QJsonObject obj = val.toObject();
        SessionInfo info;
        info.token = obj["token"].toString();
        info.ip = obj["ip"].toString();
        info.machineName = obj["machine_name"].toString();
        info.city = obj["city"].toString();
        info.country = obj["country"].toString();
        info.createdAt = static_cast<qint64>(obj["created_at"].toDouble());
        info.lastSeen = obj.contains("last_seen") ? static_cast<qint64>(obj["last_seen"].toDouble())
                                                  : info.createdAt;
        info.isHost = obj["is_host"].toBool(false);
        info.viaTunnel = obj["via_tunnel"].toBool(false);
        info.isAdmin = keepAdminFlags && obj["is_admin"].toBool(false);

        // MW-BIND-v1 pairing key. A stored key that no longer parses is dropped
        // rather than kept: the session survives, and the browser simply
        // re-registers on its next visit.
        const QString storedKey = obj["pairing_key"].toString();
        if (!storedKey.isEmpty()) {
            const QByteArray spki = QByteArray::fromBase64(storedKey.toUtf8());
            if (PairingCrypto::isValidP256Spki(spki)) info.pairingKey = spki;
        }

        // Drop sessions that are already expired by inactivity.
        const qint64 last = info.lastSeen > 0 ? info.lastSeen : info.createdAt;
        if (now - last > SESSION_TTL_SECS) {
            skipped++;
            continue;
        }

        m_sessions[info.token] = info;
        loaded++;
    }

    Logger::info(
        QString("[AuthManager] Loaded %1 sessions (%2 expired, dropped)").arg(loaded).arg(skipped));
}

QString AuthManager::generatePin()
{
    m_currentPin = generatePinInternal();
    m_pinConsumed = false; // fresh PIN, never used
    Logger::info("[Auth] New PIN generated");
    emit pinChanged(m_currentPin);
    return m_currentPin;
}

void AuthManager::regeneratePin()
{
    destroyAllSessions();
    m_rateLimits.clear();
    generatePin();
    Logger::info("[Auth] PIN regenerated, all sessions invalidated");
}

void AuthManager::autoRegeneratePin()
{
    // Generate a new PIN immediately after a successful validation.
    // Does NOT invalidate existing sessions — they remain active.
    m_currentPin = generatePinInternal();
    m_pinConsumed = true; // mark as consumed — admin must generate fresh PIN
    Logger::info("[Auth] PIN auto-regenerated after successful validation");
    emit pinChanged(m_currentPin);
}

void AuthManager::clearPin()
{
    m_currentPin = QStringLiteral("------");
    m_pinConsumed = false;
    Logger::info("[Auth] PIN cleared");
    emit pinChanged(m_currentPin);
}

bool AuthManager::hasValidPin() const
{
    return m_currentPin != QStringLiteral("------") && !m_currentPin.isEmpty();
}

// ── Certificate Authentication ────────────────────────────────────────────────

QString AuthManager::generateCertificateToken()
{
    // Generate 64 random bytes → Base64 = 88 characters (no trailing =)
    QByteArray bytes(64, '\0');
    for (int i = 0; i < 64; ++i)
        bytes[i] = static_cast<char>(QRandomGenerator::securelySeeded().bounded(256));

    QString token = QString::fromLatin1(bytes.toBase64(QByteArray::OmitTrailingEquals));

    // Persist via AppSettings
    if (m_settings) m_settings->setCertificateToken(token);

    Logger::info("[Auth] Certificate token generated (64 bytes)");
    return token;
}

QString AuthManager::certificateToken() const
{
    if (!m_settings) return {};
    return m_settings->certificateToken();
}

bool AuthManager::validateCertificate(const QString& uploadedContent) const
{
    QString stored = certificateToken();
    if (stored.isEmpty()) return false;

    // Trim whitespace from both sides before comparing (constant-time).
    return constantTimeEquals(uploadedContent.trimmed(), stored);
}

bool AuthManager::certAuthEnabled() const
{
    return m_settings && m_settings->certAuthEnabled();
}

void AuthManager::setCertAuthEnabled(bool enabled)
{
    if (m_settings) m_settings->setCertAuthEnabled(enabled);
    Logger::info(
        QString("[Auth] Certificate authentication %1").arg(enabled ? "enabled" : "disabled"));
}

QString AuthManager::cleanClientAddress(const QString& ip)
{
    // Strip IPv4-mapped IPv6 prefix (e.g., "::ffff:192.168.1.5")
    if (ip.startsWith(QStringLiteral("::ffff:"))) {
        return ip.mid(7);
    }
    return ip;
}

QString AuthManager::isPrivateIP(const QString& ip)
{
    QString clean = cleanClientAddress(ip);
    QHostAddress addr(clean);
    if (addr.isLoopback()) return QStringLiteral("Local");

    // Manual RFC 1918 check — QHostAddress has no isPrivate() in Qt 6
    QStringList parts = clean.split('.');
    if (parts.size() == 4) {
        bool ok;
        int a = parts[0].toInt(&ok);
        if (!ok) return QStringLiteral("Remote");
        int b = parts[1].toInt(&ok);
        if (!ok) return QStringLiteral("Remote");
        if (a == 10) return QStringLiteral("Local");
        if (a == 172 && b >= 16 && b <= 31) return QStringLiteral("Local");
        if (a == 192 && b == 168) return QStringLiteral("Local");
    }
    return QStringLiteral("Remote");
}

bool AuthManager::isLanAddress(const QString& ip)
{
    const QHostAddress addr(cleanClientAddress(ip));
    if (addr.isNull()) return false;
    if (addr.isLoopback()) return true;
    // 169.254/16 and fe80::/10 — an address that no router forwards.
    if (addr.isLinkLocal()) return true;

    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v4 = addr.toIPv4Address();
        if ((v4 & 0xFF000000) == 0x0A000000) return true; // 10.0.0.0/8
        if ((v4 & 0xFFF00000) == 0xAC100000) return true; // 172.16.0.0/12
        if ((v4 & 0xFFFF0000) == 0xC0A80000) return true; // 192.168.0.0/16
        return false;
    }
    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        // Unique local addresses (fc00::/7): the IPv6 answer to RFC 1918, and
        // what a v6-only LAN hands out.
        const Q_IPV6ADDR raw = addr.toIPv6Address();
        if ((raw[0] & 0xFE) == 0xFC) return true;
    }
    return false;
}

QString AuthManager::rateLimitKey(const QString& ip)
{
    QString clean = cleanClientAddress(ip);
    QHostAddress addr(clean);
    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        // Collapse to the /64 prefix: keep the first 8 bytes, zero the rest.
        Q_IPV6ADDR raw = addr.toIPv6Address();
        for (int i = 8; i < 16; ++i)
            raw[i] = 0;
        return QHostAddress(raw).toString() + QStringLiteral("/64");
    }
    return clean;
}

void AuthManager::cleanupExpired()
{
    qint64 now = QDateTime::currentSecsSinceEpoch();
    QMutableHashIterator<QString, RateLimitEntry> it(m_rateLimits);
    while (it.hasNext()) {
        it.next();
        RateLimitEntry& e = it.value();

        // If currently locked out, skip
        if (e.lockoutUntilEpoch > now) continue;

        // If last failure was more than 10 minutes ago, decrement
        qint64 secsSinceFailure =
            e.lastFailure.isValid() ? now - e.lastFailure.toSecsSinceEpoch() : 600;

        if (secsSinceFailure >= 600) { // 10 minutes
            e.failures = qMax(0, e.failures - 1);
        }

        // Remove entries with no failures
        if (e.failures <= 0) it.remove();
    }
}

int AuthManager::bucketLockout(const QString& bucket) const
{
    auto it = m_rateLimits.find(bucket);
    if (it == m_rateLimits.end()) return 0;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (it->lockoutUntilEpoch > now) return static_cast<int>(it->lockoutUntilEpoch - now);
    return 0;
}

int AuthManager::bucketRemainingAttempts(const QString& bucket) const
{
    auto it = m_rateLimits.find(bucket);
    if (it == m_rateLimits.end()) return 3;

    const RateLimitEntry& e = it.value();
    if (e.failures < 3)
        return 3 - e.failures;
    else if (e.failures < 5)
        return 5 - e.failures;
    else if (e.failures < 10)
        return 10 - e.failures;
    return 0;
}

AuthManager::ValidateResult AuthManager::recordAttempt(const QString& bucket, bool matched,
                                                       const QString& ip, const QString& what)
{
    auto& entry = m_rateLimits[bucket]; // Creates entry if not exists
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    if (matched) {
        // Success: reset all counters
        entry.failures = 0;
        entry.lockoutUntilEpoch = 0;
        Logger::info(QString("[Auth] %1 validated successfully for %2").arg(what, ip));
        return {Valid, 3, 0};
    }

    entry.failures++;
    entry.lastFailure = QDateTime::currentDateTime();

    // Apply lockout based on failure count
    if (entry.failures >= 10) {
        entry.lockoutUntilEpoch = now + (LOCKOUT_LONG_MS / 1000);
        Logger::warning(QString("[Auth] %1 failed %2 10+ times -- lockout 10min").arg(ip, what));
    } else if (entry.failures >= 5) {
        entry.lockoutUntilEpoch = now + (LOCKOUT_MEDIUM_MS / 1000);
        Logger::warning(QString("[Auth] %1 failed %2 5+ times -- lockout 2min").arg(ip, what));
    } else if (entry.failures >= 3) {
        entry.lockoutUntilEpoch = now + (LOCKOUT_SHORT_MS / 1000);
        Logger::warning(QString("[Auth] %1 failed %2 3+ times -- lockout 30s").arg(ip, what));
    }

    Logger::info(
        QString("[Auth] Invalid %1 from %2 (failure #%3)").arg(what, ip).arg(entry.failures));

    return {InvalidPin, bucketRemainingAttempts(bucket), bucketLockout(bucket)};
}

AuthManager::ValidateResult AuthManager::validatePin(const QString& ip, const QString& pin)
{
    cleanupExpired();

    const QString bucket = rateLimitKey(ip);
    if (const int locked = bucketLockout(bucket); locked > 0) {
        Logger::info(QString("[Auth] Rate limited for %1: %2s remaining").arg(ip).arg(locked));
        return {RateLimited, 0, locked};
    }

    // Compare PIN (constant-time). Reject when no valid PIN has been generated,
    // otherwise a client submitting the "--------" sentinel would authenticate.
    const bool matched = hasValidPin() && constantTimeEquals(pin, m_currentPin);
    return recordAttempt(bucket, matched, ip, QStringLiteral("PIN"));
}

// ── Remote admin password ─────────────────────────────────────────────────────

QString AuthManager::encodePassword(const QString& password)
{
    QByteArray salt(PBKDF2_SALT_BYTES, '\0');
    for (int i = 0; i < salt.size(); ++i)
        salt[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));

    const QByteArray key = QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), salt, PBKDF2_ITERATIONS, PBKDF2_KEY_BYTES);

    return QStringLiteral("pbkdf2-sha256$%1$%2$%3")
        .arg(PBKDF2_ITERATIONS)
        .arg(QString::fromLatin1(salt.toBase64()), QString::fromLatin1(key.toBase64()));
}

bool AuthManager::passwordMatches(const QString& digest, const QString& password)
{
    // "pbkdf2-sha256$<iterations>$<b64 salt>$<b64 key>"
    const QStringList parts = digest.split(QLatin1Char('$'));
    if (parts.size() != 4 || parts[0] != QLatin1String("pbkdf2-sha256")) return false;

    bool ok = false;
    const int iterations = parts[1].toInt(&ok);
    if (!ok || iterations <= 0) return false;

    const QByteArray salt = QByteArray::fromBase64(parts[2].toLatin1());
    const QByteArray expected = QByteArray::fromBase64(parts[3].toLatin1());
    if (salt.isEmpty() || expected.isEmpty()) return false;

    const QByteArray actual = QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), salt, iterations, expected.size());

    // Compare the Base64 forms so the shared constant-time helper applies.
    return constantTimeEquals(QString::fromLatin1(actual.toBase64()), parts[3]);
}

bool AuthManager::remoteAdminEnabled() const
{
    return m_settings && m_settings->remoteAdminEnabled();
}

bool AuthManager::adminPasswordSet() const
{
    return m_settings && !m_settings->adminPasswordDigest().isEmpty();
}

bool AuthManager::setAdminPassword(const QString& password)
{
    if (!m_settings) return false;
    if (password.size() < MIN_ADMIN_PASSWORD_LEN) return false;

    m_settings->setAdminPasswordDigest(encodePassword(password));
    // A password change is also a revocation: machines that unlocked with the
    // old one must present the new one.
    demoteAdminSessions();
    Logger::info("[Auth] Remote admin password set");
    return true;
}

void AuthManager::setRemoteAdminEnabled(bool enabled)
{
    if (!m_settings) return;
    m_settings->setRemoteAdminEnabled(enabled);
    // The door is gone; nobody keeps the access it used to grant.
    if (!enabled) demoteAdminSessions();
    Logger::info(QString("[Auth] Remote administration %1").arg(enabled ? "enabled" : "disabled"));
}

AuthManager::ValidateResult AuthManager::validateAdminPassword(const QString& ip,
                                                               const QString& password)
{
    cleanupExpired();

    // Own counter, so failed unlock attempts never lock a user out of PIN login.
    const QString bucket = QStringLiteral("admin|") + rateLimitKey(ip);
    if (const int locked = bucketLockout(bucket); locked > 0) {
        Logger::info(
            QString("[Auth] Admin unlock rate limited for %1: %2s remaining").arg(ip).arg(locked));
        return {RateLimited, 0, locked};
    }

    // Remote admin off, or no password ever set → nothing can match, but the
    // attempt is still counted: probing for it is exactly what an attacker would
    // do first, and the rate limit is what makes that expensive.
    bool matched = false;
    if (remoteAdminEnabled()) {
        const QString digest = m_settings->adminPasswordDigest();
        matched = !digest.isEmpty() && passwordMatches(digest, password);
    }
    return recordAttempt(bucket, matched, ip, QStringLiteral("admin password"));
}

QString AuthManager::createSession(const QString& ip, const QString& machineName, bool isHost,
                                   bool ephemeral, bool viaTunnel)
{
    // Clean IPv4-mapped IPv6 addresses before storing
    QString cleanIp = cleanClientAddress(ip);

    // Generate unique token: HMAC(ip | counter | timestamp | UUID)
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    quint64 counter = static_cast<quint64>(QRandomGenerator::securelySeeded().bounded(1000000ULL));
    QString uuid = QUuid::createUuid().toString(QUuid::Id128);
    QString data = QString("%1|%2|%3|%4").arg(cleanIp).arg(counter).arg(now).arg(uuid);
    QByteArray hmac = generateHmac(data);
    QString token = QString::fromLatin1(hmac.toBase64(QByteArray::OmitTrailingEquals));

    // Persist only the hash of the token (opaque id). The raw token lives solely
    // in the client cookie, so a leaked sessions.json cannot be replayed.
    const QString id = hashToken(token);

    SessionInfo info;
    info.token = id;
    info.ip = cleanIp;
    info.machineName = machineName.isEmpty() ? QStringLiteral("Unknown") : machineName;
    info.createdAt = QDateTime::currentSecsSinceEpoch();
    info.lastSeen = info.createdAt;
    info.isHost = isHost;
    info.ephemeral = ephemeral;
    info.viaTunnel = viaTunnel;

    m_sessions[id] = info;
    if (!ephemeral) saveSessions();
    emit sessionCreated(ip, machineName);
    emit sessionsChanged();
    Logger::info(QString("[Auth] %1 session created for %2 (machine='%3', total: %4)")
                     .arg(ephemeral ? QStringLiteral("Temporary") : QStringLiteral("Persistent"))
                     .arg(ip, machineName)
                     .arg(m_sessions.size()));
    return token; // raw token for the cookie
}

bool AuthManager::validateSession(const QString& token) const
{
    if (token.isEmpty()) return false;
    auto it = m_sessions.find(hashToken(token));
    if (it == m_sessions.end()) return false;
    // Sliding expiration: reject (but do not mutate, this is const) sessions
    // inactive past the TTL — the periodic purge removes them.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 last = it->lastSeen > 0 ? it->lastSeen : it->createdAt;
    return now - last <= ttlFor(*it);
}

bool AuthManager::isEphemeralSession(const QString& token) const
{
    if (!validateSession(token)) return false;
    auto it = m_sessions.find(hashToken(token));
    return it != m_sessions.end() && it->ephemeral;
}

bool AuthManager::logoutSession(const QString& token)
{
    if (token.isEmpty()) return false;
    const QString id = hashToken(token);
    if (!m_sessions.contains(id)) return false;
    destroySession(id);
    return true;
}

bool AuthManager::isHostSession(const QString& token) const
{
    if (!validateSession(token)) return false;
    auto it = m_sessions.find(hashToken(token));
    return it != m_sessions.end() && it->isHost;
}

bool AuthManager::isTunnelHostSession(const QString& token) const
{
    if (!validateSession(token)) return false;
    auto it = m_sessions.find(hashToken(token));
    return it != m_sessions.end() && it->isHost && it->viaTunnel;
}

bool AuthManager::isAdminSession(const QString& token) const
{
    if (!validateSession(token)) return false;
    auto it = m_sessions.find(hashToken(token));
    return it != m_sessions.end() && it->isAdmin;
}

bool AuthManager::promoteSessionToAdmin(const QString& token)
{
    if (!validateSession(token)) return false;
    auto it = m_sessions.find(hashToken(token));
    if (it == m_sessions.end()) return false;
    if (it->isAdmin) return true;

    it->isAdmin = true;
    saveSessions();
    emit sessionsChanged();
    Logger::info(
        QString("[Auth] Admin access unlocked for %1 (machine='%2')").arg(it->ip, it->machineName));
    return true;
}

int AuthManager::demoteAdminSessions()
{
    int cleared = 0;
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (!it->isAdmin) continue;
        it->isAdmin = false;
        cleared++;
    }
    if (cleared > 0) {
        saveSessions();
        emit sessionsChanged();
        Logger::info(QString("[Auth] Admin access revoked for %1 session(s)").arg(cleared));
    }
    return cleared;
}

void AuthManager::touchSession(const QString& token)
{
    if (token.isEmpty()) return;
    auto it = m_sessions.find(hashToken(token));
    if (it == m_sessions.end()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 prev = it->lastSeen;
    it->lastSeen = now;
    // Throttle disk writes: only persist when the clock advanced by >1h. A
    // temporary session has nothing on disk to refresh.
    if (!it->ephemeral && now - prev >= 3600) saveSessions();
}

bool AuthManager::bindSessionKey(const QString& token, const QByteArray& spkiDer)
{
    if (token.isEmpty()) return false;
    if (!PairingCrypto::isValidP256Spki(spkiDer)) {
        Logger::warning(QStringLiteral("[AuthManager] Rejected an unparseable pairing key"));
        return false;
    }

    auto it = m_sessions.find(hashToken(token));
    if (it == m_sessions.end()) return false;

    if (!it->pairingKey.isEmpty()) {
        // Same key again: a browser re-registering after a reconnect. Fine.
        if (it->pairingKey == spkiDer) return true;
        // A different key on a session that already has one. The legitimate
        // browser cannot do this — its key is non-extractable and stable — so
        // this is either a stolen cookie trying to pair itself, or a bug. Refuse
        // and say so: silently overwriting would hand the session away.
        Logger::warning(
            QStringLiteral(
                "[AuthManager] Refused to replace the pairing key of session %1 — a session keeps "
                "the key it was bound to; revoke it if the device really changed")
                .arg(it->token.left(8)));
        return false;
    }

    it->pairingKey = spkiDer;
    if (!it->ephemeral) saveSessions();
    Logger::info(QStringLiteral("[AuthManager] Bound pairing key %1 to session %2")
                     .arg(PairingCrypto::keyId(spkiDer).left(12), it->token.left(8)));
    return true;
}

QByteArray AuthManager::sessionPairingKey(const QString& token) const
{
    if (token.isEmpty()) return {};
    auto it = m_sessions.constFind(hashToken(token));
    if (it == m_sessions.constEnd()) return {};
    return it->pairingKey;
}

void AuthManager::setSessionGeo(const QString& token, const QString& city, const QString& country)
{
    auto it = m_sessions.find(hashToken(token));
    if (it == m_sessions.end()) return;

    it->city = city;
    it->country = country;
    saveSessions();
    Logger::info(QString("[Auth] Geo data stored for session %1: %2, %3")
                     .arg(token.left(12), city, country));
}

bool AuthManager::updateSessionAddress(const QString& token, const QString& ip)
{
    auto it = m_sessions.find(hashToken(token));
    if (it == m_sessions.end()) return false;

    // Any authenticated reconnection counts as activity (sliding expiration).
    it->lastSeen = QDateTime::currentSecsSinceEpoch();

    QString cleanIp = cleanClientAddress(ip);
    if (cleanIp.isEmpty() || it->ip == cleanIp) return false;

    Logger::info(QString("[Auth] Session IP changed: %1 -> %2").arg(it->ip, cleanIp));
    it->ip = cleanIp;
    // Drop stale geolocation; the caller re-runs the lookup for the new IP.
    it->city.clear();
    it->country.clear();
    saveSessions();
    emit sessionsChanged();
    return true;
}

void AuthManager::setSessionStreaming(const QString& token, bool streaming)
{
    if (token.isEmpty()) return; // localhost streams have no session row to flag

    const QString id = hashToken(token);
    bool changed = false;

    // Enforce a single active stream: clear any other session's flag.
    if (streaming) {
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
            if (it.key() != id && it->streaming) {
                it->streaming = false;
                changed = true;
            }
        }
    }

    auto it = m_sessions.find(id);
    if (it != m_sessions.end() && it->streaming != streaming) {
        it->streaming = streaming;
        changed = true;
    }

    if (changed) {
        saveSessions();
        emit sessionsChanged();
    }
}

void AuthManager::destroySession(const QString& token)
{
    if (m_sessions.contains(token)) {
        SessionInfo info = m_sessions.value(token);
        m_sessions.remove(token);
        saveSessions();
        emit sessionDestroyed(token);
        emit sessionsChanged();
        Logger::info(QString("[Auth] Session destroyed for %1 (machine='%2', remaining: %3)")
                         .arg(info.ip, info.machineName)
                         .arg(m_sessions.size()));
        if (info.streaming) {
            Logger::info("[Auth] Revoked session was streaming — requesting stream teardown");
            emit streamingSessionRevoked();
        }
    } else {
        Logger::warning(QString("[Auth] destroySession: token not found — token='%1' (len=%2)")
                            .arg(token)
                            .arg(token.size()));
    }
}

bool AuthManager::renameSession(const QString& token, const QString& machineName)
{
    // The id exposed to the admin UI is already the token hash (the map key).
    auto it = m_sessions.find(token);
    if (it == m_sessions.end()) {
        Logger::warning(QString("[Auth] renameSession: token not found — token='%1'").arg(token));
        return false;
    }

    it->machineName = machineName;
    saveSessions();
    emit sessionsChanged();
    Logger::info(QString("[Auth] Session renamed to '%1' for %2").arg(machineName, it->ip));
    return true;
}

void AuthManager::destroyAllSessions()
{
    int count = m_sessions.size();
    bool anyStreaming = false;
    for (const SessionInfo& s : m_sessions) {
        if (s.streaming) {
            anyStreaming = true;
            break;
        }
    }
    m_sessions.clear();
    saveSessions();
    if (count > 0) {
        emit sessionsChanged();
        Logger::info(QString("[Auth] All %1 sessions destroyed").arg(count));
    }
    if (anyStreaming) {
        Logger::info("[Auth] A destroyed session was streaming — requesting stream teardown");
        emit streamingSessionRevoked();
    }
}

void AuthManager::purgeExpiredSessions()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    int removed = 0;
    QMutableHashIterator<QString, SessionInfo> it(m_sessions);
    while (it.hasNext()) {
        it.next();
        const qint64 last = it.value().lastSeen > 0 ? it.value().lastSeen : it.value().createdAt;
        if (now - last > ttlFor(it.value())) {
            it.remove();
            removed++;
        }
    }
    if (removed > 0) {
        saveSessions();
        emit sessionsChanged();
        Logger::info(QString("[Auth] Purged %1 expired session(s)").arg(removed));
    }
}

QList<SessionInfo> AuthManager::sessions() const
{
    // Deterministic order, newest first. QHash iteration order is arbitrary and
    // — worse — changes on every rehash, so an unsorted list made the admin
    // table reshuffle between two polls: rows moved under the pointer and a
    // click landed on another device's Revoke button. Sorting here rather than
    // in the UI keeps every consumer (and the tests) on the same order.
    QList<SessionInfo> list = m_sessions.values();
    std::sort(list.begin(), list.end(), [](const SessionInfo& a, const SessionInfo& b) {
        if (a.createdAt != b.createdAt) return a.createdAt > b.createdAt;
        return a.token < b.token; // stable tie-break for same-second creations
    });
    return list;
}

QString AuthManager::sessionIdForToken(const QString& token) const
{
    if (token.isEmpty()) return {};
    const QString id = hashToken(token);
    return m_sessions.contains(id) ? id : QString();
}

int AuthManager::remainingAttempts(const QString& ip) const
{
    return bucketRemainingAttempts(rateLimitKey(ip));
}

int AuthManager::lockoutSeconds(const QString& ip) const
{
    return bucketLockout(rateLimitKey(ip));
}

bool AuthManager::isRateLimited(const QString& ip) const
{
    return lockoutSeconds(ip) > 0;
}

int AuthManager::failedAttemptCount(const QString& ip) const
{
    auto it = m_rateLimits.find(rateLimitKey(ip));
    if (it == m_rateLimits.end()) return 0;
    return it.value().failures;
}
