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
        /// No IGD answered. Claims fail at once; nothing to wait for.
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

    GatewayState gatewayState() const { return m_State; }
    /// This host's address as the router reports it; empty until Ready.
    QString publicIp() const { return m_PublicIp; }

    /// One tunnel hole from the corporate-friendly list, then the pool. The
    /// caller enforces its own cap; this only walks the router.
    void claimTunnelPort(QObject* context, Callback callback);

    /// The router-side port for a stream slot. Answered without a round trip
    /// when the slot's hole is already held from an earlier session — the
    /// mapping outlives the stream so the next launch on the slot is instant
    /// and a theft between sessions is still caught at renewal.
    void claimMediaPort(int slot, uint16_t internalPort, QObject* context, Callback callback);

    QList<Claim> heldTunnelPorts() const;
    std::optional<Claim> heldMediaPort(int slot) const;
    /// Claims handed to the router and not yet answered.
    int inFlight(Purpose purpose) const;

signals:
    void gatewayReady(const QString& publicIp);
    void gatewayMissing();
    /// A held hole the router now aims at another machine, found at renewal.
    /// Already dropped and forgotten here; the owner of the port stops
    /// offering it.
    void portLost(int purpose, int slot, quint16 externalPort, const QString& owner);

private:
    class Worker;

    struct Pending
    {
        QPointer<QObject> context;
        Callback callback;
        Purpose purpose;
        /// Stream slot for a media claim, -1 for the tunnel. Kept here because
        /// a failed claim carries no slot of its own to log or forget under.
        int slot = -1;
    };

    void deliver(quint64 token, const Claim& claim, const QString& why);
    void onDiscovered(bool ok, const QString& publicIp, const QString& lanIp);
    void onClaimed(quint64 token, const RouterPortCore::Result& result);
    void onRenewed(const QList<RouterPortCore::Lost>& lost);
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
    QString m_PublicIp;
    QString m_LanIp;
    /// Main-thread mirror of what the worker holds.
    QList<Claim> m_Held;
    QHash<quint64, Pending> m_Pending;
    quint64 m_NextToken = 1;
};
