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

#include "Tcp443Guard.h"

#include <QStringList>

namespace mw::tcp443 {

using mw::routerholes::Hole;
using mw::routerholes::Kind;
using Lookup = IUpnpGateway::Lookup;

namespace {

const QString kTcp = QStringLiteral("TCP");

Verdict left(int step, const QString& said)
{
    return {Outcome::Left, step, said};
}

} // namespace

bool publicIpv4(const QString& address)
{
    if (!mw::routerholes::dottedIpv4(address)) return false;
    const QStringList parts = address.split(QLatin1Char('.'));
    const int a = parts.at(0).toInt();
    const int b = parts.at(1).toInt();
    const int c = parts.at(2).toInt();
    if (a == 0 || a == 10 || a == 127 || a >= 224) return false;
    if (a == 100 && b >= 64 && b <= 127) return false; // an operator's shared range
    if (a == 169 && b == 254) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a == 192 && b == 168) return false;
    if (a == 192 && b == 0 && (c == 0 || c == 2)) return false; // protocols, documentation
    if (a == 192 && b == 88 && c == 99) return false;           // a relay's
    if (a == 198 && (b == 18 || b == 19)) return false;         // benches
    if (a == 198 && b == 51 && c == 100) return false;          // documentation
    if (a == 203 && b == 0 && c == 113) return false;           // documentation
    return true;
}

Guard::Guard(IUpnpGateway& gateway, mw::routerholes::IRecord* record, IProbe& probe)
    : m_Gateway(gateway)
    , m_Record(record)
    , m_Probe(probe)
{}

QString Guard::text() const
{
    return mw::routerholes::describe(Kind::Tcp443, -1, m_Record ? m_Record->tag() : QString());
}

bool Guard::ours(const IUpnpGateway::Entry& shown, const Hole& line) const
{
    // By the record and by the text, and as it was noted: the address and the
    // port. Never by the address alone.
    return line.kind == Kind::Tcp443 && line.tcp && line.external == kPort && !line.lan.isEmpty() &&
           shown.client == line.lan && shown.internalPort == line.internal &&
           shown.description == text();
}

Verdict Guard::claim(const Situation& now)
{
    if (!now.wanted) return left(0, QStringLiteral("not asked for"));
    if (!m_Record)
        return left(0, QStringLiteral("this run keeps no record of what it writes on the router"));
    if (now.blockPort == 0)
        return left(0, QStringLiteral("the router forwards no shared TCP port for it to lead to"));

    // 1. An address worth it
    if (!publicIpv4(now.publicAddress) || now.routerAddress != now.publicAddress)
        return left(1, QStringLiteral("the router's own address is not a public one browsers "
                                      "reach this host at"));

    // An entry of this installation's under an address this machine no longer
    // has is not somebody's 443: it is taken off first, when the router lets it
    const Former former = formerAddress();
    if (former == Former::Refused)
        return left(2, QStringLiteral("the router keeps a TCP 443 entry this installation wrote "
                                      "under an address it no longer has"));
    if (former == Former::Unknown)
        return left(2, QStringLiteral("the router does not say whether TCP 443 is forwarded"));

    // 2. Nobody has 443
    const QString lan = m_Gateway.lanAddress();
    IUpnpGateway::Entry shown;
    const Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (found == Lookup::Unknown)
        return left(2, QStringLiteral("the router does not say whether TCP 443 is forwarded"));
    Hole noted;
    const bool hasLine = m_Record->find(kPort, true, noted) && noted.lan == lan;
    if (found == Lookup::Found) {
        if (!hasLine || !ours(shown, noted))
            return left(2, QStringLiteral("the router already forwards TCP 443"));
        if (noted.internal == now.blockPort) {
            // This installation's, and where it should lead: kept (step 6)
            m_Held = true;
            m_Internal = noted.internal;
            return renew(now);
        }
        // This installation's, towards a port the streams no longer share:
        // taken off, and asked for anew
        if (!m_Gateway.remove(kPort, kTcp) || m_Gateway.entry(kPort, kTcp, shown) != Lookup::None)
            return left(2, QStringLiteral("the router keeps this installation's TCP 443 entry "
                                          "towards a port it left"));
        m_Record->strike(kPort, true);
    } else if (hasLine) {
        // Noted, and the router has it no more
        m_Record->strike(kPort, true);
    }

    // 3. The router leads back here
    if (!m_Probe.arrivesHere(now.publicAddress, now.blockPort))
        return left(3, QStringLiteral("the router does not lead back here from its own address: "
                                      "443 cannot be checked from this host"));

    // 4. 443 answers nobody today
    if (m_Probe.answers(now.publicAddress, kPort))
        return left(4, QStringLiteral("something answers on TCP 443 already"));

    // 5. Write, read, prove. Noted first: an entry the router has and the
    // record has not could never be taken back.
    Hole line;
    line.external = kPort;
    line.tcp = true;
    line.internal = now.blockPort;
    line.kind = Kind::Tcp443;
    line.slot = -1;
    line.lan = lan;
    m_Record->note(line);
    if (!m_Gateway.add(kPort, now.blockPort, now.leaseSec, text(), kTcp)) {
        undo(line);
        return left(5, QStringLiteral("the router refused the entry"));
    }
    if (m_Gateway.entry(kPort, kTcp, shown) != Lookup::Found || !ours(shown, line)) {
        undo(line);
        return left(5, QStringLiteral("the router does not show the entry as it was asked for"));
    }
    if (!m_Probe.arrivesHere(now.publicAddress, kPort)) {
        undo(line);
        return left(5, QStringLiteral("a connection to TCP 443 does not arrive here"));
    }
    m_Held = true;
    m_Internal = now.blockPort;
    return {Outcome::Held, 5, QStringLiteral("TCP 443 leads to the shared port")};
}

Verdict Guard::renew(const Situation& now)
{
    if (!m_Held || !m_Record) return left(6, QStringLiteral("not held"));

    // What it stood on is no more: taken off
    if (!now.wanted || now.blockPort != m_Internal || !publicIpv4(now.publicAddress) ||
        now.routerAddress != now.publicAddress) {
        release();
        return {Outcome::Lost, 6,
                QStringLiteral("what TCP 443 stood on changed: the entry is taken off")};
    }

    const QString lan = m_Gateway.lanAddress();
    Hole line;
    const bool noted = m_Record->find(kPort, true, line) && line.lan == lan &&
                       line.internal == m_Internal && line.kind == Kind::Tcp443;
    IUpnpGateway::Entry shown;
    const Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (!noted || found != Lookup::Found || !ours(shown, line)) {
        m_Held = false;
        // Gone, or another's: no longer this installation's, and the line
        // goes. Not shown at all: it may still be there, and the line stays.
        if (noted && found != Lookup::Unknown) m_Record->strike(kPort, true);
        return {Outcome::Lost, 6,
                found == Lookup::Unknown
                    ? QStringLiteral("the router no longer says what TCP 443 leads to")
                    : QStringLiteral("TCP 443 is no longer this host's on the router")};
    }

    // Written again for its lease, and read back
    if (!m_Gateway.add(kPort, m_Internal, now.leaseSec, text(), kTcp) ||
        m_Gateway.entry(kPort, kTcp, shown) != Lookup::Found || !ours(shown, line)) {
        m_Held = false;
        return {Outcome::Lost, 6, QStringLiteral("the router did not keep the TCP 443 entry")};
    }
    return {Outcome::Held, 6, QStringLiteral("TCP 443 still leads to the shared port")};
}

bool Guard::release()
{
    m_Held = false;
    if (!m_Record) return true;
    Hole line;
    if (!m_Record->find(kPort, true, line) || line.kind != Kind::Tcp443) return true;
    // Written under another address: its own rule
    if (line.lan != m_Gateway.lanAddress()) {
        const Former former = formerAddress();
        return former != Former::Refused && former != Former::Unknown;
    }
    IUpnpGateway::Entry shown;
    Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (found == Lookup::Found && ours(shown, line)) {
        m_Gateway.remove(kPort, kTcp);
        found = m_Gateway.entry(kPort, kTcp, shown);
    }
    // Gone, or another's by now: nothing of this installation's is there
    if (found == Lookup::None || (found == Lookup::Found && !ours(shown, line))) {
        m_Record->strike(kPort, true);
        return true;
    }
    return false;
}

void Guard::undo(const Hole& line)
{
    IUpnpGateway::Entry shown;
    Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (found == Lookup::Found && ours(shown, line)) {
        m_Gateway.remove(kPort, kTcp);
        found = m_Gateway.entry(kPort, kTcp, shown);
    }
    // Read back: gone, or not this installation's (the router wrote something
    // else than it was asked). Still shown as written, or not shown at all:
    // the line stays, and the next start takes the entry back.
    if (found == Lookup::None || (found == Lookup::Found && !ours(shown, line)))
        m_Record->strike(kPort, true);
}

Guard::Former Guard::formerAddress()
{
    Hole line;
    if (!m_Record || !m_Record->find(kPort, true, line) || line.kind != Kind::Tcp443 ||
        line.lan == m_Gateway.lanAddress()) {
        m_FormerRefused = false;
        return Former::None;
    }
    IUpnpGateway::Entry shown;
    const Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    // The router says nothing: the entry may be there still, and what was
    // said of it stands
    if (found == Lookup::Unknown) return Former::Unknown;
    if (found == Lookup::None || !ours(shown, line)) {
        m_Record->strike(kPort, true);
        m_FormerRefused = false;
        return Former::Gone;
    }
    // Still as this installation wrote it, towards an address that may be
    // another machine's by now
    if (m_Gateway.remove(kPort, kTcp) && m_Gateway.entry(kPort, kTcp, shown) == Lookup::None) {
        m_Record->strike(kPort, true);
        m_FormerRefused = false;
        return Former::Removed;
    }
    m_FormerRefused = true;
    return Former::Refused;
}

} // namespace mw::tcp443
