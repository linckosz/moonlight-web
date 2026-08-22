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

#include <QObject>
#include <QHash>
#include <QDateTime>
#include <QByteArray>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>

class QTimer;

/**
 * Session metadata stored server-side keyed by token.
 */
struct SessionInfo
{
    QString token; // stored value is the SHA-256 of the cookie token (opaque id),
                   // never the raw token — a stolen sessions.json cannot be replayed
    QString ip;
    QString machineName;
    QString city;
    QString country;
    qint64 createdAt = 0;   // unix timestamp (secs)
    qint64 lastSeen = 0;    // unix secs; bumped on activity (sliding expiration)
    bool streaming = false; // runtime-only: true while this session has an active stream
    bool isHost = false;    // created by redeeming the host key: this browser runs on
                            // the host machine itself (via the public domain) and is
                            // granted localhost-equivalent (admin) access
    bool isAdmin = false;   // unlocked admin access from the LAN with the remote
                            // admin password; same privileges as isHost, but this
                            // browser is on another machine
    bool ephemeral = false; // the visitor unchecked "remember me" at login: this
                            // session is never written to sessions.json and dies
                            // after EPHEMERAL_SESSION_TTL_SECS of inactivity
                            // instead of the usual 90 days

    // MW-BIND-v1: the SPKI (DER) public key this browser signs its half of the
    // WebRTC handshake with, so a relaying introduction server cannot swap the
    // DTLS fingerprints and put itself in the middle of the stream.
    //
    // It lives here, inside the session, on purpose: a pairing *is* a session,
    // so revoking the session in the admin page revokes the key with it, and
    // there is no second lifetime to keep in sync. Public key — nothing here is
    // secret, and it never leaves the machine anyway.
    QByteArray pairingKey;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["token"] = token;
        if (!pairingKey.isEmpty()) obj["pairing_key"] = QString::fromLatin1(pairingKey.toBase64());
        obj["ip"] = ip;
        obj["machine_name"] = machineName;
        obj["city"] = city;
        obj["country"] = country;
        obj["created_at"] = createdAt;
        obj["last_seen"] = lastSeen;
        obj["streaming"] = streaming;
        obj["is_host"] = isHost;
        obj["is_admin"] = isAdmin;
        obj["ephemeral"] = ephemeral;
        return obj;
    }
};

class AppSettings;

class AuthManager : public QObject
{
    Q_OBJECT

public:
    /// @param settings Optional AppSettings for HMAC key persistence.
    ///                 If null, a random HMAC key is generated each time.
    explicit AuthManager(AppSettings* settings = nullptr, QObject* parent = nullptr);
    ~AuthManager() override = default;

    // ── PIN management ─────────────────────────────────────────────────────
    QString generatePin();
    QString currentPin() const { return m_currentPin; }
    bool hasValidPin() const; // true if a real PIN has been generated
    void clearPin();          // reset PIN to "--------" (invalid)
    void regeneratePin();     // generates new PIN + invalidates all sessions

    // ── Validation ─────────────────────────────────────────────────────────
    enum Result
    {
        Valid,
        InvalidPin,
        RateLimited
    };

    struct ValidateResult
    {
        Result result = InvalidPin; // fail-closed default
        int remainingAttempts = 0;
        int lockoutSeconds = 0;
    };

    ValidateResult validatePin(const QString& ip, const QString& pin);

    // ── Remote admin password ──────────────────────────────────────────────
    // Second door into admin access, for a LAN machine that is not the host.
    // Unlike the PIN this one is persistent and reusable: it is a password, not
    // a one-shot pairing code, so it is stored as a PBKDF2 digest and shares
    // the PIN's lockout tiers on its own per-IP counter (a wrong password must
    // not lock a legitimate user out of PIN login, and vice versa).
    //
    // There is deliberately NO built-in default password. A documented default
    // is public knowledge, and while the unlock only ever promotes a session
    // that already passed the PIN, that still means "whoever can stream can also
    // administer" on every install whose owner never changed it. So the door
    // starts closed and stays closed until a password is set:
    //   - with a desktop, from the admin page, which the owner can always open;
    //   - headless, from the terminal (`moonlightweb --set-admin-password`),
    //     which the installer prompts for since there is no page to open.
    // Until then validateAdminPassword() matches nothing at all.

    /// True when a remote admin password has been set. While false the LAN
    /// unlock door is shut: nothing can satisfy validateAdminPassword().
    bool adminPasswordSet() const;
    /// Replace the password. Rejects anything shorter than
    /// MIN_ADMIN_PASSWORD_LEN (including the empty string: use
    /// setRemoteAdminEnabled(false) to close the door).
    bool setAdminPassword(const QString& password);
    /// Enable or disable remote administration. Disabling revokes every unlock.
    void setRemoteAdminEnabled(bool enabled);
    /// Whether remote administration is enabled (mirrors AppSettings).
    bool remoteAdminEnabled() const;
    /// Rate-limited verification. Always InvalidPin when remote admin is off or
    /// no password has been set.
    ValidateResult validateAdminPassword(const QString& ip, const QString& password);

    static constexpr int MIN_ADMIN_PASSWORD_LEN = 8;

    // ── Session management ─────────────────────────────────────────────────
    /// @param ephemeral The visitor declined "remember me": keep the session out
    ///                  of sessions.json and expire it after
    ///                  EPHEMERAL_SESSION_TTL_SECS of inactivity. Meant for a
    ///                  browser that is not the visitor's own (public machine).
    QString createSession(const QString& ip, const QString& machineName = QString(),
                          bool isHost = false, bool ephemeral = false);
    bool validateSession(const QString& token) const;
    /** True when the raw cookie token belongs to a valid session created without
     *  "remember me". The caller must then keep the cookie session-scoped: a
     *  Max-Age on the refresh would silently promote it back to 90 days. */
    bool isEphemeralSession(const QString& token) const;
    /** Destroy the session a raw cookie token belongs to — the visitor logging
     *  themselves out. Unlike destroySession() this takes the raw token, so a
     *  browser can only ever end its own session. Returns false when the token
     *  matches none. */
    bool logoutSession(const QString& token);
    /** True when the raw cookie token belongs to a valid *host* session (created
     *  by redeeming the host key — the browser runs on the host machine). */
    bool isHostSession(const QString& token) const;
    /** True when the raw cookie token belongs to a session that unlocked admin
     *  access with the remote admin password. Orthogonal to isHostSession:
     *  same privileges, different proof. */
    bool isAdminSession(const QString& token) const;
    /** Grant admin access to an existing session (after the password checked
     *  out). Takes the raw cookie token. Returns false for unknown sessions. */
    bool promoteSessionToAdmin(const QString& token);
    /** Drop the admin flag from every password-unlocked session. Used when the
     *  password is changed or removed, so the old secret stops granting access
     *  to the machines that already used it. Returns the number cleared. */
    int demoteAdminSessions();
    /** Bump a session's lastSeen on activity (sliding expiration). Takes the raw
     *  cookie token. No-op for unknown/empty tokens. */
    void touchSession(const QString& token);

    // ── MW-BIND-v1 pairing keys ────────────────────────────────────────────
    // See docs/design/pairing-signature.md. The key is bound to a session, so
    // destroySession() revokes it and no separate revocation path exists.

    /** Bind @p spkiDer (a browser's P-256 public key, DER) to the session the
     *  raw cookie @p token belongs to.
     *
     *  Returns false for an unknown session, an unparseable key, or an attempt
     *  to replace a key that is already bound to a *different* value. That last
     *  rule is what keeps a stolen cookie from being upgraded into a pairing:
     *  whoever holds the cookie could otherwise swap in their own key and then
     *  satisfy every signature check that follows. Re-binding the same key is
     *  allowed and idempotent, so a browser replaying its registration after a
     *  reconnect is not treated as an attack. */
    bool bindSessionKey(const QString& token, const QByteArray& spkiDer);

    /** The SPKI public key bound to the session a raw cookie @p token belongs
     *  to, or empty when there is none (or no such session). */
    QByteArray sessionPairingKey(const QString& token) const;
    /** Revoke a session by its opaque id (the value exposed in sessions()/toJson,
     *  i.e. the token hash), NOT the raw cookie token. */
    void destroySession(const QString& token);
    /** Rename a session by its opaque id (the token hash exposed in toJson),
     *  NOT the raw cookie token. No-op for unknown ids. Returns true on success. */
    bool renameSession(const QString& token, const QString& machineName);
    void destroyAllSessions();
    /** Drop sessions inactive beyond SESSION_TTL_SECS. Called periodically. */
    void purgeExpiredSessions();

    /** Store geolocation data for a session after async lookup completes. */
    void setSessionGeo(const QString& token, const QString& city, const QString& country);

    /** Refresh a session's source IP on reconnection. If the IP changed since
     *  the session was created (or last seen), it is updated and the stale
     *  geolocation is cleared. Returns true when the IP changed so the caller
     *  can re-run the async geolocation lookup. */
    bool updateSessionAddress(const QString& token, const QString& ip);

    /** Flag a session as actively streaming (or not). When set true, any other
     *  session's streaming flag is cleared first (single active stream). */
    void setSessionStreaming(const QString& token, bool streaming);

    /**
     * Returns detailed session info for all active sessions, newest first.
     * Used by the admin UI to display the sessions table — the order must not
     * depend on the hash layout, or rows shuffle between two refreshes.
     */
    QList<SessionInfo> sessions() const;

    /** The opaque id (the value carried in sessions()/toJson and expected by
     *  destroySession/renameSession) of the session a raw cookie token belongs
     *  to, or an empty string when the token matches none. Lets the admin UI
     *  point at the row that is the caller's own device. */
    QString sessionIdForToken(const QString& token) const;

    /** Returns true if the PIN was auto-regenerated after being consumed
     *  (i.e. a remote client validated it). When true, the admin UI should
     *  display "--------" to force explicit manual generation. */
    bool isPinConsumed() const { return m_pinConsumed; }

    // ── Auto-regenerate ─────────────────────────────────────────────────────
    /** Called after a successful PIN validation to auto-generate a new PIN. */
    void autoRegeneratePin();

    // ── Certificate Authentication ──────────────────────────────────────────

    /** Generate a 64-byte random certificate token and persist it via AppSettings.
     *  Returns the token as a Base64 string. */
    QString generateCertificateToken();

    /** Return the stored certificate token (from AppSettings), or empty if not set. */
    QString certificateToken() const;

    /** Validate an uploaded certificate against the stored token.
     *  Returns true if the content matches the stored token. */
    bool validateCertificate(const QString& uploadedContent) const;

    /** Whether certificate authentication is enabled. */
    bool certAuthEnabled() const;

    /** Enable or disable certificate authentication. */
    void setCertAuthEnabled(bool enabled);

    // ── Rate limit info ────────────────────────────────────────────────────
    int remainingAttempts(const QString& ip) const;
    int lockoutSeconds(const QString& ip) const;
    bool isRateLimited(const QString& ip) const;

    // ── Address helpers ────────────────────────────────────────────────────
    /** Strip IPv4-mapped IPv6 prefix (e.g. "::ffff:192.168.1.5" -> "192.168.1.5") */
    static QString cleanClientAddress(const QString& ip);
    /** Returns "Local" for private IPs (10.x, 172.16-31.x, 192.168.x, 127.x, ::1), else "Remote" */
    static QString isPrivateIP(const QString& ip);
    /** True when @p ip can only be reached from this LAN: loopback, an RFC 1918
     *  range, a link-local address, or an IPv6 unique-local address (fc00::/7).
     *  Unlike isPrivateIP this understands IPv6, which matters because it gates
     *  the admin password unlock. */
    static bool isLanAddress(const QString& ip);
    /** Rate-limit bucket key for an address: the raw IPv4, or the /64 prefix for
     *  IPv6 (a single client trivially owns a whole /64, so per-/128 buckets are
     *  pointless against guessing). */
    static QString rateLimitKey(const QString& ip);

    // ── Session persistence ───────────────────────────────────────────────────
    /** Save active sessions to disk (app data directory). */
    void saveSessions();
    /** Load active sessions from disk, discarding expired (>24h). */
    void loadSessions();

    // ── Stats ──────────────────────────────────────────────────────────────
    int activeSessionCount() const { return m_sessions.size(); }
    int failedAttemptCount(const QString& ip) const;

signals:
    void pinChanged(const QString& newPin);
    void sessionCreated(const QString& ip, const QString& machineName);
    void sessionDestroyed(const QString& token);
    /** Emitted when a destroyed session was actively streaming. The stream
     *  lifecycle owner (main.cpp) must tear the live relay down so a revoked
     *  device stops receiving the stream immediately. */
    void streamingSessionRevoked();
    /** Emitted when session list changes (created or destroyed) */
    void sessionsChanged();

private:
    struct RateLimitEntry
    {
        int failures = 0;
        QDateTime lastFailure;
        long long lockoutUntilEpoch = 0;
    };

    AppSettings* m_settings = nullptr;
    QString m_currentPin;
    bool m_pinConsumed = false;
    QByteArray m_hmacKey;
    QHash<QString, RateLimitEntry> m_rateLimits; // rate-limit key -> entry
    QHash<QString, SessionInfo> m_sessions;      // token hash (id) -> SessionInfo
    QTimer* m_purgeTimer = nullptr;

    QString generatePinInternal();
    QByteArray generateHmac(const QString& data) const;
    void cleanupExpired();
    static QByteArray generateRandomKey();

    // ── Rate limiting, shared by the PIN and the admin password ────────────
    // Each secret gets its own bucket key so a burst against one never locks
    // the other out. @p bucket is a key into m_rateLimits, not an address.

    /// Seconds of lockout remaining on @p bucket; 0 when an attempt is allowed.
    int bucketLockout(const QString& bucket) const;
    /// Record the outcome of an attempt and build the caller's reply. @p what
    /// names the secret in the log line ("PIN", "admin password").
    ValidateResult recordAttempt(const QString& bucket, bool matched, const QString& ip,
                                 const QString& what);
    /// Attempts left before the next lockout tier on @p bucket.
    int bucketRemainingAttempts(const QString& bucket) const;

    /// PBKDF2-SHA256 digest of @p password with a fresh random salt, encoded as
    /// "pbkdf2-sha256$<iterations>$<b64 salt>$<b64 key>".
    static QString encodePassword(const QString& password);
    /// Constant-time check of @p password against an encodePassword() string.
    static bool passwordMatches(const QString& digest, const QString& password);

    static constexpr int PBKDF2_ITERATIONS = 210000; // OWASP 2023 floor for SHA-256
    static constexpr int PBKDF2_SALT_BYTES = 16;
    static constexpr int PBKDF2_KEY_BYTES = 32;
    /// SHA-256 (base64url) of a raw session token — the value stored/looked up.
    static QString hashToken(const QString& token);
    /// Length-independent, constant-time string comparison (anti timing-attack).
    static bool constantTimeEquals(const QString& a, const QString& b);

    static constexpr int MAX_LOCKOUT_FAILURES = 3;
    static constexpr int LOCKOUT_SHORT_MS = 30000;
    static constexpr int LOCKOUT_MEDIUM_MS = 120000;
    static constexpr int LOCKOUT_LONG_MS = 600000;

    // Sliding session lifetime: a session inactive for this long is purged and the
    // user must re-enter a PIN. Activity (any authenticated request that touches
    // the session) resets the clock, so active users are never prompted again.
    static constexpr qint64 SESSION_TTL_SECS = 90LL * 24 * 3600; // 90 days

    // Same sliding rule, much shorter, for a session the visitor asked not to be
    // remembered — typically a browser they do not own. The cookie is already
    // session-scoped (it dies with the browser process), but a public machine is
    // rarely closed: this is what actually ends an abandoned session there.
    static constexpr qint64 EPHEMERAL_SESSION_TTL_SECS = 8LL * 3600; // 8 hours

    /// Inactivity budget of a session, which depends on whether it is remembered.
    static constexpr qint64 ttlFor(const SessionInfo& s)
    {
        return s.ephemeral ? EPHEMERAL_SESSION_TTL_SECS : SESSION_TTL_SECS;
    }
};
