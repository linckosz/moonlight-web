/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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
    QHash<QString, QList<GeoCallback>> m_pending;    // ip -> pending callbacks

    static constexpr int CACHE_TTL_MS = 86400000; // 24 hours
};
