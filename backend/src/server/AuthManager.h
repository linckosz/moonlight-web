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
struct SessionInfo {
    QString token;  // stored value is the SHA-256 of the cookie token (opaque id),
                    // never the raw token — a stolen sessions.json cannot be replayed
    QString ip;
    QString machineName;
    QString city;
    QString country;
    qint64 createdAt;       // unix timestamp (secs)
    qint64 lastSeen = 0;    // unix secs; bumped on activity (sliding expiration)
    bool streaming = false;  // runtime-only: true while this session has an active stream

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["token"] = token;
        obj["ip"] = ip;
        obj["machine_name"] = machineName;
        obj["city"] = city;
        obj["country"] = country;
        obj["created_at"] = createdAt;
        obj["last_seen"] = lastSeen;
        obj["streaming"] = streaming;
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
    bool hasValidPin() const;  // true if a real PIN has been generated
    void clearPin();           // reset PIN to "--------" (invalid)
    void regeneratePin();  // generates new PIN + invalidates all sessions

    // ── Validation ─────────────────────────────────────────────────────────
    enum Result { Valid, InvalidPin, RateLimited };

    struct ValidateResult {
        Result result;
        int remainingAttempts = 0;
        int lockoutSeconds = 0;
    };

    ValidateResult validatePin(const QString& ip, const QString& pin);

    // ── Session management ─────────────────────────────────────────────────
    QString createSession(const QString& ip, const QString& machineName = QString());
    bool validateSession(const QString& token) const;
    /** Bump a session's lastSeen on activity (sliding expiration). Takes the raw
     *  cookie token. No-op for unknown/empty tokens. */
    void touchSession(const QString& token);
    /** Revoke a session by its opaque id (the value exposed in sessions()/toJson,
     *  i.e. the token hash), NOT the raw cookie token. */
    void destroySession(const QString& token);
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
     * Returns detailed session info for all active sessions.
     * Used by the admin UI to display the sessions table.
     */
    QList<SessionInfo> sessions() const;

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
    /** Emitted when session list changes (created or destroyed) */
    void sessionsChanged();

private:
    struct RateLimitEntry {
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
    static constexpr qint64 SESSION_TTL_SECS = 90LL * 24 * 3600;  // 90 days
};
