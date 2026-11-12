# The ports every stream shares (ICE mux)

Status: prototype, behind `"ice_udp_mux": true` in settings.json (default false,
not seeded). UDP only, Windows, workers started as plain children. The rest of
this note says what exists; the last section says what does not yet.

## Why

Each stream runs in its own worker process, and each worker's ICE agent binds a
UDP port of its own (48550 + slot). A network that only lets a few well-known
ports out cannot reach those, and there are far fewer well-known ports than
streams. So the streams share two ports: the server process binds them, keeps
them, and every worker's agent uses them.

## Who does what

```
                    shared UDP ports
  peer  ───────────────►  server process (IceMux)
   ▲                         │  reads every datagram, decides whose it is
   │                         │  header + datagram, over 127.0.0.1
   │                         ▼
   └──────────────────  stream worker (libjuice agent)
      sends directly,        reads its feed socket only
      on its own handles
      of the shared ports
```

- **Server process** (`network/IceMux`, `network/IceMuxCore`). Binds the shared
  ports (IPv4 and IPv6 at once, to itself alone) and a loopback "hop" socket.
  One thread reads the shared ports and forwards each datagram it can place to
  the feed socket of the agent it is for, behind a 36-byte header: the sender's
  address, the shared socket it came in on, and the time it was read. It never
  answers a datagram itself, and while no agent is registered it reads nothing
  of what arrives.
- **Stream worker** (`streaming/IceMuxClient`). Opens the shared sockets from
  descriptions the server made for its process alone (they travel in the config
  line). For each PeerConnection it makes a feed socket on 127.0.0.1,
  connected to the server's hop socket so that it hears nobody else, draws the
  agent's ICE user fragment (12 characters) and password (24), and tells the
  server `{"event":"iceMux","op":"agent","ufrag":…,"feedPort":…}` on stdout
  before the offer leaves.
- **libjuice** (`third_party/patches/libjuice-ice-mux.patch`). An agent in poll
  mode can be given such sockets (`juice_set_shared_sockets`): it sends on the
  shared ones, reads the feed, and treats each (peer address, shared socket) as
  a pair of its own, the first socket preferred.
- **libdatachannel** (`third_party/patches/libdatachannel-ice-mux.patch`).
  `rtc::Configuration::iceSharedSockets` carries the sockets and the
  credentials to the agent.

Video, audio and input leave the worker directly through the shared ports. Only
what comes in makes the extra hop through the server process.

## Which ports

Two, in the order an agent prefers them (`network/IceMuxPorts`,
`network/RouterPortPools.h`):

1. **The conferencing port**: the first of 3478-3481 and 5349-5352 this host
   can have. A network that lets little out lets these out. Only a host that
   is open to the Internet has one, and a DEV identity never does.
2. **The stream block's port**: 48550 for the installed host, 48573 for a DEV
   identity, each walking its own half of the block (48550 upwards, 48573
   downwards) when the number is taken. The two halves never meet, so the
   installed host and a `--dev` on one machine cannot take each other's port.

The router forwards each port to itself: a router that has seen the host send
from a port keeps translating to that number, so a port is only worth
announcing if the socket really binds it. The holes are asked of
`RouterPortAllocator` like the tunnel's, with the same rules (never a
neighbour's entry, every write read back), in UDP alone: no TCP hole is made
to a port nothing listens on in TCP.

When each is bound:

- The block port is bound as the server starts, router or not: a stream on the
  LAN needs it at once, and it is the port a manual forward points at. Its
  hole is asked for when the router answers.
- The conferencing port waits for the router where there is one to ask: the
  router is asked first whether the number is a neighbour's, and only a number
  it has nothing against is bound. Where there is none to ask (UPnP off, no
  gateway), the first number free on the machine is bound all the same.
- A port bound for a claim is neither read nor given to an agent until the
  router has made its hole; refused, it is let go.

A hole the router gives to another machine (found at renewal) is asked for
again on the next number. The port that lost it stays bound and read until the
server stops, for the streams that are on it, and is no longer given to new
ones once another port serves in its place. The mux holds eight sockets at
most; past that it holds no new port.

Each agent is registered with the sockets its worker was handed, and hears each
under its own number for it (0 for its first). A datagram that comes in on a
socket an agent was not given is not handed to that agent: it could not answer
through it.

A stream that binds a socket of its own (see below) must never bind, or get a
router hole to, a port the mux holds: the hole would lead to the mux. So with
the mux the slots' own ports start at the other half of the block (48562 for
the installed host), a slot's hole takes no router-side number from the mux's
half, and no hole is made at all to a port the mux is bound to.

## Whose datagram it is

`IceMuxCore::Router`, in this order:

1. **A STUN Binding request** goes to the agent its USERNAME names, always,
   whatever is known of the sender. Unknown fragment: dropped.
2. **Anything else** goes to the agent the sender is a peer of. A sender
   becomes a *provisional* peer of an agent by sending it a Binding request,
   for 30 seconds unless renewed. An agent has few of them and makes room
   among its own, the oldest first: nothing an agent hears touches what
   another holds. An address that names several agents is the provisional
   peer of each, and what it sends goes to all of them. It becomes a
   *confirmed* peer when an agent itself reports that it passed the ICE
   integrity check (`"op":"confirmed"`): it is that agent's alone from then
   on, and nothing unverified displaces it.
3. **A STUN response that is not a confirmed peer's** is the answer to a check
   an agent sent, perhaps before the address named it: besides the agents the
   sender is a provisional peer of, every agent registered for less than a
   minute gets it, within a budget per second; an agent ignores a transaction
   that is not its own.
4. The rest is dropped.

The user fragment gives no right: the agent verifies the message integrity of
every request, as it always did. The reader of the STUN header and USERNAME
allocates nothing and stays inside the datagram
(`tests/test_ice_mux_core.cpp`, `tests/fuzz/fuzz_ice_mux.cpp`).

## When it is not used

Every one of these leaves the stream on a socket of its own, as without the
mux:

- `ice_udp_mux` false or absent;
- no port of the stream block can be bound when the server starts;
- a port that serves keeps failing to read (the reading thread leaves it alone,
  tries it again every second and says so once): streams that start meanwhile;
- the sockets cannot be handed to the worker, or the worker cannot open them or
  make its feed;
- a connection that also needs TCP (the TCP rungs keep their own socket, which
  is what their listener sits on).

## Not there yet

- Workers started through the launcher service or the elevated task, and Linux
  and macOS: they bind their own port.
- The public candidates: the holes are made, but under the mux no public
  address is announced yet, and a stream still asks for its slot's own hole
  whether or not it ends up on the shared ports.
- TCP on the shared ports, and TCP 443.
- Per-packet DSCP: the class is kept per shared socket.
- A switch in the environment serves the latency bench and nothing else:
  `MW_ICEMUX_STATS=1` (every ten seconds the server logs its counters and its
  CPU time, and each worker the delay between the server's read of a datagram
  and its agent's: median, p99, p99.9, maximum).
