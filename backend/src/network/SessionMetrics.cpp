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

#include "SessionMetrics.h"
#include "common/Logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>

namespace {

// Same compile-time tokens the version census uses, for the same reason:
// QSysInfo::productType() returns the distro name on Linux and would turn one
// bucket into a dozen.
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

QString archToken()
{
    const QString a = QSysInfo::currentCpuArchitecture();
    return a.contains(QLatin1String("arm"), Qt::CaseInsensitive) ? QStringLiteral("arm64")
                                                                 : QStringLiteral("x64");
}

// The census endpoint for this build, or empty to stay silent. Empty means the
// user was never asked or said no, or the build has no MW_PDNS_TOKEN/MW_DOMAIN
// — the case for anyone who compiled MoonlightWeb themselves.
//
// Its own host rather than a path under updates.{domain}: the two answer
// different questions, and this one must be switchable off — by us at the DNS
// level, by a user at their firewall — without touching the update path.
QString buildEndpoint()
{
    if (!qEnvironmentVariableIsEmpty("MW_NO_TELEMETRY")) return {};

    const QString domain = QString::fromUtf8(qgetenv("MW_DOMAIN")).trimmed();
    const QString token = QString::fromUtf8(qgetenv("MW_PDNS_TOKEN")).trimmed();
    if (domain.isEmpty() || token.isEmpty()) return {};

    return QStringLiteral("https://metrics.%1/v1/session").arg(domain);
}

} // namespace

SessionMetrics::SessionMetrics(QString version, bool enabled, QObject* parent)
    : QObject(parent)
    , m_version(std::move(version))
    , m_url(enabled ? buildEndpoint() : QString())
    , m_token(QString::fromUtf8(qgetenv("MW_PDNS_TOKEN")).trimmed())
    , m_nam(new QNetworkAccessManager(this))
{
    if (active())
        Logger::info(QStringLiteral("[Metrics] session counts are reported in aggregate "
                                    "(consent given; withdraw it in the settings page)"));
}

void SessionMetrics::setEnabled(bool enabled)
{
    const QString url = enabled ? buildEndpoint() : QString();
    if (url == m_url) return;
    m_url = url;
    Logger::info(active() ? QStringLiteral("[Metrics] session counts are now reported in aggregate")
                          : QStringLiteral("[Metrics] session counts are not reported"));
}

QString SessionMetrics::clientClass(const QString& userAgent)
{
    const QString ua = userAgent.toLower();
    if (ua.isEmpty()) return QStringLiteral("other");
    // Order matters: a TV stick and a tablet both claim to be Android, and an
    // iPad has claimed to be a Mac since iPadOS 13 — so the specific markers
    // are tested before the general ones.
    if (ua.contains(QLatin1String("smart-tv")) || ua.contains(QLatin1String("smarttv")) ||
        ua.contains(QLatin1String("appletv")) || ua.contains(QLatin1String("crkey")) ||
        ua.contains(QLatin1String("tizen")) || ua.contains(QLatin1String("web0s")) ||
        ua.contains(QLatin1String("webos")) || ua.contains(QLatin1String("googletv")) ||
        ua.contains(QLatin1String("bravia")))
        return QStringLiteral("tv");
    if (ua.contains(QLatin1String("ipad")) || ua.contains(QLatin1String("tablet")) ||
        (ua.contains(QLatin1String("android")) && !ua.contains(QLatin1String("mobile"))))
        return QStringLiteral("tablet");
    if (ua.contains(QLatin1String("mobi")) || ua.contains(QLatin1String("iphone")) ||
        ua.contains(QLatin1String("ipod")) || ua.contains(QLatin1String("android")))
        return QStringLiteral("mobile");
    return QStringLiteral("desktop");
}

void SessionMetrics::reportStart(const Facts& facts)
{
    post("start", facts, QString(), 0);
}

void SessionMetrics::reportEnd(const Facts& facts, int seconds)
{
    post("end", facts, QStringLiteral("seconds"), seconds);
}

void SessionMetrics::reportFailure(const Facts& facts, int httpCode)
{
    post("failed", facts, QStringLiteral("code"), httpCode);
}

void SessionMetrics::post(const char* event, const Facts& facts, const QString& extraKey,
                          int extraValue)
{
    if (!active()) return;

    QJsonObject body{
        {QStringLiteral("event"), QString::fromLatin1(event)},
        {QStringLiteral("v"), m_version},
        {QStringLiteral("os"), platformToken()},
        {QStringLiteral("arch"), archToken()},
        {QStringLiteral("h"), facts.height},
        {QStringLiteral("fps"), facts.fps},
        {QStringLiteral("codec"), facts.codec},
        {QStringLiteral("hdr"), facts.hdr},
        {QStringLiteral("yuv444"), facts.yuv444},
        {QStringLiteral("bitrate"), facts.bitrateKbps},
        {QStringLiteral("backend"), facts.backend},
        {QStringLiteral("transport"), facts.transport},
        {QStringLiteral("net"), facts.net},
        {QStringLiteral("client"), facts.client},
        {QStringLiteral("kind"), facts.kind},
    };
    if (!extraKey.isEmpty()) body.insert(extraKey, extraValue);

    QNetworkRequest req{QUrl(m_url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-API-Key", m_token.toUtf8());
    // Nothing downstream reads a redirect from this endpoint, and following one
    // would be a way to have the report land somewhere else entirely.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));

    QNetworkReply* reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, [reply]() {
        reply->deleteLater();
        // Best-effort by design: a census that cannot be reached must cost a
        // debug line, never a retry and never anything a user would notice.
        if (reply->error() != QNetworkReply::NoError)
            Logger::debug(QStringLiteral("[Metrics] report dropped: %1").arg(reply->errorString()));
    });
}
