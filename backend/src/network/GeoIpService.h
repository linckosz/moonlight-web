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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <functional>

class AppSettings;

/**
 * Asynchronous IP geolocation service (ipwho.is), used to label a session in
 * the admin list with a city and a country.
 *
 * Results are cached in-memory with a 24h TTL. No API key required.
 * Returns {"city":"Paris","country":"France"} or empty strings on failure.
 *
 * Off unless `session_location_enabled` is set. The address handed to the
 * provider is the *visitor's*, not the owner's: an invited player's public IP
 * would be sent to a company they have never heard of, so that the person who
 * invited them can read a city name. Nobody in that exchange has agreed to it,
 * which is why the answer here is no until someone says otherwise — and why the
 * flag is read on every lookup rather than latched at startup, so revoking it
 * takes effect without a restart.
 */
class GeoIpService : public QObject
{
    Q_OBJECT

public:
    using GeoCallback = std::function<void(const QString& city, const QString& country)>;

    /// @param settings consulted on every lookup; nullptr disables lookups
    ///        entirely, which is what a caller with nothing to consult means.
    explicit GeoIpService(AppSettings* settings, QObject* parent = nullptr);
    ~GeoIpService() override = default;

    /**
     * Look up geographic info for an IP address.
     * Calls the callback with (city, country) — both empty on failure/error.
     * Results are cached per IP for 24 hours.
     */
    void lookupIp(const QString& ip, const GeoCallback& callback);

    /** Synchronous cache access — returns (city, country) or ("","") if not cached. */
    QPair<QString, QString> cachedLocation(const QString& ip) const;

    /** Clear the lookup cache. */
    void clearCache();

private:
    AppSettings* m_settings;
    QNetworkAccessManager* m_nam;
    QHash<QString, QPair<QString, QString>> m_cache; // ip -> (city, country)
    QHash<QString, QList<GeoCallback>> m_pending;    // ip -> pending callbacks

    static constexpr int CACHE_TTL_MS = 86400000; // 24 hours
};
