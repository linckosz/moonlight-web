# One UDP port for every stream (ICE mux)

Status: prototype, behind `"ice_udp_mux": true` in settings.json (default false,
not seeded). UDP only, one shared port, Windows, workers started as plain
children. The rest of this note says what exists; the last section says what
does not yet.

## Why

Each stream runs in its own worker process, and each worker's ICE agent binds a
UDP port of its own (48550 + slot). A network that only lets a few well-known
ports out cannot reach those, and there are far fewer well-known ports than
streams. So the streams share a port: the server process binds it once, keeps
it, and every worker's agent uses it.

## Who does what

```
                    shared UDP port(s)
  peer  ───────────────►  server process (IceMux)
   ▲                         │  reads every datagram, decides whose it is
   │                         │  header + datagram, over 127.0.0.1
   │                         ▼
   └──────────────────  stream worker (libjuice agent)
      sends directly,        reads its feed socket only
      on its own handle
      of the shared port
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

Video, audio and input leave the worker directly through the shared port. Only
what comes in makes the extra hop through the server process.

## Whose datagram it is

`IceMuxCore::Router`, in this order:

1. **A STUN Binding request** goes to the agent its USERNAME names, always,
   whatever is known of the sender. Unknown fragment: dropped.
2. **Anything else** goes to the agent the sender is a peer of. A sender
   becomes a *provisional* peer of an agent by sending it a Binding request
   (few of them for each agent, 30 seconds unless renewed, the agent's own
   oldest gives way), and a
   *confirmed* peer when the agent itself reports that it passed the ICE
   integrity check (`"op":"confirmed"`). Nothing unverified displaces a
   confirmed peer, and a provisional peer follows one agent at a time: a
   request that names another agent is delivered to it, but moves nothing
   else that address sends, until the entry has lapsed or its agent is gone.
3. **A STUN response that is not a confirmed peer's** is the answer to a check
   an agent sent: besides the agent its sender follows, if any, every agent
   registered for less than a minute gets it, within a budget per second; an
   agent ignores a transaction that is not its own.
4. The rest is dropped.

The user fragment gives no right: the agent verifies the message integrity of
every request, as it always did. The reader of the STUN header and USERNAME
allocates nothing and stays inside the datagram
(`tests/test_ice_mux_core.cpp`, `tests/fuzz/fuzz_ice_mux.cpp`).

## When it is not used

Every one of these leaves the stream on a socket of its own, as without the
mux:

- `ice_udp_mux` false or absent;
- the shared port cannot be bound when the server starts;
- a shared port keeps failing to read (the reading thread leaves it alone,
  tries it again every second and says so once): streams that start meanwhile;
- the sockets cannot be handed to the worker, or the worker cannot open them or
  make its feed;
- a connection that also needs TCP (the TCP rungs keep their own socket, which
  is what their listener sits on).

## Not there yet

- Workers started through the launcher service or the elevated task, and Linux
  and macOS: they bind their own port.
- Two shared ports (a well-known one first), their router mappings and the
  public candidates: under the mux no public address is announced today.
- TCP on the shared ports, and TCP 443.
- Per-packet DSCP: the class is kept per shared socket.
- The latency of the extra hop, measured (`juice_set_feed_timestamp_handler`
  gives the worker the time the server read each datagram).
