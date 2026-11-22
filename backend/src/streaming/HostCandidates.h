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

#include "IcePriority.h"
#include "server/NetClassify.h"

#include <cstdint>
#include <vector>

/// What the browser is told of one of the host's own ICE candidates
/// (RelayBase::emitLocalCandidate), as a rule apart from the relay, to be
/// tested: the candidate itself, a copy at the router's public address, both,
/// or nothing.
///
/// The same rule for every transport: a passive TCP candidate on an address is
/// judged as the UDP one on that address is.
namespace mw::ice {

/// One local candidate, as the relay sees it.
struct LocalCandidate
{
    enum class Transport
    {
        Udp,
        TcpPassive,
        TcpActive,
        TcpOther
    };

    /// A host candidate. A reflexive or relayed one goes out as it is: its
    /// address is the one the rest of the world already sees.
    bool host = true;
    bool ipv4 = true;
    /// Of the candidate's own address (an unresolved one counts as Public)
    NetClassify::Kind kind = NetClassify::Kind::Public;
    Transport transport = Transport::Udp;
    uint16_t port = 0;
};

/// What the relay knows of the router's holes and of the peer.
struct CandidateView
{
    /// setForceHostCandidatePublic()
    bool forceHostPublic = false;
    bool havePublicIp = false;
    /// setSharedPublicPorts(): the holes are `udpHoles` and `tcpHoles`, each
    /// forwarded to the same number. Otherwise the stream's own hole, `ownHole`.
    bool onSharedPorts = false;
    uint16_t ownHole = 0;
    std::vector<uint16_t> udpHoles;
    std::vector<uint16_t> tcpHoles;
    /// The shared port the router's TCP 443 leads to, 0 for none; a copy on
    /// 443 only when it is one of `tcpHoles`
    uint16_t tcp443LeadsTo = 0;
    /// setSuppressIPv6Candidates()
    bool suppressIpv6 = false;
    /// setEmitLanHostCandidate(): the peer is inside a network we control
    bool emitLan = false;
};

struct CandidateVerdict
{
    enum class Why
    {
        AsIs,           ///< sent as it is
        TunnelForLan,   ///< a mesh VPN address, to a peer of our network only
        TunnelWithheld, ///< a mesh VPN address, withheld from a public peer
        NotForwarded,   ///< an internal address on a port the router does not forward
        Copied,         ///< a copy at the public address (and the LAN one to a peer of ours)
        ActiveForLan,   ///< an active TCP candidate: no copy, the LAN one to a peer of ours
        Ipv6Withheld,   ///< a non-global IPv6 address, withheld from a public peer
    };

    /// The candidate itself goes out
    bool asIs = false;
    /// A copy at the public address on this port, 0 for none
    uint16_t publicPort = 0;
    /// And a second copy on the router's TCP 443
    bool by443 = false;
    Why why = Why::AsIs;
};

/// The rule. With a hole (the router forwards a port to this stream, and its
/// public address is known), an IPv4 host candidate is announced at the public
/// address, the internal one only to a peer of our own network; a mesh VPN
/// address goes to a peer of our own network only; a non-global IPv6 one is
/// withheld from a public peer; a global IPv6 one goes out as it is.
inline CandidateVerdict hostCandidateVerdict(const LocalCandidate& c, const CandidateView& v)
{
    using Why = CandidateVerdict::Why;
    using Transport = LocalCandidate::Transport;
    CandidateVerdict out;
    if (!c.host) {
        out.asIs = true;
        return out;
    }

    const bool overTcp = c.transport != Transport::Udp;
    const bool hasHole =
        v.onSharedPorts ? !v.udpHoles.empty() || !v.tcpHoles.empty() : v.ownHole > 0;
    if (v.forceHostPublic && v.havePublicIp && hasHole) {
        if (c.kind == NetClassify::Kind::Tunnel) {
            // A mesh VPN interface carries its own NAT traversal: rewriting it
            // to the public address would push the media back out to the
            // internet. As it is, and to a peer inside our own network only.
            out.asIs = v.emitLan;
            out.why = v.emitLan ? Why::TunnelForLan : Why::TunnelWithheld;
            return out;
        }
        if (c.ipv4) {
            // The internal candidate for a peer of our own network (many
            // routers do not hairpin UDP), never for another
            out.asIs = v.emitLan;
            if (c.transport == Transport::TcpActive) {
                // Port 9, which nobody dials: nothing at the public address
                out.why = Why::ActiveForLan;
                return out;
            }
            out.publicPort = publicPortFor(c.port, v.onSharedPorts, v.ownHole,
                                           overTcp ? v.tcpHoles : v.udpHoles);
            if (out.publicPort == 0) {
                out.why = Why::NotForwarded;
                return out;
            }
            out.why = Why::Copied;
            // Checked here against this stream's own TCP ports, whatever the
            // view was told: 443 leading anywhere else gives no copy
            out.by443 = v.onSharedPorts && has443Copy(c.transport == Transport::TcpPassive,
                                                      v.tcp443LeadsTo, v.tcpHoles);
            return out;
        }
        // IPv6 needs no NAT hole: only the gate below applies
    }

    // A non-global IPv6 (ULA, a mesh VPN's, link-local) is shown, like the
    // private IPv4 host candidate above, to a peer inside our own network only
    if (v.suppressIpv6 && !c.ipv4 && NetClassify::isTrustedPeer(c.kind) && !v.emitLan) {
        out.why = Why::Ipv6Withheld;
        return out;
    }
    out.asIs = true;
    return out;
}

/// What a copy at the public address is known by: one per transport and port,
/// however many of the host's addresses copy to it. The copy on TCP 443 has
/// its own (a passive TCP one, on 443).
inline uint32_t publicCopyKey(LocalCandidate::Transport transport, uint16_t publicPort)
{
    return (static_cast<uint32_t>(transport) << 16) | publicPort;
}

} // namespace mw::ice
