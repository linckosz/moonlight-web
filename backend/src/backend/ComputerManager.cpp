#include "ComputerManager.h"
#include "NvComputer.h"
#include "NvPairingManager.h"
#include "IdentityManager.h"
#include "common/Logger.h"

#include <qmdnsengine/server.h>
#include <qmdnsengine/browser.h>
#include <qmdnsengine/service.h>
#include <qmdnsengine/resolver.h>

#include <QHostInfo>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <QCoreApplication>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QPointer>
#include <QEventLoop>
#include <QTimer>
#include <QUuid>

#include <QUdpSocket>
#include <QHostAddress>

#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>

#include <utility>

#ifdef Q_OS_WIN
// Declared locally to avoid winsock2/windows.h include-ordering conflicts with
// Qt headers. Links against iphlpapi (already in LIBS). IPAddr/DWORD/ULONG are
// all unsigned long.
extern "C" __declspec(dllimport) unsigned long __stdcall SendARP(
    unsigned long DestIP, unsigned long SrcIP,
    void* pMacAddr, unsigned long* PhysAddrLen);
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

// ============================================================================
// MdnsPendingComputer — Resolves mDNS hostname → addresses
// ============================================================================

class MdnsPendingComputer : public QObject
{
    Q_OBJECT

public:
    MdnsPendingComputer(QMdnsEngine::Server* server,
                        const QMdnsEngine::Service& service)
        : m_Hostname(service.hostname())
        , m_Port(service.port())
        , m_Server(server)
    {
        resolve();
    }

    ~MdnsPendingComputer()
    {
        delete m_Resolver;
    }

    QString hostname() const { return m_Hostname; }
    quint16 port() const { return m_Port; }

signals:
    void resolvedHost(MdnsPendingComputer* computer,
                      const QVector<QHostAddress>& addresses);

private slots:
    void handleResolvedAddress(const QHostAddress& addr)
    {
        m_Addresses.append(addr);
    }

    void handleResolvedTimeout()
    {
        if (!m_Addresses.isEmpty())
            emit resolvedHost(this, m_Addresses);
        else if (m_Retries-- > 0)
            resolve();
        else
            emit resolvedHost(this, m_Addresses);  // empty → host unreachable

        if (m_Addresses.isEmpty() && m_Retries <= 0)
            deleteLater();
    }

private:
    void resolve()
    {
        delete m_Resolver;
        m_Resolver = new QMdnsEngine::Resolver(m_Server, m_Hostname);
        connect(m_Resolver, &QMdnsEngine::Resolver::resolved,
                this, &MdnsPendingComputer::handleResolvedAddress);
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
    connect(m_MdnsWindowTimer, &QTimer::timeout,
            this, &ComputerManager::stopMdnsDiscovery);

    // NOTE: mDNS is NOT started at boot. Binding UDP 5353 at idle steals mDNS
    // traffic from any other client on the same machine (Moonlight Qt, or
    // Sunshine's own responder when co-located), so we only open a short mDNS
    // window on an explicit scan request. Known hosts stay fresh via HTTP
    // polling, which never touches 5353.

    Logger::info(QString("ComputerManager initialized: %1 hosts loaded (mDNS idle)")
                     .arg(m_Hosts.size()));
}

// --- Persistence -----------------------------------------------------------

void ComputerManager::loadHosts()
{
    QSettings settings;
    int count = settings.beginReadArray(SER_HOSTS);

    for (int i = 0; i < count; i++) {
        settings.setArrayIndex(i);

        NvComputer* computer = new NvComputer(settings);
        if (!computer->uuid.isEmpty()) {
            m_Hosts[computer->uuid] = computer;
        } else {
            delete computer;
        }
    }

    settings.endArray();
}

void ComputerManager::saveHosts()
{
    QSettings settings;
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

void ComputerManager::onPollTick()
{
    // Suspend polling while a stream session is active: a co-located native
    // client owning the session polls nothing, and our extra connection churn
    // (Connection: close + 2s abort) wedges Sunshine's single-threaded HTTP
    // server during encode, making the host appear offline to other clients.
    if (m_StreamActivePredicate && m_StreamActivePredicate())
        return;

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
        if (host->pairState == NvComputer::PS_PAIRED && !host->serverCertPem.isEmpty()
            && !m_PendingPairChecks.contains(uuid)) {
            QDateTime now = QDateTime::currentDateTime();
            if (!m_LastPairCheck.contains(uuid)
                || m_LastPairCheck[uuid].secsTo(now) >= PAIR_CHECK_INTERVAL_SEC) {
                m_LastPairCheck[uuid] = now;
                QVector<NvAddress> addrs = host->uniqueAddresses();
                if (!addrs.isEmpty()) {
                    IdentityManager* im = IdentityManager::get();
                    quint16 httpsPort = host->activeHttpsPort > 0
                        ? host->activeHttpsPort : MW_HTTPS_PORT;
                    m_PendingPairChecks.insert(uuid);
                    // [NETWORK] diagnostic: trace HTTPS pair-check (every 5 min).
                    Logger::info(QString("[NETWORK] pair-check applist HTTPS -> %1:%2 (%3)")
                                     .arg(addrs.first().address()).arg(httpsPort).arg(host->name));
                    QNetworkReply* reply = m_Http->getAppListAsync(
                        addrs.first(), httpsPort,
                        im->getCertificate(), im->getPrivateKey());
                    reply->setProperty("mwHostUuid", uuid);
                    connect(reply, &QNetworkReply::finished,
                            this, &ComputerManager::onPairCheckFinished);
                }
            }
        }

        // Skip if this host is already being polled (robust tracking via QSet)
        if (m_PollingHosts.contains(uuid))
            continue;

        QVector<NvAddress> addrs = host->uniqueAddresses();
        if (addrs.isEmpty())
            continue;

        // Use first address for polling
        NvAddress addr = addrs.first();

        // [NETWORK] diagnostic: trace every outbound serverinfo poll.
        Logger::info(QString("[NETWORK] poll serverinfo HTTP -> %1 (%2)")
                         .arg(addr.toString(), host->name));

        QNetworkReply* reply = m_Http->getServerInfoAsync(addr, clientUniqueId());
        m_PendingPolls[reply] = uuid;
        m_PollingHosts.insert(uuid);
        m_PollStartedAt[uuid].start();

        connect(reply, &QNetworkReply::finished,
                this, &ComputerManager::onPollReplyFinished);

        // 2-second timeout (abort reply, onPollReplyFinished will fire)
        QPointer<QNetworkReply> guard(reply);
        QTimer::singleShot(NvHTTP::FAST_FAIL_TIMEOUT_MS, reply, [guard]() {
            if (guard && !guard->isFinished())
                guard->abort();
        });
    }
}

void ComputerManager::onBackupPollTick()
{
    // Suspend during an active stream — see onPollTick().
    if (m_StreamActivePredicate && m_StreamActivePredicate())
        return;

    Logger::debug("Backup poll tick — safety-net refresh of idle hosts");

    // Safety net for hosts the regular tick somehow never scheduled. Skips any
    // host already in flight: a second concurrent connection to a single-
    // threaded Sunshine server wedges it. Stalled in-flight polls are already
    // cleaned up by onPollTick (>10s), so this never stays stuck.
    for (auto it = m_Hosts.begin(); it != m_Hosts.end(); ++it) {
        const QString& uuid = it.key();
        NvComputer* host = it.value();

        if (m_PollingHosts.contains(uuid))
            continue;

        QVector<NvAddress> addrs = host->uniqueAddresses();
        if (addrs.isEmpty())
            continue;

        NvAddress addr = addrs.first();

        // [NETWORK] diagnostic: trace backup serverinfo poll (every 60s).
        Logger::info(QString("[NETWORK] backup poll serverinfo HTTP -> %1 (%2)")
                         .arg(addr.toString(), host->name));

        QNetworkReply* reply = m_Http->getServerInfoAsync(addr, clientUniqueId());
        m_PendingPolls[reply] = uuid;
        m_PollingHosts.insert(uuid);
        m_PollStartedAt[uuid].start();

        connect(reply, &QNetworkReply::finished,
                this, &ComputerManager::onPollReplyFinished);

        QPointer<QNetworkReply> guard(reply);
        QTimer::singleShot(NvHTTP::FAST_FAIL_TIMEOUT_MS, reply, [guard]() {
            if (guard && !guard->isFinished())
                guard->abort();
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
        // Cap the counter at the threshold: once offline, keep polling silently
        // so a long-down host doesn't grow the counter or spam the log.
        if (host->consecutivePollFailures < OFFLINE_FAILURE_THRESHOLD) {
            host->consecutivePollFailures++;
            Logger::warning(QString("Poll failure %1/%2 for %3: %4")
                                .arg(host->consecutivePollFailures)
                                .arg(OFFLINE_FAILURE_THRESHOLD)
                                .arg(host->name, reason));
        }
        if (host->consecutivePollFailures >= OFFLINE_FAILURE_THRESHOLD
            && host->state != NvComputer::CS_OFFLINE) {
            host->state = NvComputer::CS_OFFLINE;
            changed = true;
        }
    };

    if (reply->error() != QNetworkReply::NoError) {
        registerFailure(reply->errorString());
    } else {
        QString xml = QString::fromUtf8(reply->readAll());

        try {
            NvHTTP::verifyResponseStatus(xml);

            NvAddress pollAddr(
                reply->url().host(),
                static_cast<quint16>(reply->url().port(47989)));

            NvComputer newState(xml, pollAddr);

            if (newState.uuid == host->uuid) {
                host->consecutivePollFailures = 0;
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

    try {
        m_MdnsServer = new QMdnsEngine::Server(this);
        m_MdnsBrowser = new QMdnsEngine::Browser(m_MdnsServer,
                                                  "_nvstream._tcp.local.",
                                                  nullptr, this);

        connect(m_MdnsBrowser, &QMdnsEngine::Browser::serviceAdded,
                this, &ComputerManager::onMdnsServiceAdded);

        m_MdnsActive = true;
        m_MdnsWindowTimer->start(MDNS_DISCOVERY_WINDOW_MS);
        Logger::info("[NETWORK] mDNS discovery window opened — UDP 5353 bound (_nvstream._tcp.local.)");
    } catch (const std::exception& e) {
        Logger::warning(QString("mDNS discovery unavailable: %1").arg(e.what()));
    }
}

void ComputerManager::stopMdnsDiscovery()
{
    if (!m_MdnsActive && !m_MdnsServer)
        return;

    // Cancel pending hostname resolutions first — they hold a pointer to the
    // mDNS server and must not outlive it.
    qDeleteAll(m_PendingResolutions);
    m_PendingResolutions.clear();

    delete m_MdnsBrowser;
    m_MdnsBrowser = nullptr;
    delete m_MdnsServer;  // releases UDP 5353
    m_MdnsServer = nullptr;

    m_MdnsActive = false;
    Logger::info("[NETWORK] mDNS discovery window closed — UDP 5353 released");
}

void ComputerManager::onMdnsServiceAdded(const QMdnsEngine::Service& service)
{
    Logger::info(QString("mDNS host discovered: %1").arg(QString::fromUtf8(service.hostname())));

    MdnsPendingComputer* pending = new MdnsPendingComputer(m_MdnsServer, service);
    connect(pending, &MdnsPendingComputer::resolvedHost,
            this, &ComputerManager::onMdnsResolved);

    m_PendingResolutions.append(pending);
}

void ComputerManager::onMdnsResolved(MdnsPendingComputer* computer,
                                      const QVector<QHostAddress>& addresses)
{
    m_PendingResolutions.removeOne(computer);

    // Filter for IPv4 addresses
    for (const QHostAddress& addr : addresses) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
            NvAddress nvAddr(addr, computer->port());
            tryAddHostFromAddress(nvAddr, true, computer->hostname());
            break;  // Use first IPv4 address only
        }
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
                Logger::warning(QString("Failed to query host at %1: %2")
                                 .arg(addr.toString(), e.what()));
            }
        }

        reply->deleteLater();
    });

    QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(NvHTTP::REQUEST_TIMEOUT_MS, reply, [guard]() {
        if (guard && !guard->isFinished())
            guard->abort();
    });
}

void ComputerManager::addOrUpdateHost(const QString& serverInfo, const NvAddress& addr)
{
    NvComputer newHost(serverInfo, addr);

    if (newHost.uuid.isEmpty())
        return;

    NvComputer* existing = findHostByUuid(newHost.uuid);

    if (existing) {
        // Merge new data into existing host
        if (existing->update(newHost)) {
            saveHosts();
            emit hostsChanged();

            Logger::info(QString("Host updated: %1 (%2)")
                             .arg(existing->name, existing->uuid));
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

QJsonArray ComputerManager::getHostsJson() const
{
    QJsonArray arr;
    for (auto it = m_Hosts.cbegin(); it != m_Hosts.cend(); ++it) {
        arr.append(it.value()->toJson());
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
        if (hostAddr.isNull())
            hostAddr = dnsResult.addresses().first();
    }

    NvAddress nvAddr(hostAddr, port);

    // Sync HTTP query via local QNetworkAccessManager
    QUrl url;
    url.setScheme("http");
    url.setHost(nvAddr.address());
    url.setPort(nvAddr.port());
    url.setPath("/serverinfo");
    url.setQuery("uniqueid=" + clientUniqueId()
                 + "&uuid=" + QUuid::createUuid().toString(QUuid::WithoutBraces));

    QNetworkRequest req(url);
    req.setTransferTimeout(NvHTTP::REQUEST_TIMEOUT_MS);
    req.setRawHeader("User-Agent", "Moonlight-Web/0.1");

    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(NvHTTP::REQUEST_TIMEOUT_MS, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    if (!reply->isFinished())
        reply->abort();

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
        return {500, {{"status", "error"},
                {"message", "Host was added but could not be retrieved"}}};
    }

    Logger::info(QString("Manual host added: %1 (%2) at %3")
                     .arg(host->name, host->uuid, nvAddr.toString()));

    QJsonObject obj;
    QJsonArray arr;
    arr.append(host->toJson());
    obj["hosts"] = arr;
    return {200, obj};
}

std::pair<int, QJsonObject> ComputerManager::handleDeleteHost(const QString& uuid)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host)
        return {404, {{"status", "error"}, {"message", "Host not found"}}};

    QString name = host->name;
    m_Hosts.remove(uuid);
    delete host;
    saveHosts();
    emit hostsChanged();

    Logger::info(QString("Host removed: %1 (%2)").arg(name, uuid));

    QJsonObject result;
    result["status"] = "ok";
    result["message"] = QString("Host '%1' removed").arg(name);
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
    if (addr.protocol() != QAbstractSocket::IPv4Protocol)
        return {};

    quint32 host = addr.toIPv4Address();
    // SendARP expects the IPv4 address in network byte order.
    unsigned long destIp =
        ((host & 0x000000FFu) << 24) | ((host & 0x0000FF00u) << 8) |
        ((host & 0x00FF0000u) >> 8)  | ((host & 0xFF000000u) >> 24);

    unsigned char mac[8] = {0};
    unsigned long macLen = 6;
    if (SendARP(destIp, 0, mac, &macLen) == 0 /* NO_ERROR */ && macLen == 6) {
        QByteArray out(reinterpret_cast<char*>(mac), 6);
        if (out != QByteArray(6, '\0'))  // reject all-zero
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
    if (!host)
        return {404, {{"status", "error"}, {"message", "Host not found"}}};

    if (host->macAddress.size() != 6)
        return {400, {{"status", "error"},
                {"message", "No MAC address known for this host"}}};

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
        targets.insert(h.toIPv4Address() | 0x000000FF);  // assume /24
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
        return {500, {{"status", "error"},
                {"message", "Failed to send Wake-on-LAN packet"}}};

    Logger::info(QString("Wake-on-LAN sent to %1 (%2)")
                     .arg(host->name, QString::fromUtf8(host->macAddress.toHex(':'))));

    return {200, {{"status", "ok"},
            {"message", QString("Wake-on-LAN packet sent to %1").arg(host->name)}}};
}

// --- Client unique ID -------------------------------------------------------

QString ComputerManager::clientUniqueId()
{
    return IdentityManager::get()->getUniqueId();
}

// --- Pairing ----------------------------------------------------------------
// Client generates a random PIN, user enters it in Sunshine (stdin or Web UI).

static QString generatePairingPin()
{
    int pin = QRandomGenerator::global()->bounded(10000);
    return QString::asprintf("%04d", pin);
}

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

    // Clean up any abandoned previous session
    if (m_ActivePairings.contains(uuid)) {
        delete m_ActivePairings.take(uuid);
        m_PairingPins.remove(uuid);
    }

    QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) {
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "Host has no reachable address";
        return {400, err};
    }

    NvAddress addr = addrs.first();

    // Generate PIN — returned immediately, no network call
    QString pin = generatePairingPin();
    m_PairingPins[uuid] = pin;

    // Create PairingManager (stage 1 runs later in handleSubmitPin)
    auto* pm = new NvPairingManager(
        host->appVersion,
        addr.address(),
        addr.port(),
        host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT);
    m_ActivePairings[uuid] = pm;

    Logger::info(QString("Pairing initiated for %1, PIN: %2").arg(uuid, pin));

    QJsonObject obj;
    obj["status"] = "initiated";
    obj["pin"] = pin;
    obj["message"] = "Enter this PIN in Sunshine (Web UI or stdin)";
    return {200, obj};
}

std::pair<int, QJsonObject> ComputerManager::handleSubmitPin(const QString& uuid)
{
    auto it = m_ActivePairings.find(uuid);
    if (it == m_ActivePairings.end()) {
        // Session already destroyed — check if pairing completed
        NvComputer* host = findHostByUuid(uuid);
        if (host && host->pairState == NvComputer::PS_PAIRED) {
            QJsonObject obj;
            obj["status"] = "paired";
            obj["message"] = "Already paired.";
            return {200, obj};
        }
        // PIN exists means a parallel request may have succeeded; keep polling
        if (m_PairingPins.contains(uuid)) {
            QJsonObject obj;
            obj["status"] = "awaiting_pin";
            obj["message"] = "Waiting for pairing to complete...";
            return {200, obj};
        }
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "No active pairing session. Start pairing first.";
        return {404, err};
    }

    QString pin = m_PairingPins.value(uuid);
    if (pin.isEmpty()) {
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "No PIN found for this session. Start pairing again.";
        return {400, err};
    }

    NvPairingManager* pm = it.value();
    QByteArray serverCertPem;

    // Stage 1: getservercert — BLOCKS until user enters PIN in Sunshine (up to 60s)
    NvPairingManager::InitResult initResult;
    try {
        initResult = pm->initiatePairing();
    } catch (const std::exception& e) {
        m_ActivePairings.erase(it);
        m_PairingPins.remove(uuid);
        delete pm;
        QJsonObject obj;
        obj["status"] = "awaiting_pin";
        obj["message"] = QString("Still waiting for PIN: %1").arg(e.what());
        return {200, obj};
    }

    if (initResult == NvPairingManager::INIT_ALREADY_IN_PROGRESS) {
        m_ActivePairings.erase(it);
        m_PairingPins.remove(uuid);
        delete pm;
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "Pairing already in progress on host.";
        return {409, err};
    }

    if (initResult != NvPairingManager::INIT_OK) {
        // Stage 1 failed (timeout or unreachable) — keep session for retry
        QJsonObject obj;
        obj["status"] = "awaiting_pin";
        obj["message"] = "Waiting for PIN to be entered in Sunshine...";
        return {200, obj};
    }

    // Stages 2-5: challenge-response
    NvPairingManager::PairState result;
    try {
        result = pm->completePairing(pin, serverCertPem);
    } catch (const std::exception& e) {
        m_ActivePairings.erase(it);
        m_PairingPins.remove(uuid);
        delete pm;
        QJsonObject err;
        err["status"] = "error";
        err["message"] = QString("Pairing error: %1").arg(e.what());
        return {400, err};
    }

    switch (result) {
    case NvPairingManager::PAIRED: {
        NvComputer* host = findHostByUuid(uuid);
        if (host) {
            host->serverCertPem = serverCertPem;
            host->pairState = NvComputer::PS_PAIRED;
            host->state = NvComputer::CS_ONLINE;
            saveHosts();
            emit hostsChanged();
        }

        m_ActivePairings.erase(it);
        m_PairingPins.remove(uuid);
        delete pm;

        QJsonObject obj;
        obj["status"] = "paired";
        obj["message"] = "Pairing successful. The host list will update shortly.";
        return {200, obj};
    }
    case NvPairingManager::PIN_WRONG: {
        // PIN was entered but wrong, or stages 2-3 failed — keep session for retry
        QJsonObject obj;
        obj["status"] = "awaiting_pin";
        obj["message"] = "Waiting for PIN to be entered in Sunshine...";
        return {200, obj};
    }
    case NvPairingManager::ALREADY_IN_PROGRESS:
    case NvPairingManager::FAILED:
    default: {
        m_ActivePairings.erase(it);
        m_PairingPins.remove(uuid);
        delete pm;
        QJsonObject err;
        err["status"] = "error";
        err["message"] = "Pairing failed. Close any running games on the host and try again.";
        return {400, err};
    }
    }
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

void ComputerManager::handleGetBoxArt(const QString& uuid, int appId,
                                       ResponseCallback respond)
{
    // Serve from cache if available
    auto hostIt = m_BoxArtCache.find(uuid);
    if (hostIt != m_BoxArtCache.end()) {
        auto artIt = hostIt->find(appId);
        if (artIt != hostIt->end()) {
            HttpResponse resp;
            resp.statusCode = 200;
            resp.contentType = "image/png";
            resp.body = *artIt;
            respond(resp);
            return;
        }
    }

    // Validate host
    NvComputer* host = findHostByUuid(uuid);
    if (!host || host->pairState != NvComputer::PS_PAIRED
        || host->serverCertPem.isEmpty()) {
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

    if (alreadyFetchingThisApp)
        return;

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

    quint16 httpsPort = host->activeHttpsPort > 0
        ? host->activeHttpsPort : MW_HTTPS_PORT;
    NvAddress addr = addrs.first();

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
    artReq.setRawHeader("User-Agent", "Moonlight-Web/0.1");
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
        if (!artReply->isFinished())
            artReply->abort();
    });

    connect(artReply, &QNetworkReply::finished, this,
            [this, uuid, appId, artReply]() {
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
    if (m_PendingBoxArtCallbacks[uuid].isEmpty())
        m_PendingBoxArtCallbacks.remove(uuid);

    QByteArray pngData = m_BoxArtCache.value(uuid).value(appId);

    for (const auto& cb : callbacks) {
        if (ok && !pngData.isEmpty()) {
            HttpResponse resp;
            resp.statusCode = 200;
            resp.contentType = "image/png";
            resp.body = pngData;
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
    if (!host)
        return;

    const QVector<NvApp>& apps = host->appList;

    // Find first uncached, not-pending app
    for (const auto& app : apps) {
        int appId = app.id();
        if (!m_BoxArtCache.value(uuid).contains(appId)
            && !m_PendingBoxArtCallbacks.value(uuid).contains(appId)) {
            // Mark as pending (empty callbacks → background fetch, no HTTP consumer)
            m_PendingBoxArtCallbacks[uuid][appId];
            enqueueBoxArtFetch(uuid, appId);
            return;
        }
    }
}

// --- App list -----------------------------------------------------------------

void ComputerManager::handleGetAppList(const QString& uuid, ResponseCallback respond)
{
    NvComputer* host = findHostByUuid(uuid);
    if (!host) {
        respond(HttpResponse::json(
            {{"status", "error"}, {"message", "Host not found"}}, 404));
        return;
    }

    if (host->pairState != NvComputer::PS_PAIRED || host->serverCertPem.isEmpty()) {
        respond(HttpResponse::json(
            {{"status", "error"}, {"message", "Host not paired"}}, 400));
        return;
    }

    QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) {
        respond(HttpResponse::json(
            {{"status", "error"}, {"message", "Host has no reachable address"}}, 400));
        return;
    }

    IdentityManager* im = IdentityManager::get();
    QByteArray clientCertPem = im->getCertificate();
    QByteArray clientKeyPem = im->getPrivateKey();
    quint16 httpsPort = host->activeHttpsPort > 0
        ? host->activeHttpsPort : MW_HTTPS_PORT;
    NvAddress addr = addrs.first();

    QNetworkReply* reply = m_Http->getAppListAsync(
        addr, httpsPort, clientCertPem, clientKeyPem);

    auto responded = std::make_shared<bool>(false);
    auto safeRespond = [responded, respond = std::move(respond)](HttpResponse resp) {
        if (!*responded) {
            *responded = true;
            respond(resp);
        }
    };

    // Timeout safety
    QTimer::singleShot(NvHTTP::REQUEST_TIMEOUT_MS + 2000, reply,
                       [safeRespond]() {
        safeRespond(HttpResponse::error(504, "App list request timed out"));
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, uuid, safeRespond, reply]() {
        // Re-resolve host pointer — the NvComputer may have been deleted
        // (via handleDeleteHost) since handleGetAppList was called. Capturing
        // the raw host pointer would cause a use-after-free crash if the host
        // was removed during the async HTTPS request.
        NvComputer* host = findHostByUuid(uuid);
        if (!host) {
            safeRespond(HttpResponse::error(404, "Host removed while fetching app list"));
            reply->deleteLater();
            return;
        }

        int httpStatus = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            Logger::warning(QString("App list fetch failed for %1: %2")
                             .arg(host->name, reply->errorString()));

            if (httpStatus == 401 && host->pairState == NvComputer::PS_PAIRED) {
                host->pairState = NvComputer::PS_NOT_PAIRED;
                host->serverCertPem.clear();
                saveHosts();
                emit hostsChanged();
                safeRespond(HttpResponse::json({{"status", "error"},
                    {"message", "Host is no longer paired. Please pair again."}}, 401));
            } else {
                safeRespond(HttpResponse::error(502, reply->errorString()));
            }
            reply->deleteLater();
            return;
        }

        QString xml = QString::fromUtf8(reply->readAll());

        try {
            NvHTTP::verifyResponseStatus(xml);
        } catch (const std::exception& e) {
            safeRespond(HttpResponse::error(502, e.what()));
            reply->deleteLater();
            return;
        }

        QVector<NvApp> apps = NvHTTP::parseAppList(xml);
        host->appList = apps;

        // Start background box art pre-fetching
        if (!apps.isEmpty())
            fetchNextBoxArtInBackground(uuid);

        QJsonArray appsArr;
        for (const auto& app : apps)
            appsArr.append(app.toJson());

        QJsonObject result;
        result["status"] = "ok";
        result["apps"] = appsArr;
        safeRespond(HttpResponse::json(result));

        reply->deleteLater();
    });
}

// Qt MOC needs to see the MdnsPendingComputer definition
#include "ComputerManager.moc"
