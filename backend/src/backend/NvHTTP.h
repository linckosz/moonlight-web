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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QXmlStreamReader>

#include "NvAddress.h"
#include "NvApp.h"

struct NvDisplayMode
{
    int width = 0;
    int height = 0;
    int refreshRate = 0;

    bool operator==(const NvDisplayMode& other) const
    {
        return width == other.width && height == other.height && refreshRate == other.refreshRate;
    }
};

class NvHTTP : public QObject
{
    Q_OBJECT

public:
    // Poll deadline for serverinfo. Generous on purpose: aborting a slow-but-
    // alive Sunshine mid-response wedges its single-threaded HTTP server and
    // makes the host appear offline to co-located native clients.
    static constexpr int FAST_FAIL_TIMEOUT_MS = 5000;
    static constexpr int REQUEST_TIMEOUT_MS = 5000;
    // Deadline for launch/resume. The GameStream spec allows 120s, but the
    // browser gives up on /start after 25s (BackendClient.launchApp) and
    // relaunches — which takes the slot over and kills this worker mid-request,
    // leaving no trace of WHY beyond a bare 502. Give up first instead: a
    // bounded failure surfaces the real error code in the worker log and in the
    // /start response. Sunshine answers a healthy launch in well under a second
    // (a wedged one never answers at all), so the margin is generous.
    static constexpr int LAUNCH_TIMEOUT_MS = 20000;
    // Deadline for /cancel. Sunshine answers it only once the app is gone: it
    // asks the app to close, waits its exit-timeout (5 s by default) and only
    // then kills it — so a /cancel of an app with a live process takes longer
    // than REQUEST_TIMEOUT_MS by construction. At 5 s every such quit timed
    // out (issue #24 bench, 05/10/2026: 10 s on Sunshine 2026.914 for Windows),
    // the Stop that quits the app answered 502, and stop-session read back the
    // app as still running.
    static constexpr int QUIT_TIMEOUT_MS = 30000;

    explicit NvHTTP(QNetworkAccessManager* nam, QObject* parent = nullptr);

    // Async server info fetch — caller owns the returned QNetworkReply
    QNetworkReply* getServerInfoAsync(const NvAddress& address, const QString& uniqueId);

    // HTTPS variant for paired hosts (port 47989) — returns real PairStatus
    QNetworkReply* getServerInfoAsyncHttps(const NvAddress& address, const QString& uniqueId,
                                           const QByteArray& clientCertPem,
                                           const QByteArray& clientKeyPem,
                                           const QByteArray& serverCertPem,
                                           quint16 httpsPort = MW_HTTPS_PORT);

    // App list (HTTPS, requires client cert, async)
    QNetworkReply* getAppListAsync(const NvAddress& address, quint16 httpsPort,
                                   const QByteArray& clientCertPem, const QByteArray& clientKeyPem,
                                   const QByteArray& serverCertPem);

    // Launch / quit app (HTTPS, requires client cert, async)
    // timeoutMs 0 → LAUNCH_TIMEOUT_MS. A caller that is spending a shared
    // budget passes what is left of it instead.
    QNetworkReply* launchAppAsync(const NvAddress& address, quint16 httpsPort, int appId,
                                  const QString& uniqueId, const QByteArray& rikey, int rikeyid,
                                  int width, int height, int fps, int bitrate,
                                  const QByteArray& clientCertPem, const QByteArray& clientKeyPem,
                                  const QByteArray& serverCertPem, int hdrMode = 0,
                                  int localAudioPlayMode = 0, int timeoutMs = 0);

    // Resume an existing session for this uniqueId (reconnect to our own
    // orphaned session without relaunching the app). Keyed by uniqueId so it
    // never touches another client's session.
    QNetworkReply* resumeAppAsync(const NvAddress& address, quint16 httpsPort,
                                  const QString& uniqueId, const QByteArray& rikey, int rikeyid,
                                  const QByteArray& clientCertPem, const QByteArray& clientKeyPem,
                                  const QByteArray& serverCertPem, int localAudioPlayMode = 0,
                                  int timeoutMs = 0);

    // uniqueId empty → falls back to the shared Moonlight unique ID.
    QNetworkReply* quitAppAsync(const NvAddress& address, quint16 httpsPort,
                                const QByteArray& clientCertPem, const QByteArray& clientKeyPem,
                                const QByteArray& serverCertPem,
                                const QString& uniqueId = QString());

    // Every HTTPS call to a host goes through this: once the TLS handshake is
    // done and before the request leaves, the machine must have shown the
    // certificate the host was paired with, or the reply is aborted. A host with
    // none on record (not paired yet) has nothing to compare.
    static void requireServerCert(QNetworkReply* reply, const QByteArray& serverCertPem);
    // Whether a reply ended that way, which says nothing about the pairing: it is
    // never a reason to forget the certificate on record.
    static bool serverCertRefused(const QNetworkReply* reply);

    // Closes the request's connection as soon as its reply is done. Qt keeps a
    // finished socket pooled ~120s for reuse, whatever "Connection: close" the
    // request carries; against Sunshine's single-threaded HTTPS server that
    // lingering socket blocks every other client. Set on each request instead
    // of calling QNetworkAccessManager::clearConnectionCache(): that one tears
    // down the manager's whole HTTP thread, waits up to 5 s for it on the
    // calling thread, and strands every request still in flight to any host.
    static void closeWhenDone(QNetworkRequest& req);

    // Static XML helpers
    static void verifyResponseStatus(const QString& xml);
    // The status GameStream reports *inside* an otherwise-200 body, which is
    // where a Sunshine-family host puts its own rejections. 200 when the body
    // reports no problem, 0 when it carries no status at all.
    static int responseStatusCode(const QString& xml);
    static QString getXmlString(const QString& xml, const QString& tagName);
    static QByteArray getXmlStringFromHex(const QString& xml, const QString& tagName);
    static QVector<NvDisplayMode> getDisplayModeList(const QString& serverInfo);
    static int getCurrentGame(const QString& serverInfo);
    static QVector<NvApp> parseAppList(const QString& xml);
    static QVector<int> parseQuad(const QString& quad);
    static QString parseSessionUrl(const QString& launchXml);

private:
    QUrl buildUrl(const NvAddress& address, const QString& command, const QString& uniqueId,
                  const QString& arguments = QString()) const;

    QNetworkAccessManager* m_Nam;
};
