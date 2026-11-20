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

#include "IMediaEngine.h"
#include "HostCandidates.h"
#include "IceMuxClient.h"
#include "IcePriority.h"
#include "MicRtp.h"
#include "MicUplink.h"
#include "server/NetClassify.h"

#include <rtc/rtc.hpp>

#include <QPointer>
#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QString>
#include <QStringList>

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

/// "host 192.168.1.66:48550/UDP" — enough to tell a direct path from one that
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

/// The rule's name for a candidate's transport (HostCandidates.h)
mw::ice::LocalCandidate::Transport transportOf(rtc::Candidate::TransportType type)
{
    using T = mw::ice::LocalCandidate::Transport;
    switch (type) {
    case rtc::Candidate::TransportType::Udp: return T::Udp;
    case rtc::Candidate::TransportType::TcpPassive: return T::TcpPassive;
    case rtc::Candidate::TransportType::TcpActive: return T::TcpActive;
    default: return T::TcpOther;
    }
}

const char* transportName(mw::ice::LocalCandidate::Transport transport)
{
    using T = mw::ice::LocalCandidate::Transport;
    switch (transport) {
    case T::Udp: return "UDP";
    case T::TcpPassive: return "TCP passive";
    case T::TcpActive: return "TCP active";
    case T::TcpOther: break;
    }
    return "TCP";
}

} // namespace

void RelayBase::setPublicAddress(const std::string& publicIP, uint16_t publicPort)
{
    m_PublicIP = publicIP;
    m_PublicPort = publicPort;
    m_OnSharedPorts = false;
    m_SharedHoles.clear();
    m_SharedTcpHoles.clear();
    m_Tcp443LeadsTo = 0;
    qInfo() << "[Relay] UPnP public address set:" << QString::fromStdString(publicIP) << ":"
            << publicPort;
}

void RelayBase::setSharedPublicPorts(const std::string& publicIP, std::vector<uint16_t> ports,
                                     std::vector<uint16_t> tcpPorts, uint16_t tcp443)
{
    m_PublicIP = publicIP;
    m_PublicPort = 0;
    m_OnSharedPorts = true;
    m_SharedHoles = std::move(ports);
    m_SharedTcpHoles = std::move(tcpPorts);
    // Only towards a port the router forwards in TCP: 443 leads to that port
    m_Tcp443LeadsTo = mw::ice::publicPortFor(tcp443, true, 0, m_SharedTcpHoles);
    QStringList list;
    for (const uint16_t port : m_SharedHoles)
        list << QString::number(port);
    qInfo().noquote() << "[Relay] Shared ports the router forwards:"
                      << QString::fromStdString(publicIP) << "port"
                      << list.join(QStringLiteral(", "))
                      << (m_Tcp443LeadsTo != 0 ? "(and TCP 443)" : "");
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
    // On the server's shared ports, the peer it may now trust for this agent
    mw::icemuxclient::confirmed(remote);
}

void RelayBase::wireMicUplink(rtc::Track& audioTrack, const char* logTag)
{
    if (!m_Mic) return;
    // Refused by the host under the running stream (MicUplink::revoke): the
    // page is told on this relay's thread, where its channel lives.
    QPointer<RelayBase> self(this);
    m_Mic->setOnRevoked([self](const std::string& why) {
        RelayBase* relay = self.data();
        if (!relay) return;
        const QString text = QString::fromStdString(why);
        QMetaObject::invokeMethod(
            relay, [relay, text]() { relay->sendMicOff(text); }, Qt::QueuedConnection);
    });
    std::weak_ptr<MicUplink> weak = m_Mic;
    audioTrack.onMessage([weak](rtc::message_variant message) {
        const auto* bytes = std::get_if<rtc::binary>(&message);
        if (!bytes || bytes->empty()) return;
        if (const auto mic = weak.lock())
            mic->onPacket(reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
    });
    qInfo() << logTag << "Audio offered sendrecv: the viewer's microphone plays into"
            << QString::fromStdString(m_Mic->deviceName());
}

void RelayBase::logMicAnswer(const std::string& sdp, const char* logTag) const
{
    if (!m_Mic) return;
    const mw::mic::AudioAnswer answer = mw::mic::readAudioAnswer(sdp);
    if (!answer.found || !answer.sends)
        qInfo() << logTag
                << "Microphone: the browser answered without sending (no track, or "
                   "the page has the setting off)";
    else if (!answer.hasSsrc)
        qWarning() << logTag
                   << "Microphone: the browser sends, but its answer names no SSRC — "
                      "libdatachannel cannot route that RTP to the track";
    else
        qInfo() << logTag << "Microphone: the browser will send on the audio section";
}

void RelayBase::stopMic()
{
    if (m_Mic) m_Mic->stop();
}

std::string RelayBase::micOpusFmtp() const
{
    std::string fmtp = "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1";
    if (m_Mic) fmtp += ";maxaveragebitrate=128000;maxplaybackrate=48000";
    return fmtp;
}

void RelayBase::emitLocalCandidate(const rtc::Candidate& candidate, const char* logTag)
{
    QHostAddress addr;
    const bool resolved = candidateAddress(candidate, &addr);

    mw::ice::LocalCandidate local;
    local.host = candidate.type() == rtc::Candidate::Type::Host;
    local.ipv4 = resolved ? addr.protocol() == QAbstractSocket::IPv4Protocol
                          : !looksIpv6(candidate.candidate());
    // An unresolved candidate is treated as public: the conservative verdict,
    // since the tunnel branch of the rule grants an exemption.
    local.kind = resolved ? NetClassify::classify(addr.toString()) : NetClassify::Kind::Public;
    local.transport = transportOf(candidate.transportType());
    local.port = candidate.port().value_or(0);

    mw::ice::CandidateView view;
    view.forceHostPublic = m_ForceHostPublic;
    view.havePublicIp = !m_PublicIP.empty();
    view.onSharedPorts = m_OnSharedPorts;
    view.ownHole = m_PublicPort;
    view.udpHoles = m_SharedHoles;
    view.tcpHoles = m_SharedTcpHoles;
    view.tcp443LeadsTo = m_Tcp443LeadsTo;
    view.suppressIpv6 = m_SuppressIPv6;
    view.emitLan = m_EmitLanCandidate;

    const mw::ice::CandidateVerdict verdict = mw::ice::hostCandidateVerdict(local, view);
    using Why = mw::ice::CandidateVerdict::Why;
    // No address on these lines: the kind of address, the transport, the port
    const char* kind = NetClassify::toString(local.kind);
    const char* transport = transportName(local.transport);
    switch (verdict.why) {
    case Why::TunnelForLan:
        qInfo() << logTag << "Tunnel host candidate kept as-is for a peer of our network ("
                << transport << ")";
        break;
    case Why::TunnelWithheld:
        qInfo() << logTag << "Tunnel host candidate withheld from a public peer (" << transport
                << ")";
        break;
    case Why::NotForwarded:
        if (!m_EmitLanCandidate)
            qInfo() << logTag << "Host candidate on port" << local.port << "(" << transport
                    << ") withheld from a public peer: the router does not forward it";
        break;
    case Why::Ipv6Withheld:
        qInfo() << logTag << "Non-global IPv6 candidate (" << kind << "," << transport
                << ") withheld from a public peer";
        break;
    case Why::AsIs:
    case Why::Copied:
    case Why::ActiveForLan: break;
    }

    const std::string mid = candidate.mid();
    if (verdict.asIs) emit signalingIceCandidate(std::string(candidate.candidate()), mid);
    if (verdict.publicPort == 0) return;

    // One copy per transport and port: the host's other addresses copy to the
    // very same public address and port, which the browser would only check
    // again
    if (!firstPublicCopy(mw::ice::publicCopyKey(local.transport, verdict.publicPort))) {
        qDebug() << logTag << "Public copy on port" << verdict.publicPort << "(" << transport
                 << ") already announced";
        return;
    }
    try {
        rtc::Candidate copy = candidate;
        copy.changeAddress(m_PublicIP, verdict.publicPort);
        // A reflexive address's priority, below the LAN host candidate just
        // emitted: a same-LAN peer then prefers the direct pair to the
        // router's hairpin instead of racing them (IcePriority.h).
        const uint32_t priority = mw::ice::srflxPriority(candidate.priority());
        emit signalingIceCandidate(mw::ice::withPriority(copy.candidate(), priority), mid);
        qInfo() << logTag << "Host candidate copied at the public address on port"
                << verdict.publicPort << "(" << transport << ") priority" << priority
                << (m_EmitLanCandidate ? "(+ LAN)" : "");
        // The router's TCP 443 leads to this very port: the same listener,
        // reached by the one port some networks let out. Below every other
        // candidate: a browser that gets through any other way never ends on it.
        if (verdict.by443 &&
            firstPublicCopy(mw::ice::publicCopyKey(local.transport, mw::ice::kTcp443))) {
            rtc::Candidate other = candidate;
            other.changeAddress(m_PublicIP, mw::ice::kTcp443);
            const uint32_t last = mw::ice::lastResortPriority(candidate.priority());
            emit signalingIceCandidate(mw::ice::withPriority(other.candidate(), last), mid);
            qInfo() << logTag << "Host candidate copied at the public address on 443 (TCP) priority"
                    << last;
        }
    } catch (const std::exception& e) {
        // Nothing then: the internal candidate never stands in for its copy
        qWarning() << logTag << "Failed to rewrite candidate:" << e.what();
    }
}

void RelayBase::forgetPublicCopies()
{
    std::lock_guard lock(m_CopiesLock);
    m_CopiesSent.clear();
}

bool RelayBase::firstPublicCopy(uint32_t key)
{
    std::lock_guard lock(m_CopiesLock);
    return m_CopiesSent.insert(key).second;
}

QByteArray RelayBase::micOffMessage(const QString& why)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("micoff")},
                                     {QStringLiteral("why"), why}})
        .toJson(QJsonDocument::Compact);
}

QByteArray RelayBase::inputCapsMessage(const InputMsg::Policy& policy, const IMediaEngine* engine)
{
    const bool pen = policy.keyboardMouse && engine && engine->supportsPen();
    return QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("inputcaps")},
                                     {QStringLiteral("keyboardMouse"), policy.keyboardMouse},
                                     {QStringLiteral("pen"), pen}})
        .toJson(QJsonDocument::Compact);
}

QJsonObject RelayBase::hidCapsMessage(const InputMsg::Policy& policy, IMediaEngine& engine)
{
    const QString why =
        !policy.hid ? QStringLiteral("not allowed for this player") : engine.hidUnavailableReason();
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("hidcaps")},
                       {QStringLiteral("available"), why.isEmpty()},
                       {QStringLiteral("ffb"), why.isEmpty() && engine.hidForceFeedback()},
                       {QStringLiteral("hidpp"), why.isEmpty() && engine.hidRelaysHidpp()},
                       {QStringLiteral("why"), why}};
}
