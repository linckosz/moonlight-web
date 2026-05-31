#pragma once

#include <QObject>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <functional>

/**
 * Asynchronous IP geolocation service using ip-api.com (free tier).
 *
 * Results are cached in-memory with a 24h TTL.
 * Free tier: 45 req/min, no API key required.
 * Returns {"city":"Paris","country":"France"} or empty strings on failure.
 */
class GeoIpService : public QObject
{
    Q_OBJECT

public:
    using GeoCallback = std::function<void(const QString& city, const QString& country)>;

    explicit GeoIpService(QObject* parent = nullptr);
    ~GeoIpService() override = default;

    /**
     * Look up geographic info for an IP address.
     * Calls the callback with (city, country) — both empty on failure/error.
     * Results are cached per IP for 24 hours.
     */
    void lookupIp(const QString& ip, GeoCallback callback);

    /** Synchronous cache access — returns (city, country) or ("","") if not cached. */
    QPair<QString, QString> cachedLocation(const QString& ip) const;

    /** Clear the lookup cache. */
    void clearCache();

private:
    QNetworkAccessManager* m_nam;
    QHash<QString, QPair<QString, QString>> m_cache; // ip -> (city, country)
    QHash<QString, QList<GeoCallback>> m_pending;     // ip -> pending callbacks

    static constexpr int CACHE_TTL_MS = 86400000;  // 24 hours
};
