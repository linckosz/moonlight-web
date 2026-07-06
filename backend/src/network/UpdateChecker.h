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

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

/**
 * Checks the GitHub Releases of the MoonlightWeb repo for a newer build and
 * resolves the exact installer asset for THIS machine's OS/arch, so the UI can
 * offer a one-click download of the right file (no generic "releases page").
 *
 * The result is cached: statusJson() returns the last known result immediately
 * and only triggers a background fetch when the cache is stale (kCacheHours).
 * This keeps us well under GitHub's 60 req/h unauthenticated rate limit — a
 * single host polling its own version costs almost nothing.
 *
 * Payload is public info only (version strings + GitHub URLs), so the route is
 * not localhost-gated: a remote session may legitimately see that an update is
 * available on the host.
 */
class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QString currentVersion, QObject* parent = nullptr);

    // Cached result as JSON:
    //   { current, latest, update_available, download_url, release_url,
    //     asset_name, checked_at, error }
    // Kicks a background refresh when the cache is older than kCacheHours.
    QJsonObject statusJson();

    // Force an immediate refresh, ignoring the cache window.
    void refresh();

private:
    void doFetch();
    void applyResult(const QJsonObject& release);

    // Numeric dotted-version compare ("1.2.10" > "1.2.9"). Tolerant of a leading
    // 'v' and trailing pre-release suffixes; returns false if unparseable so a
    // bad tag never nags the user.
    static bool isNewer(const QString& latest, const QString& current);

    // Best asset for this OS/arch from a release's `assets` array. Fills outName
    // with the matched asset filename; returns its browser_download_url (empty
    // when nothing matches — caller falls back to the release page).
    static QString pickAsset(const class QJsonArray& assets, QString& outName);

    static constexpr int kCacheHours = 6;

    QString m_current;
    QNetworkAccessManager* m_nam;
    QJsonObject m_result;  // last computed result (empty until first fetch)
    QDateTime m_lastCheck; // when m_result was last refreshed (invalid = never)
    bool m_inFlight = false;
};
