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
#include <memory>

#include "NvComputer.h"
#include "NvHTTP.h"
#include "streambackend/IStreamBackend.h"
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

    // Give a host a name of our own. Purely local: nothing is written to the
    // host, which is what lets it work on an offline or unpaired card too. An
    // empty name clears the alias and the host goes back to calling itself
    // whatever serverinfo says.
    std::pair<int, QJsonObject> handleRenameHost(const QString& uuid, const QString& name);

    // Restart the streaming service a host runs. Only ever through a control
    // path we already hold: the local machine's own Sunshine, or a backend whose
    // capabilities().restartService says its API can do it. A plain remote
    // GameStream host has no such path — Sunshine's REST needs a web-UI password
    // MoonlightWeb deliberately does not keep — and answers 501.
    void handleRestartHost(const QString& uuid, ResponseCallback respond);

    // Wake-on-LAN — broadcasts a magic packet to the host's MAC on the LAN
    std::pair<int, QJsonObject> handleWakeHost(const QString& uuid);

    // Pairing — client generates PIN, user enters it in Sunshine.
    // handleSubmitPin is async: it kicks off (or reports on) the background
    // pairing chain and answers the poll immediately — no nested event loop.
    std::pair<int, QJsonObject> handleStartPairing(const QString& uuid);
    void handleSubmitPin(const QString& uuid, ResponseCallback respond);

    // Declare which backend drives a host, then pair it. `type` names a
    // registered backend ("wolf"); `apiUrl` addresses its control API and
    // `apiToken` authenticates to it. An empty apiToken keeps the stored one.
    //
    // Async because registering pairs immediately: that one admin gesture is
    // what spares every player a PIN afterwards.
    // `pairUser`/`pairPassword` are the credentials used to push a pairing PIN
    // to each seat; only MultiSeat needs them. Empty values keep the stored ones.
    void handleSetBackend(const QString& uuid, const QString& type, const QString& apiUrl,
                          const QString& apiToken, const QString& pairUser,
                          const QString& pairPassword, ResponseCallback respond);

    // Stop managing a host as a backend. Leaves its GameStream pairing alone —
    // it simply reverts to a plain host.
    std::pair<int, QJsonObject> handleClearBackend(const QString& uuid);

    /// The provider that drives a host: plain GameStream unless the host was
    /// registered as a Wolf or MultiSeat backend. Callers own the result.
    /// Public because the stream path builds one per session.
    std::unique_ptr<IStreamBackend> backendForHost(const QString& uuid) const;

    /// What the host's backend can do, as the frontend consumes it: {multiUser,
    /// provisioning, lobbies}. Read off a real provider instance so it cannot
    /// claim something the code does not implement. Empty object for a plain
    /// GameStream host, which is what keeps a Sunshine card free of backend UI.
    QJsonObject backendCapabilitiesJson(const QString& uuid) const;

    // A host's backend configuration and what that backend can do. Capabilities
    // are read off a real instance rather than a table, so they cannot drift
    // from the provider. Never returns the API token.
    std::pair<int, QJsonObject> handleGetBackend(const QString& uuid) const;

    // Seat administration, expressed purely through IStreamBackend so one set
    // of routes serves every backend. A seat means what the provider says it
    // means: a Windows account plus its Apollo on MultiSeat, a paired client on
    // Wolf. All three are async — they talk to the backend.
    void handleListSeats(const QString& uuid, ResponseCallback respond);
    void handleProvisionSeat(const QString& uuid, const QJsonObject& params,
                             ResponseCallback respond);
    void handleTeardownSeat(const QString& uuid, const QString& seatId, ResponseCallback respond);

    // Free a seat from its owner without destroying it, so a seat left behind
    // by a device that never came back can be claimed again.
    void handleReleaseSeatOwner(const QString& uuid, const QString& seatId,
                                ResponseCallback respond);

    // Wolf's console concepts, for the overlay. Backends without them answer
    // 501, which lets the UI hide the control instead of showing an error.
    void handleListProfiles(const QString& uuid, ResponseCallback respond);
    void handleListLobbies(const QString& uuid, ResponseCallback respond);

    // Provisioning-only: drive the pairing chain to a terminal state under a
    // local event loop. Safe because provisioning runs once at startup, before
    // the main loop and outside the reentrant HTTP request path. Returns true
    // once the host is paired. NOT for use from the HTTP request handlers.
    bool pairHostBlocking(const QString& uuid, int timeoutMs);

    // Host lookup (public, for stream session creation)
    NvComputer* getHost(const QString& uuid) const;
    NvHTTP* http() const { return m_Http; }

    // App list — HTTPS fetch from paired host, async.
    //
    // `deviceSessionId` names who is asking. On a plain host it is ignored: the
    // backend answers with the host itself, exactly as before. On a multi-seat
    // backend the app list belongs to a seat, and this is what resolves which
    // one — the seat that user durably owns.
    void handleGetAppList(const QString& uuid, const QString& deviceSessionId,
                          ResponseCallback respond);

    // Box art — proxy PNG from Sunshine, async (fetches on demand if not cached).
    // ifNoneMatch carries the request's If-None-Match so an unchanged image is
    // answered with a bodyless 304 instead of re-sending the PNG.
    void handleGetBoxArt(const QString& uuid, int appId, const QString& ifNoneMatch,
                         ResponseCallback respond);

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
    // The address the next poll of this host should use — see m_PollAddrIndex.
    NvAddress pollAddressFor(const NvComputer* host) const;
    // allowEmpty guards the one destructive case: writing an empty array wipes
    // the stored list for good. Only the explicit delete route may do that.
    void saveHosts(bool allowEmpty = false);
    void startPolling();
    void startMdnsDiscovery();
    void stopMdnsDiscovery();
    NvComputer* findHostByUuid(const QString& uuid) const;

    // Registers the built-in "gamestream" provider. Called from the constructor.
    void registerStreamBackends();

    // A provider bound to one host. Returns nullptr for an unknown uuid.

    // Resolve a host MAC from the OS ARP cache (Windows). Empty if unavailable.
    static QByteArray resolveMacFromArp(const QString& ip);
    void addOrUpdateHost(const QString& serverInfo, const NvAddress& addr);
    void tryAddHostFromAddress(const NvAddress& addr, bool fromMdns,
                               const QString& name = QString());

    QMap<QString, NvComputer*> m_Hosts; // uuid → host, all pointers owned here

    // False until loadHosts() has run once. saveHosts() uses it to tell a
    // deliberate empty list from one that only looks empty because nothing was
    // ever read.
    bool m_HostsLoaded = false;

    QTimer* m_PollTimer = nullptr;
    QNetworkAccessManager* m_Nam = nullptr;
    NvHTTP* m_Http = nullptr;

    // Hosts whose control plane has already been probed this run. The probe is
    // a one-off per process: what a machine runs does not change under a live
    // service, and repeating it on every poll would knock on port 9550 of every
    // host on the network every few seconds.
    QSet<QString> m_MultiSeatProbed;

    // Robust polling tracking: which hosts are currently being polled + when they started
    QSet<QString> m_PollingHosts;                  // host UUIDs currently being polled
    QHash<QString, QElapsedTimer> m_PollStartedAt; // start time per host (for stalled detection)

    // Pending poll tracking: reply → uuid (for callback routing only, not for "is polling?" check)
    QMap<QNetworkReply*, QString> m_PendingPolls;

    // Which entry of uniqueAddresses() the next poll of this host should use.
    // Reset to 0 on every success, advanced on every failure, so a host whose
    // first candidate has gone dead is found again on one of the others instead
    // of staying offline forever. Absent uuid means 0.
    QHash<QString, int> m_PollAddrIndex;

    // Services seen since the current mDNS window opened — reported when it
    // closes, so a scan that found nothing says so.
    int m_MdnsSeenThisWindow = 0;

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
