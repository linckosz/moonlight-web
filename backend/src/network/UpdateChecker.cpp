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
#include <QScopeGuard>
#include <QSysInfo>
#include <QUrl>
#include <QUrlQuery>

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

// Compile-time platform token, matching the allowlist the relay validates
// against (deploy/powerdns/mw-proxy/update.go). Deliberately not
// QSysInfo::productType(), which returns the distro name on Linux and would
// turn one bucket into a dozen.
QString platformToken()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

// Whether a URL out of a release payload points at GitHub's own hosts.
//
// This is what keeps the relay from being able to escalate. It answers with a
// release payload, and SelfUpdater downloads and RUNS what download_url names —
// so an unconstrained relay could hand every host an arbitrary installer (it
// supplies the digest too, so digest verification would not catch it). Pinning
// the host means the worst a compromised relay can do is lie about *which*
// GitHub release exists, exactly like GitHub itself could.
bool isGitHubUrl(const QString& url)
{
    if (url.isEmpty()) return false;
    const QUrl u(url);
    if (u.scheme() != QLatin1String("https")) return false;
    const QString host = u.host().toLower();
    return host == QLatin1String("github.com") || host.endsWith(QLatin1String(".github.com")) ||
           host == QLatin1String("objects.githubusercontent.com") ||
           host.endsWith(QLatin1String(".githubusercontent.com"));
}

// The relay endpoint for this build, or empty to check GitHub directly.
//
// Empty means: the user opted out (MW_NO_TELEMETRY), or the build has no
// MW_PDNS_TOKEN/MW_DOMAIN — the case for anyone who compiled MoonlightWeb
// themselves, who therefore never phones our infrastructure at all.
QString buildRelayUrl(const QString& version)
{
    if (!qEnvironmentVariableIsEmpty("MW_NO_TELEMETRY")) return {};

    const QString domain = QString::fromUtf8(qgetenv("MW_DOMAIN")).trimmed();
    const QString token = QString::fromUtf8(qgetenv("MW_PDNS_TOKEN")).trimmed();
    if (domain.isEmpty() || token.isEmpty()) return {};

    QUrl url(QStringLiteral("https://updates.%1/v1/update").arg(domain));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("v"), version);
    query.addQueryItem(QStringLiteral("os"), platformToken());
    query.addQueryItem(QStringLiteral("arch"),
                       isArm64() ? QStringLiteral("arm64") : QStringLiteral("x64"));
    url.setQuery(query);
    return url.toString();
}

} // namespace

UpdateChecker::UpdateChecker(QString currentVersion, bool relayEnabled, QObject* parent)
    : QObject(parent)
    , m_current(std::move(currentVersion))
    , m_relayUrl(relayEnabled ? buildRelayUrl(m_current) : QString())
    , m_nam(new QNetworkAccessManager(this))
{
    if (!m_relayUrl.isEmpty())
        Logger::info(QStringLiteral("[Update] checking via the MoonlightWeb update relay"));
}

bool UpdateChecker::relayAvailable()
{
    // Any version does: the question is whether the endpoint can be built at
    // all, not what would be sent to it.
    return !buildRelayUrl(QStringLiteral("0")).isEmpty();
}

void UpdateChecker::setRelayEnabled(bool enabled)
{
    const QString url = enabled ? buildRelayUrl(m_current) : QString();
    if (url == m_relayUrl) return;
    m_relayUrl = url;
    Logger::info(m_relayUrl.isEmpty()
                     ? QStringLiteral("[Update] checking GitHub directly, reporting nothing")
                     : QStringLiteral("[Update] checking via the MoonlightWeb update relay"));
}

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
    // MW_NO_TELEMETRY means what it says. It used to only steer the check away
    // from the relay and on to GitHub, which reports nothing but is still an
    // outgoing request every six hours to a third party — not what someone who
    // set that variable was asking for. Nothing to check means no check.
    if (!qEnvironmentVariableIsEmpty("MW_NO_TELEMETRY")) return;

    // The relay when this build has one, GitHub otherwise. Either way a single
    // release JSON comes back in the same shape, so nothing downstream cares
    // which one answered.
    if (m_relayUrl.isEmpty())
        fetchFrom(QString::fromLatin1(kReleasesApi), false);
    else
        fetchFrom(m_relayUrl, true);
}

void UpdateChecker::fetchFrom(const QString& url, bool viaRelay)
{
    m_inFlight = true;

    QNetworkRequest req{QUrl(url)};
    // GitHub rejects requests without a User-Agent; the Accept header pins the
    // stable v3 media type. The relay answers with GitHub's own payload, so it
    // takes the same headers.
    req.setRawHeader("User-Agent", "MoonlightWeb-UpdateChecker");
    req.setRawHeader("Accept", "application/vnd.github+json");
    // The relay gates on the same restricted key as the DNS API. A build
    // without that key never reaches here — buildRelayUrl() returns empty and
    // the check goes straight to GitHub.
    if (viaRelay) req.setRawHeader("X-API-Key", qgetenv("MW_PDNS_TOKEN"));
    req.setTransferTimeout(8000);

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, viaRelay]() {
        reply->deleteLater();
        m_inFlight = false;
        m_lastCheck = QDateTime::currentDateTimeUtc();
        // Signalled on the way out of every branch below, including the ones
        // that keep the previous result: whoever is waiting on this check has to
        // be released whether or not it learned anything.
        auto done = qScopeGuard([this] { emit checkFinished(); });

        if (reply->error() != QNetworkReply::NoError) {
            if (viaRelay) {
                // The relay is a convenience, never a dependency: retry against
                // GitHub before reporting anything. checkFinished() must wait
                // for that second attempt, or SelfUpdater would act on a check
                // that has not actually finished.
                Logger::info(QStringLiteral("[Update] relay unreachable (%1) — asking GitHub")
                                 .arg(reply->errorString()));
                done.dismiss();
                fetchFrom(QString::fromLatin1(kReleasesApi), false);
                return;
            }
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
            if (viaRelay) {
                Logger::info(QStringLiteral("[Update] relay sent an unusable body — asking GitHub"));
                done.dismiss();
                fetchFrom(QString::fromLatin1(kReleasesApi), false);
                return;
            }
            Logger::warning(QStringLiteral("[Update] check: unexpected response"));
            return;
        }
        if (!applyResult(doc.object(), viaRelay) && viaRelay) {
            // The relay answered with something we refuse to act on. Ask the
            // authoritative source instead of stalling on a rejected payload.
            done.dismiss();
            fetchFrom(QString::fromLatin1(kReleasesApi), false);
        }
    });
}

bool UpdateChecker::applyResult(const QJsonObject& release, bool viaRelay)
{
    const QString tag = release.value("tag_name").toString();
    QString latest = tag;
    if (latest.startsWith('v') || latest.startsWith('V')) latest = latest.mid(1);

    const bool available = isNewer(latest, m_current);

    QString assetName;
    qint64 assetSize = 0;
    QString assetDigest;
    QString downloadUrl =
        pickAsset(release.value("assets").toArray(), assetName, assetSize, assetDigest);
    const QString releaseUrl = release.value("html_url").toString();
    // No matching asset for this platform → send the user to the release page.
    if (downloadUrl.isEmpty()) downloadUrl = releaseUrl;

    // Nothing is written to m_result before this check: a payload that would
    // send the updater off GitHub is discarded whole, not partially applied.
    if (viaRelay && !isGitHubUrl(downloadUrl)) {
        Logger::warning(
            QStringLiteral("[Update] relay pointed the download off GitHub (%1) — ignoring it")
                .arg(downloadUrl));
        return false;
    }

    QJsonObject obj;
    obj["current"] = m_current;
    obj["latest"] = latest;
    obj["update_available"] = available;
    obj["download_url"] = downloadUrl;
    obj["release_url"] = releaseUrl;
    obj["asset_name"] = assetName;
    obj["asset_size"] = assetSize;
    obj["asset_digest"] = assetDigest;
    obj["checked_at"] = m_lastCheck.toString(Qt::ISODate);
    m_result = obj;

    if (available)
        Logger::info(QStringLiteral("[Update] new version available: %1 (current %2)")
                         .arg(latest, m_current));
    return true;
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

QString UpdateChecker::pickAsset(const QJsonArray& assets, QString& outName, qint64& outSize,
                                 QString& outDigest)
{
    // Build the ordered list of filename suffixes/tokens to accept for this
    // platform, most-specific first. The CI asset names embed the version but
    // keep the platform/arch/extension suffix intact so this matching still
    // holds (release.yml):
    //   Windows: MoonlightWeb-installer-<ver>-win-x64.exe / -win-arm64.exe
    //   macOS:   moonlightweb-<ver>-macos-arm64.pkg
    //   Linux:   moonlightweb-<ver>-linux-x64.{deb,rpm,AppImage}
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
                outSize = static_cast<qint64>(a.value("size").toDouble());
                outDigest = a.value("digest").toString();
                return a.value("browser_download_url").toString();
            }
        }
    }
    outName.clear();
    outSize = 0;
    outDigest.clear();
    return {};
}
