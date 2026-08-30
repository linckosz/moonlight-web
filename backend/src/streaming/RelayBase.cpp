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

#include "RelayBase.h"

#include "server/NetClassify.h"

#include <rtc/rtc.hpp>

#include <QDebug>
#include <QHostAddress>
#include <QString>

namespace {

/// Resolved address of a candidate. Host candidates carry a literal IP, so this
/// succeeds for every candidate we consider rewriting.
bool candidateAddress(const rtc::Candidate& candidate, QHostAddress* out)
{
    const auto addr = candidate.address();
    if (!addr.has_value()) return false;
    return out->setAddress(QString::fromStdString(*addr));
}

/// Fallback for an unresolved candidate: the address field sits after the first
/// space, so a colon past that point means IPv6. Kept because it is what this
/// code did before the resolved address was used, and an unresolved candidate
/// must keep behaving exactly as it used to.
bool looksIpv6(const std::string& candStr)
{
    const size_t firstSpace = candStr.find(' ');
    return firstSpace != std::string::npos &&
           candStr.find(':', firstSpace + 1) != std::string::npos;
}

/// "host 192.168.1.66:48010/UDP" — enough to tell a direct path from one that
/// leaves through the router and comes back (same public IP on both ends).
QString describeCandidate(const rtc::Candidate& c)
{
    const char* type = "unknown";
    switch (c.type()) {
    case rtc::Candidate::Type::Host: type = "host"; break;
    case rtc::Candidate::Type::ServerReflexive: type = "srflx"; break;
    case rtc::Candidate::Type::PeerReflexive: type = "prflx"; break;
    case rtc::Candidate::Type::Relayed: type = "relay"; break;
    case rtc::Candidate::Type::Unknown: break;
    }
    const char* transport = c.transportType() == rtc::Candidate::TransportType::Udp ? "UDP" : "TCP";

    const auto addr = c.address();
    const auto port = c.port();
    if (!addr.has_value() || !port.has_value()) {
        // Unresolved (an mDNS .local candidate, typically): the raw line is all
        // there is, and it still names the type.
        return QStringLiteral("%1 %2").arg(type, QString::fromStdString(c.candidate()));
    }
    return QStringLiteral("%1 %2:%3/%4")
        .arg(type, QString::fromStdString(*addr))
        .arg(*port)
        .arg(transport);
}

} // namespace

void RelayBase::setPublicAddress(const std::string& publicIP, uint16_t publicPort)
{
    m_PublicIP = publicIP;
    m_PublicPort = publicPort;
    qInfo() << "[Relay] UPnP public address set:" << QString::fromStdString(publicIP) << ":"
            << publicPort;
}

void RelayBase::logSelectedCandidatePair(rtc::PeerConnection& pc, const char* logTag)
{
    rtc::Candidate local;
    rtc::Candidate remote;
    if (!pc.getSelectedCandidatePair(&local, &remote)) {
        qInfo() << logTag << "Selected candidate pair: none reported";
        return;
    }
    qInfo() << logTag << "Selected candidate pair: local" << describeCandidate(local) << "-> remote"
            << describeCandidate(remote);
}

void RelayBase::emitLocalCandidate(const rtc::Candidate& candidate, const char* logTag)
{
    rtc::Candidate modCandidate = candidate;

    QHostAddress addr;
    const bool resolved = candidateAddress(candidate, &addr);
    const bool isIpv4 = resolved ? addr.protocol() == QAbstractSocket::IPv4Protocol
                                 : !looksIpv6(candidate.candidate());
    // An unresolved candidate is treated as public: the conservative verdict,
    // since the tunnel branch below grants an exemption.
    const NetClassify::Kind kind =
        resolved ? NetClassify::classify(addr.toString()) : NetClassify::Kind::Public;

    // If UPnP is active and this is a host candidate, rewrite it with the
    // public IP and mapped port. This gives the browser a reachable endpoint
    // through the UPnP-opened router port.
    if (m_ForceHostPublic && !m_PublicIP.empty() && m_PublicPort > 0 &&
        candidate.type() == rtc::Candidate::Type::Host) {
        if (kind == NetClassify::Kind::Tunnel) {
            // A mesh VPN interface carries its own NAT traversal and is already
            // reachable by any peer that can see it. Rewriting it to the public
            // IP would duplicate the LAN candidate *and* push the media back out
            // to the internet — silently defeating the tunnel the user chose.
            // Emit it untouched, and only to a peer inside our own network.
            if (m_EmitLanCandidate) {
                qInfo() << logTag << "Tunnel host candidate kept as-is:" << addr.toString();
                emit signalingIceCandidate(std::string(candidate.candidate()),
                                           std::string(candidate.mid()));
            } else {
                qInfo() << logTag << "Dropping tunnel host candidate for a public peer";
            }
            return;
        }

        if (isIpv4) {
            // For a peer inside our own network (incl. one reaching us through
            // the public URL via NAT hairpin), also advertise the original
            // private host candidate so it can connect directly to 192.168.x.x —
            // many routers don't hairpin UDP, so a public-only candidate never
            // becomes reachable locally. Gated on m_EmitLanCandidate (false for
            // public peers) so the internal address is never leaked outside.
            if (m_EmitLanCandidate)
                emit signalingIceCandidate(candidate.candidate(), std::string(candidate.mid()));
            try {
                modCandidate.changeAddress(m_PublicIP, m_PublicPort);
                qInfo() << logTag << "Host candidate ->" << QString::fromStdString(m_PublicIP)
                        << ":" << m_PublicPort << (m_EmitLanCandidate ? "(+ LAN)" : "");
            } catch (const std::exception& e) {
                qWarning() << logTag << "Failed to rewrite candidate:" << e.what();
            }
        } else {
            // Only the REWRITE is skipped — IPv6 needs no NAT hole, so the
            // candidate goes out as it is (subject to the non-global gate
            // below). The old wording said "Skipping IPv6 candidate", which
            // read as a drop.
            qInfo() << logTag << "IPv6 candidate kept unrewritten (no NAT to traverse):"
                    << QString::fromStdString(candidate.candidate());
        }
    }

    // IPv6 under UPnP. This used to suppress every IPv6 candidate so ICE was
    // forced onto the IPv4 UPnP path, on the grounds that residential IPv6 often
    // blocks unsolicited inbound traffic (DTLS/SCTP timing out silently).
    //
    // That cost more than it bought. The IPv4 path it forces goes to the public
    // address, so a peer on our own LAN reaches us by hairpinning through the
    // router — measured 2026-08-30: three DataChannel sessions from a Mac two
    // metres away nominated `srflx <public v4> -> prflx 192.168.1.254`, the
    // router's own LAN address, at 20 Mbps. The two MediaTrack sessions of the
    // same run reached a direct IPv6 pair instead, and only because ICE
    // rediscovered it peer-reflexively — suppression never stopped IPv6, it just
    // made winning it a race the transports lost or won at random.
    //
    // So: a GLOBAL IPv6 address is emitted. It is no more private than the
    // public IPv4 already advertised beside it, and it is an ADDITIONAL pair —
    // ICE falls back to the IPv4 pairs if its checks fail, which is precisely
    // the blocked-inbound case the suppression was guarding against.
    //
    // A non-global IPv6 (ULA — including a mesh-VPN address — or link-local)
    // stays behind the same gate as the private IPv4 host candidate above: only
    // a peer inside a network we control may see it.
    // `kind` classifies THIS candidate's own address, so a global v6 is Public
    // (emit) and a ULA/link-local one is Private (gate it).
    if (m_SuppressIPv6 && !isIpv4 && NetClassify::isTrustedPeer(kind) && !m_EmitLanCandidate) {
        qInfo() << logTag << "Suppressing non-global IPv6 candidate:"
                << QString::fromStdString(modCandidate.candidate()).left(80);
        return;
    }

    emit signalingIceCandidate(std::string(modCandidate.candidate()),
                               std::string(modCandidate.mid()));
}
