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

#include "UpdateChecker.h"
#include "common/Logger.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QUrl>

namespace {

// The public MoonlightWeb repository whose Releases carry the installers built
// by .github/workflows/release.yml. Kept as a single source of truth here.
const char* kReleasesApi = "https://api.github.com/repos/linckosz/moonlight-web/releases/latest";

// arm64 vs x64 — CI names installers with these tokens. QSysInfo reports e.g.
// "arm64" / "x86_64" for the running process.
bool isArm64()
{
    const QString a = QSysInfo::currentCpuArchitecture();
    return a.contains(QLatin1String("arm"), Qt::CaseInsensitive);
}

} // namespace

UpdateChecker::UpdateChecker(QString currentVersion, QObject* parent)
    : QObject(parent)
    , m_current(std::move(currentVersion))
    , m_nam(new QNetworkAccessManager(this))
{}

QJsonObject UpdateChecker::statusJson()
{
    // Serve the cache; refresh in the background when stale (never blocks the
    // HTTP handler — the first caller just gets update_available=false).
    const bool stale = !m_lastCheck.isValid() ||
                       m_lastCheck.secsTo(QDateTime::currentDateTimeUtc()) > kCacheHours * 3600;
    if (stale && !m_inFlight) doFetch();

    if (m_result.isEmpty()) {
        // No result yet (first call / fetch in flight): report the current
        // version with no update so the UI can render silently.
        QJsonObject obj;
        obj["current"] = m_current;
        obj["update_available"] = false;
        obj["checked_at"] = m_lastCheck.isValid() ? m_lastCheck.toString(Qt::ISODate) : QString();
        return obj;
    }
    return m_result;
}

void UpdateChecker::refresh()
{
    if (!m_inFlight) doFetch();
}

void UpdateChecker::doFetch()
{
    m_inFlight = true;

    QNetworkRequest req{QUrl(QString::fromLatin1(kReleasesApi))};
    // GitHub rejects requests without a User-Agent; the Accept header pins the
    // stable v3 media type.
    req.setRawHeader("User-Agent", "MoonlightWeb-UpdateChecker");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(8000);

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_inFlight = false;
        m_lastCheck = QDateTime::currentDateTimeUtc();

        if (reply->error() != QNetworkReply::NoError) {
            Logger::warning(QStringLiteral("[Update] check failed: %1").arg(reply->errorString()));
            // Keep any previous good result; on the very first failure, record
            // the error so the UI can stay silent but debuggable.
            if (m_result.isEmpty()) {
                m_result["current"] = m_current;
                m_result["update_available"] = false;
                m_result["error"] = reply->errorString();
                m_result["checked_at"] = m_lastCheck.toString(Qt::ISODate);
            }
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            Logger::warning(QStringLiteral("[Update] check: unexpected response"));
            return;
        }
        applyResult(doc.object());
    });
}

void UpdateChecker::applyResult(const QJsonObject& release)
{
    const QString tag = release.value("tag_name").toString();
    QString latest = tag;
    if (latest.startsWith('v') || latest.startsWith('V')) latest = latest.mid(1);

    const bool available = isNewer(latest, m_current);

    QString assetName;
    QString downloadUrl = pickAsset(release.value("assets").toArray(), assetName);
    const QString releaseUrl = release.value("html_url").toString();
    // No matching asset for this platform → send the user to the release page.
    if (downloadUrl.isEmpty()) downloadUrl = releaseUrl;

    QJsonObject obj;
    obj["current"] = m_current;
    obj["latest"] = latest;
    obj["update_available"] = available;
    obj["download_url"] = downloadUrl;
    obj["release_url"] = releaseUrl;
    obj["asset_name"] = assetName;
    obj["checked_at"] = m_lastCheck.toString(Qt::ISODate);
    m_result = obj;

    if (available)
        Logger::info(QStringLiteral("[Update] new version available: %1 (current %2)")
                         .arg(latest, m_current));
}

bool UpdateChecker::isNewer(const QString& latest, const QString& current)
{
    auto parse = [](QString s) {
        s = s.trimmed();
        if (!s.isEmpty() && (s[0] == 'v' || s[0] == 'V')) s = s.mid(1);
        QList<int> out;
        const QStringList parts = s.split('.');
        for (const QString& p : parts) {
            QString num;
            for (const QChar c : p) {
                if (!c.isDigit()) break; // stop at first non-digit ("3-rc1" → 3)
                num += c;
            }
            out << (num.isEmpty() ? 0 : num.toInt());
        }
        return out;
    };

    const QList<int> a = parse(latest);
    const QList<int> b = parse(current);
    if (a.isEmpty()) return false; // unparseable latest — never nag

    const int n = qMax(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av != bv) return av > bv;
    }
    return false; // equal
}

QString UpdateChecker::pickAsset(const QJsonArray& assets, QString& outName)
{
    // Build the ordered list of filename suffixes/tokens to accept for this
    // platform, most-specific first. The CI asset names (release.yml):
    //   Windows: MoonlightWeb-installer-win-x64.exe / -arm64.exe
    //   macOS:   moonlightweb-macos-arm64.pkg
    //   Linux:   moonlightweb-linux-x64.{deb,rpm,AppImage}
    QStringList wanted;
#if defined(Q_OS_WIN)
    wanted << (isArm64() ? QStringLiteral("win-arm64.exe") : QStringLiteral("win-x64.exe"));
#elif defined(Q_OS_MACOS)
    wanted << QStringLiteral(".pkg");
#else // Linux / other Unix — prefer the native package for the running distro.
    if (QFile::exists(QStringLiteral("/etc/debian_version")))
        wanted << QStringLiteral(".deb") << QStringLiteral(".AppImage") << QStringLiteral(".rpm");
    else if (QFile::exists(QStringLiteral("/etc/redhat-release")) ||
             QFile::exists(QStringLiteral("/etc/fedora-release")) ||
             QFile::exists(QStringLiteral("/etc/SuSE-release")))
        wanted << QStringLiteral(".rpm") << QStringLiteral(".AppImage") << QStringLiteral(".deb");
    else
        wanted << QStringLiteral(".AppImage") << QStringLiteral(".deb") << QStringLiteral(".rpm");
#endif

    for (const QString& suffix : wanted) {
        for (const QJsonValue& v : assets) {
            const QJsonObject a = v.toObject();
            const QString name = a.value("name").toString();
            if (name.endsWith(suffix, Qt::CaseInsensitive)) {
                outName = name;
                return a.value("browser_download_url").toString();
            }
        }
    }
    outName.clear();
    return {};
}
