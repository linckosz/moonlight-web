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
#include <QString>

class QNetworkAccessManager;

/**
 * Counts streaming sessions, in aggregate, for the project's own planning.
 *
 * Why it exists
 * -------------
 * The version census (UpdateChecker) says which builds are alive. It says
 * nothing about how they are used, so "is 720p still worth supporting?", "did
 * AV1 take off?", "how long does a session actually last?" have no data behind
 * them. This reports the SHAPE of a session — never who ran it or what they
 * ran.
 *
 * What is sent
 * ------------
 * Resolution, frame rate, negotiated codec, HDR/4:4:4 flags, a bitrate band,
 * the backend family, the transport that won, whether the viewer was on the LAN
 * or the internet, a coarse device class, whether it was the owner or an
 * invited player, and — when it ends — how long it lasted.
 *
 * What is NOT sent, ever
 * ----------------------
 * No host name, host UUID, account, session token or pairing identity; no
 * application or game id; no address (the receiving end runs the values through
 * an allowlist and lets Umami hash address+UA behind a daily-rotating salt,
 * which is the whole reason it reports there rather than into a table of ours).
 * Deliberately nothing free-form: every field is a number or a token this file
 * chose. See deploy/powerdns/mw-proxy/session.go for the receiving half.
 *
 * When it stays quiet
 * -------------------
 * Reporting is off unless ALL of these hold: the person running this machine
 * was asked at first launch and said yes (AppSettings::metricsConsentDecision),
 * the instance has not opted out since (settings key "session_metrics_enabled",
 * or MW_NO_TELEMETRY in the environment), and the build carries MW_DOMAIN +
 * MW_PDNS_TOKEN — so a self-built binary never phones our infrastructure at
 * all. Unasked counts as no: silence is not consent.
 *
 * It is never load-bearing: every report is fire-and-forget, a failure is
 * dropped without a retry, and nothing about a stream depends on the answer.
 */
class SessionMetrics : public QObject
{
    Q_OBJECT
public:
    /// One session's shape. Defaults are the "unknown" values the receiving
    /// end buckets away, so a partially-filled Facts is always safe to send.
    struct Facts
    {
        int height = 0;      ///< stream height in pixels
        int fps = 0;         ///< requested frame rate
        int bitrateKbps = 0; ///< requested bitrate; bucketed server-side
        bool hdr = false;
        bool yuv444 = false;
        QString codec;     ///< NEGOTIATED codec: h264 | hevc | av1
        QString backend;   ///< gamestream | wolf | multiseat | gfe
        QString transport; ///< webrtc-dc-udp | webrtc-media-tcp | wss | …
        QString net;       ///< loopback | private | tunnel | public
        QString client;    ///< desktop | mobile | tablet | tv
        QString kind;      ///< owner | player
    };

    /// enabled: consent given AND not opted out
    /// (AppSettings::sessionMetricsAllowed()). Even when true, nothing is sent
    /// unless the build carries the credentials.
    explicit SessionMetrics(QString version, bool enabled, QObject* parent = nullptr);

    /// Whether anything will actually be sent. Callers do not need to check —
    /// every report is a no-op when this is false — it is for logging.
    bool active() const { return !m_url.isEmpty(); }

    /// Apply a consent answer without a restart: the moment someone says no in
    /// the UI, the next session must already be uncounted. Re-derives the
    /// endpoint, so an opt-out still wins on a build that carries credentials.
    void setEnabled(bool enabled);

    /// A session reached the point of carrying a picture.
    void reportStart(const Facts& facts);
    /// That session is over, after @p seconds. Only call it for a session whose
    /// start was reported, or the two counts stop matching.
    void reportEnd(const Facts& facts, int seconds);
    /// A launch never got there. @p httpCode is the status the browser was told.
    void reportFailure(const Facts& facts, int httpCode);

    /// Coarse device class from a browser User-Agent: desktop | mobile | tablet
    /// | tv. The header itself never leaves this process — only this token does.
    static QString clientClass(const QString& userAgent);

private:
    void post(const char* event, const Facts& facts, const QString& extraKey, int extraValue);

    QString m_version;
    QString m_url;   ///< empty = reporting off, for any of the reasons above
    QString m_token; ///< the same restricted key the DNS/update paths present
    QNetworkAccessManager* m_nam;
};
