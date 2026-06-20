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

#include "GeoIpService.h"
#include "common/Logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

GeoIpService::GeoIpService(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void GeoIpService::lookupIp(const QString& ip, GeoCallback callback)
{
    if (!callback) return;

    // Skip local/private IPs — no geolocation possible
    if (ip.isEmpty() || ip == "127.0.0.1" || ip == "::1"
        || ip.startsWith("192.168.") || ip.startsWith("10.")
        || ip.startsWith("172.16.") || ip.startsWith("172.17.")
        || ip.startsWith("172.18.") || ip.startsWith("172.19.")
        || ip.startsWith("172.20.") || ip.startsWith("172.21.")
        || ip.startsWith("172.22.") || ip.startsWith("172.23.")
        || ip.startsWith("172.24.") || ip.startsWith("172.25.")
        || ip.startsWith("172.26.") || ip.startsWith("172.27.")
        || ip.startsWith("172.28.") || ip.startsWith("172.29.")
        || ip.startsWith("172.30.") || ip.startsWith("172.31.")
        || ip.startsWith("169.254.")) {
        callback(QString(), QString());
        return;
    }

    // Check cache
    auto it = m_cache.find(ip);
    if (it != m_cache.end()) {
        callback(it->first, it->second);
        return;
    }

    // Check if there's already a pending request for this IP
    auto pendingIt = m_pending.find(ip);
    if (pendingIt != m_pending.end()) {
        pendingIt->append(callback);
        return;
    }

    // Start new request
    m_pending[ip].append(callback);

    // HTTPS provider (ipwho.is): free, no API key, and crucially encrypted so a
    // network observer cannot harvest the client IPs we look up or tamper with
    // the geo result. Returns {success, city, country}.
    QUrl url(QStringLiteral("https://ipwho.is/%1").arg(ip));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("success,city,country"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "Moonlight-Web/1.0");
    req.setTransferTimeout(5000);  // 5s timeout

    QNetworkReply* reply = m_nam->get(req);

    connect(reply, &QNetworkReply::finished, this, [this, ip, reply]() {
        reply->deleteLater();

        QString city;
        QString country;

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray data = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                if (obj.value("success").toBool()) {
                    city = obj.value("city").toString();
                    country = obj.value("country").toString();
                }
            }
        } else {
            Logger::warning(QString("[GeoIp] Failed to lookup %1: %2")
                .arg(ip, reply->errorString()));
        }

        // Cache result (even empty — avoids re-fetching unreachable IPs)
        m_cache[ip] = qMakePair(city, country);

        // Fire all pending callbacks for this IP
        auto pendingIt = m_pending.find(ip);
        if (pendingIt != m_pending.end()) {
            for (const auto& cb : pendingIt.value()) {
                if (cb) cb(city, country);
            }
            m_pending.erase(pendingIt);
        }
    });
}

QPair<QString, QString> GeoIpService::cachedLocation(const QString& ip) const
{
    auto it = m_cache.find(ip);
    if (it != m_cache.end())
        return *it;
    return {};
}

void GeoIpService::clearCache()
{
    m_cache.clear();
    Logger::info("[GeoIp] Cache cleared");
}
