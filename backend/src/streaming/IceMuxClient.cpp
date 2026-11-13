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
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QList>
#include <QRandomGenerator>
#include <QStringList>

#include <algorithm>
#include <chrono>
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

    std::vector<uintptr_t> opened;
#ifndef _WIN32
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
    return true;
}

bool available()
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    return !s.shared.empty();
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
    for (const QJsonValue& v : announced.value(QStringLiteral("ports")).toArray()) {
        const int port = v.toInt();
        if (port <= 0 || port > 65535) continue;
        // Only a port this worker sends from can be one of its candidates
        const auto mine = static_cast<uint16_t>(port);
        const bool has = std::find(s.ports.cbegin(), s.ports.cend(), mine) != s.ports.cend();
        const bool twice =
            std::find(out.ports.cbegin(), out.ports.cend(), mine) != out.ports.cend();
        if (has && !twice) out.ports.push_back(mine);
    }
    if (out.ports.empty()) return;
    out.ip = address.toString().toStdString();
    s.announced = std::move(out);
}

Public publicPorts()
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    return s.announced;
}

std::optional<rtc::IceSharedSockets> openAgent()
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
    s.agentOpen = true;
    s.tell({{QStringLiteral("event"), QStringLiteral("iceMux")},
            {QStringLiteral("op"), QStringLiteral("agent")},
            {QStringLiteral("ufrag"), QString::fromStdString(out.ufrag)},
            {QStringLiteral("feedPort"), static_cast<int>(feedPort)}});
    return out;
}

void confirmed(const rtc::Candidate& candidate)
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
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
    s.tell({{QStringLiteral("event"), QStringLiteral("iceMux")},
            {QStringLiteral("op"), QStringLiteral("gone")}});
}

} // namespace mw::icemuxclient
