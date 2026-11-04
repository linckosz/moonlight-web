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

#include <array>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// The DSCP our WebRTC packets leave with (plan « le son et la priorité des
// paquets », D5 and D9).
//
// AF41 for video and data channels by default. On the Freebox's Wi-Fi every
// AF4x value goes to the video queue (VI), ahead of the house's downloads; the
// night of 11-12/10 measured what that buys against a download onto the
// client's own station: 32 to 40 ms off every click, where the unmarked stream
// fell under 8 Mbit/s each time. Upstream libdatachannel's own values (EF /
// AF42 / AF11) put the SCTP video, the default, in AF11: the Wi-Fi's background
// queue.
//
// EF for the sound: an access point that follows RFC 8325 puts it in the voice
// queue (VO), ahead of the video's, where a key frame's packets can no longer
// hold it back. The sound is ~0.25 Mbit/s, so VO costs the house nothing; the
// video stays out of it, since VO does not aggregate (frames on the audio road
// too: rtc::Track::setVideoDscp). The Freebox's mesh link sends EF to VI with
// the rest: there the bench of 21/10 saw no loss either way, the same jitter
// and a NetEq buffer within its noise. Each datagram carries its own class on
// Windows, Linux and macOS alike.
//
// libjuice sets it on the socket: Linux; Windows through our patch
// (libjuice-windows-diffserv.patch; an unelevated IP_TOS marks there, under
// the service too); macOS through another (libjuice-macos-diffserv.patch: its
// dual-stack socket marks IPv6 only, so each packet to an IPv4 peer carries
// the class itself). An installer QoS policy was set aside: Windows leaves it
// out of force after a reboot on a PC outside a domain.
//
// `packet_marking: false` in settings.json marks nothing. The bench's
// MW_DSCP="<audio>,<video>,<data>" (each 0-63) overrides both. The stream
// worker gets both from the server (Handover below).
namespace PacketMarking {

/// EF: Expedited Forwarding.
constexpr unsigned int kEf = 46;
/// AF41: Assured Forwarding class 4, low drop probability.
constexpr unsigned int kAf41 = 34;

/// Audio, video and data DSCP for the setting and an MW_DSCP value (empty, or
/// malformed, is no override).
inline std::array<unsigned int, 3> resolve(bool marking, const std::string& override_)
{
    std::array<unsigned int, 3> out{};
    size_t at = 0;
    int i = 0;
    for (; i < 3; ++i) {
        const size_t comma = override_.find(',', at);
        const std::string part =
            override_.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (part.empty() || part.size() > 2 ||
            part.find_first_not_of("0123456789") != std::string::npos)
            break;
        const int v = std::stoi(part);
        if (v > 63) break;
        out[i] = static_cast<unsigned int>(v);
        if (i < 2 && comma == std::string::npos) break;
        if (i == 2 && comma != std::string::npos) {
            i = -1; // a fourth value
            break;
        }
        at = comma + 1;
    }
    if (i == 3) return out;
    if (!marking) return {0, 0, 0};
    return {kEf, kAf41, kAf41};
}

/// What the server read, for a stream worker that cannot read it itself: a
/// worker the Windows service starts runs as SYSTEM, whose settings.json is
/// not the owner's and whose environment is not the server's. Each value left
/// unset (an older parent, a stream in the server's own process) is read by
/// this process, as before.
struct Handover
{
    std::optional<bool> marking;     // packet_marking
    std::optional<std::string> dscp; // MW_DSCP
};

namespace detail {
inline std::mutex& handoverMutex()
{
    static std::mutex m;
    return m;
}
inline Handover& handoverSlot()
{
    static Handover h;
    return h;
}
} // namespace detail

/// Set once by the worker from its config, before its first peer connection.
inline void setHandover(Handover h)
{
    std::lock_guard<std::mutex> lock(detail::handoverMutex());
    detail::handoverSlot() = std::move(h);
}

inline Handover handover()
{
    std::lock_guard<std::mutex> lock(detail::handoverMutex());
    return detail::handoverSlot();
}

/// resolve() with the server's values where it handed them over, and this
/// process's own otherwise, read only then.
template <class ReadMarking, class ReadDscp>
std::array<unsigned int, 3> resolve(const Handover& h, ReadMarking ownMarking, ReadDscp ownDscp)
{
    return resolve(h.marking ? *h.marking : ownMarking(), h.dscp ? *h.dscp : ownDscp());
}

/// The bench's silent carrier (MW_DSCP_CARRIER=1, off by default): the stream's
/// sound is offered sendrecv instead of sendonly, so a browser with the bench key
/// mw_dscp_carrier (frontend DscpCarrier.js) can answer it with a sender of its
/// own at a high network priority, and mark what it sends on the way up. What
/// arrives on that track is dropped.
inline bool carrierOffered()
{
    const char* v = std::getenv("MW_DSCP_CARRIER");
    return v && std::string(v) == "1";
}

/// The audio, video and data DSCP this process marks with: the handover's
/// values, else its own settings and environment.
std::array<unsigned int, 3> current();

/// Apply the above to libdatachannel, process-wide. Called before each peer
/// connection is made; cheap, and logs only when the values change.
void apply();

} // namespace PacketMarking
