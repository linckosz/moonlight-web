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

#include "ControlTunnel.h"

#include "server/TunnelFrame.h"

#include "common/Logger.h"
#include "common/LoopbackKey.h"
#include "streaming/PacketMarking.h"
#include "common/PairingCrypto.h"
#include "network/RendezvousClient.h"
#include "network/RouterPortAllocator.h"
#include "network/RouterPortPools.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/HttpServer.h"
#include "server/TunnelPeerPolicy.h"
#include "streaming/IceUdpDrop.h"
#include "streaming/SdpFingerprint.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWebSocket>

#include <rtc/rtc.hpp>

#include <utility>

namespace {

/// Stand-in address for a browser whose real one we have not learned yet.
///
/// It is a label, not an address, and writing it as one keeps it honest: the
/// browser's address is seen by the introduction server and by nobody else.
/// Anything that classifies it finds it is neither loopback nor private, which
/// is the right answer to "may this caller skip the login".
QString peerLabel(const QString& sessionId)
{
    return QStringLiteral("tunnel:") + sessionId.left(16);
}

/// Headers a browser may not dictate over the tunnel.
///
/// Artefacts of a hop that does not exist here, plus Content-Length, which is
/// recomputed from what actually arrived. Everything else is forwarded
/// untouched — this is a proxy, and a proxy that edits what it carries stops
/// being predictable.
///
/// X-MW-Admin-Key USED to be on this list, on the reasoning that the key is only
/// ever handed to a local caller so accepting one off the wire would undo that
/// in a line. That premise no longer holds: a host session redeemed through the
/// tunnel is granted the key deliberately, because admin WRITES require it and
/// an admin page that loads and then refuses every change is worse than one that
/// never loads. Stripping it here would have made that grant ornamental.
///
/// Forwarding it gives nothing away. The key is a CSRF token, not a claim about
/// where the caller sits; sending a wrong one achieves nothing, and obtaining
/// the right one already requires being the host machine or holding the LAN
/// admin unlock. Whether it is HONOURED is decided in one place — HttpServer's
/// ctx.adminKeyOk, which accepts it on this path only for a session that was
/// itself created by a tunnel redemption. Keep the decision there; a second copy
/// of it here is how the two would drift.
bool isForbiddenHeader(const QString& name)
{
    static const QStringList kBlocked = {
        QStringLiteral("connection"),
        QStringLiteral("upgrade"),
        QStringLiteral("transfer-encoding"),
        QStringLiteral("content-length"),
        QStringLiteral("expect"),
        QStringLiteral("keep-alive"),
        QStringLiteral("te"),
    };
    return kBlocked.contains(name);
}

constexpr int kMaxSocketsPerPeer = 8;
constexpr int kMaxInFlightRequests = 24;

} // namespace

ControlTunnel::ControlTunnel(HttpServer* http, AuthManager* auth, AppSettings* settings,
                             RendezvousClient* rendezvous, RouterPortAllocator* routerPorts,
                             QObject* parent)
    : QObject(parent)
    , m_Http(http)
    , m_Auth(auth)
    , m_Settings(settings)
    , m_Rendezvous(rendezvous)
    , m_Ports(routerPorts)
{
    connect(m_Rendezvous, &RendezvousClient::sessionOpened, this, &ControlTunnel::onSessionOpened);
    connect(m_Rendezvous, &RendezvousClient::sessionClosed, this, &ControlTunnel::onSessionClosed);
    connect(m_Rendezvous, &RendezvousClient::signalReceived, this, &ControlTunnel::onSignal);

    // The holes are opened when the line first comes up, not here. This object
    // is constructed unconditionally, and a machine with Internet Access off
    // never announces itself — it should not be touching the router either.
    // Nor should one whose owner turned UPnP off: the allocator reads that
    // switch, and this is the first place the tunnel honours it.
    connect(m_Rendezvous, &RendezvousClient::onlineChanged, this, [this](bool online) {
        // An open tunnel does not need the line, and its own end says when it
        // goes. One still being set up does: nothing more will reach it.
        if (!online) {
            dropUnfinishedPeers();
            return;
        }
        if (!m_Ports->enabled()) return;
        m_Ports->ensureStarted();
        // Already discovered on behalf of a stream: the signal below has fired.
        if (m_Ports->gatewayState() == RouterPortAllocator::GatewayState::Ready &&
            (m_PublicIP.empty() || m_Ports->heldTunnelPorts().isEmpty()))
            onGatewayReady(m_Ports->publicIp());
    });
    connect(m_Ports, &RouterPortAllocator::gatewayReady, this, &ControlTunnel::onGatewayReady);
    connect(m_Ports, &RouterPortAllocator::gatewayMissing, this,
            [this]() { giveUpWaiting(QStringLiteral("no IGD on this network")); });
    connect(m_Ports, &RouterPortAllocator::portLost, this, &ControlTunnel::onPortLost);
    connect(m_Ports, &RouterPortAllocator::switchedOff, this, [this]() {
        const QList<uint16_t> spare = m_FreePorts;
        m_FreePorts.clear();
        for (const uint16_t port : spare)
            m_Ports->releaseTunnelPort(port);
    });

    Logger::info(QStringLiteral("[Tunnel] Ready — host key %1").arg(hostKeyFingerprint()));
}

ControlTunnel::~ControlTunnel()
{
    const QStringList ids = m_Peers.keys();
    for (const QString& id : ids)
        dropPeer(id, QStringLiteral("shutting down"));
    // The holes themselves are the allocator's to remove.
    m_FreePorts.clear();
}

// ── The router holes ────────────────────────────────────────────────────────

void ControlTunnel::onGatewayReady(const QString& publicIp)
{
    m_PublicIP = publicIp.toStdString();
    // One hole up front, so the first browser is served the moment it arrives
    // rather than waiting a SOAP round trip for it. The rest come on demand.
    if (m_Ports->heldTunnelPorts().isEmpty() &&
        m_Ports->inFlight(RouterPortAllocator::Purpose::Tunnel) == 0)
        requestTunnelPort();
}

void ControlTunnel::requestTunnelPort()
{
    using Purpose = RouterPortAllocator::Purpose;
    if (m_Ports->inFlight(Purpose::Tunnel) > 0) return;
    if (m_Ports->heldTunnelPorts().size() >= mw::routerports::kTunnelPortCap) return;

    m_Ports->claimTunnelPort(this, [this](const RouterPortAllocator::Claim& claim,
                                          const QString& why) {
        if (claim.external == 0) {
            giveUpWaiting(why);
            return;
        }
        // Switched off while the router was answering.
        if (!m_Ports->enabled()) {
            m_Ports->releaseTunnelPort(claim.external);
            giveUpWaiting(QStringLiteral("UPnP is off"));
            return;
        }
        if (m_PublicIP.empty()) m_PublicIP = m_Ports->publicIp().toStdString();
        m_FreePorts.append(claim.external);
        // The port by name, not just a count: the first thing anyone
        // diagnosing a dead tunnel needs is which entry in the router's table
        // is supposed to be ours.
        Logger::info(QStringLiteral("[Tunnel] Router hole %1 ready (public %2), %3 of %4 — more "
                                    "are claimed as browsers arrive")
                         .arg(claim.external)
                         .arg(QString::fromStdString(m_PublicIP))
                         .arg(m_Ports->heldTunnelPorts().size())
                         .arg(mw::routerports::kTunnelPortCap));
        serveWaitingPeers();
        if (m_WaitingPeers.isEmpty()) return;
        // More browsers than holes: ask again, and if the cap says no, they
        // are not kept waiting for a hole that is not coming.
        requestTunnelPort();
        if (m_Ports->inFlight(Purpose::Tunnel) == 0)
            giveUpWaiting(QStringLiteral("every hole this host may hold is in use"));
    });
}

void ControlTunnel::serveWaitingPeers()
{
    while (!m_WaitingPeers.isEmpty() && !m_FreePorts.isEmpty()) {
        const QString id = m_WaitingPeers.takeFirst();
        Peer* pp = peer(id);
        if (!pp || !pp->awaitingPort) continue;
        startPeer(*pp);
    }
}

void ControlTunnel::giveUpWaiting(const QString& why)
{
    const QStringList waiting = m_WaitingPeers;
    m_WaitingPeers.clear();
    for (const QString& id : waiting) {
        Peer* pp = peer(id);
        if (!pp || !pp->awaitingPort) continue;
        Logger::info(
            QStringLiteral("[Tunnel] Session %1 gets an ephemeral port — %2").arg(id.left(8), why));
        startPeer(*pp);
    }
}

void ControlTunnel::onPortLost(int purpose, int slot, quint16 port, const QString& owner)
{
    Q_UNUSED(slot);
    if (purpose != static_cast<int>(RouterPortAllocator::Purpose::Tunnel)) return;
    m_FreePorts.removeAll(port);
    for (const auto& owned : std::as_const(m_Peers)) {
        if (owned->port != port) continue;
        // Its socket keeps working through the reflexive path, or it does not;
        // nothing can be done for a live connection except say so.
        Logger::warning(QStringLiteral("[Tunnel] Session %1 is bound to port %2, which the router "
                                       "now forwards to %3")
                            .arg(owned->sessionId.left(8))
                            .arg(port)
                            .arg(owner));
    }
    // Somewhere else in the list, most likely: this is the moment the second
    // host on the LAN appeared, and it is holding what used to be ours. The
    // next browser should still find a hole waiting.
    if (m_FreePorts.isEmpty()) requestTunnelPort();
}

uint16_t ControlTunnel::takeTunnelPort()
{
    if (m_FreePorts.isEmpty()) return 0;
    return m_FreePorts.takeFirst();
}

void ControlTunnel::releaseTunnelPort(uint16_t port)
{
    if (port == 0 || m_FreePorts.contains(port)) return;
    if (!m_Ports->enabled()) {
        m_Ports->releaseTunnelPort(port);
        return;
    }
    // Only a hole this host still holds: one lost at renewal is gone for good.
    for (const RouterPortAllocator::Claim& c : m_Ports->heldTunnelPorts()) {
        if (c.external != port) continue;
        m_FreePorts.append(port);
        return;
    }
}

QString ControlTunnel::hostKeyFingerprint() const
{
    const QByteArray spki = m_Settings->hostSigningPublicKey();
    if (spki.isEmpty()) return {};
    const QByteArray digest = QCryptographicHash::hash(spki, QCryptographicHash::Sha256);

    QStringList parts;
    parts.reserve(digest.size());
    for (char c : digest)
        parts
            << QStringLiteral("%1").arg(static_cast<quint8>(c), 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char(':'));
}

ControlTunnel::Peer* ControlTunnel::peer(const QString& sessionId)
{
    auto it = m_Peers.find(sessionId);
    return it == m_Peers.end() ? nullptr : it.value().get();
}

QString ControlTunnel::peerAddress(const Peer& p) const
{
    // The address ICE actually settled on. This is not something the browser
    // told us — it is where our own stack is sending packets — which makes it
    // the one trustworthy statement about where this peer sits, and it is what
    // lets a browser on the same LAN keep its direct path when it launches a
    // stream. It grants no admin right: serveRequest() shuts the local
    // exemption for every tunnel arrival regardless of what this returns. It
    // does place a caller on this LAN for the rules that ask where one is
    // (AuthManager::fromThisLan): the remote admin unlock, a device asking by
    // its QR code, and a paired session answering such a request.
    if (!p.pc) return peerLabel(p.sessionId);
    try {
        rtc::Candidate local;
        rtc::Candidate remote;
        if (p.pc->getSelectedCandidatePair(&local, &remote)) {
            const auto addr = remote.address();
            if (addr.has_value() && !addr->empty()) return QString::fromStdString(*addr);
        }
    } catch (const std::exception&) {
        // No pair yet, or none that can be described. The label stands in.
    }
    return peerLabel(p.sessionId);
}

// ── Session lifecycle ───────────────────────────────────────────────────────

void ControlTunnel::onSessionOpened(const QString& sessionId)
{
    if (m_Peers.contains(sessionId)) {
        Logger::warning(QStringLiteral("[Tunnel] Session %1 announced twice").arg(sessionId));
        return;
    }

    auto owned = std::make_shared<Peer>();
    Peer& p = *owned;
    p.sessionId = sessionId;
    m_Peers.insert(sessionId, std::move(owned));

    Logger::info(QStringLiteral("[Tunnel] Browser arrived on session %1").arg(sessionId.left(8)));

    // A browser that never opens its channel — gone mid-handshake, its answer
    // lost — was otherwise kept, router hole and all, until the host restarted.
    QTimer::singleShot(TunnelPeerPolicy::kHandshakeDeadlineMs, this, [this, sessionId]() {
        Peer* pp = peer(sessionId);
        if (!pp || channelOpen(*pp)) return;
        m_Rendezvous->closeSession(sessionId);
        dropPeer(sessionId, QStringLiteral("no channel within %1 s")
                                .arg(TunnelPeerPolicy::kHandshakeDeadlineMs / 1000));
    });

    // A hole is free: build the connection on it now, the common case. It may
    // have been held for hours, so the router is asked again behind it.
    p.port = takeTunnelPort();
    if (p.port != 0 || !m_Ports->enabled()) {
        if (p.port != 0) m_Ports->recheck(p.port);
        createPeerConnection(p);
        return;
    }

    // None free. Claim one for this browser and build the connection when it
    // lands — a few hundred milliseconds, or two seconds more if the gateway
    // is still being discovered — unless nothing is coming: no gateway, the
    // cap reached, the pool exhausted. Then an ephemeral port straight away,
    // which is what every tunnel had before any of this existed.
    using Purpose = RouterPortAllocator::Purpose;
    using State = RouterPortAllocator::GatewayState;
    m_Ports->ensureStarted();
    const State state = m_Ports->gatewayState();
    const bool gatewayMayAnswer = state == State::Ready || state == State::Discovering;
    const bool oneIsComing = m_Ports->inFlight(Purpose::Tunnel) > 0;
    const bool roomForOne = m_Ports->heldTunnelPorts().size() < mw::routerports::kTunnelPortCap;
    if (!gatewayMayAnswer || (!oneIsComing && !roomForOne)) {
        createPeerConnection(p);
        return;
    }

    p.awaitingPort = true;
    m_WaitingPeers.append(sessionId);
    Logger::info(
        QStringLiteral("[Tunnel] Session %1 waiting for a router hole").arg(sessionId.left(8)));
    requestTunnelPort();
    QTimer::singleShot(mw::routerports::kClaimWaitMs, this, [this, sessionId]() {
        Peer* pp = peer(sessionId);
        if (!pp || !pp->awaitingPort) return;
        Logger::info(QStringLiteral("[Tunnel] Session %1 — no hole within %2 ms, ephemeral port")
                         .arg(sessionId.left(8))
                         .arg(mw::routerports::kClaimWaitMs));
        startPeer(*pp);
    });
}

void ControlTunnel::startPeer(Peer& p)
{
    p.awaitingPort = false;
    m_WaitingPeers.removeAll(p.sessionId);
    if (p.port == 0) p.port = takeTunnelPort();
    createPeerConnection(p);
}

void ControlTunnel::createPeerConnection(Peer& p)
{
    // libdatachannel throws when it cannot gather candidates, and the usual
    // reason is the pinned port: another MoonlightWeb on this machine, or
    // anything else, already bound it. Left uncaught in a Qt slot, that
    // exception took the whole host down with every stream it was carrying.
    // A port that will not bind is given back and the browser gets an
    // ephemeral one; only when that fails too is this one session refused.
    const QString id = p.sessionId;
    try {
        buildPeerConnection(p);
        return;
    } catch (const std::exception& e) {
        p.pc.reset();
        p.dc.reset();
        if (p.port == 0) {
            Logger::error(QStringLiteral("[Tunnel] Session %1 refused — no peer connection: %2")
                              .arg(id.left(8), QString::fromUtf8(e.what())));
            m_Rendezvous->closeSession(id);
            dropPeer(id, QStringLiteral("the peer connection could not be built"));
            return;
        }
        Logger::warning(QStringLiteral("[Tunnel] Session %1 could not bind port %2 (%3) — "
                                       "trying an ephemeral port")
                            .arg(id.left(8))
                            .arg(p.port)
                            .arg(QString::fromUtf8(e.what())));
        releaseTunnelPort(p.port);
        p.port = 0;
    }
    createPeerConnection(p);
}

void ControlTunnel::buildPeerConnection(Peer& p)
{
    const QString sessionId = p.sessionId;

    // ICE for the control channel.
    //
    // STUN is always on and LAN host candidates are always emitted, which is a
    // deliberate departure from the stream path. There, the browser's address
    // comes from /start and the two cases can be told apart; here it is not
    // known yet, and the two failure modes are not symmetric. Withholding the
    // LAN candidates costs a browser on the same network its direct path — on a
    // router without hairpin NAT, any path at all — while emitting them
    // discloses a private address that says nothing about how to reach it, to
    // someone already holding a 128-bit identifier for this machine.
    rtc::Configuration config;
    config.iceTransportPolicy = rtc::TransportPolicy::All;
    config.enableIceTcp = true;
    // Our ICE-TCP only dials, and so does a browser: where the network lets no
    // UDP out, this line is the first thing that fails. With the owner's
    // switch, it also listens on its port's TCP side while the checks run
    // (docs/design/ice-tcp-passive.md).
    config.enableIceTcpPassive = m_Settings->iceTcpPassive();
    p.dropUdp = m_Settings->debugIceDropUdp();
    if (p.dropUdp)
        Logger::warning(
            QStringLiteral("[Tunnel] Session %1: DEBUG debug_ice_drop_udp is ON — every "
                           "UDP candidate is dropped, only passive ICE-TCP can bind")
                .arg(sessionId.left(8)));

    // Bind to a port the router forwards, when there is one.
    //
    // Without it this connection takes an ephemeral port, and the only address
    // it can offer a browser out on the internet is the reflexive one STUN
    // reports. That address is a hole the router opened towards the STUN server
    // — a router that filters by peer address, which most do, drops the
    // browser's checks arriving from somewhere else entirely, and the tunnel
    // dies in silence while a stream to the same machine connects on its own
    // mapped port. Pinning the socket is what makes the mapping mean anything.
    if (p.port != 0) {
        config.portRangeBegin = p.port;
        config.portRangeEnd = p.port;
    }
    // The ICE-TCP-on-443 prototype's bench key (AppSettings::
    // debugTunnelPublicAddress), only where no router hole is held: a port the
    // router forwards by a rule of its own, offered under the address given.
    // Held by something else, the bind fails and this one session is refused
    // (createPeerConnection), which is the answer such a bench wants.
    QString pinnedIp;
    quint16 pinnedPort = 0;
    if (p.port == 0 && TunnelPeerPolicy::parsePublicAddress(m_Settings->debugTunnelPublicAddress(),
                                                            &pinnedIp, &pinnedPort)) {
        config.portRangeBegin = pinnedPort;
        config.portRangeEnd = pinnedPort;
        Logger::warning(QStringLiteral("[Tunnel] Session %1: DEBUG debug_tunnel_public_address is "
                                       "ON — port %2 bound, offered as %3:%2")
                            .arg(sessionId.left(8))
                            .arg(pinnedPort)
                            .arg(pinnedIp));
    }
    // The settings value, which defaults to our own server. This is the host
    // half of the connection bootstrap/v1/tunnel.js makes: that page refuses
    // Google's web fonts so that nobody outside learns who is connecting to
    // whom, and asking Google's STUN server from either end gave that away
    // anyway. Both ends now ask the machine they are already talking to.
    config.iceServers.emplace_back(m_Settings->stunServer().toStdString());

    PacketMarking::apply();
    p.pc = std::make_shared<rtc::PeerConnection>(config);

    const QString id = sessionId;
    // Every callback below is about THIS peer connection. The one a pinned
    // port refused fails as well, and its word arrives queued, after the
    // ephemeral one has replaced it: taken for the session's end, it closed
    // the browser's line ("The introduction server hung up") whenever another
    // program held the hole's port — the service beside a --dev instance, on
    // one machine.
    const int attempt = ++p.attempt;
    const bool dropUdp = p.dropUdp;

    p.pc->onLocalDescription([this, id, attempt, dropUdp](const rtc::Description& sdp) {
        const std::string text =
            dropUdp ? mw::ice::stripUdpCandidates(std::string(sdp)) : std::string(sdp);
        QMetaObject::invokeMethod(
            this,
            [this, id, attempt, text]() {
                Peer* pp = peer(id);
                if (!pp || pp->attempt != attempt) return;
                pp->pendingOffer = text;
                pp->localFingerprint = SdpFingerprint::extract(QString::fromStdString(text));
                if (pp->localFingerprint.isEmpty()) {
                    abort(*pp, QStringLiteral("our own offer carries no single sha-256 "
                                              "fingerprint"));
                    return;
                }
                // The host's signature covers the browser's nonce, so the offer
                // waits for the hello that carries it. That ordering is what
                // lets the browser check us BEFORE it creates any DTLS state.
                sendSignedOffer(*pp);
            },
            Qt::QueuedConnection);
    });

    const std::string publicIP = pinnedPort != 0 ? pinnedIp.toStdString() : m_PublicIP;
    const uint16_t mappedPort = pinnedPort != 0 ? pinnedPort : p.port;
    p.pc->onLocalCandidate([this, id, attempt, publicIP, mappedPort,
                            dropUdp](const rtc::Candidate& candidate) {
        const std::string cand = candidate.candidate();
        const std::string mid = candidate.mid();
        if (dropUdp && mw::ice::isUdpCandidate(cand)) return;

        // The same socket, as the rest of the world sees it.
        //
        // A LAN address tells a browser at the office nothing — it cannot route
        // to 192.168.x.x — so the mapped port has to be advertised under the
        // address the router answers on. This is the stream path's own trick
        // (RelayBase::emitLocalCandidate), and it is the candidate that
        // connects in practice: measured on the stream side as ICE going from
        // checking to connected in a single step.
        //
        // Only IPv4 host candidates over UDP, or over TCP for the passive one,
        // whose listener shares the UDP socket's port (the router maps that
        // port's TCP side too): IPv6 traverses no NAT and needs no rewrite, an
        // active TCP candidate is never dialled, and an unresolved mDNS
        // candidate has no address to compare. Nothing new is disclosed either
        // — the reflexive candidate already carries this exact address.
        const bool tcp = candidate.transportType() == rtc::Candidate::TransportType::TcpPassive;
        std::string publicCand;
        if (!publicIP.empty() && mappedPort != 0 &&
            candidate.type() == rtc::Candidate::Type::Host &&
            (candidate.transportType() == rtc::Candidate::TransportType::Udp || tcp)) {
            const auto addr = candidate.address();
            const bool isIpv4 =
                addr.has_value() && QHostAddress(QString::fromStdString(*addr)).protocol() ==
                                        QAbstractSocket::IPv4Protocol;
            if (isIpv4) {
                try {
                    rtc::Candidate rewritten = candidate;
                    rewritten.changeAddress(publicIP, mappedPort);
                    publicCand = rewritten.candidate();
                } catch (const std::exception& e) {
                    Logger::warning(
                        QStringLiteral("[Tunnel] Could not rewrite a host candidate: %1")
                            .arg(QString::fromUtf8(e.what())));
                }
            }
        }

        QMetaObject::invokeMethod(
            this,
            [this, id, attempt, cand, mid, publicCand, tcp, publicIP, mappedPort]() {
                Peer* pp = peer(id);
                if (!pp || pp->attempt != attempt) return;
                const auto send = [this, &id, &mid](const std::string& line) {
                    m_Rendezvous->sendSignal(
                        id, QJsonObject{{QStringLiteral("type"), QStringLiteral("ice")},
                                        {QStringLiteral("candidate"), QString::fromStdString(line)},
                                        {QStringLiteral("mid"), QString::fromStdString(mid)}});
                };
                send(cand);
                // One public candidate, however many adapters this machine has:
                // they would all rewrite to the same address and port, and the
                // browser would check the same path several times over.
                bool& sent = tcp ? pp->publicTcpCandidateSent : pp->publicCandidateSent;
                if (!publicCand.empty() && !sent) {
                    sent = true;
                    send(publicCand);
                    // Logged because this one line is the difference between a
                    // browser out on the internet having a reachable address to
                    // aim at and having none, and because the failure it fixes
                    // is silent at both ends.
                    Logger::info(QStringLiteral("[Tunnel] Session %1 offered %2:%3 as its public "
                                                "address%4")
                                     .arg(id.left(8), QString::fromStdString(publicIP))
                                     .arg(mappedPort)
                                     .arg(tcp ? QStringLiteral(" (TCP, listening)") : QString()));
                }
            },
            Qt::QueuedConnection);
    });

    p.pc->onStateChange([this, id, attempt](rtc::PeerConnection::State state) {
        if (state != rtc::PeerConnection::State::Failed &&
            state != rtc::PeerConnection::State::Closed)
            return;
        QMetaObject::invokeMethod(
            this,
            [this, id, attempt]() {
                const Peer* pp = peer(id);
                // A replaced connection's end is not the session's (see above).
                if (pp && pp->attempt != attempt) return;
                // Detached: the server has already closed this session.
                if (pp && pp->detached) {
                    dropPeer(id, QStringLiteral("the peer connection ended"));
                    return;
                }
                // The introduction server is told, and that is not a courtesy.
                // It is the only party that knows this session is over: the
                // browser's socket to it is still open — a page whose connection
                // failed keeps its socket, it has nothing else — and a session
                // it still counts is one of the few a machine is allowed at
                // once. Left unsaid, a handful of failed attempts is a machine
                // that answers nobody until the tabs are closed.
                m_Rendezvous->closeSession(id);
                dropPeer(id, QStringLiteral("the peer connection ended"));
            },
            Qt::QueuedConnection);
    });

    // The host is the offerer, as it is on the stream path — creating the
    // channel is what makes libdatachannel produce the offer.
    p.dc = p.pc->createDataChannel("mw-ctl");

    p.dc->onMessage(
        [this, id](rtc::binary data) {
            const QByteArray bytes(reinterpret_cast<const char*>(data.data()),
                                   static_cast<qsizetype>(data.size()));
            QMetaObject::invokeMethod(
                this, [this, id, bytes]() { onChannelMessage(id, bytes); }, Qt::QueuedConnection);
        },
        [](rtc::string) { /* the tunnel speaks binary only */ });

    // The channel is the whole tunnel: once it closes there is nothing left to
    // keep. This used to be left to the server's `close`, which a detached
    // tunnel no longer gets.
    p.dc->onClosed([this, id, attempt]() {
        QMetaObject::invokeMethod(
            this,
            [this, id, attempt]() {
                const Peer* pp = peer(id);
                if (!pp || pp->attempt != attempt) return;
                if (!pp->detached) m_Rendezvous->closeSession(id);
                dropPeer(id, QStringLiteral("the channel closed"));
            },
            Qt::QueuedConnection);
    });

    p.dc->onBufferedAmountLow([this, id]() {
        QMetaObject::invokeMethod(
            this,
            [this, id]() {
                if (Peer* pp = peer(id)) drain(*pp);
            },
            Qt::QueuedConnection);
    });
    p.dc->setBufferedAmountLowThreshold(TunnelFrame::kChunkBytes);
}

void ControlTunnel::onSessionClosed(const QString& sessionId)
{
    Peer* pp = peer(sessionId);
    if (!pp || pp->detached) return;

    // The server's `close` is not always the browser leaving: a restart of the
    // proxy in front of it closes every socket, and a browser's that goes before
    // this machine's line is reported as a departure. A tunnel whose channel is
    // open no longer needs the server, so it stays — for a browser on this LAN,
    // by the address ICE settled on, or a device a request has vouched for. A
    // browser that really left closes the channel itself, or ICE consent runs
    // out. See TunnelPeerPolicy.h.
    const bool open = channelOpen(*pp);
    const bool onLan = AuthManager::isLanAddress(peerAddress(*pp));
    if (!TunnelPeerPolicy::keepOnClose(open, onLan, pp->known)) {
        dropPeer(sessionId, QStringLiteral("the browser left"));
        return;
    }
    pp->detached = true;
    pp->detachOrder = ++m_DetachSeq;
    const QString who = onLan ? QStringLiteral("on this LAN") : QStringLiteral("a known device");
    Logger::info(QStringLiteral("[Tunnel] Session %1 kept after the server closed it — its channel "
                                "is open and it is %2")
                     .arg(sessionId.left(8), who));
    enforceDetachedCap();
}

bool ControlTunnel::channelOpen(const Peer& p)
{
    return p.dc && p.dc->isOpen();
}

void ControlTunnel::noteCaller(Peer& p, const HttpRequest& req)
{
    if (p.known) return;
    p.known = m_KnownCaller ? m_KnownCaller(req) : m_Http->isAuthenticated(req);
}

void ControlTunnel::dropUnfinishedPeers()
{
    const QStringList ids = m_Peers.keys();
    for (const QString& id : ids) {
        const Peer* pp = peer(id);
        if (!pp || channelOpen(*pp)) continue;
        dropPeer(id, QStringLiteral("the line to the introduction server dropped before the "
                                    "channel opened"));
    }
}

void ControlTunnel::enforceDetachedCap()
{
    for (;;) {
        std::vector<TunnelPeerPolicy::Detached> detached;
        for (const auto& owned : std::as_const(m_Peers)) {
            if (owned->detached)
                detached.push_back({owned->sessionId, owned->detachOrder, owned->known});
        }
        const QString victim = TunnelPeerPolicy::evictOne(detached);
        if (victim.isEmpty()) return;
        dropPeer(victim, QStringLiteral("more than %1 tunnels kept without the server")
                             .arg(TunnelPeerPolicy::kMaxDetached));
    }
}

void ControlTunnel::dropPeer(const QString& sessionId, const QString& why)
{
    auto it = m_Peers.find(sessionId);
    if (it == m_Peers.end()) return;

    const std::shared_ptr<Peer> owned = it.value();
    m_Peers.erase(it);
    m_WaitingPeers.removeAll(sessionId);

    for (auto& ws : owned->sockets) {
        if (ws) {
            ws->abort();
            ws->deleteLater();
        }
    }
    owned->sockets.clear();

    // Detach the callbacks before the objects go. libdatachannel may still be
    // inside one on a thread of its own, and it must not come back to a peer
    // that is halfway through being destroyed.
    if (owned->dc) {
        owned->dc->onMessage(nullptr, nullptr);
        owned->dc->onBufferedAmountLow(nullptr);
        owned->dc->onClosed(nullptr);
        owned->dc->close();
    }
    if (owned->pc) {
        owned->pc->onLocalDescription(nullptr);
        owned->pc->onLocalCandidate(nullptr);
        owned->pc->onStateChange(nullptr);
        owned->pc->close();
    }

    // After the close, which is the best this can do: libdatachannel lets the
    // socket go on its own thread. A browser arriving in that same instant may
    // fail to bind and fall back to an ephemeral port — the behaviour every
    // tunnel had before the pool existed, so a lost race costs a reachable
    // candidate, never the connection.
    releaseTunnelPort(owned->port);

    Logger::info(QStringLiteral("[Tunnel] Session %1 closed — %2").arg(sessionId.left(8), why));
}

void ControlTunnel::abort(Peer& p, const QString& why)
{
    // The session id is copied out first: dropPeer() destroys the peer this
    // reference points into.
    const QString id = p.sessionId;
    Logger::error(
        QStringLiteral("[Tunnel] MW-BIND-v1 refused session %1: %2").arg(id.left(8), why));
    m_Rendezvous->closeSession(id);
    dropPeer(id, QStringLiteral("pairing verification failed"));
}

// ── Signalling ──────────────────────────────────────────────────────────────

void ControlTunnel::onSignal(const QString& sessionId, const QJsonObject& payload)
{
    Peer* p = peer(sessionId);
    if (!p) return;

    const QString type = payload.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("hello"))
        handleHello(*p, payload);
    else if (type == QLatin1String("sdp"))
        handleAnswer(*p, payload);
    else if (type == QLatin1String("ice"))
        handleIce(*p, payload);
    else if (type == QLatin1String("diag"))
        handleDiag(*p, payload);
}

void ControlTunnel::handleHello(Peer& p, const QJsonObject& msg)
{
    if (p.helloSeen) {
        abort(p, QStringLiteral("a second hello on the same connection"));
        return;
    }

    // The browser presents the key it will sign with. On the stream path the
    // host already knows that key, because the session it belongs to was paired
    // with a PIN; here there is no session yet — the login screen is what this
    // channel is being opened to fetch. So the key is taken as presented, and
    // used to bind the channel rather than to say who is holding it.
    p.browserSpki =
        QByteArray::fromBase64(msg.value(QStringLiteral("public_key")).toString().toUtf8());
    if (!PairingCrypto::isValidP256Spki(p.browserSpki)) {
        abort(p, QStringLiteral("the hello does not carry a usable P-256 public key"));
        return;
    }

    p.browserKeyId = PairingCrypto::keyId(p.browserSpki);
    if (msg.value(QStringLiteral("key_id")).toString() != p.browserKeyId) {
        abort(p, QStringLiteral("the hello's key id does not match the key it presented"));
        return;
    }

    p.nonceBrowser = QByteArray::fromBase64(msg.value(QStringLiteral("nonce")).toString().toUtf8());
    if (p.nonceBrowser.size() < PairingCrypto::MIN_NONCE_BYTES) {
        abort(p, QStringLiteral("the hello nonce is too short to be single-use"));
        return;
    }

    p.helloSeen = true;
    sendSignedOffer(p);
}

void ControlTunnel::sendSignedOffer(Peer& p)
{
    if (p.pendingOffer.empty() || !p.helloSeen) return;

    p.nonceHost = PairingCrypto::generateNonce();
    if (p.nonceHost.isEmpty()) {
        abort(p, QStringLiteral("no secure random available for the host nonce"));
        return;
    }

    const QString hostId = m_Settings->rendezvousId();
    const QByteArray toSign =
        PairingCrypto::hostDigestInput(hostId, p.browserKeyId, p.nonceBrowser, p.localFingerprint);
    const QByteArray sig = PairingCrypto::sign(m_Settings->hostSigningKeyPem(), toSign);
    if (sig.isEmpty()) {
        // Sending it unsigned would teach the browser that unsigned offers are
        // acceptable, which is the downgrade this protocol exists to prevent.
        abort(p, QStringLiteral("could not sign the offer with the host key"));
        return;
    }

    m_Rendezvous->sendSignal(
        p.sessionId, QJsonObject{
                         {QStringLiteral("type"), QStringLiteral("sdp")},
                         {QStringLiteral("sdp"), QString::fromStdString(p.pendingOffer)},
                         {QStringLiteral("protocol"), QString::fromLatin1(PairingCrypto::PROTOCOL)},
                         {QStringLiteral("host_id"), hostId},
                         {QStringLiteral("host_key"),
                          QString::fromLatin1(m_Settings->hostSigningPublicKey().toBase64())},
                         {QStringLiteral("nonce"), QString::fromLatin1(p.nonceHost.toBase64())},
                         {QStringLiteral("sig"), QString::fromLatin1(sig.toBase64())},
                     });
    p.pendingOffer.clear();
}

void ControlTunnel::handleAnswer(Peer& p, const QJsonObject& msg)
{
    if (p.answerVerified) return;
    if (!p.helloSeen) {
        abort(p, QStringLiteral("an answer arrived before the hello"));
        return;
    }

    const QString sdp = msg.value(QStringLiteral("sdp")).toString();
    const QString remoteFingerprint = SdpFingerprint::extract(sdp);
    if (remoteFingerprint.isEmpty()) {
        abort(p, QStringLiteral("the answer does not commit to one sha-256 fingerprint"));
        return;
    }

    const QByteArray sig =
        QByteArray::fromBase64(msg.value(QStringLiteral("sig")).toString().toUtf8());
    if (sig.isEmpty()) {
        abort(p, QStringLiteral("the answer carries no signature"));
        return;
    }

    const QByteArray signedBytes = PairingCrypto::browserDigestInput(
        m_Settings->rendezvousId(), p.nonceHost, p.localFingerprint, remoteFingerprint);
    if (!PairingCrypto::verify(p.browserSpki, signedBytes, sig)) {
        abort(p, QStringLiteral("the answer's signature does not verify"));
        return;
    }

    // The offer goes out only once the connection exists, so an answer cannot
    // precede it; the invariant is still worth a line, since a browser that
    // sent one anyway would otherwise dereference nothing.
    if (!p.pc) {
        abort(p, QStringLiteral("an answer arrived before the connection was built"));
        return;
    }

    p.answerVerified = true;
    try {
        const std::string answer =
            p.dropUdp ? mw::ice::stripUdpCandidates(sdp.toStdString()) : sdp.toStdString();
        p.pc->setRemoteDescription(rtc::Description(answer));
    } catch (const std::exception& e) {
        abort(p, QStringLiteral("the answer was not usable: %1").arg(QString::fromUtf8(e.what())));
        return;
    }
    Logger::info(QStringLiteral("[Tunnel] Session %1 bound — both fingerprints verified, key %2")
                     .arg(p.sessionId.left(8), p.browserKeyId.left(12)));
}

void ControlTunnel::handleIce(Peer& p, const QJsonObject& msg)
{
    // Candidates are taken only once the answer has verified: before that there
    // is no remote description to attach them to, and doing the work anyway
    // would be work done on behalf of a peer that has not proved anything.
    if (!p.answerVerified) return;
    const std::string candidate = msg.value(QStringLiteral("candidate")).toString().toStdString();
    if (p.dropUdp && mw::ice::isUdpCandidate(candidate)) return;
    try {
        p.pc->addRemoteCandidate(
            rtc::Candidate(candidate, msg.value(QStringLiteral("mid")).toString().toStdString()));
    } catch (const std::exception&) {
        // A candidate we cannot parse is one path out of several, not a failure.
    }
}

namespace {

/// A clock for the failure reports' hourly limit, which wall-clock changes
/// must not move.
qint64 diagClockMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

} // namespace

void ControlTunnel::handleDiag(Peer& p, const QJsonObject& msg)
{
    // Only from a browser whose signed answer verified, once per connection,
    // and only while its channel is not open: a report from anyone else is
    // dropped without a word, so it cannot be used to write in this log.
    if (!p.answerVerified || p.diagSeen || channelOpen(p)) return;
    p.diagSeen = true;

    const qint64 now = diagClockMs();
    closeDiagWindow();
    if (!m_DiagLimiter.admit(now)) {
        // Counted, and said once when the hour is over.
        if (!m_DiagWindowTimer) {
            m_DiagWindowTimer = true;
            QTimer::singleShot(m_DiagLimiter.msLeft(now) + 1, this, [this]() {
                m_DiagWindowTimer = false;
                closeDiagWindow();
            });
        }
        return;
    }
    Logger::info(QStringLiteral("[Tunnel] Session %1: %2")
                     .arg(p.sessionId.left(8), TunnelDiag::describe(msg)));
}

void ControlTunnel::closeDiagWindow()
{
    if (const int ignored = m_DiagLimiter.closeWindow(diagClockMs()))
        Logger::info(QStringLiteral("[Tunnel] %1 more browser failure reports in the past hour, "
                                    "past the %2 an hour that are written")
                         .arg(ignored)
                         .arg(TunnelDiag::kMaxLinesPerWindow));
}

// ── The channel ─────────────────────────────────────────────────────────────

void ControlTunnel::sendFrame(Peer& p, const QByteArray& frame)
{
    p.outbox.append(frame);
    drain(p);
}

void ControlTunnel::drain(Peer& p)
{
    if (p.draining || !p.dc || !p.dc->isOpen()) return;
    p.draining = true;

    // Stop pushing once the channel has a comfortable backlog, and let
    // onBufferedAmountLow bring us back. Handing a whole response over at once
    // would park it in libdatachannel's queue, where a browser that walked away
    // leaves it stranded.
    while (!p.outbox.isEmpty() && p.dc->bufferedAmount() < 4 * TunnelFrame::kChunkBytes) {
        const QByteArray frame = p.outbox.takeFirst();
        try {
            p.dc->send(reinterpret_cast<const std::byte*>(frame.constData()),
                       static_cast<size_t>(frame.size()));
        } catch (const std::exception& e) {
            Logger::warning(QStringLiteral("[Tunnel] Send failed on session %1: %2")
                                .arg(p.sessionId.left(8), QString::fromUtf8(e.what())));
            p.outbox.clear();
            break;
        }
    }
    p.draining = false;
}

void ControlTunnel::onChannelMessage(const QString& sessionId, const QByteArray& message)
{
    Peer* pp = peer(sessionId);
    if (!pp) return;

    quint8 kind = 0;
    quint32 id = 0;
    QByteArray payload;
    if (!TunnelFrame::parse(message, &kind, &id, &payload)) return;

    switch (kind) {
    case TunnelFrame::Request: handleRequestFrame(*pp, id, payload); break;
    case TunnelFrame::WsOpen: handleWsOpen(*pp, id, payload); break;
    case TunnelFrame::WsText: handleWsText(*pp, id, QString::fromUtf8(payload)); break;
    case TunnelFrame::WsClose: closeWs(*pp, id, QString::fromUtf8(payload)); break;
    default: break;
    }
}

void ControlTunnel::handleRequestFrame(Peer& p, quint32 id, const QByteArray& payload)
{
    if (payload.size() > TunnelFrame::kMaxRequestBytes) {
        sendResponse(p.sessionId, id, HttpResponse::error(413, "Payload Too Large"), QString());
        return;
    }
    if (p.inFlight >= kMaxInFlightRequests) {
        sendResponse(p.sessionId, id, HttpResponse::error(429, "Too Many Requests"), QString());
        return;
    }

    QJsonObject head;
    QByteArray body;
    if (!TunnelFrame::decodeHead(payload, &head, &body)) return;

    HttpRequest req;
    req.method = head.value(QStringLiteral("m")).toString().toUpper();
    req.body = body;
    req.clientAddress = peerAddress(p);
    req.tunnelKey = p.browserSpki;

    // Split the target the way HttpParser does for a socket request, so a route
    // sees the same shape whichever way the request arrived.
    const QUrl target(head.value(QStringLiteral("p")).toString(), QUrl::StrictMode);
    if (!target.isValid() || !target.path().startsWith(QLatin1Char('/'))) {
        sendResponse(p.sessionId, id, HttpResponse::error(400, "Bad Request"), QString());
        return;
    }
    req.path = target.path();
    const QUrlQuery query(target);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    for (const auto& item : items)
        req.queryParams.insert(item.first, item.second);

    const QJsonObject headers = head.value(QStringLiteral("h")).toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        const QString name = it.key().toLower();
        if (isForbiddenHeader(name)) continue;
        req.headers.insert(name, it.value().toString());
    }
    req.headers.insert(QStringLiteral("content-length"), QString::number(req.body.size()));

    noteCaller(p, req);

    const QString hostHeader = req.headers.value(QStringLiteral("host"));
    const QString sessionId = p.sessionId;
    const bool mayDeflate = head.value(QLatin1String(TunnelFrame::kEncodingField)).toString() ==
                                QLatin1String(TunnelFrame::kDeflate) &&
                            TunnelFrame::mayDeflate(req.method, req.path);
    p.inFlight++;

    m_Http->serveRequest(std::move(req), HttpServer::Arrival::Tunnel,
                         [this, sessionId, id, hostHeader, mayDeflate](const HttpResponse& resp) {
                             Peer* pp = peer(sessionId);
                             if (!pp) return;
                             pp->inFlight--;
                             sendResponse(sessionId, id, resp, hostHeader, mayDeflate);
                         });
}

void ControlTunnel::sendResponse(const QString& sessionId, quint32 id, const HttpResponse& response,
                                 const QString& hostHeader, bool mayDeflate)
{
    Peer* p = peer(sessionId);
    if (!p) return;

    // The same security headers a socket response carries. They are not
    // decoration here: the service worker hands this back as the document's own
    // Response, so this Content-Security-Policy is the one the browser enforces
    // on the page.
    QJsonObject headers;
    const QMap<QString, QString> security = HttpServer::securityHeaders(hostHeader);
    for (auto it = security.cbegin(); it != security.cend(); ++it)
        headers.insert(it.key(), it.value());
    if (!response.contentType.isEmpty())
        headers.insert(QStringLiteral("Content-Type"), response.contentType);
    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        headers.insert(it.key(), it.value());

    QJsonObject headObj{{QStringLiteral("s"), response.statusCode}, {QStringLiteral("h"), headers}};

    // The interface's files, deflated for a page that can inflate them: about a
    // quarter of the bytes for the scripts, which over a long round trip and a
    // TCP path is what the first visit spends its time on (docs/design/
    // ice-tcp-passive.md §5.5). Kept only when it is smaller.
    QByteArray body = response.body;
    if (mayDeflate &&
        TunnelFrame::worthDeflating(response.statusCode, response.contentType, body.size())) {
        QByteArray deflated = TunnelFrame::deflate(body);
        if (!deflated.isEmpty() && deflated.size() < body.size()) {
            body = std::move(deflated);
            headObj.insert(QLatin1String(TunnelFrame::kEncodingField),
                           QLatin1String(TunnelFrame::kDeflate));
        }
    }

    const QByteArray head = TunnelFrame::encodeHead(headObj, QByteArray());
    sendFrame(*p, TunnelFrame::build(TunnelFrame::Response, id, head));

    for (qsizetype offset = 0; offset < body.size(); offset += TunnelFrame::kChunkBytes)
        sendFrame(*p, TunnelFrame::build(TunnelFrame::Body, id,
                                         body.mid(offset, TunnelFrame::kChunkBytes)));

    sendFrame(*p, TunnelFrame::build(TunnelFrame::End, id, QByteArray()));
}

// ── Streaming signalling, carried through ───────────────────────────────────

void ControlTunnel::handleWsOpen(Peer& p, quint32 id, const QByteArray& payload)
{
    QJsonObject head;
    if (!TunnelFrame::decodeHead(payload, &head, nullptr)) return;

    if (p.sockets.size() >= kMaxSocketsPerPeer) {
        closeWs(p, id, QStringLiteral("too many signalling sockets"));
        return;
    }

    // The same authorisation a WebSocket upgrade gets on the socket path,
    // decided by HttpServer so there is one gate and not two. The cookies come
    // from the browser's own jar, kept by the service worker; nothing here can
    // be more privileged than the session behind them.
    HttpRequest asRequest;
    asRequest.method = QStringLiteral("GET");
    asRequest.path = head.value(QStringLiteral("p")).toString();
    asRequest.clientAddress = peerAddress(p);
    asRequest.tunnelKey = p.browserSpki;
    const QJsonObject headers = head.value(QStringLiteral("h")).toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it)
        asRequest.headers.insert(it.key().toLower(), it.value().toString());
    noteCaller(p, asRequest);

    QString error;
    const quint16 port = m_Http->authorizeTunnelWebSocket(asRequest, &error);
    if (port == 0) {
        closeWs(p, id, error);
        return;
    }

    QWebSocket* ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    p.sockets.insert(id, ws);

    const QString sessionId = p.sessionId;

    // Two different endings share the disconnected handler below, and telling
    // them apart is worth the flag: a socket that closes after carrying traffic
    // is the session ending, while one that never opened means nothing was
    // listening on that port — ordinarily a slot whose worker has not started.
    // Reporting both as a bare close leaves a browser with "it closed" and no
    // way to tell a finished session from one that never began.
    auto everOpened = std::make_shared<bool>(false);

    connect(ws, &QWebSocket::connected, this, [this, sessionId, id, everOpened]() {
        *everOpened = true;
        if (Peer* pp = peer(sessionId))
            sendFrame(*pp, TunnelFrame::build(TunnelFrame::WsOpened, id, QByteArray()));
    });
    connect(ws, &QWebSocket::textMessageReceived, this, [this, sessionId, id](const QString& text) {
        if (Peer* pp = peer(sessionId))
            sendFrame(*pp, TunnelFrame::build(TunnelFrame::WsText, id, text.toUtf8()));
    });
    connect(ws, &QWebSocket::disconnected, this, [this, sessionId, id, everOpened]() {
        Peer* pp = peer(sessionId);
        if (!pp) return;
        if (QPointer<QWebSocket> gone = pp->sockets.take(id)) gone->deleteLater();
        const QByteArray why =
            *everOpened ? QByteArray() : QByteArray("no stream is listening on that slot");
        sendFrame(*pp, TunnelFrame::build(TunnelFrame::WsClose, id, why));
    });

    QNetworkRequest request(QUrl(QStringLiteral("ws://127.0.0.1:%1/").arg(port)));
    request.setRawHeader(mw::loopback::kHeader, mw::loopback::key());
    ws->open(request);
}

void ControlTunnel::handleWsText(Peer& p, quint32 id, const QString& text)
{
    QPointer<QWebSocket> ws = p.sockets.value(id);
    if (ws && ws->isValid()) ws->sendTextMessage(text);
}

void ControlTunnel::closeWs(Peer& p, quint32 id, const QString& reason)
{
    if (QPointer<QWebSocket> ws = p.sockets.take(id)) {
        ws->close();
        ws->deleteLater();
    }
    sendFrame(p, TunnelFrame::build(TunnelFrame::WsClose, id, reason.toUtf8()));
}
