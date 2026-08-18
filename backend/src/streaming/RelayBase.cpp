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

} // namespace

void RelayBase::setPublicAddress(const std::string& publicIP, uint16_t publicPort)
{
    m_PublicIP = publicIP;
    m_PublicPort = publicPort;
    qInfo() << "[Relay] UPnP public address set:" << QString::fromStdString(publicIP) << ":"
            << publicPort;
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
                qInfo() << logTag << "Host candidate ->" << QString::fromStdString(m_PublicIP) << ":"
                        << m_PublicPort << (m_EmitLanCandidate ? "(+ LAN)" : "");
            } catch (const std::exception& e) {
                qWarning() << logTag << "Failed to rewrite candidate:" << e.what();
            }
        } else {
            qInfo() << logTag << "Skipping IPv6 candidate (cannot rewrite to IPv4):"
                    << QString::fromStdString(candidate.candidate());
        }
    }

    // When UPnP is active, suppress IPv6 candidates entirely so the browser's
    // ICE agent is forced onto the IPv4 UPnP path. Residential IPv6 often fails
    // because the router firewall blocks unsolicited inbound traffic
    // (DTLS/SCTP timeout).
    if (m_SuppressIPv6 && !isIpv4) {
        qInfo() << logTag << "Suppressing IPv6 candidate (UPnP active):"
                << QString::fromStdString(modCandidate.candidate()).left(80);
        return;
    }

    emit signalingIceCandidate(std::string(modCandidate.candidate()),
                               std::string(modCandidate.mid()));
}
