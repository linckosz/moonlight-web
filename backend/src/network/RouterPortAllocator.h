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

#include "RouterPortCore.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>

class AppSettings;
class QThread;
class QTimer;
namespace mw::tcp443 {
class Probe;
}

/**
 * @brief Every hole this host opens in the router, from one place.
 *
 * Two things reach this machine from the internet: the control tunnel and the
 * stream. Each used to open its own router entries on its own — the tunnel on
 * the main thread, the stream inside its worker process — and neither could
 * see what the other held, let alone what a SECOND MoonlightWeb host on the
 * same LAN held. Measured on a Livebox: two hosts asking for the same entry
 * do not get a refusal, they get the entry silently repointed at whichever
 * renewed last, and the symptom comes and goes on the half hour.
 *
 * So there is one allocator. It walks the candidate lists in RouterPortPools.h
 * with the policy in RouterPortCore (never take a neighbour's entry, read
 * every write back), remembers what it obtained in settings.json so a restart
 * lands on the same ports, renews everything it holds every half hour, and
 * hands the stream its external port before the worker is spawned — the
 * worker itself no longer touches the router at all.
 *
 * The holes are removed when the host stops. A host that is killed leaves
 * them, so each one is noted before it is made (RouterHoles.h), and the next
 * start takes back what it finds of them, once the router has been found:
 * only an entry that is this installation's by that record, by its address,
 * by its port and by its description, and never on a doubt
 * (RouterPortCore::sweep).
 *
 * Threading: every call to the router is a SOAP round trip, and discovery is
 * two seconds. All of it runs on a thread of its own; the public methods here
 * are for the main thread, and results come back through callbacks on the
 * main thread, delivered only while the context object given is still alive.
 *
 * Nothing happens until ensureStarted(): a machine whose owner declined
 * Internet Access, or turned UPnP off, never touches the router.
 */
class RouterPortAllocator : public QObject
{
    Q_OBJECT

public:
    using Claim = RouterPortCore::Claim;
    using Purpose = RouterPortCore::Purpose;
    /// A claimed hole, or one with external == 0 and the reason in `why`.
    using Callback = std::function<void(const Claim& claim, const QString& why)>;

    enum class GatewayState
    {
        /// ensureStarted() not called yet.
        Unknown,
        /// Discovery in progress; claims queue behind it.
        Discovering,
        Ready,
        /// No IGD answered. Claims fail at once; nothing to wait for. Looked
        /// for again later on its own — see scheduleRediscovery().
        Missing,
    };

    explicit RouterPortAllocator(AppSettings* settings, QObject* parent = nullptr);
    ~RouterPortAllocator() override;

    /// Whether the router may be touched at all: UPnP on, Internet Access on
    /// (which a LAN-only edition never is). Read live, so the admin toggle
    /// applies to the next claim without a restart.
    bool enabled() const;

    /// Discover the gateway, once. Idempotent; safe to call on every
    /// rendezvous line-up and on every stream start.
    void ensureStarted();

    /// Find the gateway without being allowed to map anything: the admin
    /// page's "UPnP available", Internet Access's double-NAT check and the
    /// operator's probe read it before any hole is asked for. The one
    /// discovery of the process — nothing else looks for the router. Asks
    /// again after a miss (a router that was off at start-up). Never on a
    /// LAN-only edition.
    void discover();

    GatewayState gatewayState() const { return m_State; }
    /// This host's address as the router reports it; empty until Ready.
    QString publicIp() const { return m_PublicIp; }
    /// The gateway's own address and this host's address on its LAN; empty
    /// until Ready.
    QString gatewayAddress() const { return m_GatewayAddress; }
    QString lanIp() const { return m_LanIp; }

    /// One tunnel hole from the corporate-friendly list, then the pool. The
    /// caller enforces its own cap; this only walks the router.
    void claimTunnelPort(QObject* context, Callback callback);

    /// The router-side port for a stream slot. Answered without a round trip
    /// when the slot's hole is already held from an earlier session — the
    /// mapping outlives the stream so the next launch on the slot is instant
    /// and a theft between sessions is still caught at renewal. The held
    /// entry is rechecked behind that answer (recheck()).
    void claimMediaPort(int slot, uint16_t internalPort, QObject* context, Callback callback);

    /// The hole of one of the streams' shared ports (ice_udp_mux): `which` is
    /// RouterPortCore::kMuxStandard or kMuxBlock. The router forwards such a
    /// port to itself, so the mux has to be the one bound to it: `hold` binds
    /// a number the router has nothing against, `drop` lets go of one that
    /// did not become a hole after all. Both run on the allocator's thread
    /// (RouterPortCore::Request). `current` is the port the mux serves in
    /// that role now, 0 for none: asked for first, so that the hole lands on
    /// the socket the streams already have. Answered at once when the hole is
    /// held, or when this kind of instance has no such port.
    void claimMuxPort(int which, uint16_t current,
                      std::function<RouterPortCore::Hold(uint16_t)> hold,
                      std::function<void(uint16_t)> drop, QObject* context, Callback callback);

    /// The streams share ports, and `holds(port)` says whether the mux has
    /// that one bound (asked on the main thread). From then on a stream
    /// slot's own hole takes no router-side number from the mux's stretch of
    /// the block, and none is made to a port the mux holds: it would lead to
    /// the mux.
    void setStreamMux(std::function<bool(uint16_t)> holds) { m_MuxHolds = std::move(holds); }

    /// TCP 443 on the router, for a browser on a network that lets out
    /// nothing else (settings.json ice_tcp_443, Tcp443Guard): asked for by
    /// this allocator, on its thread, once the streams' shared block port has
    /// its TCP hole, and only while every switch it takes is on and the run
    /// is not a DEV identity's. `probe` dials for it and must outlive this
    /// object; `publicAddress` says, on the main thread, the address browsers
    /// reach this host at (empty when it is not known). Before
    /// ensureStarted() or discover(); without this call 443 is never asked
    /// for.
    void setTcp443(mw::tcp443::Probe* probe, std::function<QString()> publicAddress)
    {
        m_Probe443 = probe;
        m_PublicAddress = std::move(publicAddress);
    }
    /// The shared port the router's TCP 443 leads to, 0 when it leads to
    /// none of this host's: read back on the router, and proved by a
    /// connection when it was written.
    quint16 tcp443LeadsTo() const { return m_Tcp443Port; }
    /// The router keeps a TCP 443 entry this installation wrote under an
    /// address this machine no longer has, and will not remove it: the person
    /// who runs the router has to. False again once it is seen to be gone.
    bool tcp443StaleRemovalRefused() const { return m_Tcp443Refused; }

    /// Renew every held hole now, as the half-hour timer does: the admin
    /// page's Force refresh, so a router that dropped its table gets the
    /// entries back without waiting for the timer.
    void renewNow();

    /// Renew one held hole now, without holding up whoever is about to use
    /// it: a stream launched or a browser served on a hole held for hours.
    /// The write lands in tens of milliseconds, long before their
    /// connectivity checks give up. A hole found pointing elsewhere is
    /// dropped and reported through portLost(), as at renewal.
    void recheck(uint16_t externalPort);

    /// Take a stream slot's hole off the router now instead of at exit. For
    /// the invited players' slots, whose holes should not outlive the guest:
    /// the owner's slots keep theirs, as described above. The number stays
    /// remembered, so the next claim on the slot asks for the same one.
    void releaseMediaPort(int slot);

    /// Take a tunnel hole off the router. Remembered, like a media port.
    void releaseTunnelPort(quint16 externalPort);

    /// Take a shared port's hole off the router. Remembered; the socket is
    /// the mux's, and stays bound.
    void releaseMuxPort(int which);

    /// UPnP or Internet Access was just switched. Off, whoever holds a hole it
    /// is not using gives it back (switchedOff); a stream or a tunnel still on
    /// one gives it back when it ends. On, whoever keeps holes for as long as
    /// the process runs asks for them (switchedOn).
    void switchChanged();

    QList<Claim> heldTunnelPorts() const;
    std::optional<Claim> heldMediaPort(int slot) const;
    std::optional<Claim> heldMuxPort(int which) const;
    /// Claims handed to the router and not yet answered.
    int inFlight(Purpose purpose) const;

signals:
    void gatewayReady(const QString& publicIp);
    void gatewayMissing();
    /// A held hole the router now aims at another machine, found at renewal.
    /// Already dropped and forgotten here; the owner of the port stops
    /// offering it.
    void portLost(int purpose, int slot, quint16 externalPort, const QString& owner);
    void switchedOff();
    void switchedOn();
    /// tcp443LeadsTo() changed: 443 is held from now on, or no longer is.
    void tcp443Changed(quint16 leadsTo);

private:
    class Worker;

    struct Pending
    {
        QPointer<QObject> context;
        Callback callback;
        Purpose purpose;
        /// Stream slot for a media claim, -1 for the tunnel, which of its two
        /// ports for the mux. Kept here because a failed claim carries no slot
        /// of its own to log or forget under.
        int slot = -1;
    };

    void deliver(quint64 token, const Claim& claim, const QString& why);
    void onDiscovered(bool ok, const QString& publicIp, const QString& lanIp,
                      const QString& gateway);
    /// Spawn the worker thread and start a discovery on it.
    void startWorker();
    /// After a miss, look for the router again a little later, backing off
    /// from half a minute to ten. A single miss used to stick until restart:
    /// a host started at logon, before the network was up, or behind a router
    /// that answered late once, showed "UPnP not available" all day.
    void scheduleRediscovery();
    void onClaimed(quint64 token, const RouterPortCore::Result& result);
    void onRenewed(const QList<RouterPortCore::Lost>& lost);
    /// TCP 443 is looked at again on the worker's thread, with what stands
    /// now: the switches, the address, the shared block port and its hole.
    void consider443();
    void onTcp443(quint16 leadsTo, bool staleRemovalRefused);
    quint64 submit(RouterPortCore::Request request, QObject* context, Callback callback);
    /// Answer a claim without asking the router, on the next event-loop turn.
    void answerLater(Purpose purpose, int slot, QObject* context, Callback callback,
                     const Claim& claim, const QString& why);
    /// Why no claim can be made right now ("UPnP is off", "no IGD"), or empty.
    /// Starts discovery as a side effect when it is allowed and not yet begun.
    QString refusal();

    AppSettings* m_Settings = nullptr;
    QThread* m_Thread = nullptr;
    Worker* m_Worker = nullptr;

    GatewayState m_State = GatewayState::Unknown;
    /// Discoveries missed in a row, for the back-off and for logging the
    /// first miss only.
    int m_Misses = 0;
    QTimer* m_Rediscover = nullptr;
    QString m_PublicIp;
    QString m_LanIp;
    QString m_GatewayAddress;
    /// Main-thread mirror of what the worker holds.
    QList<Claim> m_Held;
    /// See setStreamMux(). Unset: no stream shares a port.
    std::function<bool(uint16_t)> m_MuxHolds;
    /// See setTcp443(). Main-thread mirror of what the worker's guard holds.
    mw::tcp443::Probe* m_Probe443 = nullptr;
    std::function<QString()> m_PublicAddress;
    quint16 m_Tcp443Port = 0;
    bool m_Tcp443Refused = false;
    QHash<quint64, Pending> m_Pending;
    quint64 m_NextToken = 1;
};
