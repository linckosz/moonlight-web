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

#include "RouterPortAllocator.h"
#include "RouterHoles.h"
#include "PortIdentity.h"
#include "RouterPortPools.h"
#include "SocketTables.h"
#include "UPNPClient.h"
#include "common/Edition.h"
#include "common/Logger.h"
#include "server/AppSettings.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <memory>

namespace {

/// The router as UPNPClient exposes it, behind the policy's seam.
class UpnpGatewayAdapter : public IUpnpGateway
{
public:
    explicit UpnpGatewayAdapter(UPNPClient* client)
        : m_Client(client)
    {}

    QString lanAddress() const override { return QString::fromStdString(m_Client->lanAddress()); }

    bool add(uint16_t externalPort, uint16_t internalPort, uint32_t leaseSec,
             const QString& description, const QString& protocol) override
    {
        return m_Client->addPortMapping(externalPort, internalPort, leaseSec,
                                        description.toStdString(), protocol.toStdString());
    }

    Lookup entry(uint16_t externalPort, const QString& protocol, Entry& out) override
    {
        UPNPClient::MappingEntry found;
        switch (m_Client->portMapping(externalPort, protocol.toStdString(), found)) {
        case UPNPClient::Mapping::None: return Lookup::None;
        case UPNPClient::Mapping::Unknown: return Lookup::Unknown;
        case UPNPClient::Mapping::Found: break;
        }
        out.client = QString::fromStdString(found.internalClient);
        out.internalPort = portOf(found.internalPort);
        out.description = QString::fromStdString(found.description);
        bool number = false;
        const int seconds = QString::fromStdString(found.leaseDuration).trimmed().toInt(&number);
        out.leaseLeft = number && seconds >= 0 ? seconds : -1;
        return Lookup::Found;
    }

    bool remove(uint16_t externalPort, const QString& protocol) override
    {
        return m_Client->removePortMapping(externalPort, protocol.toStdString());
    }

private:
    /// A port as a router writes it: figures and nothing else. 0 for anything
    /// that is not one.
    static uint16_t portOf(const std::string& text)
    {
        if (text.empty() || text.size() > 5) return 0;
        unsigned value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') return 0;
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        return value <= 65535 ? static_cast<uint16_t>(value) : 0;
    }

    UPNPClient* m_Client;
};

/// What the worker thread is started with, read on the main thread.
struct Startup
{
    QString previousLan;
    /// Where the record of this host's router entries is kept.
    QString dataDir;
    bool devInstance = false;
    /// What a version that kept no such record remembers in the settings.
    mw::routerholes::Earlier earlier;
};

Startup startupOf(const AppSettings& settings)
{
    Startup s;
    s.previousLan = settings.routerLanIp();
    s.dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    s.devInstance = mw::portidentity::devPorts();
    s.earlier.lan = s.previousLan;
    s.earlier.tunnel = settings.rememberedTunnelPorts();
    s.earlier.media = settings.rememberedMediaPorts();
    return s;
}

/// A line of the record as the log names it: no address, and no tag.
QString holeName(const mw::routerholes::Hole& hole)
{
    QString what;
    switch (hole.kind) {
    case mw::routerholes::Kind::Tunnel: what = QStringLiteral("tunnel"); break;
    case mw::routerholes::Kind::Streams: what = QStringLiteral("streams' shared port"); break;
    case mw::routerholes::Kind::Media: what = QStringLiteral("media slot %1").arg(hole.slot); break;
    }
    return QStringLiteral("%1 %2 (%3)")
        .arg(hole.tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"))
        .arg(hole.external)
        .arg(what);
}

/// What became of an entry an earlier run left on the router. Every removal
/// is said; so is every entry that was looked at and left.
void logSwept(const mw::routerholes::Hole& hole, const RouterPortCore::SweepResult& result)
{
    using Swept = RouterPortCore::Swept;
    const QString name = holeName(hole);
    switch (result.outcome) {
    case Swept::Removed:
        Logger::info(
            QStringLiteral("[UPNP] Left by an earlier run: %1 removed from the router").arg(name));
        break;
    case Swept::NotOurs:
        Logger::info(QStringLiteral("[UPNP] Left by an earlier run: %1 now has another %2 on the "
                                    "router — left as it is")
                         .arg(name, result.detail));
        break;
    case Swept::Unknown:
        Logger::info(QStringLiteral("[UPNP] Left by an earlier run: the router does not say what "
                                    "%1 is — left as it is, asked again at the next start")
                         .arg(name));
        break;
    case Swept::Stayed:
        Logger::warning(QStringLiteral("[UPNP] Left by an earlier run: %1 is still on the router "
                                       "after its removal was asked — asked again at the next "
                                       "start")
                            .arg(name));
        break;
    case Swept::InUse:
        Logger::info(result.detail == QLatin1String("listened")
                         ? QStringLiteral("[UPNP] Left by an earlier version: %1 leads to a port "
                                          "something on this machine listens on — left as it is")
                               .arg(name)
                         : QStringLiteral("[UPNP] Left by an earlier version: this system does "
                                          "not say whether anything listens on the port %1 leads "
                                          "to — left as it is")
                               .arg(name));
        break;
    case Swept::Gone:
    case Swept::OtherAddress:
    case Swept::HeldAgain: break;
    }
}

/// Whether this machine can actually listen there. See RouterPortCore's
/// constructor for why it is asked before the mapping is made.
bool portIsFree(uint16_t port)
{
    QUdpSocket probe;
    return probe.bind(QHostAddress::AnyIPv4, port, QAbstractSocket::DontShareAddress);
}

QString purposeName(RouterPortCore::Purpose purpose, int slot)
{
    switch (purpose) {
    case RouterPortCore::Purpose::Tunnel: return QStringLiteral("tunnel");
    case RouterPortCore::Purpose::Mux:
        return slot == RouterPortCore::kMuxStandard
                   ? QStringLiteral("streams' shared port (conferencing)")
                   : QStringLiteral("streams' shared port (stream block)");
    case RouterPortCore::Purpose::Media: break;
    }
    return QStringLiteral("media slot %1").arg(slot);
}

QString skipLine(const RouterPortCore::Skipped& s)
{
    switch (s.reason) {
    case RouterPortCore::SkipReason::LocalBusy:
        return QStringLiteral("%1 (taken on this machine)").arg(s.port);
    case RouterPortCore::SkipReason::Foreign:
        return QStringLiteral("%1 (%2)").arg(s.port).arg(s.owner);
    case RouterPortCore::SkipReason::Refused:
        return QStringLiteral("%1 (refused by the router)").arg(s.port);
    case RouterPortCore::SkipReason::KeptElsewhere:
        return QStringLiteral("%1 (kept pointed at %2 after our write)").arg(s.port).arg(s.owner);
    }
    return QString::number(s.port);
}

} // namespace

// ── The thread that talks to the router ─────────────────────────────────────

class RouterPortAllocator::Worker : public QObject
{
public:
    explicit Worker(RouterPortAllocator* owner)
        : m_Owner(owner)
    {}

    ~Worker() override
    {
        // On this thread, where the client was created.
        m_Core.reset();
        m_Gateway.reset();
        m_Upnp.reset();
        // After the core, which goes by it. Its lock ends here.
        m_Record.reset();
    }

    void start(const Startup& startup)
    {
        if (m_Upnp) return;
        auto upnp = std::make_unique<UPNPClient>();
        if (!upnp->discover(2000)) {
            QMetaObject::invokeMethod(
                m_Owner, [o = m_Owner]() { o->onDiscovered(false, {}, {}, {}); },
                Qt::QueuedConnection);
            return;
        }
        const QString publicIp = QString::fromStdString(upnp->getExternalIPAddress());
        const QString lan = QString::fromStdString(upnp->lanAddress());
        const QString gateway = upnp->gatewayAddress().toString();
        m_Upnp = std::move(upnp);
        m_Gateway = std::make_unique<UpnpGatewayAdapter>(m_Upnp.get());
        m_Core = std::make_unique<RouterPortCore>(*m_Gateway, &portIsFree);
        m_Core->setPreviousLanAddress(startup.previousLan);
        // What this host writes on the router is noted, so that what a run
        // that was killed left there can be taken back (RouterHoles.h). Opened
        // once: its lock is this process's for as long as it lives.
        if (!m_RecordTried) {
            m_RecordTried = true;
            QString said;
            m_Record = mw::routerholes::File::open(startup.dataDir, startup.devInstance,
                                                   QSysInfo::machineUniqueId(), startup.earlier,
                                                   lan, &said);
            if (!said.isEmpty()) Logger::info(QStringLiteral("[UPNP] Note: ") + said);
        }
        m_Core->setRecord(m_Record.get());
        m_Core->setListens(&mw::sockettables::listens);

        m_Renew = new QTimer(this);
        connect(m_Renew, &QTimer::timeout, this, [this]() { renew(); });
        m_Renew->start(mw::routerports::kRenewIntervalMs);

        QMetaObject::invokeMethod(
            m_Owner,
            [o = m_Owner, publicIp, lan, gateway]() {
                o->onDiscovered(true, publicIp, lan, gateway);
            },
            Qt::QueuedConnection);

        // What an earlier run left on the router. Queued, so that the claims
        // that waited for this discovery are served first, and one line at a
        // time, so that those that come meanwhile do not wait for the sweep.
        m_Sweep = m_Core->sweepList();
        if (!m_Sweep.isEmpty())
            QMetaObject::invokeMethod(this, [this]() { sweepFirst(); }, Qt::QueuedConnection);
    }

    void sweepFirst()
    {
        if (!m_Core || m_Sweep.isEmpty()) return;
        if (!m_Core->sweepBelieved(m_Sweep)) {
            Logger::info(QStringLiteral("[UPNP] Asked about a port no MoonlightWeb ever maps, the "
                                        "router did not answer that it has no such entry: "
                                        "nothing an earlier run left on it is removed"));
            m_Sweep.clear();
            return;
        }
        sweepNext();
    }

    void sweepNext()
    {
        if (!m_Core || m_Sweep.isEmpty()) return;
        const mw::routerholes::Hole hole = m_Sweep.takeFirst();
        logSwept(hole, m_Core->sweep(hole));
        if (!m_Sweep.isEmpty())
            QMetaObject::invokeMethod(this, [this]() { sweepNext(); }, Qt::QueuedConnection);
    }

    void claim(quint64 token, const RouterPortCore::Request& request)
    {
        RouterPortCore::Result result;
        if (m_Core) result = m_Core->claim(request);
        QMetaObject::invokeMethod(
            m_Owner, [o = m_Owner, token, result]() { o->onClaimed(token, result); },
            Qt::QueuedConnection);
    }

    void renew()
    {
        if (!m_Core) return;
        const QList<RouterPortCore::Lost> lost = m_Core->renew();
        if (m_Core->renewFailures() == 2 && m_Core->leaseSec() == 0)
            Logger::warning(QStringLiteral("[UPNP] Renewal failed twice — switching to permanent "
                                           "mappings"));
        if (lost.isEmpty()) return;
        QMetaObject::invokeMethod(
            m_Owner, [o = m_Owner, lost]() { o->onRenewed(lost); }, Qt::QueuedConnection);
    }

    void renew(uint16_t externalPort)
    {
        if (!m_Core) return;
        const QList<RouterPortCore::Lost> lost = m_Core->renew(externalPort);
        if (lost.isEmpty()) return;
        QMetaObject::invokeMethod(
            m_Owner, [o = m_Owner, lost]() { o->onRenewed(lost); }, Qt::QueuedConnection);
    }

    void release(const RouterPortCore::Claim& claim)
    {
        if (m_Core) m_Core->release(claim);
    }

    void releaseAll()
    {
        if (m_Core) m_Core->releaseAll();
    }

private:
    RouterPortAllocator* m_Owner;
    std::unique_ptr<UPNPClient> m_Upnp;
    std::unique_ptr<UpnpGatewayAdapter> m_Gateway;
    /// Null when another process has it: this run then notes nothing.
    std::unique_ptr<mw::routerholes::File> m_Record;
    bool m_RecordTried = false;
    std::unique_ptr<RouterPortCore> m_Core;
    /// The lines of the record still to be looked at.
    QList<mw::routerholes::Hole> m_Sweep;
    QTimer* m_Renew = nullptr;
};

// ── The main-thread façade ──────────────────────────────────────────────────

RouterPortAllocator::RouterPortAllocator(AppSettings* settings, QObject* parent)
    : QObject(parent)
    , m_Settings(settings)
{}

RouterPortAllocator::~RouterPortAllocator()
{
    if (!m_Thread) return;
    // The mappings are removed on the way out — the numbers stay in the
    // settings, which is what makes the next start land on the same ones.
    QMetaObject::invokeMethod(
        m_Worker, [w = m_Worker]() { w->releaseAll(); }, Qt::BlockingQueuedConnection);
    m_Thread->quit();
    m_Thread->wait();
    delete m_Thread;
}

bool RouterPortAllocator::enabled() const
{
    return m_Settings->upnpEnabled() && m_Settings->internetAccessEnabled();
}

void RouterPortAllocator::ensureStarted()
{
    if (m_State != GatewayState::Unknown) return;
    if (!enabled()) return;
    startWorker();
}

void RouterPortAllocator::discover()
{
    if (mw::edition::lanOnly()) return;
    if (m_State == GatewayState::Unknown) {
        startWorker();
        return;
    }
    if (m_State != GatewayState::Missing) return;
    // Asked again: the worker keeps no client after a miss, so start() looks
    // for the router afresh.
    m_State = GatewayState::Discovering;
    const Startup startup = startupOf(*m_Settings);
    QMetaObject::invokeMethod(
        m_Worker, [w = m_Worker, startup]() { w->start(startup); }, Qt::QueuedConnection);
}

void RouterPortAllocator::startWorker()
{
    m_State = GatewayState::Discovering;
    m_Thread = new QThread(this);
    m_Thread->setObjectName(QStringLiteral("mw-upnp"));
    m_Worker = new Worker(this);
    m_Worker->moveToThread(m_Thread);
    connect(m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Thread->start();

    const Startup startup = startupOf(*m_Settings);
    QMetaObject::invokeMethod(
        m_Worker, [w = m_Worker, startup]() { w->start(startup); }, Qt::QueuedConnection);
}

void RouterPortAllocator::onDiscovered(bool ok, const QString& publicIp, const QString& lanIp,
                                       const QString& gateway)
{
    if (!ok) {
        m_State = GatewayState::Missing;
        if (m_Misses == 0)
            Logger::info(QStringLiteral("[UPNP] No IGD — tunnel connections will rely on their "
                                        "reflexive address, streams on STUN alone; looking "
                                        "again in the background"));
        ++m_Misses;
        scheduleRediscovery();
        emit gatewayMissing();
        return;
    }
    if (m_Misses > 0)
        Logger::info(QStringLiteral("[UPNP] Gateway answered after %1 missed discover%2")
                         .arg(m_Misses)
                         .arg(m_Misses == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
    m_Misses = 0;
    m_State = GatewayState::Ready;
    m_PublicIp = publicIp;
    m_LanIp = lanIp;
    m_GatewayAddress = gateway;
    Logger::info(QStringLiteral("[UPNP] Gateway found, public %1, this host %2")
                     .arg(publicIp.isEmpty() ? QStringLiteral("(unknown)") : publicIp, lanIp));
    // The address the remembered ports will have been obtained under, from
    // now on. The worker already took the previous one for the reclaim rule.
    m_Settings->setRouterLanIp(lanIp);
    emit gatewayReady(publicIp);
}

void RouterPortAllocator::scheduleRediscovery()
{
    if (!m_Rediscover) {
        m_Rediscover = new QTimer(this);
        m_Rediscover->setSingleShot(true);
        connect(m_Rediscover, &QTimer::timeout, this, [this]() {
            if (m_State != GatewayState::Missing) return;
            // Turned off since: the router is not to be touched, but the
            // switch may come back on, so keep the next look booked.
            if (!m_Settings->upnpEnabled()) {
                scheduleRediscovery();
                return;
            }
            discover();
        });
    }
    // 30 s, 1, 2, 4, 8 min, then every 10. A claim arriving while one runs
    // queues behind it on the worker, as behind the first discovery.
    const int shift = qMin(m_Misses - 1, 5);
    m_Rediscover->start(qMin(mw::routerports::kRediscoverFirstMs << qMax(shift, 0),
                             mw::routerports::kRediscoverMaxMs));
}

quint64 RouterPortAllocator::submit(RouterPortCore::Request request, QObject* context,
                                    Callback callback)
{
    const quint64 token = m_NextToken++;
    m_Pending.insert(token, Pending{context, std::move(callback), request.purpose, request.slot});
    QMetaObject::invokeMethod(
        m_Worker, [w = m_Worker, token, request]() { w->claim(token, request); },
        Qt::QueuedConnection);
    return token;
}

void RouterPortAllocator::answerLater(Purpose purpose, int slot, QObject* context,
                                      Callback callback, const Claim& claim, const QString& why)
{
    // Queued even when the answer is known now: every caller is written for a
    // callback that arrives after it returns, and one that fired inside the
    // call would run into state the caller has not finished setting up.
    const quint64 token = m_NextToken++;
    m_Pending.insert(token, Pending{context, std::move(callback), purpose, slot});
    QMetaObject::invokeMethod(
        this, [this, token, claim, why]() { deliver(token, claim, why); }, Qt::QueuedConnection);
}

QString RouterPortAllocator::refusal()
{
    if (!enabled()) return QStringLiteral("UPnP is off");
    ensureStarted();
    if (m_State == GatewayState::Missing) return QStringLiteral("no IGD");
    return {};
}

void RouterPortAllocator::deliver(quint64 token, const Claim& claim, const QString& why)
{
    const auto it = m_Pending.find(token);
    if (it == m_Pending.end()) return;
    const Pending pending = it.value();
    m_Pending.erase(it);
    // Only while whoever asked is still around: a browser that left, a stream
    // that was cancelled. The hole itself is kept either way — it is held for
    // this host, not for that one caller.
    if (pending.context && pending.callback) pending.callback(claim, why);
}

void RouterPortAllocator::claimTunnelPort(QObject* context, Callback callback)
{
    // A DEV identity walks its own block only (RouterPortPools.h), and forgets
    // what it remembers from before it had one.
    const bool dev = mw::portidentity::devPorts();
    QList<uint16_t> remembered;
    for (const quint16 p : m_Settings->rememberedTunnelPorts()) {
        if (RouterPortCore::tunnelPortAllowed(dev, p))
            remembered.append(p);
        else
            m_Settings->forgetTunnelPort(p);
    }
    RouterPortCore::Request request = RouterPortCore::tunnelRequest(dev, remembered);

    const QString why = refusal();
    if (!why.isEmpty()) {
        answerLater(Purpose::Tunnel, -1, context, std::move(callback), {}, why);
        return;
    }
    submit(std::move(request), context, std::move(callback));
}

void RouterPortAllocator::claimMediaPort(int slot, uint16_t internalPort, QObject* context,
                                         Callback callback)
{
    // The switches first, even for a hole already held: turning UPnP off must
    // stop the very next stream from advertising a public address. The hole
    // itself stays mapped and renewed until exit, which is harmless.
    const QString why = refusal();
    if (!why.isEmpty()) {
        answerLater(Purpose::Media, slot, context, std::move(callback), {}, why);
        return;
    }

    // A port the mux is bound to is the mux's: a hole made for this slot
    // would lead to it.
    const bool muxHoldsPort = m_MuxHolds && m_MuxHolds(internalPort);

    // Held from an earlier stream on this slot: no round trip, no wait. The
    // entry is written again behind the answer: the router may have lost it
    // meanwhile (rebooted, or a lease run out while this machine slept).
    const auto held = heldMediaPort(slot);
    if (held && !muxHoldsPort && held->internal == internalPort) {
        answerLater(Purpose::Media, slot, context, std::move(callback), *held, {});
        recheck(held->external);
        return;
    }
    // Held, but to a port the mux has come to since, or to another port than
    // the one this stream binds: it leads nowhere this stream listens.
    if (held) releaseMediaPort(slot);

    // Up to 0.3.0 the slot's own number was 48010 + slot, and that is what a
    // host upgraded from it remembers: asking for it again would keep the old
    // number on the router, pointed at the new block. Asked for from scratch.
    quint16 remembered = m_Settings->rememberedMediaPort(slot);
    if (remembered >= 48010 && remembered <= 48033) {
        m_Settings->forgetMediaPort(slot);
        remembered = 0;
    }
    RouterPortCore::Request request =
        RouterPortCore::mediaRequest(static_cast<bool>(m_MuxHolds), mw::portidentity::devPorts(),
                                     slot, internalPort, remembered, muxHoldsPort);
    if (!request.hasCandidate()) {
        answerLater(Purpose::Media, slot, context, std::move(callback), {},
                    QStringLiteral("its port is one every stream shares"));
        return;
    }
    submit(std::move(request), context, std::move(callback));
}

void RouterPortAllocator::claimMuxPort(int which, uint16_t current,
                                       std::function<RouterPortCore::Hold(uint16_t)> hold,
                                       std::function<void(uint16_t)> drop, QObject* context,
                                       Callback callback)
{
    // The switches first, as for a media port.
    const QString why = refusal();
    if (!why.isEmpty()) {
        answerLater(Purpose::Mux, which, context, std::move(callback), {}, why);
        return;
    }
    if (const auto held = heldMuxPort(which)) {
        answerLater(Purpose::Mux, which, context, std::move(callback), *held, {});
        recheck(held->external);
        return;
    }

    // The port the mux serves now, then the one the router gave last time. A
    // remembered port this kind of instance may not hold is forgotten: a
    // --dev that was the installed host's settings once.
    const bool dev = mw::portidentity::devPorts();
    QList<uint16_t> first;
    if (current != 0) first.append(current);
    if (const quint16 remembered = m_Settings->rememberedMuxPort(which)) {
        if (!RouterPortCore::muxPortAllowed(dev, which, remembered))
            m_Settings->forgetMuxPort(which);
        else if (!first.contains(remembered))
            first.append(remembered);
    }
    RouterPortCore::Request request = RouterPortCore::muxRequest(dev, which, first);
    if (!request.hasCandidate()) {
        answerLater(Purpose::Mux, which, context, std::move(callback), {},
                    QStringLiteral("this kind of instance has no such port"));
        return;
    }
    request.hold = std::move(hold);
    request.drop = std::move(drop);
    // Where no stream listens in TCP there is no TCP hole: it would lead to
    // whichever program binds the number next
    request.tcp = request.tcp && m_Settings->iceTcpPassive();
    submit(std::move(request), context, std::move(callback));
}

void RouterPortAllocator::renewNow()
{
    if (m_State != GatewayState::Ready || !enabled()) return;
    Logger::info(QStringLiteral("[UPNP] Renewing every held hole now"));
    QMetaObject::invokeMethod(m_Worker, [w = m_Worker]() { w->renew(); }, Qt::QueuedConnection);
}

void RouterPortAllocator::recheck(uint16_t externalPort)
{
    if (m_State != GatewayState::Ready || !enabled()) return;
    QMetaObject::invokeMethod(
        m_Worker, [w = m_Worker, externalPort]() { w->renew(externalPort); }, Qt::QueuedConnection);
}

void RouterPortAllocator::releaseMediaPort(int slot)
{
    const auto held = heldMediaPort(slot);
    if (!held) return;
    const Claim claim = *held;
    m_Held.removeAll(claim);
    QMetaObject::invokeMethod(
        m_Worker, [w = m_Worker, claim]() { w->release(claim); }, Qt::QueuedConnection);
    Logger::info(QStringLiteral("[UPNP] %1: released %2")
                     .arg(purposeName(Purpose::Media, slot))
                     .arg(claim.external));
}

void RouterPortAllocator::onClaimed(quint64 token, const RouterPortCore::Result& result)
{
    const auto it = m_Pending.find(token);
    const Purpose purpose = it == m_Pending.end() ? Purpose::Tunnel : it->purpose;
    const int slot = it == m_Pending.end() ? -1 : it->slot;
    const QString name = purposeName(purpose, slot);

    // Discovery failed after this claim was queued behind it: the worker had no
    // router to walk. Not a pool exhausted by neighbours, and not worth a
    // warning — the missing gateway was already logged once.
    if (m_State == GatewayState::Missing) {
        deliver(token, {}, QStringLiteral("no IGD"));
        return;
    }

    // A remembered port that was passed over is a port this host no longer
    // has a claim on: forgotten, so the next start does not knock there first.
    QStringList skipped;
    for (const RouterPortCore::Skipped& s : result.skipped) {
        skipped << skipLine(s);
        if (s.reason == RouterPortCore::SkipReason::LocalBusy) continue;
        switch (purpose) {
        case Purpose::Tunnel: m_Settings->forgetTunnelPort(s.port); break;
        case Purpose::Media:
            if (m_Settings->rememberedMediaPort(slot) == s.port) m_Settings->forgetMediaPort(slot);
            break;
        case Purpose::Mux:
            if (m_Settings->rememberedMuxPort(slot) == s.port) m_Settings->forgetMuxPort(slot);
            break;
        }
    }

    if (!result.ok) {
        Logger::warning(QStringLiteral("[UPNP] %1: nothing left — every candidate is held by a "
                                       "neighbour or refused (%2)")
                            .arg(name, skipped.join(QStringLiteral(", "))));
        deliver(token, {}, QStringLiteral("no router port left"));
        return;
    }

    const Claim& claim = result.claim;
    m_Held.append(claim);
    switch (purpose) {
    case Purpose::Tunnel: m_Settings->rememberTunnelPort(claim.external); break;
    case Purpose::Media: m_Settings->rememberMediaPort(slot, claim.external); break;
    case Purpose::Mux: m_Settings->rememberMuxPort(slot, claim.external); break;
    }

    QString line = QStringLiteral("[UPNP] %1: claimed %2").arg(name).arg(claim.external);
    if (claim.internal != claim.external) line += QStringLiteral(" -> %1").arg(claim.internal);
    line += claim.tcp ? QStringLiteral(" (UDP+TCP") : QStringLiteral(" (UDP only");
    line += QStringLiteral(", public %1)").arg(m_PublicIp);
    if (!result.reclaimedFrom.isEmpty())
        line += QStringLiteral(", taken back from this host's old address %1")
                    .arg(result.reclaimedFrom);
    if (!skipped.isEmpty())
        line += QStringLiteral(", after skipping %1").arg(skipped.join(QStringLiteral(", ")));
    Logger::info(line);

    deliver(token, claim, {});
}

void RouterPortAllocator::onRenewed(const QList<RouterPortCore::Lost>& lost)
{
    for (const RouterPortCore::Lost& l : lost) {
        Logger::warning(QStringLiteral("[UPNP] %1: port %2 now forwards to %3 — dropped and "
                                       "forgotten, this machine will stop offering it")
                            .arg(purposeName(l.claim.purpose, l.claim.slot))
                            .arg(l.claim.external)
                            .arg(l.owner));
        m_Held.removeAll(l.claim);
        switch (l.claim.purpose) {
        case Purpose::Tunnel: m_Settings->forgetTunnelPort(l.claim.external); break;
        case Purpose::Media: m_Settings->forgetMediaPort(l.claim.slot); break;
        case Purpose::Mux:
            if (m_Settings->rememberedMuxPort(l.claim.slot) == l.claim.external)
                m_Settings->forgetMuxPort(l.claim.slot);
            break;
        }
        emit portLost(static_cast<int>(l.claim.purpose), l.claim.slot, l.claim.external, l.owner);
    }
}

void RouterPortAllocator::releaseTunnelPort(quint16 externalPort)
{
    for (const Claim& held : std::as_const(m_Held)) {
        if (held.purpose != Purpose::Tunnel || held.external != externalPort) continue;
        const Claim claim = held;
        m_Held.removeAll(claim);
        QMetaObject::invokeMethod(
            m_Worker, [w = m_Worker, claim]() { w->release(claim); }, Qt::QueuedConnection);
        Logger::info(QStringLiteral("[UPNP] %1: released %2")
                         .arg(purposeName(Purpose::Tunnel, -1))
                         .arg(claim.external));
        return;
    }
}

void RouterPortAllocator::releaseMuxPort(int which)
{
    const auto held = heldMuxPort(which);
    if (!held) return;
    const Claim claim = *held;
    m_Held.removeAll(claim);
    QMetaObject::invokeMethod(
        m_Worker, [w = m_Worker, claim]() { w->release(claim); }, Qt::QueuedConnection);
    Logger::info(QStringLiteral("[UPNP] %1: released %2")
                     .arg(purposeName(Purpose::Mux, which))
                     .arg(claim.external));
}

void RouterPortAllocator::switchChanged()
{
    if (enabled())
        emit switchedOn();
    else
        emit switchedOff();
}

QList<RouterPortAllocator::Claim> RouterPortAllocator::heldTunnelPorts() const
{
    QList<Claim> out;
    for (const Claim& c : m_Held)
        if (c.purpose == Purpose::Tunnel) out.append(c);
    return out;
}

std::optional<RouterPortAllocator::Claim> RouterPortAllocator::heldMediaPort(int slot) const
{
    for (const Claim& c : m_Held)
        if (c.purpose == Purpose::Media && c.slot == slot) return c;
    return std::nullopt;
}

std::optional<RouterPortAllocator::Claim> RouterPortAllocator::heldMuxPort(int which) const
{
    for (const Claim& c : m_Held)
        if (c.purpose == Purpose::Mux && c.slot == which) return c;
    return std::nullopt;
}

int RouterPortAllocator::inFlight(Purpose purpose) const
{
    int n = 0;
    for (const Pending& p : m_Pending)
        if (p.purpose == purpose) ++n;
    return n;
}
