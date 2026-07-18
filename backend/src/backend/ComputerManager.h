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
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QJsonArray>

#include <functional>

#include "NvComputer.h"
#include "NvHTTP.h"
#include "../common/Types.h"

class NvPairingManager;

// Forward declarations for qmdnsengine
namespace QMdnsEngine {
class Server;
class Browser;
class Service;
} // namespace QMdnsEngine

class MdnsPendingComputer;

class ComputerManager : public QObject
{
    Q_OBJECT

public:
    explicit ComputerManager(QObject* parent = nullptr);
    ~ComputerManager() override;

    // Lifecycle
    void init();

    // Suspend all Sunshine polling while a stream session is active. Native
    // Moonlight clients stop polling when they own the session; doing the same
    // avoids hammering Sunshine's single-threaded HTTP server during encode,
    // which otherwise wedges it and makes the host appear offline to other
    // clients. The predicate reads the live relay pointers (no desync).
    void setStreamActivePredicate(std::function<bool()> predicate)
    {
        m_StreamActivePredicate = std::move(predicate);
    }

    // REST API methods
    QJsonArray getHostsJson() const;
    void handleScanRequest();
    std::pair<int, QJsonObject> handleAddManualHost(const QString& address);
    std::pair<int, QJsonObject> handleDeleteHost(const QString& uuid);

    // Wake-on-LAN — broadcasts a magic packet to the host's MAC on the LAN
    std::pair<int, QJsonObject> handleWakeHost(const QString& uuid);

    // Pairing — client generates PIN, user enters it in Sunshine.
    // handleSubmitPin is async: it kicks off (or reports on) the background
    // pairing chain and answers the poll immediately — no nested event loop.
    std::pair<int, QJsonObject> handleStartPairing(const QString& uuid);

    // Pair the SECONDARY identity (IdentityManager::get(1)) with an
    // already-paired host — the installer's "double pairing" that enables the
    // dual-stream standby slot to launch under its own certificate. Same
    // chain/PIN flow as handleStartPairing; on success sets host->paired2.
    std::pair<int, QJsonObject> handleStartPairingSecondary(const QString& uuid);
    void handleSubmitPin(const QString& uuid, ResponseCallback respond);

    // Provisioning-only: drive the pairing chain to a terminal state under a
    // local event loop. Safe because provisioning runs once at startup, before
    // the main loop and outside the reentrant HTTP request path. Returns true
    // once the host is paired. NOT for use from the HTTP request handlers.
    bool pairHostBlocking(const QString& uuid, int timeoutMs);

    // Host lookup (public, for stream session creation)
    NvComputer* getHost(const QString& uuid) const;
    NvHTTP* http() const { return m_Http; }

    // App list — HTTPS fetch from paired host, async
    void handleGetAppList(const QString& uuid, ResponseCallback respond);

    // Box art — proxy PNG from Sunshine, async (fetches on demand if not cached)
    void handleGetBoxArt(const QString& uuid, int appId, ResponseCallback respond);

    // Unique client ID, persisted in QSettings
    static QString clientUniqueId();

signals:
    void hostsChanged();
    void scanCompleted();
    void hostAddCompleted(bool success, const QString& error, const QString& uuid);

private slots:
    void onPollTick();
    void onPollReplyFinished();
    void onBackupPollTick();

    // mDNS
    void onMdnsServiceAdded(const QMdnsEngine::Service& service);
    void onMdnsResolved(MdnsPendingComputer* computer, const QVector<QHostAddress>& addresses);

private:
    void loadHosts();
    void saveHosts();
    void startPolling();
    void startMdnsDiscovery();
    void stopMdnsDiscovery();
    NvComputer* findHostByUuid(const QString& uuid) const;

    // Resolve a host MAC from the OS ARP cache (Windows). Empty if unavailable.
    static QByteArray resolveMacFromArp(const QString& ip);
    void addOrUpdateHost(const QString& serverInfo, const NvAddress& addr);
    void tryAddHostFromAddress(const NvAddress& addr, bool fromMdns,
                               const QString& name = QString());

    QMap<QString, NvComputer*> m_Hosts; // uuid → host, all pointers owned here

    QTimer* m_PollTimer = nullptr;
    QNetworkAccessManager* m_Nam = nullptr;
    NvHTTP* m_Http = nullptr;

    // Robust polling tracking: which hosts are currently being polled + when they started
    QSet<QString> m_PollingHosts;                  // host UUIDs currently being polled
    QHash<QString, QElapsedTimer> m_PollStartedAt; // start time per host (for stalled detection)

    // Pending poll tracking: reply → uuid (for callback routing only, not for "is polling?" check)
    QMap<QNetworkReply*, QString> m_PendingPolls;

    // Backup polling timer — forces full refresh every 30s regardless of tracking state
    QTimer* m_BackupPollTimer = nullptr;

    // Returns true while a stream session is active (set by main.cpp). When
    // active, all polling is suspended to avoid wedging Sunshine during encode.
    std::function<bool()> m_StreamActivePredicate;

    // HTTPS pair verification via /applist for paired hosts
    QMap<QString, QDateTime> m_LastPairCheck;
    QSet<QString> m_PendingPairChecks; // uuid of hosts being verified
    void onPairCheckFinished();

    // Active pairing sessions: uuid → PairingManager
    QMap<QString, NvPairingManager*> m_ActivePairings;
    QMap<QString, QString> m_PairingPins; // uuid → PIN (generated by client)
    QMap<QString, int> m_PairingIdentityIndex; // uuid → identity being paired (0/1)

    // Terminal error from a background pairing chain, delivered to the next poll
    // and then cleared. uuid → user-facing message.
    QMap<QString, QString> m_PairingError;

    // Drives stage 1 → stages 2-5 as an async chain against the session's
    // NvPairingManager. Keeps m_SubmitInFlight set for the whole run.
    void startPairingChain(const QString& uuid);

    // A pairing chain is running for these uuids. While set, the session's
    // NvPairingManager must not be freed (its async callbacks still reference
    // it) and no second chain is started for the same host.
    QSet<QString> m_SubmitInFlight; // uuid of hosts with a pairing chain in flight

    // Box art cache: uuid -> (appId -> pngData)
    QMap<QString, QMap<int, QByteArray>> m_BoxArtCache;

    // In-flight box art requests: uuid -> (appId -> list of pending callbacks)
    QMap<QString, QMap<int, QList<ResponseCallback>>> m_PendingBoxArtCallbacks;

    // Serialize HTTPS box art fetches per host (only one at a time)
    QSet<QString> m_ActiveBoxArtFetches;          // hosts with an active fetch
    QMap<QString, QList<int>> m_BoxArtFetchQueue; // per-host queue of appIds

    // Prevent overlapping mDNS scans
    QDateTime m_LastScanTime;

    void enqueueBoxArtFetch(const QString& uuid, int appId);
    void startBoxArtFetch(const QString& uuid, int appId);
    void onBoxArtFetchComplete(const QString& uuid, int appId, bool ok);
    void fetchNextBoxArtInBackground(const QString& uuid);

    // mDNS state
    QMdnsEngine::Server* m_MdnsServer = nullptr;
    QMdnsEngine::Browser* m_MdnsBrowser = nullptr;
    QList<MdnsPendingComputer*> m_PendingResolutions;
    bool m_MdnsActive = false;
    // Tears down the mDNS sockets at the end of each discovery window,
    // releasing UDP 5353 so other mDNS clients (e.g. Moonlight Qt) can use it.
    QTimer* m_MdnsWindowTimer = nullptr;
};
