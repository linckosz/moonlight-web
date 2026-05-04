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
#include <QCoreApplication>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QPointer>
#include <utility>

#define SER_HOSTS "hosts"

static const int POLL_INTERVAL_MS = 5000;

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
    startMdnsDiscovery();

    Logger::info(QString("ComputerManager initialized: %1 hosts loaded")
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
    m_PollTimer = new QTimer(this);
    m_PollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_PollTimer, &QTimer::timeout, this, &ComputerManager::onPollTick);
    m_PollTimer->start();
}

void ComputerManager::onPollTick()
{
    for (auto it = m_Hosts.begin(); it != m_Hosts.end(); ++it) {
        const QString& uuid = it.key();
        NvComputer* host = it.value();

        // Skip if already polling this host
        if (m_PendingPolls.values().contains(uuid))
            continue;

        QVector<NvAddress> addrs = host->uniqueAddresses();
        if (addrs.isEmpty())
            continue;

        // Use first address for polling
        NvAddress addr = addrs.first();

        QNetworkReply* reply = m_Http->getServerInfoAsync(addr, clientUniqueId());
        m_PendingPolls[reply] = uuid;

        connect(reply, &QNetworkReply::finished,
                this, &ComputerManager::onPollReplyFinished);

        // 2-second timeout
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

    if (reply->error() != QNetworkReply::NoError) {
        if (host->state != NvComputer::CS_OFFLINE) {
            host->state = NvComputer::CS_OFFLINE;
            changed = true;
        }
    } else {
        QString xml = QString::fromUtf8(reply->readAll());

        try {
            NvHTTP::verifyResponseStatus(xml);

            NvAddress pollAddr(
                reply->url().host(),
                static_cast<quint16>(reply->url().port(47989)));

            NvComputer newState(xml, pollAddr);

            if (newState.uuid == host->uuid) {
                changed = host->update(newState);
            }
        } catch (const std::exception& e) {
            Logger::warning(QString("Poll error for %1: %2").arg(host->name, e.what()));
            if (host->state != NvComputer::CS_OFFLINE) {
                host->state = NvComputer::CS_OFFLINE;
                changed = true;
            }
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
    try {
        m_MdnsServer = new QMdnsEngine::Server(this);
        m_MdnsBrowser = new QMdnsEngine::Browser(m_MdnsServer,
                                                  "_nvstream._tcp.local.",
                                                  nullptr, this);

        connect(m_MdnsBrowser, &QMdnsEngine::Browser::serviceAdded,
                this, &ComputerManager::onMdnsServiceAdded);

        m_MdnsActive = true;
        Logger::info("mDNS discovery started for _nvstream._tcp.local.");
    } catch (const std::exception& e) {
        Logger::warning(QString("mDNS discovery unavailable: %1").arg(e.what()));
    }
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

    // Use a one-shot connection: try once, don't add to polling cycle
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
    // Re-trigger mDNS browser. If already active, qmdnsengine handles dedup.
    if (!m_MdnsActive) {
        startMdnsDiscovery();
    }

    // Also trigger a poll tick immediately
    onPollTick();

    Logger::info("Manual scan requested");
}

void ComputerManager::handleAddManualHost(const QString& address)
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
        // Try DNS resolution
        QHostInfo::lookupHost(addrStr, this, [this, port](const QHostInfo& info) {
            if (!info.addresses().isEmpty()) {
                for (const QHostAddress& addr : info.addresses()) {
                    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
                        tryAddHostFromAddress(NvAddress(addr, port), false);
                        return;
                    }
                }
                // Use first resolved address
                tryAddHostFromAddress(
                    NvAddress(info.addresses().first(), port), false);
            } else {
                emit hostAddCompleted(false, "DNS resolution failed", QString());
            }
        });
        return;
    }

    NvAddress nvAddr(hostAddr, port);
    tryAddHostFromAddress(nvAddr, false);

    Logger::info(QString("Manual host add requested: %1").arg(nvAddr.toString()));
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

// Qt MOC needs to see the MdnsPendingComputer definition
#include "ComputerManager.moc"
