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

A bench key, `debug_dev_standard_ports`, lets a DEV identity walk the installed
host's ports for one run, so that the conferencing port can be tried without a
release build. It is granted only where nothing on the machine holds one of the
eight, read from the system's socket tables as the server starts; refused, the
instance says so and keeps its own ports (`network/PortIdentity.h`). Never for
a user: nothing writes it and no API reads it.

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
the mux is in use and reads well, and the worker is one that gets the shared
sockets. A connection that also listens in TCP goes there too when
`ice_tcp_passive` is on and one of the shared ports can be listened on in TCP
(« TCP on the shared ports », below). Such a stream asks for no hole of its
own. Any other stream binds its slot's own port and asks for that port's hole,
as without the mux.

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

A passive TCP candidate is treated the same way, by the ports the router
forwards in TCP, which the config line names apart: a shared port whose TCP
forward is a neighbour's has its UDP copy at the public address and no TCP
one.

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

## TCP on the shared ports

A browser that cannot send UDP dials a TCP port (`ice-tcp-passive.md`). On the
shared ports that port is the server's: the same numbers as the UDP ones,
listened on by the server process and by no worker (`network/IceMuxTcp`).

**When.** A port is listened on while an agent expects a connection there, and
no longer. A worker says so with its agent (`"op":"agent"` with
`"tcp": true`), and the server opens that agent's window on the ports and
under the fragment it has just registered the agent with: a worker opens a
window for its own agent and for no other. The listeners are bound before the
worker's line is done with, so that a port answers by the time the browser
learns of it. The window ends when the agent is connected or gone, when its
worker is gone, or after 45 seconds, whichever comes first. Between two
windows nothing of MoonlightWeb listens there: the router's TCP hole on the
number then leads to a closed port, or to another program of this machine
that takes the number meanwhile. Each time a listener is closed the log has
one line for it, with the port and why nothing is expected there any more
(connected, no connection in time, gone, a try of the way here over).

**What the server reads.** Of each connection, its first frame (RFC 4571: two
bytes of length, then a STUN message of 20 to 1024 bytes) and never a byte
more: it asks the socket for exactly what the frame still lacks. The frame
must be a Binding request whose USERNAME starts with the fragment of an agent
that expects a connection on the very port this one came in on
(`IceMuxCore::readStun`, the reader of the UDP side). The server writes
nothing on a connection and checks no integrity: it has no password. It
routes; the agent admits.

**What that costs is bounded** (`network/IceMuxTcpCore`, a fixed table):

- 8 connections wait for their frame at once, 2 of them from one source (an
  IPv4 address, an IPv6 /64). A third from a source takes the place of that
  source's oldest; a full table gives its oldest place, whatever that
  connection has sent;
- a frame has 2 seconds from the accept;
- an agent is handed at most 2 connections its worker has not answered for,
  and 32 in the life of its window: a fragment travels in the clear with every
  check, so anyone can name it;
- everything else is closed at once, by a reset.

**The hand-over.** Unlike the UDP sockets, these arrive while the stream runs,
one at a time. The server keeps its end of a connection until the worker says
it has its own, or will not have it (`"op":"tcpGot"`), or for 2 seconds; it
reads nothing of it meanwhile. A connection is kept from child processes from
the instant it is accepted on Linux; elsewhere by the line after the accept,
under the lock a stream worker is started under (`common/LaunchLock.h`).

| Worker | How the connection reaches it |
|---|---|
| Handed its sockets (a plain child, or the console session on the owner's account) | A command on its standard input carries a description of the socket made for that process alone (`WSADuplicateSocket`). Which process that is, is read off a handle the server took on the child it started, which must still run: never a number it was told. |
| Takes its sockets (launcher service, elevated task, sign-in screen) | The command names the server's handle, and the two ports of the connection. The worker copies it through the handle it holds on the server for the stream's life, opened and checked as for the UDP sockets (`IceMuxHandles::Server`), 32 copies at most per window, counted from the worker's own agent and nothing the server says. It copies only while its agent still expects a connection, and holds its copy against those two ports. A handle the server let go of before the worker copied it could be another one's number by then: so a connection that was named is kept until that worker answers for it or is gone, whatever becomes of its agent and however long that takes, four such at most for one worker. |
| Linux, macOS | One message of a channel made for that worker's start, whose other end crosses the start as the shared sockets do: the descriptor itself (`SCM_RIGHTS`), with the agent it is for and the frame. |

A worker in the console session on another account than the owner's has no
shared sockets and no channel, and is handed nothing.

The worker checks what it got before it uses it
(`IceMuxHandles::connectionChecked`): a stream socket, connected, that listens
for nothing, on one of the shared ports this worker holds, and kept from
whatever the worker starts. Then the agent takes it
(`juice_tcp_passive_offer`, in `libjuice-ice-mux.patch`). From there nothing
is new: the connection goes through the agent's own gate, the one a connection
to its own listener goes through, on that first frame: a Binding request for
this agent, whose integrity holds under its password, while it still checks.

**The router.** The claim of a shared port asks for the TCP hole on the same
number (`RouterPortCore::muxRequest`), under the tunnel's rule: an entry that
is free or this host's, read back, noted in the record with its tag. A
neighbour's TCP forward on the number is left alone, and the port is then
announced for UDP only. With `ice_tcp_passive` off, no TCP hole is asked for
and nothing listens.

## TCP 443 on the router

Some networks let out TCP 443 and nothing else. For a browser there, the host
asks its router for one more entry, TCP only: the router's 443 towards the
stream block's shared port, where the server listens (above). No socket of the
host binds 443, and its HTTPS port is not involved. It is behind
`"ice_tcp_443": true` in settings.json (default false, not seeded), needs the
shared ports and `ice_tcp_passive`, UPnP and Internet Access, and is never
asked for by a DEV identity.

**The guard** (`network/Tcp443Guard`). 443 is the port people forward by hand
and the one a router keeps for itself, so the host asks for it only when it
can show, alone, that nobody has it and that it then leads here. The guard
runs once the block port has its TCP hole, in this order, and the first step
that fails leaves 443 as it is, with one line in the log that names the step
and no address:

1. the address browsers reach this host at is a public IPv4 one, and it is the
   one the router says is its own: no second router in front;
2. the router says it has no TCP entry on 443. An entry, whoever it leads to,
   or an answer that says nothing, ends the run. Only this installation's own
   (by its record, its description with the tag, the address and the port
   noted) is kept, and renewed; but never one that this run of the server wrote
   and could not prove (a run given up while it waited for its proof, on a
   router that kept the entry): that one is taken off, and the run goes on as
   for a free 443. A router that keeps it ends the run here, and the next one
   waits an hour;
3. the router leads back here from its own address: a connection dialled to
   the block port there arrives at this host's listener;
4. nothing completes a handshake on 443 there: a rule made by hand does not
   show in what step 2 asks;
5. the router is asked once more that it has no entry on 443 (the two dials
   took their time); then the entry is noted in the record, written, read
   back, and a connection dialled to 443 must arrive here. Otherwise it is
   removed, the removal read back, and the line struck.

Held, it is written again and read back with the other holes, every half
hour, without a dial. It is taken off when the server stops, when a switch it
stands on goes off, and when the block port loses its hole or moves. A run
that was killed leaves a line in the record, and the start-up sweep takes the
entry back as it does any other; nothing is asked for on 443 before that sweep
is over, and what is looked at then is the situation as it was last told, not
one from before a switch went off.

**Where it runs.** A dial waits for up to 2.5 seconds, and a run makes three.
The allocator's thread, which every claim and every renewal goes through, does
not wait for them: it makes a run a step at a time (`Guard::begin()`, then
`after()` with each dial's answer), and between two steps it serves whatever
else is asked of it. The dials are made on a thread that does nothing else
(`Dialler`, in `network/Tcp443Probe`), started by the first dial, so a host
that never asks for 443 has none. That thread is given a dial (an address, a
port, the shared port a connection must come in on) and gives back a yes or a
no with the number of the run it was for; it never sees the router, the record
or the guard, and it logs nothing. Everything that is asked of the router,
written on it or noted stays the allocator's thread's alone.

There is one run at a time and one dial at a time (`Keeper`). A look that
comes while a run is under way begins nothing if what the run was begun on
still stands. If it does not (a switch went off, the block port moved or lost
its hole, the public address changed), the run is given up: its dial is cut
short, which takes about a tenth of a second, and an entry it had already
written is removed, the removal read back; a router that keeps the entry gets
a warning in the log, and the line stays in the record. The answer of a dial
whose run was given up is dropped when it comes: it moves no run, and is
neither counted nor logged. The first dial of the next run is not made before
that answer is back, so the listener never expects two connections of this
host's own at once. The same holds when the server stops with a run under
way, and the dial thread is over before the allocator's own goes.

**How it knows a connection arrived here** (`network/Tcp443Probe`). For steps
3 and 5 the host makes a fragment for that one try, sixteen characters, which
no agent's fragment has, and tells its listener to expect one connection that
names it, for 2.5 seconds; two such tries at most are under way. It dials and
sends one frame: a Binding request that holds that name and nothing else, 46
bytes. The listener reads it as it reads any first frame, counts the try, and
closes the connection: it is handed to no agent and to no worker, and nothing
is written back. For step 4 it dials and sends nothing.

**How often** (`Pace`, in `Tcp443Guard.h`). A run that stopped before it
dialled may be made again whenever the holes are looked at. One that dialled
and stopped (steps 3 and 4, and the question step 5 asks the router before it
writes) waits an hour, counted from the time it ended at. One that wrote and
did not keep what it wrote (step 5) is the last until the server starts anew,
and says so in a warning. An entry that was held and is lost is asked for
again at once, once, and an hour later if it is lost again within the hour. A
run that this host gave up itself (a switch, the block port) is not counted.
One given up because the public address changed is not this host's doing, and
is paced as a lost entry is: the next run may begin at once, once, and after an
hour if the address changes again within the hour, so that an address that
flips does not have the router written and cleared at every look.

**A former address.** An entry of this installation's noted under another
address than this machine has today is not left for later, as the other kinds
are: that address may be another machine's by now. The router is asked at each
start and each renewal, and where it still shows the entry as it was written
(by the record AND the description, never by the address alone) the entry is
removed. A router that will not remove it leaves a warning in the log, without
the address, and a yes in the status the admin page reads
(`router443_stale_removal_refused`: a yes or a no, for the admin alone), under
which the page shows a text of its own. It is a no again once the entry is
seen to be gone.

**What the browser is told.** The passive TCP candidate on the block port has
one more copy at the public address, on port 443, below every other candidate
of the connection (`streaming/IcePriority.h`): a browser that gets through any
other way never ends on it. A stream's worker learns which shared port 443
leads to with the ports the router forwards (`"tcp443"` in `"iceMuxPublic"`).

**The entry link.** The page itself comes through the tunnel, so the tunnel's
connections are reached that way too. Their ICE agents live in the server
process, each on its own port with its own listener, as before. With 443 held,
such an agent is also expected on the block port, under the fragment its own
description carries, within the bounds of a worker's agent (2 connections it
has not taken, 32 in a window, 45 seconds). It sends the browser the copy on
443 of its public TCP candidate, and a connection whose first frame names it
is taken out of the listener and offered to it (`juice_tcp_passive_offer`,
which an agent that listens itself takes as well): same process, nothing is
copied. Its own gate decides, as for a connection to its own listener. These
agents are numbered apart from the workers' agents, so that what names the one
is never handed, or taken, as the other's.

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
- a connection that also listens in TCP, when `ice_tcp_passive` is off, when
  no shared port can be listened on in TCP as the stream starts, or when the
  worker cannot be handed the connections (that rung then keeps its own
  socket, which is what its listener sits on).

## Not there yet

- Without a hole, the public address of the block port: an agent asks the STUN
  server from its first shared port only.
- A switch in the environment serves the latency bench and nothing else:
  `MW_ICEMUX_STATS=1` (every ten seconds the server logs its counters and its
  CPU time, and each worker the delay between the server's read of a datagram
  and its agent's: median, p99, p99.9, maximum).
