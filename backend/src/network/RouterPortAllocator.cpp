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
#include "RouterPortPools.h"
#include "UPNPClient.h"
#include "common/Logger.h"
#include "server/AppSettings.h"

#include <QCoreApplication>
#include <QMetaObject>
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

    bool owner(uint16_t externalPort, const QString& protocol, QString& client) override
    {
        std::string who;
        std::string internalPort;
        if (!m_Client->getExistingPortMapping(externalPort, protocol.toStdString(), who,
                                              internalPort))
            return false;
        client = QString::fromStdString(who);
        return true;
    }

    bool remove(uint16_t externalPort, const QString& protocol) override
    {
        return m_Client->removePortMapping(externalPort, protocol.toStdString());
    }

private:
    UPNPClient* m_Client;
};

/// Whether this machine can actually listen there. See RouterPortCore's
/// constructor for why it is asked before the mapping is made.
bool portIsFree(uint16_t port)
{
    QUdpSocket probe;
    return probe.bind(QHostAddress::AnyIPv4, port, QAbstractSocket::DontShareAddress);
}

QString purposeName(RouterPortCore::Purpose purpose, int slot)
{
    return purpose == RouterPortCore::Purpose::Tunnel ? QStringLiteral("tunnel")
                                                      : QStringLiteral("media slot %1").arg(slot);
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
    }

    void start(const QString& previousLan)
    {
        if (m_Upnp) return;
        auto upnp = std::make_unique<UPNPClient>();
        if (!upnp->discover(2000)) {
            QMetaObject::invokeMethod(
                m_Owner, [o = m_Owner]() { o->onDiscovered(false, {}, {}); }, Qt::QueuedConnection);
            return;
        }
        const QString publicIp = QString::fromStdString(upnp->getExternalIPAddress());
        const QString lan = QString::fromStdString(upnp->lanAddress());
        m_Upnp = std::move(upnp);
        m_Gateway = std::make_unique<UpnpGatewayAdapter>(m_Upnp.get());
        m_Core = std::make_unique<RouterPortCore>(*m_Gateway, &portIsFree);
        m_Core->setPreviousLanAddress(previousLan);

        m_Renew = new QTimer(this);
        connect(m_Renew, &QTimer::timeout, this, [this]() { renew(); });
        m_Renew->start(mw::routerports::kRenewIntervalMs);

        QMetaObject::invokeMethod(
            m_Owner, [o = m_Owner, publicIp, lan]() { o->onDiscovered(true, publicIp, lan); },
            Qt::QueuedConnection);
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

    void releaseAll()
    {
        if (m_Core) m_Core->releaseAll();
    }

private:
    RouterPortAllocator* m_Owner;
    std::unique_ptr<UPNPClient> m_Upnp;
    std::unique_ptr<UpnpGatewayAdapter> m_Gateway;
    std::unique_ptr<RouterPortCore> m_Core;
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

    m_State = GatewayState::Discovering;
    m_Thread = new QThread(this);
    m_Thread->setObjectName(QStringLiteral("mw-upnp"));
    m_Worker = new Worker(this);
    m_Worker->moveToThread(m_Thread);
    connect(m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Thread->start();

    const QString previousLan = m_Settings->routerLanIp();
    QMetaObject::invokeMethod(
        m_Worker, [w = m_Worker, previousLan]() { w->start(previousLan); }, Qt::QueuedConnection);
}

void RouterPortAllocator::onDiscovered(bool ok, const QString& publicIp, const QString& lanIp)
{
    if (!ok) {
        m_State = GatewayState::Missing;
        Logger::info(QStringLiteral("[UPNP] No IGD — tunnel connections will rely on their "
                                    "reflexive address, streams on STUN alone"));
        emit gatewayMissing();
        return;
    }
    m_State = GatewayState::Ready;
    m_PublicIp = publicIp;
    m_LanIp = lanIp;
    Logger::info(QStringLiteral("[UPNP] Gateway found, public %1, this host %2")
                     .arg(publicIp.isEmpty() ? QStringLiteral("(unknown)") : publicIp, lanIp));
    // The address the remembered ports will have been obtained under, from
    // now on. The worker already took the previous one for the reclaim rule.
    m_Settings->setRouterLanIp(lanIp);
    emit gatewayReady(publicIp);
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
    RouterPortCore::Request request;
    request.purpose = Purpose::Tunnel;
    for (const quint16 p : m_Settings->rememberedTunnelPorts())
        request.remembered.append(p);
    for (const uint16_t p : mw::routerports::kTunnelPreferred)
        request.preferred.append(p);
    request.poolBegin = mw::routerports::kTunnelPoolBegin;
    request.poolEnd = mw::routerports::kTunnelPoolEnd;
    request.description = QStringLiteral("MoonlightWeb tunnel");

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

    // Held from an earlier stream on this slot: no round trip, no wait.
    if (const auto held = heldMediaPort(slot)) {
        answerLater(Purpose::Media, slot, context, std::move(callback), *held, {});
        return;
    }

    RouterPortCore::Request request;
    request.purpose = Purpose::Media;
    request.slot = slot;
    request.internalPort = internalPort;
    if (const quint16 remembered = m_Settings->rememberedMediaPort(slot))
        request.remembered.append(remembered);
    request.preferred.append(internalPort);
    request.poolBegin = mw::routerports::kMediaPoolBegin;
    request.poolEnd = mw::routerports::kMediaPoolEnd;
    request.description = QStringLiteral("MoonlightWeb media slot %1").arg(slot);

    submit(std::move(request), context, std::move(callback));
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
        if (purpose == Purpose::Tunnel)
            m_Settings->forgetTunnelPort(s.port);
        else if (m_Settings->rememberedMediaPort(slot) == s.port)
            m_Settings->forgetMediaPort(slot);
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
    if (purpose == Purpose::Tunnel)
        m_Settings->rememberTunnelPort(claim.external);
    else
        m_Settings->rememberMediaPort(slot, claim.external);

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
        if (l.claim.purpose == Purpose::Tunnel)
            m_Settings->forgetTunnelPort(l.claim.external);
        else
            m_Settings->forgetMediaPort(l.claim.slot);
        emit portLost(static_cast<int>(l.claim.purpose), l.claim.slot, l.claim.external, l.owner);
    }
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

int RouterPortAllocator::inFlight(Purpose purpose) const
{
    int n = 0;
    for (const Pending& p : m_Pending)
        if (p.purpose == purpose) ++n;
    return n;
}
