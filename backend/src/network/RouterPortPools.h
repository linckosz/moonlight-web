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

#include <cstdint>

/// Which router ports MoonlightWeb asks for, and in what order.
///
/// Several MoonlightWeb hosts can share one LAN — and one router. Every hole
/// this project opens is drawn from the lists below, and every host walks
/// them in the same order, skipping what a neighbour already holds. The first
/// host installed therefore keeps the best ports for as long as it runs, and
/// the tenth still gets a hole of its own. See RouterPortAllocator.h for the
/// walk itself.
namespace mw::routerports {

// ── The control tunnel ──────────────────────────────────────────────────────
//
// 3478-3481 is the STUN/TURN range, and 5349-5352 its TLS half (TURNS). Two
// things follow from that, and both matter more than the numbers being pretty:
//
//   it is a port a corporate firewall already lets out. Such networks
//   routinely permit UDP to 3478 — it is what every conferencing product on
//   earth uses — while dropping it to arbitrary high ports. The browser's
//   connectivity checks are aimed at THIS port, so it is the destination that
//   has to be acceptable, not ours.
//
//   it is nowhere near anything else this project binds. The stream block
//   starts at 48550, GameStream owns 47984-48010, MultiSeat and Wolf sit above
//   48095. A hole here can never be mistaken for one of those, in a router
//   table or in a log.
//
// Eight of them: a first host takes what it needs from the front, a second
// host finds those owned and walks on. Measured on a Livebox, 09/09/2026:
// asked for an entry a neighbour holds, the router does not refuse — it
// silently repoints it at the newcomer, the first host keeps advertising the
// address, and every connectivity check aimed at it lands on the other
// machine. Hence the walk, and hence the read-back after every write.
constexpr uint16_t kTunnelPreferred[] = {3478, 3479, 3480, 3481, 5349, 5350, 5351, 5352};

// Where the ninth tunnel port on a LAN comes from. Nothing corporate about
// these — a browser at the office will not reach them — but a phone on 5G or
// a friend's home connection will, and that is the difference between the
// third host on a LAN being reachable and not. Thirty-two: eight more hosts
// at the cap below.
constexpr uint16_t kTunnelPoolBegin = 46000;
constexpr uint16_t kTunnelPoolEnd = 46031;

// How many tunnel holes one host holds at most — one per browser connected
// through it at once: a phone, a laptop, an admin tab. Claimed one at a time,
// as browsers arrive; beyond the cap the extra connections fall back to an
// ephemeral port, no worse off than every tunnel was before any of this.
constexpr int kTunnelPortCap = 4;

// A DEV identity's tunnel holes (the DEV build, or --dev: Edition.h) come from
// a block of their own, and from nowhere else — neither the list above nor the
// installed host's pool.
//
// A --dev runs beside the installed host on one machine, behind one LAN
// address, and the router cannot tell the two apart: an entry pointed at that
// address reads as ours to both, so the walk handed the --dev the very holes
// the installed host held. A tunnel socket is only bound while a browser is
// connected, so the --dev also found the port free, bound it, and kept it.
// 09/10/2026: a --dev held UDP 3479 for an afternoon, and every browser the
// installed 0.3.1 put on that hole looped on its lock screen, from 5G and the
// office alike. The price: no corporate firewall passes these ports, so a --dev
// is reached from the LAN, 5G or a home connection. Between the installed
// host's pool and the media pool.
constexpr uint16_t kDevTunnelPoolBegin = 46064;
constexpr uint16_t kDevTunnelPoolEnd = 46095;

// ── The stream ──────────────────────────────────────────────────────────────
//
// Each stream slot binds 48550 + slot locally, and that never changes: the
// packages' firewall rule, the --dev one and every diagnostic know the block.
// What the ROUTER forwards to it is negotiated — the slot's own number first,
// so a lone host looks exactly as it always did, then a pool for the host whose
// neighbour got there first. A router forwards any external port to any
// internal one; the browser is told the external, the socket keeps the internal.
//
// Where the block sits is chosen for the machines it runs on, which also run
// the streaming servers. Sunshine and Apollo own 47984-48010 (48010 is their
// RTSP, over TCP — which the media socket binds too under ICE-TCP), MultiSeat
// seats and Wolf 48095 to ~48340, and a GameStream server moved up by a
// thousand, the usual way to run a second one, 48984-49010. 49152 and above is
// where Windows and macOS hand out ephemeral ports, and where Hyper-V, WSL and
// Docker reserve whole blocks at boot. Linux hands out 32768-60999, so the
// packages reserve the block there (ip_local_reserved_ports). Until 0.3.1 it
// was 48010-48033, right on Sunshine's RTSP port.
constexpr uint16_t kMediaBasePort = 48550;
// One port per stream slot: the ceiling of concurrent streams, and the block
// the packages open, 48550-48573.
constexpr int kMediaPortCount = 24;
constexpr uint16_t kMediaPoolBegin = 46100;
constexpr uint16_t kMediaPoolEnd = 46199;

// ── The streams' shared ports (ice_udp_mux) ─────────────────────────────────
//
// With the mux on, the streams of one host no longer bind a port each: they
// share two, held by the main process from start to stop. The router forwards
// each to itself — external and internal are one number, as for the tunnel: a
// router that has seen the host send from a port keeps translating to that
// number, so a port is only worth advertising if the socket really binds it.
//
//   the first comes from the conferencing list at the top of this file, the
//   ports a corporate network lets out. The tunnel draws from the same list;
//   one allocator hands them out, so a port is never given twice.
//
//   the second is the head of the stream block: 48550, the port a manual
//   forward on a router without UPnP has always pointed at, and one the
//   packages' firewall rules already open. A neighbour on it, and the walk
//   goes on up the block — never to the media pool, which is external only.
//
// A DEV identity holds one port, walked down from the top of the block
// (48573), and never a conferencing port, for the reason its tunnel has a
// block of its own. Each walk keeps to its own stretch of the block, so the
// installed host and a --dev on one machine cannot meet, whatever the router's
// table says.
constexpr int kMuxBlockSpan = 12;

/// Whether `port` is in the stretch of the stream block the mux's block port
/// is drawn from: its foot for the installed host, its top for a DEV identity.
constexpr bool inMuxBlockStretch(bool devInstance, int port)
{
    const int first = kMediaBasePort;
    const int last = kMediaBasePort + kMediaPortCount - 1;
    return devInstance ? port > last - kMuxBlockSpan && port <= last
                       : port >= first && port < first + kMuxBlockSpan;
}

/// The port a stream slot binds when it has a socket of its own.
///
/// Without the mux, 48550 + slot. With it, a stream still binds one where it
/// cannot share (a connection that also listens in TCP, a worker the shared
/// sockets could not be handed to), and the router hole made for it leads to
/// that port: it must never be one the mux holds, or the hole would lead to
/// the mux. So the slots start at the other end of the block from the mux's
/// stretch: 48562 for the installed host, 48550 as ever for a DEV identity.
/// Only a thirteenth slot comes round to the stretch; what the mux really
/// holds there is refused when the hole is asked for (RouterPortAllocator).
constexpr uint16_t slotMediaPort(int slot, bool muxInUse, bool devInstance)
{
    const int start = muxInUse && !devInstance ? kMuxBlockSpan : 0;
    return static_cast<uint16_t>(kMediaBasePort + (slot + start) % kMediaPortCount);
}

// ── Leases ──────────────────────────────────────────────────────────────────
//
// An hour, renewed at the half. Latched to permanent (0) when a router refuses
// a leased mapping or renewal fails twice — a lease expiring under a live
// session takes the connection with it, and the mappings are removed at exit
// either way.
constexpr uint32_t kLeaseSec = 3600;
constexpr int kRenewIntervalMs = 1800000;

// How long a browser waits for a hole to be claimed on its behalf before its
// connection is built on an ephemeral port instead. Covers the gateway
// discovery (two seconds) plus one round of SOAP; anything longer and the
// browser's page would look dead.
constexpr int kClaimWaitMs = 4000;

// Looking for the router again after a discovery found none: half a minute
// first (the network at logon, a router slow to answer once), doubling up to
// ten minutes. Each look is one M-SEARCH of two seconds on the allocator's
// thread.
constexpr int kRediscoverFirstMs = 30000;
constexpr int kRediscoverMaxMs = 600000;

} // namespace mw::routerports
