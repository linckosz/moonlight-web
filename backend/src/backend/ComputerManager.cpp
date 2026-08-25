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

#include "ComputerManager.h"

#include "streambackend/BackendProbe.h"
#include "streambackend/GameStreamBackend.h"
#include "streambackend/StreamBackendRegistry.h"
#include "NvComputer.h"
#include "NvPairingManager.h"
#include "PairingChain.h"
#include "WolfApiClient.h"
#include "streambackend/StreamBackendSetup.h"
#include "IdentityManager.h"
#include "SunshineInstaller.h"
#include "common/Logger.h"

#include <qmdnsengine/server.h>
#include <qmdnsengine/browser.h>
#include <qmdnsengine/service.h>
#include <qmdnsengine/resolver.h>

#include <QHostInfo>
#include <QStringList>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <QCoreApplication>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QPointer>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QTimer>
#include <QUuid>

#include <QUdpSocket>
#include <QHostAddress>
#include <QNetworkInterface>

#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>

#include <utility>

#ifdef Q_OS_WIN
// Declared locally to avoid winsock2/windows.h include-ordering conflicts with
// Qt headers. Links against iphlpapi (already in LIBS). IPAddr/DWORD/ULONG are
// all unsigned long.
extern "C"
    __declspec(dllimport) unsigned long __stdcall SendARP(unsigned long DestIP, unsigned long SrcIP,
                                                          void* pMacAddr,
                                                          unsigned long* PhysAddrLen);
#endif

#define SER_HOSTS "hosts"

// Poll every 10s, not 5s: halves the connection churn against each host's
// single-threaded HTTP server, which co-located native clients also poll.
static const int POLL_INTERVAL_MS = 10000;

// HTTPS /applist pair verification is expensive on Sunshine (TLS handshake +
// applist generation). Running it every poll tick saturates Sunshine's HTTPS
// server and makes co-located native Moonlight clients see hosts as offline.
// Pairing state changes rarely, so a slow re-check is plenty.
static const int PAIR_CHECK_INTERVAL_SEC = 300;

// mDNS runs only in short bursts: binding UDP 5353 permanently steals unicast
// mDNS responses from other clients on the same machine (Moonlight Qt) on
// Windows. Known hosts are kept fresh via HTTP polling, so a brief window per
// scan is enough to discover new hosts.
static const int MDNS_DISCOVERY_WINDOW_MS = 8000;

// Longest alias a host card will take. Not a storage limit — it is what still
// fits a card's name row next to the status badge on a phone.
static const int kMaxHostNameLength = 64;

// Sunshine drops its mDNS registration and its tray on the way out; giving it a
// beat before starting the new one keeps the two from fighting over the ports.
static const int RESTART_RELAUNCH_DELAY_MS = 1500;

// ============================================================================
// MdnsPendingComputer — Resolves mDNS hostname → addresses
// ============================================================================

class MdnsPendingComputer : public QObject
{
    Q_OBJECT

public:
    MdnsPendingComputer(QMdnsEngine::Server* server, const QMdnsEngine::Service& service)
        : m_Hostname(service.hostname())
        , m_Port(service.port())
        , m_Server(server)
    {
        resolve();
    }

    ~MdnsPendingComputer() override { delete m_Resolver; }

    QString hostname() const { return m_Hostname; }
    quint16 port() const { return m_Port; }

signals:
    void resolvedHost(MdnsPendingComputer* computer, const QVector<QHostAddress>& addresses);

private slots:
    void handleResolvedAddress(const QHostAddress& addr) { m_Addresses.append(addr); }

    void handleResolvedTimeout()
    {
        if (!m_Addresses.isEmpty())
            emit resolvedHost(this, m_Addresses);
        else if (m_Retries-- > 0)
            resolve();
        else
            emit resolvedHost(this, m_Addresses); // empty → host unreachable

        if (m_Addresses.isEmpty() && m_Retries <= 0) deleteLater();
    }

private:
    void resolve()
    {
        delete m_Resolver;
        m_Resolver = new QMdnsEngine::Resolver(m_Server, m_Hostname);
        connect(m_Resolver, &QMdnsEngine::Resolver::resolved, this,
                &MdnsPendingComputer::handleResolvedAddress);
        QTimer::singleShot(2000, this, &MdnsPendingComputer::handleResolvedTimeout);
    }

    QByteArray m_Hostname;
    quint16 m_Port;
    QMdnsEngine::Server* m_Server;
    QMdnsEngine::Resolver* m_Resolver = nullptr;
    QVector<QHostAddress> m_Addresses;
    int m_Retries = 3;
};

// ============================================================================
// ComputerManager
// ============================================================================

ComputerManager::ComputerManager(QObject* parent)
    : QObject(parent)
    , m_Nam(new QNetworkAccessManager(this))
    , m_Http(new NvHTTP(m_Nam, this))
{
    m_Nam->setProxy(QNetworkProxy::NoProxy);
    registerStreamBackends();
}

void ComputerManager::registerStreamBackends()
{
    // The factories live in StreamBackendSetup because the stream worker — a
    // separate process with no ComputerManager — needs the same ones. Only the
    // two dependencies that genuinely differ are supplied here.
    StreamBackendSetup::registerAll(
        m_Http, m_Nam, [this](const QString& uuid) { return findHostByUuid(uuid); },
        [this](const QString& uuid, const QByteArray& serverCertPem) {
            NvComputer* host = findHostByUuid(uuid);
            if (!host) return;
            host->serverCertPem = serverCertPem;
            host->pairState = NvComputer::PS_PAIRED;
            host->state = NvComputer::CS_ONLINE;
            saveHosts();
            emit hostsChanged();
        });
}

std::unique_ptr<IStreamBackend> ComputerManager::backendForHost(const QString& uuid) const
{
    QJsonObject config;
    config[QStringLiteral("hostUuid")] = uuid;

    // An unconfigured host is a plain GameStream host. Keeping that the default
    // is what guarantees a Sunshine card behaves exactly as it does today.
    QString type = QStringLiteral("gamestream");
    if (NvComputer* host = findHostByUuid(uuid); host && !host->backendType.isEmpty()) {
        type = host->backendType;
        config[QStringLiteral("apiUrl")] = host->backendApiUrl;
        config[QStringLiteral("apiToken")] = host->backendApiToken;
        config[QStringLiteral("pairUser")] = host->backendPairUser;
        config[QStringLiteral("pairPassword")] = host->backendPairPassword;
    }

    return StreamBackendRegistry::instance().create(type, config);
}

ComputerManager::~ComputerManager()
{
    qDeleteAll(m_Hosts);
    qDeleteAll(m_PendingResolutions);
    qDeleteAll(m_ActivePairings);
}

void ComputerManager::init()
{
    loadHosts();
    startPolling();

    // Single-shot timer that closes each mDNS discovery window and frees 5353.
    m_MdnsWindowTimer = new QTimer(this);
    m_MdnsWindowTimer->setSingleShot(true);
    connect(m_MdnsWindowTimer, &QTimer::timeout, this, &ComputerManager::stopMdnsDiscovery);

    // NOTE: mDNS is NOT started at boot. Binding UDP 5353 at idle steals mDNS
    // traffic from any other client on the same machine (Moonlight Qt, or
    // Sunshine's own responder when co-located), so we only open a short mDNS
    // window on an explicit scan request. Known hosts stay fresh via HTTP
    // polling, which never touches 5353.

    Logger::info(
        QString("ComputerManager initialized: %1 hosts loaded (mDNS idle)").arg(m_Hosts.size()));

    // The addresses a restored host will actually be polled at, in order. When a
    // host that worked before a restart comes back offline, this line names the
    // reason: the path we are trying is not the one that used to answer.
    for (auto it = m_Hosts.cbegin(); it != m_Hosts.cend(); ++it) {
        QStringList candidates;
        for (const NvAddress& na : it.value()->uniqueAddresses()) candidates << na.toString();
        Logger::info(QString("[NETWORK] host %1 candidates: %2")
                         .arg(it.value()->name,
                              candidates.isEmpty() ? QStringLiteral("(none — never polled)")
                                                   : candidates.join(QStringLiteral(", "))));
    }
}

// --- Persistence -----------------------------------------------------------

void ComputerManager::loadHosts()
{
    QSettings settings;
    int count = settings.beginReadArray(SER_HOSTS);

    // Where the list came from and whether it was readable at all. Without this
    // a store that is empty for the wrong reason (another user's home under a
    // service, a file we may not read) is indistinguishable from a first run.
    Logger::info(QString("Host store: %1 (%2 entries, status %3)")
                     .arg(settings.fileName())
                     .arg(count)
                     .arg(static_cast<int>(settings.status())));

    int rejected = 0;
    for (int i = 0; i < count; i++) {
        settings.setArrayIndex(i);

        NvComputer* computer = new NvComputer(settings);
        if (!computer->uuid.isEmpty()) {
            m_Hosts[computer->uuid] = computer;
        } else {
            rejected++;
            delete computer;
        }
    }

    settings.endArray();

    if (rejected > 0)
        Logger::warning(QString("Host store: %1 stored entries carried no uuid and were dropped")
                            .arg(rejected));

    m_HostsLoaded = true;
}

void ComputerManager::saveHosts(bool allowEmpty)
{
    QSettings settings;

    // An empty in-memory list is a legitimate state only right after the last
    // host was deleted. Every other way to get here — a store we failed to read,
    // a save racing the load — would silently write hosts/size = 0 and destroy a
    // perfectly good list, which is unrecoverable from the UI. Refuse instead.
    if (m_Hosts.isEmpty() && !allowEmpty) {
        const int stored = settings.beginReadArray(SER_HOSTS);
        settings.endArray();
        if (stored > 0) {
            Logger::error(QString("Refusing to overwrite %1 stored host(s) with an empty list "
                                  "(the stored list was %2)")
                              .arg(stored)
                              .arg(m_HostsLoaded ? "read at startup" : "never read"));
            return;
        }
    }

    settings.beginWriteArray(SER_HOSTS);

    int i = 0;
    for (auto it = m_Hosts.cbegin(); it != m_Hosts.cend(); ++it, ++i) {
        settings.setArrayIndex(i);
        it.value()->serialize(settings);
    }

    settings.endArray();
    settings.sync();
}

// --- Polling ----------------------------------------------------------------

void ComputerManager::startPolling()
{
    // Regular poll timer: every 5 seconds, quick check of all hosts
    m_PollTimer = new QTimer(this);
    m_PollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_PollTimer, &QTimer::timeout, this, &ComputerManager::onPollTick);
    m_PollTimer->start();

    // Backup poll timer: every 60 seconds, safety-net refresh. Skips hosts
    // already being polled (see onBackupPollTick) to avoid a second concurrent
    // connection to the same single-threaded Sunshine server.
    m_BackupPollTimer = new QTimer(this);
    m_BackupPollTimer->setInterval(60000);
    connect(m_BackupPollTimer, &QTimer::timeout, this, &ComputerManager::onBackupPollTick);
    m_BackupPollTimer->start();
}

// The candidate this host's next poll should use. Rotating through the list
// instead of always taking first() is what lets a host whose leading address has
// gone dead — a stale <LocalIP> from a multi-homed Sunshine, a changed DHCP
// lease — be found again on one of the others. One address per tick keeps the
// single-connection-per-host rule Sunshine's single-threaded server needs.
NvAddress ComputerManager::pollAddressFor(const NvComputer* host) const
{
    const QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) return NvAddress();
    return addrs.at(m_PollAddrIndex.value(host->uuid, 0) % addrs.size());
}

void ComputerManager::onPollTick()
{
    // Suspend polling while a stream session is active: a co-located native
    // client owning the session polls nothing, and our extra connection churn
    // (Connection: close + 2s abort) wedges Sunshine's single-threaded HTTP
    // server during encode, making the host appear offline to other clients.
    if (m_StreamActivePredicate && m_StreamActivePredicate()) return;

    // --- Stalled poll cleanup: force-remove hosts stuck in polling for >10s ---
    QList<QString> stalled;
    for (auto it = m_PollStartedAt.cbegin(); it != m_PollStartedAt.cend(); ++it) {
        if (it->hasExpired(10000)) {
            stalled.append(it.key());
        }
    }
    for (const QString& uuid : stalled) {
        Logger::warning(QString("Poll timeout for %1 — forcing cleanup").arg(uuid));
        m_PollingHosts.remove(uuid);
        m_PollStartedAt.remove(uuid);
    }

    // --- Main poll loop ---
    for (auto it = m_Hosts.begin(); it != m_Hosts.end(); ++it) {
        const QString& uuid = it.key();
        NvComputer* host = it.value();

        // Verify pairing via /applist for hosts claiming to be paired
        if (host->pairState == NvComputer::PS_PAIRED && !host->serverCertPem.isEmpty() &&
            !m_PendingPairChecks.contains(uuid)) {
            QDateTime now = QDateTime::currentDateTime();
            if (!m_LastPairCheck.contains(uuid) ||
                m_LastPairCheck[uuid].secsTo(now) >= PAIR_CHECK_INTERVAL_SEC) {
                m_LastPairCheck[uuid] = now;
                const NvAddress pairAddr = pollAddressFor(host);
                if (!pairAddr.isNull()) {
                    IdentityManager* im = IdentityManager::get();
                    quint16 httpsPort =
                        host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT;
                    m_PendingPairChecks.insert(uuid);
                    // [NETWORK] diagnostic: trace HTTPS pair-check (every 5 min).
                    Logger::info(QString("[NETWORK] pair-check applist HTTPS -> %1:%2 (%3)")
                                     .arg(pairAddr.address())
                                     .arg(httpsPort)
                                     .arg(host->name));
                    QNetworkReply* reply = m_Http->getAppListAsync(
                        pairAddr, httpsPort, im->getCertificate(), im->getPrivateKey());
                    reply->setProperty("mwHostUuid", uuid);
                    connect(reply, &QNetworkReply::finished, this,
                            &ComputerManager::onPairCheckFinished);
                }
            }
        }

        // Skip if this host is already being polled (robust tracking via QSet)
        if (m_PollingHosts.contains(uuid)) continue;

        const NvAddress addr = pollAddressFor(host);
        if (addr.isNull()) continue;

        // [NETWORK] diagnostic: trace every outbound serverinfo poll.
        Logger::info(
            QString("[NETWORK] poll serverinfo HTTP -> %1 (%2)").arg(addr.toString(), host->name));

        QNetworkReply* reply = m_Http->getServerInfoAsync(addr, clientUniqueId());
        m_PendingPolls[reply] = uuid;
        m_PollingHosts.insert(uuid);
        m_PollStartedAt[uuid].start();

        connect(reply, &QNetworkReply::finished, this, &ComputerManager::onPollReplyFinished);

        // 2-second timeout (abort reply, onPollReplyFinished will fire)
        QPointer<QNetworkReply> guard(reply);
        QTimer::singleShot(NvHTTP::FAST_FAIL_TIMEOUT_MS, reply, [guard]() {
            if (guard && !guard->isFinished()) guard->abort();
        });
    }
}

void ComputerManager::onBackupPollTick()
{
    // Suspend during an active stream — see onPollTick().
    if (m_StreamActivePredicate && m_StreamActivePredicate()) return;

    Logger::debug("Backup poll tick — safety-net refresh of idle hosts");

    // Safety net for hosts the regular tick somehow never scheduled. Skips any
    // host already in flight: a second concurrent connection to a single-
    // threaded Sunshine server wedges it. Stalled in-flight polls are already
    // cleaned up by onPollTick (>10s), so this never stays stuck.
    for (auto it = m_Hosts.begin(); it != m_Hosts.end(); ++it) {
        const QString& uuid = it.key();
        NvComputer* host = it.value();

        if (m_PollingHosts.contains(uuid)) continue;

        const NvAddress addr = pollAddressFor(host);
        if (addr.isNull()) continue;

        // [NETWORK] diagnostic: trace backup serverinfo poll (every 60s).
        Logger::info(QString("[NETWORK] backup poll serverinfo HTTP -> %1 (%2)")
                         .arg(addr.toString(), host->name));

        QNetworkReply* reply = m_Http->getServerInfoAsync(addr, clientUniqueId());
        m_PendingPolls[reply] = uuid;
        m_PollingHosts.insert(uuid);
        m_PollStartedAt[uuid].start();

        connect(reply, &QNetworkReply::finished, this, &ComputerManager::onPollReplyFinished);

        QPointer<QNetworkReply> guard(reply);
        QTimer::singleShot(NvHTTP::FAST_FAIL_TIMEOUT_MS, reply, [guard]() {
            if (guard && !guard->isFinished()) guard->abort();
        });
    }
}

void ComputerManager::onPollReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    QString uuid = m_PendingPolls.take(reply);
    m_PollingHosts.remove(uuid);
    m_PollStartedAt.remove(uuid);
    if (uuid.isEmpty()) {
        reply->deleteLater();
        return;
    }

    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        reply->deleteLater();
        return;
    }

    bool changed = false;

    // Debounce: a single transient error (stale keep-alive, 2s abort, brief
    // network blip) must not flip a healthy host offline. Only go offline
    // after several consecutive failures.
    static constexpr int OFFLINE_FAILURE_THRESHOLD = 3;

    auto registerFailure = [&](const QString& reason) {
        // Try the next known address on the following tick. The one we just used
        // may simply be the wrong path to a host that is perfectly up — a stale
        // <LocalIP>, a bridge address, an IP the lease moved elsewhere.
        const int candidates = host->uniqueAddresses().size();
        if (candidates > 1)
            m_PollAddrIndex[uuid] = (m_PollAddrIndex.value(uuid, 0) + 1) % candidates;

        // Cap the counter at the threshold: once offline, keep polling silently
        // so a long-down host doesn't grow the counter or spam the log.
        if (host->consecutivePollFailures < OFFLINE_FAILURE_THRESHOLD) {
            host->consecutivePollFailures++;
            Logger::warning(QString("Poll failure %1/%2 for %3: %4")
                                .arg(host->consecutivePollFailures)
                                .arg(OFFLINE_FAILURE_THRESHOLD)
                                .arg(host->name, reason));
        }
        if (host->consecutivePollFailures >= OFFLINE_FAILURE_THRESHOLD &&
            host->state != NvComputer::CS_OFFLINE) {
            host->state = NvComputer::CS_OFFLINE;
            changed = true;
        }
    };

    if (reply->error() != QNetworkReply::NoError) {
        // A TCP reset (ConnectionRefused) means the machine answered at the IP
        // level but nothing is listening on the GameStream port — the host is
        // powered on, just not running MoonlightWeb/Sunshine. A timeout / host-
        // unreachable error means the machine itself is down. Surface that split
        // so the frontend can show "Unavailable" vs "Offline" + Wake-on-LAN.
        bool refused = (reply->error() == QNetworkReply::ConnectionRefusedError);
        if (host->reachable != refused) {
            host->reachable = refused;
            changed = true;
        }
        registerFailure(reply->errorString());
    } else {
        // The HTTP connection itself succeeded → the host is reachable, whatever
        // the response body turns out to say.
        if (!host->reachable) {
            host->reachable = true;
            changed = true;
        }

        QString xml = QString::fromUtf8(reply->readAll());

        try {
            NvHTTP::verifyResponseStatus(xml);

            NvAddress pollAddr(reply->url().host(), static_cast<quint16>(reply->url().port(47989)));

            NvComputer newState(xml, pollAddr);

            if (newState.uuid == host->uuid) {
                host->consecutivePollFailures = 0;
                // This address works: update() has just made it activeAddress, so
                // index 0 is it. Stay there until it stops answering.
                m_PollAddrIndex.remove(uuid);
                changed = host->update(newState);

                // Capture the MAC from ARP while reachable (Sunshine often
                // omits it) so Wake-on-LAN works later when the host is down.
                if (host->macAddress.isEmpty()) {
                    QByteArray mac = resolveMacFromArp(pollAddr.address());
                    if (!mac.isEmpty()) {
                        host->macAddress = mac;
                        changed = true;
                    }
                }

                // Work out for ourselves whether this host has a MultiSeat
                // control API, so that nobody ever has to declare what their
                // machine runs. Once per process per host: the answer does not
                // change under a running service, and this rides along on the
                // poll of every host on the network.
                if (!m_MultiSeatProbed.contains(uuid)) {
                    m_MultiSeatProbed.insert(uuid);
                    QPointer<ComputerManager> self(this);
                    BackendProbe::probeMultiSeat(
                        m_Nam, pollAddr.address(), [self, uuid](bool present) {
                            if (!self || !present) return;
                            NvComputer* h = self->findHostByUuid(uuid);
                            if (!h || h->multiSeatApiPresent) return;
                            h->multiSeatApiPresent = true;
                            Logger::info(
                                QString("[Backend] MultiSeat control API answered on %1 — its "
                                        "setup will be offered on this host")
                                    .arg(h->name));
                            emit self->hostsChanged();
                        });
                }
            }
        } catch (const std::exception& e) {
            registerFailure(e.what());
        }
    }

    if (changed) {
        saveHosts();
        emit hostsChanged();
    }

    reply->deleteLater();
}

// --- mDNS Discovery ---------------------------------------------------------

void ComputerManager::startMdnsDiscovery()
{
    // Already browsing — just extend the current window instead of rebinding.
    if (m_MdnsActive) {
        m_MdnsWindowTimer->start(MDNS_DISCOVERY_WINDOW_MS);
        return;
    }

    m_MdnsServer = new QMdnsEngine::Server(this);

    // QMdnsEngine reports a refused UDP 5353 bind through this signal, never by
    // throwing — the try/catch that used to sit here could not fire. Unconnected,
    // a discovery that bound nothing still logged "window opened" and then found
    // no host, which reads as "there is no Sunshine on this network".
    connect(m_MdnsServer, &QMdnsEngine::Server::error, this, [](const QString& message) {
        Logger::warning(QString("[NETWORK] mDNS socket error: %1 — discovery will find nothing")
                            .arg(message));
    });

    m_MdnsBrowser =
        new QMdnsEngine::Browser(m_MdnsServer, "_nvstream._tcp.local.", nullptr, this);

    connect(m_MdnsBrowser, &QMdnsEngine::Browser::serviceAdded, this,
            &ComputerManager::onMdnsServiceAdded);

    m_MdnsSeenThisWindow = 0;
    m_MdnsActive = true;
    m_MdnsWindowTimer->start(MDNS_DISCOVERY_WINDOW_MS);
    Logger::info("[NETWORK] mDNS discovery window opened — UDP 5353 bound (_nvstream._tcp.local.)");
}

void ComputerManager::stopMdnsDiscovery()
{
    if (!m_MdnsActive && !m_MdnsServer) return;

    // Cancel pending hostname resolutions first — they hold a pointer to the
    // mDNS server and must not outlive it.
    qDeleteAll(m_PendingResolutions);
    m_PendingResolutions.clear();

    delete m_MdnsBrowser;
    m_MdnsBrowser = nullptr;
    delete m_MdnsServer; // releases UDP 5353
    m_MdnsServer = nullptr;

    m_MdnsActive = false;
    Logger::info(QString("[NETWORK] mDNS discovery window closed — UDP 5353 released "
                         "(%1 service(s) seen)")
                     .arg(m_MdnsSeenThisWindow));
}

void ComputerManager::onMdnsServiceAdded(const QMdnsEngine::Service& service)
{
    m_MdnsSeenThisWindow++;
    Logger::info(QString("mDNS host discovered: %1").arg(QString::fromUtf8(service.hostname())));

    MdnsPendingComputer* pending = new MdnsPendingComputer(m_MdnsServer, service);
    connect(pending, &MdnsPendingComputer::resolvedHost, this, &ComputerManager::onMdnsResolved);

    m_PendingResolutions.append(pending);
}

// Among the IPv4 addresses an mDNS host advertises, pick the one most likely
// reachable. A multi-homed Sunshine machine advertises every NIC — including
// virtual adapters (VirtualBox 192.168.56.x, Hyper-V, etc.) the router cannot
// route to. Prefer a candidate on the same subnet as our default-route LAN
// interface so the real LAN address always wins over a virtual one; taking the
// first IPv4 blindly silently switches activeAddress to a dead interface.
static QHostAddress chooseBestMdnsAddress(const QVector<QHostAddress>& addresses)
{
    QVector<QHostAddress> ipv4;
    for (const QHostAddress& a : addresses)
        if (a.protocol() == QAbstractSocket::IPv4Protocol) ipv4.append(a);
    if (ipv4.size() <= 1) return ipv4.isEmpty() ? QHostAddress() : ipv4.first();

    // Default-route local IP: connecting a UDP socket makes the OS pick the
    // routable interface, never a host-only/virtual adapter (no packet sent).
    QUdpSocket probe;
    probe.connectToHost(QHostAddress("8.8.8.8"), 53);
    QHostAddress localIp = probe.localAddress();
    probe.close();

    if (!localIp.isNull()) {
        int prefix = 24; // sane default if the interface entry is not found
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces())
            for (const QNetworkAddressEntry& e : iface.addressEntries())
                if (e.ip() == localIp && e.prefixLength() > 0) prefix = e.prefixLength();

        for (const QHostAddress& cand : ipv4)
            if (cand.isInSubnet(localIp, prefix))
                return cand; // same subnet as our routable LAN — best match
    }

    return ipv4.first();
}

void ComputerManager::onMdnsResolved(MdnsPendingComputer* computer,
                                     const QVector<QHostAddress>& addresses)
{
    m_PendingResolutions.removeOne(computer);

    QHostAddress best = chooseBestMdnsAddress(addresses);
    if (!best.isNull()) {
        NvAddress nvAddr(best, computer->port());
        tryAddHostFromAddress(nvAddr, true, computer->hostname());
    }

    computer->deleteLater();
}

// --- Host management --------------------------------------------------------

void ComputerManager::tryAddHostFromAddress(const NvAddress& addr, bool fromMdns,
                                            const QString& name)
{
    Q_UNUSED(fromMdns)
    Q_UNUSED(name)

    QNetworkReply* reply = m_Http->getServerInfoAsync(addr, clientUniqueId());

    connect(reply, &QNetworkReply::finished, this, [this, reply, addr]() {
        if (reply->error() == QNetworkReply::NoError) {
            QString xml = QString::fromUtf8(reply->readAll());

            try {
                NvHTTP::verifyResponseStatus(xml);
                addOrUpdateHost(xml, addr);
            } catch (const std::exception& e) {
                Logger::warning(
                    QString("Failed to query host at %1: %2").arg(addr.toString(), e.what()));
            }
        }

        reply->deleteLater();
    });

    QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(NvHTTP::REQUEST_TIMEOUT_MS, reply, [guard]() {
        if (guard && !guard->isFinished()) guard->abort();
    });
}

void ComputerManager::addOrUpdateHost(const QString& serverInfo, const NvAddress& addr)
{
    NvComputer newHost(serverInfo, addr);

    if (newHost.uuid.isEmpty()) return;

    NvComputer* existing = findHostByUuid(newHost.uuid);

    if (existing) {
        // Merge new data into existing host
        if (existing->update(newHost)) {
            saveHosts();
            emit hostsChanged();

            Logger::info(QString("Host updated: %1 (%2)").arg(existing->name, existing->uuid));
        }
    } else {
        // New host discovered
        NvComputer* computer = new NvComputer(newHost);
        m_Hosts[computer->uuid] = computer;
        saveHosts();
        emit hostsChanged();

        Logger::info(QString("Host added: %1 (%2) at %3")
                         .arg(computer->name, computer->uuid, addr.toString()));
    }
}

NvComputer* ComputerManager::findHostByUuid(const QString& uuid) const
{
    return m_Hosts.value(uuid, nullptr);
}

NvComputer* ComputerManager::getHost(const QString& uuid) const
{
    return m_Hosts.value(uuid, nullptr);
}

// --- REST API methods -------------------------------------------------------

namespace {

/// Whether this machine has a Sunshine we can stop and start. Probed once: it
/// walks well-known install paths, and /api/hosts asks per host on every poll.
bool localSunshinePresent()
{
    static const bool present = SunshineInstaller::detect().installed;
    return present;
}

} // namespace

QJsonObject ComputerManager::backendCapabilitiesJson(const QString& uuid) const
{
    // Capabilities come from an actual provider instance rather than a lookup
    // table, so a backend cannot advertise something its code does not do. An
    // unmanaged host has none, which is what keeps a Sunshine card plain.
    NvComputer* host = findHostByUuid(uuid);
    if (!host || host->backendType.isEmpty()) return {};
    std::unique_ptr<IStreamBackend> backend = backendForHost(uuid);
    if (!backend) return {};
    const BackendCapabilities caps = backend->capabilities();
    return QJsonObject{{"multiUser", caps.multiUser},
                       {"provisioning", caps.provisioning},
                       {"lobbies", caps.lobbies},
                       {"restartService", caps.restartService}};
}

QJsonArray ComputerManager::getHostsJson() const
{
    QJsonArray arr;
    for (auto it = m_Hosts.cbegin(); it != m_Hosts.cend(); ++it) {
        QJsonObject obj = it.value()->toJson();
        // The frontend keys backend affordances (hiding Share on a co-op host,
        // offering seat management) off capabilities, and it reads them from the
        // host list — not from the per-host /backend call — so they have to ride
        // along here, or every host looks capability-less and the gates misfire.
        const QJsonObject caps = backendCapabilitiesJson(it.key());
        if (!caps.isEmpty()) obj["capabilities"] = caps;
        // Whether the kebab can offer a restart at all. Computed here rather
        // than in NvComputer::toJson() because it is not a property of the host:
        // it is whether *we* hold a way to bounce its service without asking
        // anyone for a password.
        obj["restartSupported"] = it.value()->isLocalMachine()
                                      ? localSunshinePresent()
                                      : caps.value("restartService").toBool();
        arr.append(obj);
    }
    return arr;
}

void ComputerManager::handleScanRequest()
{
    // Prevent overlapping scans (3 s cooldown)
    QDateTime now = QDateTime::currentDateTime();
    if (m_LastScanTime.isValid() && m_LastScanTime.msecsTo(now) < 3000) {
        Logger::info("Scan request skipped — cooldown active");
        return;
    }
    m_LastScanTime = now;

    // Open a fresh mDNS discovery window (extends it if already active).
    startMdnsDiscovery();

    // Also trigger a poll tick immediately
    onPollTick();

    Logger::info("Scan started");
}

std::pair<int, QJsonObject> ComputerManager::handleAddManualHost(const QString& address)
{
    // Parse address: "IP" or "IP:port"
    QString addrStr = address.trimmed();
    quint16 port = MW_HTTP_PORT;

    int colonIdx = addrStr.lastIndexOf(':');
    if (colonIdx > 0) {
        bool ok;
        quint16 parsed = addrStr.mid(colonIdx + 1).toUShort(&ok);
        if (ok) {
            port = parsed;
            addrStr = addrStr.left(colonIdx);
        }
    }

    QHostAddress hostAddr(addrStr);
    if (hostAddr.isNull()) {
        // Sync DNS resolution
        QHostInfo dnsResult;
        QEventLoop dnsLoop;
        QHostInfo::lookupHost(addrStr, &dnsLoop, [&](const QHostInfo& info) {
            dnsResult = info;
            dnsLoop.quit();
        });
        QTimer::singleShot(5000, &dnsLoop, &QEventLoop::quit);
        dnsLoop.exec(QEventLoop::ExcludeUserInputEvents);

        if (dnsResult.addresses().isEmpty()) {
            return {400, {{"status", "error"}, {"message", "DNS resolution failed"}}};
        }

        for (const QHostAddress& addr : dnsResult.addresses()) {
            if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
                hostAddr = addr;
                break;
            }
        }
        if (hostAddr.isNull()) hostAddr = dnsResult.addresses().first();
    }

    NvAddress nvAddr(hostAddr, port);

    // Sync HTTP query via local QNetworkAccessManager
    QUrl url;
    url.setScheme("http");
    url.setHost(nvAddr.address());
    url.setPort(nvAddr.port());
    url.setPath("/serverinfo");
    url.setQuery("uniqueid=" + clientUniqueId() +
                 "&uuid=" + QUuid::createUuid().toString(QUuid::WithoutBraces));

    QNetworkRequest req(url);
    req.setTransferTimeout(NvHTTP::REQUEST_TIMEOUT_MS);
    req.setRawHeader("User-Agent", "MoonlightWeb/0.1");

    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(NvHTTP::REQUEST_TIMEOUT_MS, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    if (!reply->isFinished()) reply->abort();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        delete reply;
        return {400, {{"status", "error"}, {"message", err}}};
    }

    QString xml = QString::fromUtf8(reply->readAll());
    delete reply;

    try {
        NvHTTP::verifyResponseStatus(xml);
    } catch (const std::exception& e) {
        return {400, {{"status", "error"}, {"message", e.what()}}};
    }

    addOrUpdateHost(xml, nvAddr);

    // Look up the resulting host to return its JSON
    NvComputer parsedHost(xml, nvAddr);
    NvComputer* host = findHostByUuid(parsedHost.uuid);
    if (!host) {
        return {500,
                {{"status", "error"}, {"message", "Host was added but could not be retrieved"}}};
    }

    // Remember it as the manual address. This is the operator's answer to a host
    // discovery cannot reach, so it has to outlive the process — until now it
    // only landed in activeAddress and was gone at the next restart.
    if (host->manualAddress != nvAddr) {
        host->manualAddress = nvAddr;
        saveHosts();
    }

    Logger::info(
        QString("Manual host added: %1 (%2) at %3").arg(host->name, host->uuid, nvAddr.toString()));

    QJsonObject obj;
    QJsonArray arr;
    arr.append(host->toJson());
    obj["hosts"] = arr;
    return {200, obj};
}

std::pair<int, QJsonObject> ComputerManager::handleDeleteHost(const QString& uuid)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) return {404, {{"status", "error"}, {"message", "Host not found"}}};

    QString name = host->name;
    m_Hosts.remove(uuid);
    m_PollAddrIndex.remove(uuid);
    delete host;
    saveHosts(/*allowEmpty=*/true);
    emit hostsChanged();

    Logger::info(QString("Host removed: %1 (%2)").arg(name, uuid));

    QJsonObject result;
    result["status"] = "ok";
    result["message"] = QString("Host '%1' removed").arg(name);
    return {200, result};
}

// --- Rename ------------------------------------------------------------------

// The alias lives here and nowhere else. Renaming a host on the host itself
// would mean holding its web-UI password, which MoonlightWeb refuses to do, and
// it would also be useless on the cards that need a name most: an offline or
// never-paired host, which cannot be asked anything at all.
std::pair<int, QJsonObject> ComputerManager::handleRenameHost(const QString& uuid,
                                                             const QString& name)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) return {404, {{"status", "error"}, {"message", "Host not found"}}};

    // Simplify() rather than trimmed(): a name is one line, and a pasted one can
    // carry newlines or runs of spaces that would break the card's layout.
    QString alias = name.simplified();
    if (alias.length() > kMaxHostNameLength) alias.truncate(kMaxHostNameLength);

    if (alias == host->customName) {
        // Nothing to write, but the answer still carries the current state: the
        // caller patches its copy from it, so a field missing here would read as
        // "the alias is gone".
        return {200,
                {{"status", "ok"},
                 {"name", alias.isEmpty() ? host->name : alias},
                 {"customName", alias}}};
    }

    host->customName = alias;
    saveHosts();
    emit hostsChanged();

    Logger::info(alias.isEmpty()
                     ? QString("Host %1 renamed back to \"%2\"").arg(uuid, host->name)
                     : QString("Host %1 renamed to \"%2\"").arg(uuid, alias));

    QJsonObject result;
    result["status"] = "ok";
    result["name"] = alias.isEmpty() ? host->name : alias;
    result["customName"] = alias;
    return {200, result};
}

// --- Wake-on-LAN -------------------------------------------------------------

// Resolve a host's MAC from the OS ARP cache. Sunshine often omits the MAC in
// serverinfo, so we capture it from ARP while the host is reachable (the entry
// is guaranteed present right after a successful poll). Returns 6 bytes, or
// empty if unavailable. Windows-only; other platforms return empty.
QByteArray ComputerManager::resolveMacFromArp(const QString& ip)
{
#ifdef Q_OS_WIN
    QHostAddress addr(ip);
    if (addr.protocol() != QAbstractSocket::IPv4Protocol) return {};

    quint32 host = addr.toIPv4Address();
    // SendARP expects the IPv4 address in network byte order.
    unsigned long destIp = ((host & 0x000000FFu) << 24) | ((host & 0x0000FF00u) << 8) |
                           ((host & 0x00FF0000u) >> 8) | ((host & 0xFF000000u) >> 24);

    unsigned char mac[8] = {0};
    unsigned long macLen = 6;
    if (SendARP(destIp, 0, mac, &macLen) == 0 /* NO_ERROR */ && macLen == 6) {
        QByteArray out(reinterpret_cast<char*>(mac), 6);
        if (out != QByteArray(6, '\0')) // reject all-zero
            return out;
    }
    return {};
#else
    Q_UNUSED(ip)
    return {};
#endif
}

std::pair<int, QJsonObject> ComputerManager::handleWakeHost(const QString& uuid)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) return {404, {{"status", "error"}, {"message", "Host not found"}}};

    if (host->macAddress.size() != 6)
        return {400, {{"status", "error"}, {"message", "No MAC address known for this host"}}};

    // Magic packet: 6×0xFF followed by 16 repetitions of the 6-byte MAC.
    QByteArray packet;
    packet.append(6, static_cast<char>(0xFF));
    for (int i = 0; i < 16; ++i)
        packet.append(host->macAddress);

    // Targets: global broadcast + subnet-directed broadcast derived from the
    // host's known addresses (a sleeping host won't answer ARP, so unicast to
    // its last IP is useless — broadcasting is what wakes it).
    QSet<quint32> targets;
    targets.insert(QHostAddress(QHostAddress::Broadcast).toIPv4Address());

    auto addSubnetBroadcast = [&](const NvAddress& a) {
        if (a.address().isEmpty()) return;
        QHostAddress h(a.address());
        if (h.protocol() != QAbstractSocket::IPv4Protocol) return;
        targets.insert(h.toIPv4Address() | 0x000000FF); // assume /24
    };
    addSubnetBroadcast(host->localAddress);
    addSubnetBroadcast(host->activeAddress);

    // WoL is conventionally sent to UDP discard (9) and echo (7).
    static const quint16 wolPorts[] = {9, 7};

    QUdpSocket socket;
    bool sentAny = false;
    for (quint32 ip : targets) {
        for (quint16 port : wolPorts) {
            if (socket.writeDatagram(packet, QHostAddress(ip), port) == packet.size())
                sentAny = true;
        }
    }

    if (!sentAny)
        return {500, {{"status", "error"}, {"message", "Failed to send Wake-on-LAN packet"}}};

    Logger::info(QString("Wake-on-LAN sent to %1 (%2)")
                     .arg(host->name, QString::fromUtf8(host->macAddress.toHex(':'))));

    return {
        200,
        {{"status", "ok"}, {"message", QString("Wake-on-LAN packet sent to %1").arg(host->name)}}};
}

// --- Client unique ID -------------------------------------------------------

QString ComputerManager::clientUniqueId()
{
    return IdentityManager::get()->getUniqueId();
}

// --- Pairing ----------------------------------------------------------------
// Client generates a random PIN, user enters it in Sunshine (stdin or Web UI).


std::pair<int, QJsonObject> ComputerManager::handleStartPairing(const QString& uuid)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "Host not found";
        return {404, err};
    }

    if (host->state != NvComputer::CS_ONLINE) {
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "Host is offline";
        return {400, err};
    }

    if (host->pairState == NvComputer::PS_PAIRED) {
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "Already paired";
        return {400, err};
    }

    // Clean up any abandoned previous session. If a pairing chain is still in
    // flight against it, freeing the NvPairingManager would leave its pending
    // async callbacks dangling (use-after-free) — return the PIN already in use
    // instead (it is bound to the in-flight stage-1 salt).
    if (m_ActivePairings.contains(uuid)) {
        if (m_SubmitInFlight.contains(uuid)) {
            QJsonObject obj;
            obj["status"] = "initiated";
            obj["pin"] = m_PairingPins.value(uuid);
            obj["message"] = "Enter this PIN in Sunshine (Web UI or stdin)";
            return {200, obj};
        }
        delete m_ActivePairings.take(uuid);
        m_PairingPins.remove(uuid);
    }
    m_PairingError.remove(uuid);

    QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) {
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "Host has no reachable address";
        return {400, err};
    }

    const NvAddress& addr = addrs.first();

    // Generate PIN — returned immediately, no network call
    QString pin = PairingChain::generatePin();
    m_PairingPins[uuid] = pin;

    // Create PairingManager (stage 1 runs later, in the background chain kicked
    // off by the first handleSubmitPin poll)
    auto* pm =
        new NvPairingManager(host->appVersion, addr.address(), addr.port(),
                             host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT);
    m_ActivePairings[uuid] = pm;

    Logger::info(QString("Pairing initiated for %1, PIN: %2").arg(uuid, pin));

    QJsonObject obj;
    obj["status"] = "initiated";
    obj["pin"] = pin;
    obj["message"] = "Enter this PIN in Sunshine (Web UI or stdin)";
    return {200, obj};
}

void ComputerManager::handleSubmitPin(const QString& uuid, ResponseCallback respond)
{
    auto reply = [&respond](const char* status, const QString& message, int code) {
        respond(HttpResponse::json({{"status", status}, {"message", message}}, code));
    };

    // A background chain finished with a terminal error — deliver it once.
    if (m_PairingError.contains(uuid)) {
        reply("error", m_PairingError.take(uuid), 400);
        return;
    }

    // Already paired (the chain completed on a previous poll).
    NvComputer* host = findHostByUuid(uuid);
    if (host && host->pairState == NvComputer::PS_PAIRED) {
        reply("paired", "Already paired.", 200);
        return;
    }

    // A pairing chain is running for this host — keep polling.
    if (m_SubmitInFlight.contains(uuid)) {
        reply("awaiting_pin", "Pairing already in progress...", 200);
        return;
    }

    auto it = m_ActivePairings.find(uuid);
    if (it == m_ActivePairings.end()) {
        // No session and no terminal result — a PIN still on file means a start
        // is expected; otherwise the caller must start pairing first.
        if (m_PairingPins.contains(uuid)) {
            reply("awaiting_pin", "Waiting for pairing to complete...", 200);
            return;
        }
        reply("error", "No active pairing session. Start pairing first.", 404);
        return;
    }

    if (m_PairingPins.value(uuid).isEmpty()) {
        reply("error", "No PIN found for this session. Start pairing again.", 400);
        return;
    }

    // Kick off the async pairing chain and answer immediately. Stage 1
    // (getservercert) blocks up to 60s server-side until the user enters the
    // PIN in Sunshine; the frontend keeps polling every 5s and picks up the
    // terminal result (paired / error) on a later poll. No request is held open
    // and no nested event loop runs in the HTTP dispatch path.
    startPairingChain(uuid);
    reply("awaiting_pin", "Waiting for PIN to be entered in Sunshine...", 200);
}

void ComputerManager::startPairingChain(const QString& uuid)
{
    if (m_SubmitInFlight.contains(uuid)) return; // one chain per host

    auto it = m_ActivePairings.find(uuid);
    if (it == m_ActivePairings.end()) return;
    NvPairingManager* pm = it.value();

    m_SubmitInFlight.insert(uuid);

    // Sequencing lives in PairingChain, shared with the backends that pair
    // without a human. Everything below is this manager's own policy: the
    // wording the user sees, the NvComputer bookkeeping, and session teardown.
    //
    // No announcer: on a Sunshine host a person is already reading the PIN off
    // our UI and typing it in.
    PairingChain::run(pm, m_PairingPins.value(uuid), {},
                      [this, uuid](const PairingChain::Result& result) {
        // Re-resolve the session: while in flight nothing frees it, but the host
        // could still be gone if the whole ComputerManager path changed.
        auto it = m_ActivePairings.find(uuid);
        if (it == m_ActivePairings.end()) {
            m_SubmitInFlight.remove(uuid);
            return;
        }
        NvPairingManager* pm = it.value();

        switch (result.outcome) {
        case PairingChain::Outcome::Paired: {
            NvComputer* host = findHostByUuid(uuid);
            if (host) {
                host->serverCertPem = result.serverCertPem;
                host->pairState = NvComputer::PS_PAIRED;
                host->state = NvComputer::CS_ONLINE;
                saveHosts();
                emit hostsChanged();
            }
            m_ActivePairings.erase(it);
            m_PairingPins.remove(uuid);
            delete pm;
            break;
        }

        case PairingChain::Outcome::Retry:
            // Stage 1 never landed, or the PIN was not accepted yet — keep the
            // session so the next poll restarts the chain.
            break;

        case PairingChain::Outcome::HostBusy:
            m_ActivePairings.erase(it);
            m_PairingPins.remove(uuid);
            delete pm;
            m_PairingError[uuid] = "Pairing already in progress on host.";
            break;

        case PairingChain::Outcome::Failed:
        default:
            m_ActivePairings.erase(it);
            m_PairingPins.remove(uuid);
            delete pm;
            m_PairingError[uuid] =
                "Pairing failed. Close any running games on the host and try again.";
            break;
        }

        m_SubmitInFlight.remove(uuid);
    });
}

void ComputerManager::handleSetBackend(const QString& uuid, const QString& type,
                                       const QString& apiUrl, const QString& apiToken,
                                       const QString& pairUser, const QString& pairPassword,
                                       ResponseCallback respond)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }

    if (!StreamBackendRegistry::instance().isRegistered(type)) {
        respond(HttpResponse::json(
            {{"status", "error"},
             {"message", QStringLiteral("Unknown backend type '%1'. Known: %2")
                             .arg(type, StreamBackendRegistry::instance().knownTypes().join(
                                            QStringLiteral(", ")))}},
            400));
        return;
    }

    // An empty token means "keep the stored one", so the dialog can be reopened
    // to fix a URL without making the admin retype a secret the browser was
    // never shown.
    const QString effectiveToken = apiToken.isEmpty() ? host->backendApiToken : apiToken;
    const QString effectivePairPassword =
        pairPassword.isEmpty() ? host->backendPairPassword : pairPassword;

    // A MultiSeat we detected ourselves needs no URL from anyone: its control
    // API sits on a fixed port of the host we are already polling. Asking for an
    // address the server can work out is the kind of question this whole feature
    // exists to stop asking — and the browser is never told a host's address, so
    // it could not have filled the field in even if we wanted it to.
    QString effectiveUrl = apiUrl;
    if (effectiveUrl.isEmpty() && type == QStringLiteral("multiseat") &&
        host->multiSeatApiPresent && !host->activeAddress.isNull()) {
        effectiveUrl = QStringLiteral("http://%1:%2")
                           .arg(host->activeAddress.address())
                           .arg(BackendProbe::kMultiSeatApiPort);
    }

    // Build the candidate from the values being proposed, WITHOUT storing them
    // yet. Persisting first would let a failed attempt — a mistyped key is the
    // obvious one — overwrite a configuration that was working.
    QJsonObject config;
    config[QStringLiteral("hostUuid")] = uuid;
    config[QStringLiteral("apiUrl")] = effectiveUrl;
    config[QStringLiteral("apiToken")] = effectiveToken;
    config[QStringLiteral("pairUser")] = pairUser;
    config[QStringLiteral("pairPassword")] = effectivePairPassword;

    std::shared_ptr<IStreamBackend> backend(
        StreamBackendRegistry::instance().create(type, config).release());
    if (!backend) {
        respond(HttpResponse::json(
            {{"status", "error"}, {"message", "Could not build a backend for this host"}}, 500));
        return;
    }

    // Persist the entered configuration right away, whether or not the control
    // API answers. The admin asked to point this host at this URL; recording it
    // and then reporting reachability — with the seat panel reflecting an
    // unreachable backend as empty — is clearer than silently keeping the old
    // value. The trade-off is that a mistyped URL replaces the working one; the
    // escape hatch is "Stop managing", which clears the config entirely.
    if (NvComputer* h = findHostByUuid(uuid)) {
        h->backendType = type;
        h->backendApiUrl = effectiveUrl;
        h->backendApiToken = effectiveToken;
        h->backendPairUser = pairUser;
        h->backendPairPassword = effectivePairPassword;
        saveHosts();
        emit hostsChanged();
    }

    // Pair right away. Registering a backend is the one admin gesture allowed to
    // involve a human, and it is precisely what spares every player a PIN. A new
    // host's server certificate is committed by the backend itself (m_Commit)
    // when the handshake succeeds; the config above is already stored regardless.
    backend->ensurePaired(
        [respond = std::move(respond), backend](bool ok, const BackendError& err) {
            if (ok) {
                respond(HttpResponse::json(
                    {{"status", "paired"}, {"message", "Backend registered and paired"}}, 200));
            } else {
                respond(HttpResponse::json({{"status", "error"}, {"message", err.message}}, 502));
            }

            // Release a turn later. Dropping the last reference here would
            // destroy the backend — and any NvPairingManager it owns — from
            // inside that manager's own callback, which is still on the stack.
            // That is the lifetime rule NvPairingManager states and that
            // m_SubmitInFlight enforces on the Sunshine path.
            QTimer::singleShot(0, [backend]() {});
        });
}

namespace {

QJsonObject seatToJson(const SeatRef& seat)
{
    return QJsonObject{{"id", seat.id},
                       {"name", seat.name},
                       {"address", seat.address},
                       {"httpPort", seat.httpPort},
                       {"httpsPort", seat.httpsPort},
                       {"busy", seat.busy}};
}

/// Answer a backend failure the way the rest of the API does, mapping the
/// interface's error kinds onto the statuses the frontend already handles.
HttpResponse backendFailure(const BackendError& err)
{
    int status = 502;
    switch (err.kind) {
    case BackendError::NotFound:
        status = 404;
        break;
    case BackendError::Unsupported:
        // The backend is fine; it just does not offer this. Distinct from a
        // failure so the UI can hide the control rather than show an error.
        status = 501;
        break;
    case BackendError::NotPaired:
        status = 401;
        break;
    case BackendError::Timeout:
        status = 504;
        break;
    default:
        break;
    }
    return HttpResponse::json({{"status", "error"}, {"message", err.message}}, status);
}

} // namespace

void ComputerManager::handleListSeats(const QString& uuid, ResponseCallback respond)
{
    std::shared_ptr<IStreamBackend> backend(backendForHost(uuid).release());
    if (!backend) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }

    backend->listSeats([respond = std::move(respond), backend](bool ok, const BackendError& err,
                                                               const QVector<SeatRef>& seats) {
        if (!ok) {
            respond(backendFailure(err));
        } else {
            QJsonArray arr;
            for (const SeatRef& seat : seats) {
                arr.append(seatToJson(seat));
            }
            respond(HttpResponse::json({{"seats", arr}}));
        }
        // Released a turn later: see handleSetBackend for why.
        QTimer::singleShot(0, [backend]() {});
    });
}

void ComputerManager::handleProvisionSeat(const QString& uuid, const QJsonObject& params,
                                          ResponseCallback respond)
{
    std::shared_ptr<IStreamBackend> backend(backendForHost(uuid).release());
    if (!backend) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }

    backend->provisionSeat(params, [respond = std::move(respond), backend](
                                       bool ok, const BackendError& err, const SeatRef& seat) {
        if (!ok) {
            respond(backendFailure(err));
        } else {
            respond(HttpResponse::json({{"status", "ok"}, {"seat", seatToJson(seat)}}, 201));
        }
        QTimer::singleShot(0, [backend]() {});
    });
}

void ComputerManager::handleReleaseSeatOwner(const QString& uuid, const QString& seatId,
                                             ResponseCallback respond)
{
    std::shared_ptr<IStreamBackend> backend(backendForHost(uuid).release());
    if (!backend) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }

    backend->releaseSeatOwner(seatId, [respond = std::move(respond), backend](
                                          bool ok, const BackendError& err) {
        if (!ok) {
            respond(backendFailure(err));
        } else {
            respond(HttpResponse::json({{"status", "ok"}}));
        }
        QTimer::singleShot(0, [backend]() {});
    });
}

void ComputerManager::handleTeardownSeat(const QString& uuid, const QString& seatId,
                                         ResponseCallback respond)
{
    std::shared_ptr<IStreamBackend> backend(backendForHost(uuid).release());
    if (!backend) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }

    backend->teardownSeat(seatId, [respond = std::move(respond), backend](bool ok,
                                                                          const BackendError& err) {
        if (!ok) {
            respond(backendFailure(err));
        } else {
            respond(HttpResponse::json({{"status", "ok"}}));
        }
        QTimer::singleShot(0, [backend]() {});
    });
}

// The restart never travels with a credential we asked the user for. Two paths
// only: the machine we run on (its Sunshine is ours to stop and start), and a
// backend whose own control API can do it — MultiSeat bouncing each seat's
// Apollo, for instance. Everything else answers 501, which is what keeps the
// menu entry absent rather than broken.
//
// Deliberately not localhost-only: the browser asking for it is already an
// authenticated MoonlightWeb client, and the person most likely to need this is
// exactly the one who is not sitting at the host.
void ComputerManager::handleRestartHost(const QString& uuid, ResponseCallback respond)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }

    if (host->isLocalMachine()) {
        if (!localSunshinePresent()) {
            respond(HttpResponse::json(
                {{"status", "error"}, {"message", "No Sunshine install found on this machine"}},
                501));
            return;
        }

        Logger::info(QStringLiteral("Restarting the local Sunshine (host %1)").arg(uuid));
        SunshineInstaller::stop();
        // Answered only once the new instance has been spawned, so the UI's
        // spinner covers the gap rather than ending before the host goes away.
        QTimer::singleShot(RESTART_RELAUNCH_DELAY_MS, this, [respond = std::move(respond)]() {
            const bool ok = SunshineInstaller::launch();
            if (!ok) {
                Logger::error(QStringLiteral("Sunshine did not come back after a restart"));
                respond(HttpResponse::json(
                    {{"status", "error"}, {"message", "Sunshine did not restart"}}, 502));
                return;
            }
            respond(HttpResponse::json({{"status", "ok"}}));
        });
        return;
    }

    std::shared_ptr<IStreamBackend> backend(backendForHost(uuid).release());
    if (!backend) {
        respond(HttpResponse::json(
            {{"status", "error"},
             {"message", "This host has no control API MoonlightWeb can restart it through"}},
            501));
        return;
    }

    backend->restartService([respond = std::move(respond), backend](bool ok,
                                                                    const BackendError& err) {
        if (!ok) {
            respond(backendFailure(err));
        } else {
            respond(HttpResponse::json({{"status", "ok"}}));
        }
        // Released a turn later: see handleSetBackend for why.
        QTimer::singleShot(0, [backend]() {});
    });
}

namespace {

/// Both listings answer the same shape, so they share one adapter.
void respondWithJsonArray(ResponseCallback respond, const QString& key,
                          std::shared_ptr<IStreamBackend> backend, bool ok,
                          const BackendError& err, const QJsonArray& items)
{
    if (!ok) {
        respond(backendFailure(err));
    } else {
        respond(HttpResponse::json({{key, items}}));
    }
    QTimer::singleShot(0, [backend]() {});
}

} // namespace

void ComputerManager::handleListProfiles(const QString& uuid, ResponseCallback respond)
{
    std::shared_ptr<IStreamBackend> backend(backendForHost(uuid).release());
    if (!backend) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }
    backend->listProfiles([respond = std::move(respond), backend](
                              bool ok, const BackendError& err, const QJsonArray& items) mutable {
        respondWithJsonArray(std::move(respond), QStringLiteral("profiles"), backend, ok, err,
                             items);
    });
}

void ComputerManager::handleListLobbies(const QString& uuid, ResponseCallback respond)
{
    std::shared_ptr<IStreamBackend> backend(backendForHost(uuid).release());
    if (!backend) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }
    backend->listLobbies([respond = std::move(respond), backend](
                             bool ok, const BackendError& err, const QJsonArray& items) mutable {
        respondWithJsonArray(std::move(respond), QStringLiteral("lobbies"), backend, ok, err, items);
    });
}

std::pair<int, QJsonObject> ComputerManager::handleGetBackend(const QString& uuid) const
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        return {404, QJsonObject{{"status", "error"}, {"message", "Host not found"}}};
    }

    QJsonObject obj;
    obj["type"] = host->backendType;
    obj["apiUrl"] = host->backendApiUrl;
    // The token is write-only; the browser only ever learns that one is stored.
    obj["configured"] = !host->backendApiToken.isEmpty();

    // Same capabilities the host list carries, read off a real provider instance
    // so it cannot claim what the code does not do. Absent for an unmanaged host.
    const QJsonObject caps = backendCapabilitiesJson(uuid);
    if (!caps.isEmpty()) obj["capabilities"] = caps;

    return {200, obj};
}

std::pair<int, QJsonObject> ComputerManager::handleClearBackend(const QString& uuid)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        return {404, QJsonObject{{"status", "error"}, {"message", "Host not found"}}};
    }

    host->backendType.clear();
    host->backendApiUrl.clear();
    host->backendApiToken.clear();
    host->backendPairUser.clear();
    host->backendPairPassword.clear();
    saveHosts();
    emit hostsChanged();

    // The GameStream pairing is left intact on purpose: the host carries on as
    // a plain Sunshine-style host, which is the point of unmanaging it rather
    // than deleting it.
    return {200, QJsonObject{{"status", "ok"}, {"message", "Backend management removed"}}};
}

bool ComputerManager::pairHostBlocking(const QString& uuid, int timeoutMs)
{
    if (!m_ActivePairings.contains(uuid)) return false;

    auto pairedNow = [this, uuid]() {
        NvComputer* host = findHostByUuid(uuid);
        return host && host->pairState == NvComputer::PS_PAIRED;
    };

    QEventLoop loop;

    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(timeoutMs);

    // Poll for a terminal state; if the chain goes idle (stage-1 timeout or a
    // rejected PIN) while the session survives, restart it for another attempt.
    QTimer poll;
    poll.setInterval(250);
    connect(&poll, &QTimer::timeout, this, [this, uuid, &loop, pairedNow]() {
        if (pairedNow() || m_PairingError.contains(uuid)) {
            loop.quit();
            return;
        }
        if (!m_SubmitInFlight.contains(uuid) && m_ActivePairings.contains(uuid))
            startPairingChain(uuid);
    });
    poll.start();

    startPairingChain(uuid);
    loop.exec();

    return pairedNow();
}

// --- HTTPS Pair Verification -------------------------------------------------

void ComputerManager::onPairCheckFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    // Force-evict the pooled TLS socket. Qt ignores a request-side
    // "Connection: close" (hop-by-hop header it manages itself) and Sunshine
    // answers keep-alive, so the socket would otherwise sit Established ~120s
    // holding Sunshine's single-threaded HTTPS server and cycling co-located
    // native clients offline. The response body is already buffered in `reply`.
    m_Nam->clearConnectionCache();

    QString uuid = reply->property("mwHostUuid").toString();
    m_PendingPairChecks.remove(uuid);
    if (uuid.isEmpty()) {
        reply->deleteLater();
        return;
    }

    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        reply->deleteLater();
        return;
    }

    bool pairLost = false;

    if (reply->error() == QNetworkReply::NoError) {
        QString xml = QString::fromUtf8(reply->readAll());
        try {
            NvHTTP::verifyResponseStatus(xml);
        } catch (const std::exception&) {
            // 401 → pairing genuinely lost
            pairLost = true;
        }
    }
    // Network errors (timeout, connection refused) → don't touch pairState

    if (pairLost && host->pairState == NvComputer::PS_PAIRED) {
        host->pairState = NvComputer::PS_NOT_PAIRED;
        host->serverCertPem.clear();
        saveHosts();
        emit hostsChanged();
        Logger::info(QString("Pairing lost for host: %1").arg(host->name));
    }

    reply->deleteLater();
}

// --- Box Art -----------------------------------------------------------------

// Box art is the slowest thing on a host card: each PNG is a separate HTTPS
// round trip to Sunshine, serialized one at a time per host, so a ten-app grid
// fills in over several seconds. Nothing about a cover changes between visits
// (a new cover comes with a new appid, hence a new URL), so the browser is
// allowed to keep it: a validator plus a day of freshness turns every later
// page load into zero requests, and the day after into a bodyless 304 served
// from RAM. `private` keeps it out of shared caches — the grid says which games
// a host has.
static QString boxArtETag(const QByteArray& png)
{
    return QStringLiteral("\"%1\"").arg(QString::fromLatin1(
        QCryptographicHash::hash(png, QCryptographicHash::Md5).toHex()));
}

static void applyBoxArtCacheHeaders(HttpResponse& resp, const QString& etag)
{
    resp.headers["ETag"] = etag;
    resp.headers["Cache-Control"] = "private, max-age=86400";
}

void ComputerManager::handleGetBoxArt(const QString& uuid, int appId, const QString& ifNoneMatch,
                                      ResponseCallback respond)
{
    // Serve from cache if available
    auto hostIt = m_BoxArtCache.find(uuid);
    if (hostIt != m_BoxArtCache.end()) {
        auto artIt = hostIt->find(appId);
        if (artIt != hostIt->end()) {
            const QString etag = boxArtETag(*artIt);
            if (!ifNoneMatch.isEmpty() && ifNoneMatch.contains(etag)) {
                HttpResponse notModified;
                notModified.statusCode = 304;
                applyBoxArtCacheHeaders(notModified, etag);
                respond(notModified);
                return;
            }
            HttpResponse resp;
            resp.statusCode = 200;
            resp.contentType = "image/png";
            resp.body = *artIt;
            applyBoxArtCacheHeaders(resp, etag);
            respond(resp);
            return;
        }
    }

    // Validate host
    NvComputer* host = findHostByUuid(uuid);
    if (!host || host->pairState != NvComputer::PS_PAIRED || host->serverCertPem.isEmpty()) {
        respond(HttpResponse::error(404, "Host not found or not paired"));
        return;
    }

    if (host->uniqueAddresses().isEmpty()) {
        respond(HttpResponse::error(404, "Host has no reachable address"));
        return;
    }

    // Dedup: if already fetching this exact appId, just append the callback
    auto& pendingList = m_PendingBoxArtCallbacks[uuid][appId];
    bool alreadyFetchingThisApp = !pendingList.isEmpty();
    pendingList.append(std::move(respond));

    if (alreadyFetchingThisApp) return;

    // Serialize: only one HTTPS fetch per host at a time
    enqueueBoxArtFetch(uuid, appId);
}

// --- Box art fetch queue (serialize HTTPS requests per host) ---

void ComputerManager::enqueueBoxArtFetch(const QString& uuid, int appId)
{
    if (m_ActiveBoxArtFetches.contains(uuid)) {
        m_BoxArtFetchQueue[uuid].append(appId);
    } else {
        m_ActiveBoxArtFetches.insert(uuid);
        startBoxArtFetch(uuid, appId);
    }
}

void ComputerManager::startBoxArtFetch(const QString& uuid, int appId)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        onBoxArtFetchComplete(uuid, appId, false);
        return;
    }

    QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) {
        onBoxArtFetchComplete(uuid, appId, false);
        return;
    }

    quint16 httpsPort = host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT;
    const NvAddress& addr = addrs.first();

    IdentityManager* im = IdentityManager::get();
    QByteArray cert = im->getCertificate();
    QByteArray key = im->getPrivateKey();

    QUrl artUrl(QString("https://%1:%2/appasset?appid=%3&uniqueid=%4&uuid=%5")
                    .arg(addr.address())
                    .arg(httpsPort)
                    .arg(appId)
                    .arg(IdentityManager::get()->getUniqueId(),
                         QUuid::createUuid().toString(QUuid::WithoutBraces)));

    QNetworkRequest artReq(artUrl);
    artReq.setTransferTimeout(5000);
    artReq.setRawHeader("User-Agent", "MoonlightWeb/0.1");
    // Close immediately — don't leave the TLS socket pooled ~120s holding
    // Sunshine's single-threaded HTTPS server (see NvHTTP::getAppListAsync).
    artReq.setRawHeader("Connection", "close");

    QSslConfiguration sslConfig = artReq.sslConfiguration();
    sslConfig.setLocalCertificate(QSslCertificate(cert, QSsl::Pem));
    sslConfig.setPrivateKey(QSslKey(key, QSsl::Rsa, QSsl::Pem));
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    artReq.setSslConfiguration(sslConfig);

    QNetworkReply* artReply = m_Nam->get(artReq);

    QTimer::singleShot(5000, artReply, [artReply]() {
        if (!artReply->isFinished()) artReply->abort();
    });

    connect(artReply, &QNetworkReply::finished, this, [this, uuid, appId, artReply]() {
        bool ok = false;
        if (artReply->error() == QNetworkReply::NoError) {
            QByteArray data = artReply->readAll();
            if (!data.isEmpty()) {
                m_BoxArtCache[uuid][appId] = data;
                ok = true;
            }
        }
        artReply->deleteLater();
        // Evict the pooled TLS socket — see onPairCheckFinished().
        m_Nam->clearConnectionCache();
        onBoxArtFetchComplete(uuid, appId, ok);
    });
}

void ComputerManager::onBoxArtFetchComplete(const QString& uuid, int appId, bool ok)
{
    // Notify all callbacks waiting for this appId
    auto callbacks = m_PendingBoxArtCallbacks[uuid].take(appId);
    if (m_PendingBoxArtCallbacks[uuid].isEmpty()) m_PendingBoxArtCallbacks.remove(uuid);

    QByteArray pngData = m_BoxArtCache.value(uuid).value(appId);

    for (const auto& cb : callbacks) {
        if (ok && !pngData.isEmpty()) {
            HttpResponse resp;
            resp.statusCode = 200;
            resp.contentType = "image/png";
            resp.body = pngData;
            applyBoxArtCacheHeaders(resp, boxArtETag(pngData));
            cb(resp);
        } else {
            cb(HttpResponse::error(502, "Failed to fetch box art"));
        }
    }

    // Process next queued appId for this host
    auto it = m_BoxArtFetchQueue.find(uuid);
    if (it != m_BoxArtFetchQueue.end() && !it->isEmpty()) {
        int nextAppId = it->takeFirst();
        startBoxArtFetch(uuid, nextAppId);
    } else {
        m_ActiveBoxArtFetches.remove(uuid);
        // Try to continue background pre-fetching
        fetchNextBoxArtInBackground(uuid);
    }
}

void ComputerManager::fetchNextBoxArtInBackground(const QString& uuid)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) return;

    const QVector<NvApp>& apps = host->appList;

    // Find first uncached, not-pending app
    for (const auto& app : apps) {
        int appId = app.id();
        if (!m_BoxArtCache.value(uuid).contains(appId) &&
            !m_PendingBoxArtCallbacks.value(uuid).contains(appId)) {
            // Mark as pending (empty callbacks → background fetch, no HTTP consumer)
            m_PendingBoxArtCallbacks[uuid][appId];
            enqueueBoxArtFetch(uuid, appId);
            return;
        }
    }
}

// --- App list -----------------------------------------------------------------

// HTTP shaping only: the GameStream conversation (request, TLS pool eviction,
// timeout, XML parsing) lives in GameStreamBackend. What stays here is the host
// bookkeeping the backend deliberately does not own — dropping the pair state on
// a 401, caching the app list, kicking off box-art prefetch — plus the exact
// response shapes this route has always returned.
void ComputerManager::handleGetAppList(const QString& uuid, const QString& deviceSessionId,
                                      ResponseCallback respond)
{
    auto backend = std::shared_ptr<IStreamBackend>(backendForHost(uuid));
    if (!backend) {
        respond(HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }

    // Whose app list this is. A plain host answers with itself, so this is the
    // same call it has always made; a multi-seat backend resolves the seat the
    // asking user owns, because its apps live on that seat's own Apollo.
    backend->allocateSeat(deviceSessionId, [this, uuid, respond, backend](
                                               bool seatOk, const BackendError& seatErr,
                                               const SeatRef& seat) {
    if (!seatOk) {
        respond(backendFailure(seatErr));
        return;
    }

    // `backend` is captured so it outlives the async call.
    backend->getAppList(seat.id, [this, uuid, respond, backend](bool ok, const BackendError& err,
                                                             const QVector<NvApp>& apps) {
        if (!ok) {
            // Pre-flight failure: the host was already gone when we looked.
            if (err.kind == BackendError::NotFound) {
                respond(
                    HttpResponse::json({{"status", "error"}, {"message", "Host not found"}}, 404));
                return;
            }

            // The NvComputer may have been deleted (handleDeleteHost) during the
            // async request.
            NvComputer* host = findHostByUuid(uuid);
            if (!host) {
                respond(HttpResponse::error(404, "Host removed while fetching app list"));
                return;
            }

            switch (err.kind) {
            case BackendError::NotPaired:
                // httpStatus 0 = we never sent the request; 401 = the host
                // rejected our certificate mid-flight.
                if (err.httpStatus != 401) {
                    respond(HttpResponse::json(
                        {{"status", "error"}, {"message", "Host not paired"}}, 400));
                    return;
                }
                Logger::warning(
                    QString("App list fetch failed for %1: %2").arg(host->name, err.message));
                if (host->pairState == NvComputer::PS_PAIRED) {
                    host->pairState = NvComputer::PS_NOT_PAIRED;
                    host->serverCertPem.clear();
                    saveHosts();
                    emit hostsChanged();
                    respond(HttpResponse::json(
                        {{"status", "error"},
                         {"message", "Host is no longer paired. Please pair again."}},
                        401));
                } else {
                    respond(HttpResponse::error(502, err.message));
                }
                return;

            case BackendError::NoAddress:
                respond(HttpResponse::json(
                    {{"status", "error"}, {"message", "Host has no reachable address"}}, 400));
                return;

            case BackendError::Timeout:
                respond(HttpResponse::error(504, "App list request timed out"));
                return;

            case BackendError::Protocol:
                // Bad XML / non-OK status in the payload: no warning line here,
                // matching the original behaviour.
                respond(HttpResponse::error(502, err.message));
                return;

            default:
                Logger::warning(
                    QString("App list fetch failed for %1: %2").arg(host->name, err.message));
                respond(HttpResponse::error(502, err.message));
                return;
            }
        }

        NvComputer* host = findHostByUuid(uuid);
        if (!host) {
            respond(HttpResponse::error(404, "Host removed while fetching app list"));
            return;
        }

        host->appList = apps;

        // Start background box art pre-fetching
        if (!apps.isEmpty()) fetchNextBoxArtInBackground(uuid);

        QJsonArray appsArr;
        for (const auto& app : apps)
            appsArr.append(app.toJson());

        QJsonObject result;
        result["status"] = "ok";
        result["apps"] = appsArr;
        respond(HttpResponse::json(result));
    });
    });
}

// Qt MOC needs to see the MdnsPendingComputer definition
#include "ComputerManager.moc"
