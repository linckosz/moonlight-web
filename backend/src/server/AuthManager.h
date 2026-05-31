#pragma once

#include <QObject>
#include <QHash>
#include <QDateTime>
#include <QByteArray>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>

/**
 * Session metadata stored server-side keyed by token.
 */
struct SessionInfo {
    QString token;
    QString ip;
    QString machineName;
    QString city;
    QString country;
    qint64 createdAt;  // unix timestamp (secs)

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["token"] = token;
        obj["ip"] = ip;
        obj["machine_name"] = machineName;
        obj["city"] = city;
        obj["country"] = country;
        obj["created_at"] = createdAt;
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
    void destroySession(const QString& token);
    void destroyAllSessions();

    /** Store geolocation data for a session after async lookup completes. */
    void setSessionGeo(const QString& token, const QString& city, const QString& country);

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
    QHash<QString, RateLimitEntry> m_rateLimits; // ip -> entry
    QHash<QString, SessionInfo> m_sessions;      // token -> SessionInfo

    QString generatePinInternal();
    QByteArray generateHmac(const QString& data) const;
    void cleanupExpired();
    static QByteArray generateRandomKey();

    static constexpr int MAX_LOCKOUT_FAILURES = 3;
    static constexpr int LOCKOUT_SHORT_MS = 30000;
    static constexpr int LOCKOUT_MEDIUM_MS = 120000;
    static constexpr int LOCKOUT_LONG_MS = 600000;
};
