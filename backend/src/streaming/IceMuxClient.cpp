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

#include "IceMuxClient.h"
#include "IceMuxHandles.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QList>
#include <QRandomGenerator>
#include <QSocketNotifier>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#endif

namespace mw::icemuxclient {

namespace {

struct State
{
    std::mutex lock;
    Emit tell;
    std::vector<uintptr_t> shared; // the server's UDP sockets, the preferred one first
    std::vector<uint16_t> ports;   // the port of each
    quint16 hopPort = 0;
    bool agentOpen = false;
    Public announced; // what the router forwards of them, see setPublic()

    // TCP on the shared ports: the server takes the connections, and this
    // worker is handed them
    bool tcp = false;
    /// Windows, a worker above the server: the server, to copy them out of
    std::unique_ptr<mw::icemuxhandles::Server> server;
    /// Elsewhere: this worker's end of the channel they cross on
    int channel = -1;
    /// The agent open now: its feed's port, which is what a connection names
    /// it by; whether it expects one; and how it is given one
    quint16 agentFeedPort = 0;
    bool agentTcp = false;
    TcpOffer offer;
};

State& state()
{
    static State s;
    return s;
}

/// `count` characters of the alphabet ICE allows in a fragment or a password,
/// six random bits each, from the system's generator.
std::string randomIceString(int count)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        out.push_back(alphabet[QRandomGenerator::system()->bounded(64)]);
    return out;
}

// ── Bench only (MW_ICEMUX_STATS=1): what the hop through the server costs ───
// The server stamps each datagram when it reads it off the shared port, and
// libjuice hands the stamp over when the agent reads the datagram off its
// feed. Both clocks are std::chrono::steady_clock, one clock for the machine.
// Everything below runs on libjuice's polling thread and nowhere else.

/// Sixteen buckets for each power of two of nanoseconds: 6 % wide at most.
constexpr size_t kHopBuckets = 976;
uint64_t g_HopCounts[kHopBuckets];
uint64_t g_HopTotal = 0, g_HopMaxNs = 0, g_HopWindowCount = 0, g_HopWindowMaxNs = 0;
uint64_t g_HopNextPrintNs = 0;

size_t hopBucket(uint64_t ns)
{
    if (ns < 16) return static_cast<size_t>(ns);
    int top = 63;
    while (!(ns >> top))
        --top;
    return static_cast<size_t>(top - 3) * 16 + static_cast<size_t>((ns >> (top - 4)) & 15);
}

uint64_t hopBucketFloorNs(size_t bucket)
{
    if (bucket < 16) return bucket;
    return static_cast<uint64_t>(16 + bucket % 16) << (bucket / 16 - 1);
}

/// The delay below which `share` of the datagrams came, in microseconds.
double hopQuantileUs(double share)
{
    const auto rank = static_cast<uint64_t>(share * static_cast<double>(g_HopTotal));
    uint64_t seen = 0;
    for (size_t i = 0; i < kHopBuckets; ++i) {
        seen += g_HopCounts[i];
        if (seen > rank) return static_cast<double>(hopBucketFloorNs(i)) / 1000.0;
    }
    return static_cast<double>(g_HopMaxNs) / 1000.0;
}

void onFeedTimestamp(uint64_t stamp)
{
    using namespace std::chrono;
    const auto now = static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    if (stamp == 0 || now < stamp) return;
    const uint64_t hop = now - stamp;
    ++g_HopCounts[hopBucket(hop)];
    ++g_HopTotal;
    ++g_HopWindowCount;
    if (hop > g_HopMaxNs) g_HopMaxNs = hop;
    if (hop > g_HopWindowMaxNs) g_HopWindowMaxNs = hop;
    if (now < g_HopNextPrintNs) return;
    if (g_HopNextPrintNs != 0) {
        qInfo().noquote() << QStringLiteral("[IceMux] hop n=%1 p50=%2us p99=%3us p99.9=%4us "
                                            "max=%5us (last 10 s: n=%6 max=%7us)")
                                 .arg(g_HopTotal)
                                 .arg(hopQuantileUs(0.50), 0, 'f', 1)
                                 .arg(hopQuantileUs(0.99), 0, 'f', 1)
                                 .arg(hopQuantileUs(0.999), 0, 'f', 1)
                                 .arg(static_cast<double>(g_HopMaxNs) / 1000.0, 0, 'f', 1)
                                 .arg(g_HopWindowCount)
                                 .arg(static_cast<double>(g_HopWindowMaxNs) / 1000.0, 0, 'f', 1);
    }
    g_HopNextPrintNs = now + 10'000'000'000ull;
    g_HopWindowCount = 0;
    g_HopWindowMaxNs = 0;
}

void closeShared(uintptr_t socket)
{
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket));
#else
    ::close(static_cast<int>(socket));
#endif
}

/// The connection goes to the agent it came for, if that agent is still the
/// one open and still expects one: true when the agent has it. Closed here
/// otherwise. What admits it is the agent's own gate, on its first frame.
bool give(uintptr_t socket, quint32 feed, const char* frame, size_t size)
{
    TcpOffer offer;
    {
        State& s = state();
        const std::lock_guard<std::mutex> lock(s.lock);
        if (s.agentOpen && s.agentTcp && feed == s.agentFeedPort) offer = s.offer;
    }
    // Outside the lock: the agent's own thread may be asking for it
    if (offer && offer(socket, frame, size)) return true;
    closeShared(socket);
    return false;
}

/// The server is told what became of a connection it handed, so that it lets
/// go of its own end: `number` as it named it, 0 where it names none.
void answer(quint32 number, bool taken)
{
    Emit tell;
    {
        State& s = state();
        const std::lock_guard<std::mutex> lock(s.lock);
        tell = s.tell;
    }
    if (!tell) return;
    tell({{QStringLiteral("event"), QStringLiteral("iceMux")},
          {QStringLiteral("op"), QStringLiteral("tcpGot")},
          {QStringLiteral("n"), static_cast<double>(number)},
          {QStringLiteral("ok"), taken}});
}

#ifndef _WIN32
/// A whole number of the config line, 0 for anything that is not one.
quint64 wholeNumber(const QJsonValue& value)
{
    if (!value.isDouble()) return 0;
    const double d = value.toDouble();
    if (!(d >= 1 && d <= 2147483647.0)) return 0;
    const auto n = static_cast<quint64>(d);
    return static_cast<double>(n) == d ? n : 0;
}

/// What the channel holds now: each message is one connection, checked as it
/// is read (IceMuxHandles::receiveConnection) and answered for. False when
/// the server's end is gone.
bool readChannel()
{
    State& s = state();
    // So many a turn: the rest waits for the next
    for (int n = 0; n < 8; ++n) {
        int channel = -1;
        std::vector<uint16_t> ports;
        {
            const std::lock_guard<std::mutex> lock(s.lock);
            channel = s.channel;
            ports = s.ports;
        }
        if (channel < 0) return false;
        mw::icemuxhandles::Received got;
        QString why;
        const mw::icemuxhandles::Receipt receipt =
            mw::icemuxhandles::receiveConnection(channel, ports, &got, &why);
        if (receipt == mw::icemuxhandles::Receipt::Nothing) return true;
        if (receipt == mw::icemuxhandles::Receipt::Closed) return false;
        bool taken = false;
        if (receipt == mw::icemuxhandles::Receipt::Connection)
            taken = give(got.socket, got.feed, got.frame.data(), got.frame.size());
        else
            qWarning().noquote() << "[IceMux] what the server sent is no connection for this "
                                    "stream:"
                                 << why;
        answer(0, taken);
    }
    return true;
}

/// Read on the thread this is called on, the worker's main one, as messages
/// come: never from a thread of its own.
void watchChannel(int channel)
{
    auto* notifier =
        new QSocketNotifier(channel, QSocketNotifier::Read, QCoreApplication::instance());
    QObject::connect(notifier, &QSocketNotifier::activated, notifier, [notifier]() {
        if (!readChannel()) notifier->setEnabled(false);
    });
}
#endif

} // namespace

bool configure(const QJsonObject& mux, quint32 serverPid,
               const std::function<bool(const QString& image)>& isServer, Emit tell)
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    const int hop = mux.value(QStringLiteral("hop")).toInt();
    const QJsonArray sockets = mux.value(QStringLiteral("sockets")).toArray();
    const QJsonArray handles = mux.value(QStringLiteral("handles")).toArray();
    const QJsonArray fds = mux.value(QStringLiteral("fds")).toArray();
    const QJsonArray ports = mux.value(QStringLiteral("ports")).toArray();
    // Whether the server also takes this stream's TCP connections there
    const bool tcpTold = mux.value(QStringLiteral("tcp")).toBool();

    std::vector<uintptr_t> opened;
#ifndef _WIN32
    // The channel those cross on was left open across this process's start
    // like the sockets: kept from whatever this process starts before
    // anything else, then checked. Closed again if nothing comes of the rest.
    struct Channel
    {
        int fd = -1;
        ~Channel()
        {
            if (fd >= 0) ::close(fd);
        }
    } channel;
    if (mux.contains(QStringLiteral("channel"))) {
        QString whyNot;
        channel.fd = mw::icemuxhandles::channelInherited(
            wholeNumber(mux.value(QStringLiteral("channel"))), &whyNot);
        if (channel.fd < 0)
            qWarning().noquote() << "[IceMux] no channel for this stream's TCP connections:"
                                 << whyNot;
    }
    // Outside Windows the server left the sockets open across this process's
    // start, and names their descriptors. They are kept from whatever this
    // process starts, and checked, before anything else of the config is
    // looked at (IceMuxHandles::inherited()).
    if (!fds.isEmpty()) {
        std::vector<mw::icemuxhandles::Wanted> told;
        for (qsizetype i = 0; i < fds.size(); ++i) {
            const quint64 port = wholeNumber(ports.at(i));
            told.push_back(
                {wholeNumber(fds.at(i)), static_cast<uint16_t>(port <= 65535 ? port : 0)});
        }
        QString whyNot;
        opened = mw::icemuxhandles::inherited(told, &whyNot);
        if (opened.empty()) {
            qWarning().noquote() << "[IceMux] the shared sockets cannot be used:" << whyNot
                                 << "— this stream takes a socket of its own";
            return false;
        }
    }
    Q_UNUSED(serverPid);
    Q_UNUSED(isServer);
#endif
    const auto refuse = [&opened]() {
        for (const uintptr_t o : opened)
            closeShared(o);
        return false;
    };

    // Handed, to take, or left open: one way, and a port for each.
    const int ways =
        (sockets.isEmpty() ? 0 : 1) + (handles.isEmpty() ? 0 : 1) + (fds.isEmpty() ? 0 : 1);
    const qsizetype count = sockets.size() + handles.size() + fds.size();
    if (hop <= 0 || hop > 65535 || !tell || ways != 1 ||
        count > static_cast<qsizetype>(mw::icemuxhandles::kMax) || ports.size() != count)
        return refuse();
    QList<quint16> named;
    for (const QJsonValue& v : ports) {
        const int port = v.toInt();
        if (port <= 0 || port > 65535) return refuse();
        named.append(static_cast<quint16>(port));
    }

#ifdef _WIN32
    // Descriptors left open are another system's way
    if (!fds.isEmpty()) return false;
    const bool handed = !sockets.isEmpty();
    // The first sockets of this process: nothing has started Winsock yet.
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return false;
    if (handed) {
        for (qsizetype i = 0; i < count; ++i) {
            // From the description the server made for this process alone,
            // and kept only when it is the UDP socket of the port it said
            const QByteArray info = QByteArray::fromBase64(sockets.at(i).toString().toLatin1());
            QString whyNot;
            const uintptr_t one = mw::icemuxhandles::open(
                std::vector<uint8_t>(info.cbegin(), info.cend()), named.at(i), &whyNot);
            if (one == mw::icemuxhandles::kNoSocket) {
                qWarning().noquote()
                    << "[IceMux]" << whyNot << "— this stream takes a socket of its own";
                return refuse();
            }
            opened.push_back(one);
        }
    } else {
        // This worker runs above the server, which could not hand it
        // anything: it takes the sockets out of the server it was started by.
        std::vector<mw::icemuxhandles::Wanted> wanted;
        for (qsizetype i = 0; i < count; ++i) {
            bool number = false;
            const quint64 handle = handles.at(i).toString().toULongLong(&number);
            if (!number || handle == 0) return false;
            wanted.push_back({handle, named.at(i)});
        }
        QString why;
        opened = mw::icemuxhandles::adopt(serverPid, wanted, isServer, &why);
        if (opened.empty()) {
            qWarning().noquote() << "[IceMux] the shared sockets cannot be taken from the server:"
                                 << why << "— this stream takes a socket of its own";
            return false;
        }
    }
    const char* how = handed ? "(handed by the server)" : "(taken from the server)";
#else
    // Descriptions and handles are Windows' way
    if (fds.isEmpty()) return refuse();
    const char* how = "(left open by the server)";
#endif
    s.shared = std::move(opened);
    s.ports.assign(named.cbegin(), named.cend());
    s.hopPort = static_cast<quint16>(hop);
    s.tell = std::move(tell);
    QStringList list;
    for (const quint16 port : named)
        list << QString::number(port);
    qInfo().noquote() << "[IceMux] worker on shared UDP port" << list.join(QStringLiteral(", "))
                      << how;

    // TCP there too, when the server says it takes the connections and there
    // is a way for them to reach this process
    s.tcp = false;
    s.server.reset();
#ifdef _WIN32
    if (tcpTold && handed) {
        // Each comes described for this process, in a command
        s.tcp = true;
    } else if (tcpTold) {
        // Each is copied out of the server, as the UDP sockets just were:
        // through a handle on it kept for the stream's life, and checked as
        // that one was
        QString whyNot;
        s.server = mw::icemuxhandles::Server::open(serverPid, isServer, &whyNot);
        s.tcp = s.server != nullptr;
        if (!s.tcp)
            qWarning().noquote() << "[IceMux] no TCP connection can be taken from the server:"
                                 << whyNot;
    }
#else
    if (tcpTold && channel.fd >= 0) {
        s.channel = channel.fd;
        channel.fd = -1;
        s.tcp = true;
        watchChannel(s.channel);
    }
#endif
    if (s.tcp)
        qInfo() << "[IceMux] the server takes this stream's TCP connections on the shared port(s)";
    else if (tcpTold)
        qWarning() << "[IceMux] a connection that listens in TCP takes a socket of its own";
    return true;
}

bool available()
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    return !s.shared.empty();
}

bool tcpAvailable()
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    return !s.shared.empty() && s.tcp;
}

void setPublic(const QJsonObject& announced)
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    s.announced = Public{};
    if (s.shared.empty()) return;
    // An IPv4 address in figures, and nothing else: it goes into a candidate.
    const QString ip = announced.value(QStringLiteral("ip")).toString();
    QHostAddress address;
    if (!address.setAddress(ip) || address.protocol() != QAbstractSocket::IPv4Protocol) return;
    Public out;
    // Only a port this worker sends from can be one of its candidates
    const auto mine = [&s](const QJsonValue& list) {
        std::vector<uint16_t> kept;
        for (const QJsonValue& v : list.toArray()) {
            const int port = v.toInt();
            if (port <= 0 || port > 65535) continue;
            const auto one = static_cast<uint16_t>(port);
            const bool has = std::find(s.ports.cbegin(), s.ports.cend(), one) != s.ports.cend();
            const bool twice = std::find(kept.cbegin(), kept.cend(), one) != kept.cend();
            if (has && !twice) kept.push_back(one);
        }
        return kept;
    };
    out.ports = mine(announced.value(QStringLiteral("ports")));
    if (out.ports.empty()) return;
    // In TCP, a port the router forwards in UDP too and no other
    for (const uint16_t port : mine(announced.value(QStringLiteral("tcpPorts"))))
        if (std::find(out.ports.cbegin(), out.ports.cend(), port) != out.ports.cend())
            out.tcpPorts.push_back(port);
    // TCP 443 leads to one of those, or to nothing this worker listens for
    const int by443 = announced.value(QStringLiteral("tcp443")).toInt();
    if (by443 > 0 && by443 <= 65535 &&
        std::find(out.tcpPorts.cbegin(), out.tcpPorts.cend(), static_cast<uint16_t>(by443)) !=
            out.tcpPorts.cend())
        out.tcp443 = static_cast<uint16_t>(by443);
    out.ip = address.toString().toStdString();
    s.announced = std::move(out);
}

Public publicPorts()
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    return s.announced;
}

std::optional<rtc::IceSharedSockets> openAgent(bool tcp)
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    if (s.shared.empty()) return std::nullopt;
    uint16_t feedPort = 0;
    // Never closed: a socket the agent's thread may still be polling must not
    // become another one under it, and a worker makes very few agents.
    const uintptr_t feed = mw::icemuxhandles::openFeed(s.hopPort, &feedPort);
    if (feed == mw::icemuxhandles::kNoSocket) {
        qWarning() << "[IceMux] no feed socket — this connection takes a socket of its own";
        return std::nullopt;
    }
    rtc::IceSharedSockets out;
    out.feed = feed;
    out.send = s.shared;
    out.ufrag = randomIceString(12);
    out.pwd = randomIceString(24);
    if (qEnvironmentVariableIntValue("MW_ICEMUX_STATS") > 0) out.feedTimestamp = &onFeedTimestamp;
    // An agent that listens in TCP: the server does so for it, and it offers
    // its passive candidates on the shared ports though it listens nowhere
    const bool expects = tcp && s.tcp;
    out.tcp = expects;
    s.agentOpen = true;
    s.agentTcp = expects;
    s.agentFeedPort = feedPort;
    s.offer = nullptr;
    // A window opens, by this worker's own agent and nothing the server
    // says: the copies it may make of the server's handles are counted anew
    if (expects && s.server) s.server->windowOpened();
    QJsonObject event{{QStringLiteral("event"), QStringLiteral("iceMux")},
                      {QStringLiteral("op"), QStringLiteral("agent")},
                      {QStringLiteral("ufrag"), QString::fromStdString(out.ufrag)},
                      {QStringLiteral("feedPort"), static_cast<int>(feedPort)}};
    if (expects) event[QStringLiteral("tcp")] = true;
    s.tell(event);
    return out;
}

void setAgentOffer(TcpOffer offer)
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    if (s.agentOpen && s.agentTcp) s.offer = std::move(offer);
}

void takeConnection(const QJsonObject& command)
{
#ifdef _WIN32
    const double told = command.value(QStringLiteral("n")).toDouble();
    const quint32 number =
        told >= 1 && told <= 4294967295.0 ? static_cast<quint32>(told) : quint32{0};
    const int feed = command.value(QStringLiteral("feed")).toInt();
    const QByteArray frame =
        QByteArray::fromBase64(command.value(QStringLiteral("frame")).toString().toLatin1());
    const QByteArray info =
        QByteArray::fromBase64(command.value(QStringLiteral("socket")).toString().toLatin1());
    const bool wellFormed = feed > 0 && feed <= 65535 &&
                            frame.size() >= static_cast<qsizetype>(mw::icemuxhandles::kFrameMin) &&
                            frame.size() <= static_cast<qsizetype>(mw::icemuxhandles::kFrameMax);
    uintptr_t socket = mw::icemuxhandles::kNoSocket;
    QString why;
    {
        State& s = state();
        std::unique_lock<std::mutex> lock(s.lock);
        // Asked before anything is opened or copied: whether the agent it is
        // for is still the one open, and still expects a connection
        const bool expected = wellFormed && s.tcp && s.agentOpen && s.agentTcp &&
                              feed == static_cast<int>(s.agentFeedPort);
        const std::vector<uint16_t> ports = expected ? s.ports : std::vector<uint16_t>();
        if (!s.server) {
            lock.unlock();
            // Described for this process: the system put this process's own
            // handle on the connection in the description when the server
            // made it. So it is opened whatever else is true, and one this
            // worker will not have is closed here, not left in it: without
            // the ports to hold it against, the check below refuses it.
            if (!info.isEmpty())
                socket = mw::icemuxhandles::openConnection(
                    std::vector<uint8_t>(info.cbegin(), info.cend()), ports, &why);
            else
                why = QStringLiteral("no connection described");
        } else if (expected) {
            // Copied out of the server, by the number it names. Under the
            // lock: the count of copies is the window's.
            bool isNumber = false;
            const quint64 handle =
                command.value(QStringLiteral("handle")).toString().toULongLong(&isNumber);
            const int local = command.value(QStringLiteral("lport")).toInt();
            const int remote = command.value(QStringLiteral("rport")).toInt();
            if (!isNumber || handle == 0 || local <= 0 || local > 65535 || remote <= 0 ||
                remote > 65535) {
                why = QStringLiteral("no connection named");
            } else {
                socket = s.server->copyConnection(handle, ports, &why);
                // The copy must be the very connection the server spoke of:
                // its two ports, which the server gives for this and for
                // nothing else. What is not is closed, and never used.
                uint16_t here = 0, there = 0;
                if (socket != mw::icemuxhandles::kNoSocket &&
                    (!mw::icemuxhandles::connectionPorts(socket, &here, &there) || here != local ||
                     there != remote)) {
                    closeShared(socket);
                    socket = mw::icemuxhandles::kNoSocket;
                    why = QStringLiteral("what was copied is not the connection the server named");
                }
            }
        }
        // Not expected: nothing of the server's is copied at all
        if (!expected)
            why = wellFormed ? QStringLiteral("its agent expects no connection")
                             : QStringLiteral("not a connection as the server hands one");
    }
    bool taken = false;
    if (socket != mw::icemuxhandles::kNoSocket)
        taken = give(socket, static_cast<quint32>(feed), frame.constData(),
                     static_cast<size_t>(frame.size()));
    else
        qWarning().noquote() << "[IceMux] a connection the server handed is not taken:" << why;
    // At once, taken or not: the server keeps its end until it hears
    answer(number, taken);
#else
    // Connections cross on the channel here, never in a command
    Q_UNUSED(command);
#endif
}

void confirmed(const rtc::Candidate& candidate)
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    // Connected: it expects no connection any more
    s.agentTcp = false;
    s.offer = nullptr;
    if (!s.agentOpen || !candidate.isResolved()) return;
    const auto address = candidate.address();
    const auto port = candidate.port();
    if (!address || !port) return;
    s.tell({{QStringLiteral("event"), QStringLiteral("iceMux")},
            {QStringLiteral("op"), QStringLiteral("confirmed")},
            {QStringLiteral("address"), QString::fromStdString(*address)},
            {QStringLiteral("port"), static_cast<int>(*port)}});
}

void closeAgent()
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    if (!s.agentOpen) return;
    s.agentOpen = false;
    s.agentTcp = false;
    s.offer = nullptr;
    s.tell({{QStringLiteral("event"), QStringLiteral("iceMux")},
            {QStringLiteral("op"), QStringLiteral("gone")}});
}

} // namespace mw::icemuxclient
