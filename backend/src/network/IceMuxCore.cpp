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

#include "IceMuxCore.h"

#include <algorithm>

namespace mw::icemux {

namespace {

constexpr size_t kStunHeader = 20;
constexpr uint32_t kStunCookie = 0x2112A442;
constexpr uint16_t kStunClassMask = 0x0110;
constexpr uint16_t kStunBinding = 0x0001;
constexpr uint16_t kAttrUsername = 0x0006;
constexpr size_t kUsernameMax = 513; // RFC 8489

uint16_t be16(const uint8_t* p) noexcept
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) noexcept
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

} // namespace

StunView readStun(const uint8_t* data, size_t size) noexcept
{
    StunView view;
    // RFC 7983: STUN starts with two zero bits. Over UDP the message is the
    // whole datagram, its length a multiple of four.
    if (!data || size < kStunHeader || (data[0] & 0xC0) != 0) return view;
    const uint16_t type = be16(data);
    const size_t length = be16(data + 2);
    if ((length & 3) != 0 || kStunHeader + length != size || be32(data + 4) != kStunCookie)
        return view;

    const uint16_t cls = type & kStunClassMask;
    if (cls == 0x0100 || cls == 0x0110) {
        view.kind = Kind::StunResponse;
        return view;
    }
    if (type != kStunBinding) {
        view.kind = Kind::StunOther;
        return view;
    }
    view.kind = Kind::BindingRequest;

    // Each turn moves at least four bytes on: the walk ends within `size`.
    size_t pos = kStunHeader;
    while (size - pos >= 4) {
        const uint16_t attr = be16(data + pos);
        const size_t len = be16(data + pos + 2);
        pos += 4;
        if (len > size - pos) break;
        if (attr == kAttrUsername) {
            if (len > kUsernameMax) break;
            const std::string_view name(reinterpret_cast<const char*>(data + pos), len);
            const size_t colon = name.find(':');
            if (colon != std::string_view::npos) view.localUfrag = name.substr(0, colon);
            break;
        }
        const size_t padded = (len + 3) & ~static_cast<size_t>(3);
        if (padded > size - pos) break;
        pos += padded;
    }
    return view;
}

void writeFeedHeader(uint8_t* out, const Peer& from, uint64_t timestamp) noexcept
{
    std::fill(out, out + kFeedHeaderSize, uint8_t{0});
    out[0] = 1; // JUICE_FEED_VERSION
    out[1] = from.via;
    out[2] = from.family;
    out[4] = static_cast<uint8_t>(from.port >> 8);
    out[5] = static_cast<uint8_t>(from.port);
    for (int i = 0; i < 4; ++i)
        out[8 + i] = static_cast<uint8_t>(from.scope >> (8 * i));
    std::copy(from.address.begin(), from.address.end(), out + 12);
    for (int i = 0; i < 8; ++i)
        out[28 + i] = static_cast<uint8_t>(timestamp >> (8 * i));
}

size_t PeerHash::operator()(const Peer& p) const noexcept
{
    uint64_t h = 1469598103934665603ull; // FNV-1a
    const auto mix = [&h](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    for (uint8_t b : p.address)
        mix(b);
    mix(static_cast<uint8_t>(p.port >> 8));
    mix(static_cast<uint8_t>(p.port));
    for (int s = 0; s < 32; s += 8)
        mix(static_cast<uint8_t>(p.scope >> s));
    mix(p.family);
    mix(p.via);
    return static_cast<size_t>(h);
}

bool Router::addAgent(uint32_t feed, std::string_view ufrag, uint64_t nowMs)
{
    if (ufrag.size() < kUfragMin || ufrag.size() > kUfragMax || m_Agents.count(feed)) return false;
    // The characters ICE allows in a fragment (RFC 8839 ice-char), no other
    const auto iceChar = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '+' || c == '/';
    };
    if (!std::all_of(ufrag.begin(), ufrag.end(), iceChar)) return false;
    std::string key(ufrag);
    if (m_ByUfrag.count(key)) return false;
    m_ByUfrag.emplace(key, feed);
    m_Agents.emplace(feed, Agent{std::move(key), nowMs, 0});
    return true;
}

void Router::removeAgent(uint32_t feed)
{
    const auto agent = m_Agents.find(feed);
    if (agent == m_Agents.end()) return;
    m_ByUfrag.erase(agent->second.ufrag);
    m_Agents.erase(agent);
    for (auto it = m_Confirmed.begin(); it != m_Confirmed.end();)
        it = it->second == feed ? m_Confirmed.erase(it) : std::next(it);
    for (auto it = m_Provisional.begin(); it != m_Provisional.end();)
        it = it->second.feed == feed ? m_Provisional.erase(it) : std::next(it);
}

size_t Router::provisionalCount(uint32_t feed) const noexcept
{
    const auto agent = m_Agents.find(feed);
    return agent == m_Agents.end() ? 0 : agent->second.provisional;
}

Router::ProvisionalMap::iterator Router::dropProvisional(ProvisionalMap::iterator entry)
{
    const auto agent = m_Agents.find(entry->second.feed);
    if (agent != m_Agents.end() && agent->second.provisional > 0) --agent->second.provisional;
    return m_Provisional.erase(entry);
}

void Router::dropOldestOf(uint32_t feed)
{
    auto oldest = m_Provisional.end();
    for (auto it = m_Provisional.begin(); it != m_Provisional.end(); ++it)
        if (it->second.feed == feed &&
            (oldest == m_Provisional.end() || it->second.seenMs < oldest->second.seenMs))
            oldest = it;
    if (oldest != m_Provisional.end()) dropProvisional(oldest);
}

void Router::addProvisional(const Peer& peer, uint32_t feed, uint64_t nowMs)
{
    const auto agent = m_Agents.find(feed);
    if (agent == m_Agents.end()) return;
    // Each agent has its share of this level, and makes room in it at its own
    // expense. The whole table full all the same (many agents): the agent
    // that holds the most gives one up.
    if (agent->second.provisional >= kProvisionalPerFeedMax) dropOldestOf(feed);
    if (m_Provisional.size() >= kProvisionalMax) expire(nowMs);
    if (m_Provisional.size() >= kProvisionalMax) {
        const auto most =
            std::max_element(m_Agents.begin(), m_Agents.end(), [](const auto& a, const auto& b) {
                return a.second.provisional < b.second.provisional;
            });
        dropOldestOf(most->first);
    }
    if (m_Provisional.size() >= kProvisionalMax) return;
    if (m_Provisional.emplace(peer, Provisional{feed, nowMs, 0, 0}).second)
        ++agent->second.provisional;
}

void Router::addNegotiating(Route& out, uint64_t nowMs, uint32_t except) const
{
    for (const auto& [feed, agent] : m_Agents)
        if (feed != except && nowMs - agent.sinceMs <= kNegotiationMs) out.feeds.push_back(feed);
}

void Router::confirm(uint32_t feed, const Peer& peer)
{
    const auto agent = m_Agents.find(feed);
    if (agent == m_Agents.end()) return;
    const auto held = m_Confirmed.find(peer);
    if (held != m_Confirmed.end()) {
        if (held->second == feed) return;
        // The same address and socket, verified by another agent since: the
        // earlier one is the stream that address left.
        const auto earlier = m_Agents.find(held->second);
        if (earlier != m_Agents.end() && earlier->second.confirmed > 0) --earlier->second.confirmed;
        m_Confirmed.erase(held);
    }
    if (agent->second.confirmed >= kConfirmedPerFeedMax) return;
    ++agent->second.confirmed;
    m_Confirmed.emplace(peer, feed);
    if (const auto known = m_Provisional.find(peer); known != m_Provisional.end())
        dropProvisional(known);
}

void Router::expire(uint64_t nowMs)
{
    for (auto it = m_Provisional.begin(); it != m_Provisional.end();)
        it = nowMs - it->second.seenMs > kProvisionalLifeMs ? dropProvisional(it) : std::next(it);
}

Route Router::route(const Peer& from, const uint8_t* data, size_t size, uint64_t nowMs)
{
    Route out;
    // Nobody streams: nothing is even looked at.
    if (m_Agents.empty()) return out;

    const StunView stun = readStun(data, size);
    if (stun.kind == Kind::StunOther) return out;

    if (stun.kind == Kind::BindingRequest) {
        // Always by the user fragment, whatever the table says of the sender:
        // an address two agents are checking with is a fact of life behind a
        // NAT, and the fragment is what tells them apart.
        // No longer than kUfragMax: the key below stays inside the string
        // itself, nothing is allocated for a datagram from anybody.
        if (stun.localUfrag.size() > kUfragMax) return out;
        const auto agent = m_ByUfrag.find(std::string(stun.localUfrag));
        if (agent == m_ByUfrag.end()) return out;
        const uint32_t feed = agent->second;
        const auto confirmed = m_Confirmed.find(from);
        if (confirmed == m_Confirmed.end() || confirmed->second != feed) {
            const auto known = m_Provisional.find(from);
            if (known != m_Provisional.end()) {
                // A confirmed peer of another agent keeps its other traffic
                // there; the request alone goes by the fragment.
                // And a provisional peer follows one agent at a time: the one
                // it follows renews it, another takes it only once it lapsed
                // (an agent gone has taken its peers with it). One address and
                // port never serves two live agents, so nothing true needs
                // more, and a request alone moves nobody's traffic.
                if (confirmed == m_Confirmed.end()) {
                    if (known->second.feed == feed) {
                        known->second.seenMs = nowMs;
                    } else if (nowMs - known->second.seenMs > kProvisionalLifeMs) {
                        dropProvisional(known);
                        addProvisional(from, feed, nowMs);
                    }
                }
            } else if (confirmed == m_Confirmed.end()) {
                addProvisional(from, feed, nowMs);
            }
        }
        out.feeds.push_back(feed);
        return out;
    }

    if (const auto confirmed = m_Confirmed.find(from); confirmed != m_Confirmed.end()) {
        out.feeds.push_back(confirmed->second);
        return out;
    }
    if (const auto known = m_Provisional.find(from); known != m_Provisional.end()) {
        Provisional& entry = known->second;
        if (nowMs - entry.seenMs <= kProvisionalLifeMs) {
            out.feeds.push_back(entry.feed);
            // A response answers a check, and the check may be that of an
            // agent other than the one this address follows so far: the
            // agents still negotiating get it too, so many a second for each
            // address. An agent ignores a transaction that is not its own.
            if (stun.kind == Kind::StunResponse) {
                if (nowMs - entry.sharedWindowMs >= 1000) {
                    entry.sharedWindowMs = nowMs;
                    entry.sharedInWindow = 0;
                }
                if (entry.sharedInWindow < kSharedResponsesPerSecond) {
                    ++entry.sharedInWindow;
                    addNegotiating(out, nowMs, entry.feed);
                }
            }
            return out;
        }
        dropProvisional(known);
    }
    if (stun.kind != Kind::StunResponse) return out;

    // A response from an address nobody has heard a request from yet: the
    // answer to a check one of the agents still negotiating sent first.
    if (nowMs - m_UnclaimedWindowMs >= 1000) {
        m_UnclaimedWindowMs = nowMs;
        m_UnclaimedInWindow = 0;
    }
    if (m_UnclaimedInWindow >= kUnclaimedPerSecond) return out;
    ++m_UnclaimedInWindow;
    addNegotiating(out, nowMs, 0);
    return out;
}

} // namespace mw::icemux
