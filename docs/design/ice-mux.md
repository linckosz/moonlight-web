# The ports every stream shares (ICE mux)

Status: prototype, behind `"ice_udp_mux": true` in settings.json (default false,
not seeded). UDP only. The rest of this note says what exists; the last section
says what does not yet.

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
- **Stream worker** (`streaming/IceMuxClient`). Gets the shared sockets (see
  « How a worker gets the sockets »). For each PeerConnection it makes a feed
  socket on 127.0.0.1,
  connected to the server's hop socket so that it hears nobody else, draws the
  agent's ICE user fragment (12 characters) and password (24), and tells the
  server `{"event":"iceMux","op":"agent","ufrag":…,"feedPort":…}` on stdout
  before the offer leaves.
- **libjuice** (`third_party/patches/libjuice-ice-mux.patch`). An agent in poll
  mode can be given such sockets (`juice_set_shared_sockets`): it sends on the
  shared ones, reads the feed, and treats each (peer address, shared socket) as
  a pair of its own, the first socket preferred. A datagram's class (its
  Differentiated Services byte) goes with the datagram, as ancillary data: a
  shared socket is one socket for every worker, and its own class would be
  whichever of them set it last, so it is never set. The class is the one the
  worker's own tracks ask for, nothing a peer sent. Where the system refuses
  a class for one datagram, what leaves by the shared ports leaves unmarked.
- **libdatachannel** (`third_party/patches/libdatachannel-ice-mux.patch`).
  `rtc::Configuration::iceSharedSockets` carries the sockets and the
  credentials to the agent.

Video, audio and input leave the worker directly through the shared ports. Only
what comes in makes the extra hop through the server process.

## How a worker gets the sockets

It depends on whose process the worker is, which the way it is started says
(`StreamWorkerHost::muxHandoverFor`):

- **A plain child of the server**: the server's own account, at its own level.
  The server hands the sockets over. On Windows it makes a description of each
  for that process alone (`WSADuplicateSocket`), and the descriptions travel in
  the config line. On Linux and macOS, where every worker is such a child, the
  worker is started with them: the server's sockets are closed in every process
  it starts (close-on-exec), and the server lets them through in that one child
  alone, between its fork and its exec, then names their descriptors in the
  config line. They cross the package's launcher the same way when the native
  worker is started through it, which opens and closes nothing. The share is
  taken before the start, so no socket of it is closed, and no number of theirs
  given to something else, before the child has them. The worker, as the first
  thing it does with its config, closes them to whatever it starts in turn.
- **A worker a server that is a service puts in the console session** runs as
  the user sitting there. That user may be another account than the one the
  installation was made by, and a socket in a process of theirs is theirs to
  read and to send from, whatever the worker itself does with it. So such a
  worker is handed the sockets, like a plain child, only when the token it is
  started with is the owner's; any other account gets none, and binds a port
  of its own. « The owner » is a SID, never a name: the installer, elevated,
  has the executable record the account it runs as under
  `HKLM\SOFTWARE\<product>\Setup` (`OwnerSid`), where only an administrator or
  SYSTEM writes (`common/OwnerAccount`). The server compares it, at each
  launch, with the account of the very token the worker gets. With nothing
  recorded the answer is no: an installation made before this record shares
  with no console worker until its owner runs the installer again (an install
  over it, a repair). An installer the system itself runs records nothing and
  keeps what is recorded: that is how a server that is a service updates
  itself, and the system's own accounts are never the owner. No guess is made
  from who logs on, and a SID is never given to a second account: once the
  owner's account is deleted nobody matches, until an administrator installs
  again or repairs. A stream that runs is not moved: a change of user at the
  console matters for the next launch.
- **A worker above the server** (started by the launcher service as SYSTEM, or
  by the elevated task). The server may not open such a process, so it cannot
  hand it anything, and it gains no right over it. It tells the worker, in the
  config line, the numbers of its own handles and the port of each, and the
  worker copies them out of the server itself (`streaming/IceMuxHandles`), the
  way it takes the kept devices:
  - the server is the process at the other end of the pipes the worker was
    started on, the one whose image it checked then; its image is checked
    again on the very process the copies come from;
  - that process is opened to copy handles and to be asked what it is,
    nothing more, and is let go as soon as the copies are made;
  - for the time of the copy the worker could copy any handle of the server.
    That is accepted because such a worker is at least as privileged as the
    server, and it is checked: a process below the server does not copy;
  - four handles at most, each copied as it is and left with the server.

Either way the worker uses a socket only once it is known to be a UDP socket
bound to the port the server said; anything else is closed, and the stream
binds a port of its own. The server registers a worker's agents on the sockets
it gave that worker, whatever the worker says.

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
again on the next number. The port that lost it is no longer given to new
streams once another port serves in its place, and stays bound and read for
the streams that are on it: a worker holds a lease on the sockets it was
given, from its start to its end, and its agents register on those sockets
whatever replaced them since. Once no worker holds such a port and no agent is
registered on it, the reading thread closes it, between two reads, and its
number is free for another port; what the mux had learned of peers through
it is forgotten. Nothing is closed while the router is being asked for a
port: the answer may be that a replaced one serves again. The mux holds eight
sockets at most; past that it holds no new port until one is closed.

A host that stops removes its holes; one that is killed leaves them, for one
lease or for as long as a router that keeps no lease runs. So every hole of
the host, the tunnel's and the slots' as well as these, is noted before it is
made, and its description ends with a tag of the installation, six characters
drawn once. The next start takes back what it finds of them, once the router
has answered: only an entry that is this installation's by that record, by
the address it forwards to, by its port and by its description, and none on a
doubt (`network/RouterHoles.h`, `RouterPortCore::sweep`).

Each agent is registered with the sockets its worker was handed, and hears each
under its own number for it (0 for its first). A datagram that comes in on a
socket an agent was not given is not handed to that agent: it could not answer
through it.

A stream that binds a socket of its own (see below) must never bind, or get a
router hole to, a port the mux holds: the hole would lead to the mux. So with
the mux the slots' own ports start at the other half of the block (48562 for
the installed host), a slot's hole takes no router-side number from the mux's
half, and no hole is made at all to a port the mux is bound to.

## What the browser is told

When a stream starts, the server decides where it goes before it asks the
router for anything (`main.cpp`, `streamSharesPorts`): on the shared ports when
the mux is in use and reads well, the worker is one that gets the shared
sockets, and the connection is not one that also listens in TCP. Such a stream
asks for no hole of its own. Any other stream binds its slot's own port and
asks for that port's hole, as without the mux.

A stream on the shared ports is told, in its config line, the router's public
address and which of the shared ports the router forwards right then
(`IceMuxPorts::announced`): a port that serves and whose hole is held, on that
very number. The agent has a host candidate for each shared port on each
interface, and the relay does with each what it does with a stream's own port
(`RelayBase::emitLocalCandidate`):

- a candidate on a port the router forwards is also announced at the public
  address, on the same number, with a reflexive candidate's priority, so that
  a peer on the same LAN still prefers the direct pair. The conferencing port's
  copy comes out ahead of the block port's, as their host candidates do;
- a candidate on a shared port the router does not forward has no such copy.
  Nothing at the public address leads to it: a hole never made, or one the
  router gave to another machine;
- the internal address itself goes only to a peer of our own network, as ever.

When the router forwards none of the shared ports, the candidates go out as
they are, as for a stream without a hole.

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
   on, and nothing unverified displaces it. Nor does another agent's word
   while the peer still sends this one something: a second agent's
   confirmation of the same address moves it only after fifteen seconds of
   quiet, which a stream that runs never leaves.
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
- the worker runs in the console session (started by a server that is a
  service) as another account than the one the installation was made by;
- the sockets cannot be handed to the worker, or the worker cannot open them or
  make its feed;
- a connection that also needs TCP (the TCP rungs keep their own socket, which
  is what their listener sits on).

## Not there yet

- Without a hole, the public address of the block port: an agent asks the STUN
  server from its first shared port only.
- TCP on the shared ports, and TCP 443.
- A switch in the environment serves the latency bench and nothing else:
  `MW_ICEMUX_STATS=1` (every ten seconds the server logs its counters and its
  CPU time, and each worker the delay between the server's read of a datagram
  and its agent's: median, p99, p99.9, maximum).
