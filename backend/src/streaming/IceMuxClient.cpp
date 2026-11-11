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

#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QRandomGenerator>

#include <chrono>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace mw::icemuxclient {

namespace {

struct State
{
    std::mutex lock;
    Emit tell;
    std::vector<uintptr_t> shared; // the server's UDP sockets, the preferred one first
    quint16 hopPort = 0;
    bool agentOpen = false;
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

#ifdef _WIN32
/// One of the server's sockets, from the description it made for this process
/// alone. Checked for what it must be before anything is sent through it.
SOCKET openShared(const QByteArray& info)
{
    if (info.size() != static_cast<qsizetype>(sizeof(WSAPROTOCOL_INFOW))) return INVALID_SOCKET;
    WSAPROTOCOL_INFOW description;
    memcpy(&description, info.constData(), sizeof(description));
    const SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                &description, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    int type = 0;
    int typeLen = sizeof(type);
    sockaddr_storage bound{};
    int boundLen = sizeof(bound);
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLen) != 0 ||
        type != SOCK_DGRAM || getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/// A feed: a loopback socket nobody else can bind, which hears the server's
/// hop socket and nothing else.
SOCKET openFeed(quint16 hopPort, quint16* port)
{
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    const int on = 1;
    const int room = 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&room), sizeof(room));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(local);
    sockaddr_in hop = local;
    hop.sin_port = htons(hopPort);
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) != 0 ||
        connect(s, reinterpret_cast<const sockaddr*>(&hop), sizeof(hop)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    *port = ntohs(local.sin_port);
    return s;
}
#endif

} // namespace

bool configure(const QJsonObject& mux, Emit tell)
{
#ifdef _WIN32
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    const int hop = mux.value(QStringLiteral("hop")).toInt();
    const QJsonArray sockets = mux.value(QStringLiteral("sockets")).toArray();
    if (hop <= 0 || hop > 65535 || sockets.isEmpty() || sockets.size() > 4 || !tell) return false;
    // The first sockets of this process: nothing has started Winsock yet.
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return false;
    std::vector<uintptr_t> opened;
    for (const QJsonValue& v : sockets) {
        const SOCKET one = openShared(QByteArray::fromBase64(v.toString().toLatin1()));
        if (one == INVALID_SOCKET) {
            qWarning() << "[IceMux] a shared socket cannot be opened, error" << WSAGetLastError()
                       << "— this stream takes a socket of its own";
            for (uintptr_t o : opened)
                closesocket(static_cast<SOCKET>(o));
            return false;
        }
        opened.push_back(static_cast<uintptr_t>(one));
    }
    s.shared = std::move(opened);
    s.hopPort = static_cast<quint16>(hop);
    s.tell = std::move(tell);
    qInfo() << "[IceMux] worker on" << s.shared.size() << "shared UDP socket(s)";
    return true;
#else
    Q_UNUSED(mux);
    Q_UNUSED(tell);
    return false;
#endif
}

bool available()
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    return !s.shared.empty();
}

std::optional<rtc::IceSharedSockets> openAgent()
{
#ifdef _WIN32
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.lock);
    if (s.shared.empty()) return std::nullopt;
    quint16 feedPort = 0;
    // Never closed: a socket the agent's thread may still be polling must not
    // become another one under it, and a worker makes very few agents.
    const SOCKET feed = openFeed(s.hopPort, &feedPort);
    if (feed == INVALID_SOCKET) {
        qWarning() << "[IceMux] no feed socket, error" << WSAGetLastError()
                   << "— this connection takes a socket of its own";
        return std::nullopt;
    }
    rtc::IceSharedSockets out;
    out.feed = static_cast<uintptr_t>(feed);
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
#else
    return std::nullopt;
#endif
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
