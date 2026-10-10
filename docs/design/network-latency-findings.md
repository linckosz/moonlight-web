# Network latency findings

A running record of what has been observed about the network's share of
MoonlightWeb's latency: the WebRTC DataChannel (SCTP over DTLS over UDP, through
libdatachannel and usrsctp), its buffers and congestion window, the browser's
receive side, Wi-Fi against Ethernet. It keeps the raw numbers, how they were
measured, what was tried and failed and why, and the open questions, so that a
later study can start from here without the plans or the chat that produced
them.

Entries are dated (DD/MM/YYYY). The newest work is appended at the end of each
section; nothing is rewritten after the fact except to mark it superseded.
Times are local (Europe/Paris).

Sources: the plan « Wi-Fi : la vidéo qui attend dans SCTP »
(`wifi-sctp-descente.md`, outside the repo), its bench `scripts/bench/wifi/`
(README holds the tables), the Ultra LAN POC (`docs/design/ultra-lan-poc.md`
§6), the native host's design (`docs/design/native-capture-encoder.md`) and
bench (`docs/bench-native-host.md`).

## 1. The transport, as it is

What the numbers below were measured against (code as of 04/10/2026).

- **Video** goes over a WebRTC DataChannel, **ordered**, with
  `maxPacketLifeTime` 500 ms (`DataChannelRelay::createDataChannels`,
  `kVideoFrameLifetimeMs`; PR-SCTP timed reliability). Unordered was tried and
  removed: every reordering looked to the client like a hole in `frameId`,
  which invalidated the reference and asked for an IDR (the comment in
  `createDataChannels` tells it).
- Each encoded frame is cut by the host into **messages of up to 16,000 bytes
  of payload + a 17-byte header** (`FrameSender::buildFragments`), each sent
  as one DataChannel message. usrsctp cuts each message into DATA chunks of
  about one MTU: libdatachannel's MTU is fixed at 1,280 bytes, so a 16 KB
  message is ~13 packets.
- **The input channel** (id 2, reliable, ordered, JSON) shares the same SCTP
  association: pongs, cursor shapes, rumble, clipboard, link stats. The host's
  messages on it wait behind the video (see §3, W0/W1).
- **libdatachannel's settings** (`SctpTransport::SetSettings`, unless the
  host overrides them): send buffer 1 MiB sysctl (but see the 256 KiB
  finding below), receive buffer 1 MiB, initial cwnd 10 MTU, **max burst 10
  MTU**, congestion control RFC 2581 (module 0), delayed SACK 20 ms, RTO min
  200 ms, max chunks on queue 10 K, local max message size 256 KiB.
- **The host's own send path**: a sender thread (`FrameSender`) with a queue
  of at most 2 frames (a third evicts the oldest delta); `bufferedAmount` (what
  libdatachannel holds after usrsctp refused it) watched by `SendBacklog`: a
  delta is dropped only after the buffer stayed above its "drained" floor
  (50 ms of bitrate, 8-48 KB) for **250 ms** (`kToleranceMs`). After a drop,
  without named drops or ride-out, the relay closes an "awaiting IDR" gate and
  drops every delta until a keyframe (`gatedDelta`).
- **The native host's rate governor** (`encode::RateGovernor`): cuts 20 % on
  overuse (receiver's one-way-delay rise ≥ 30 ms, min-based; frame gaps;
  sender evictions; since 03/10/2026 SCTP retransmissions ≥ 3 ‰ in a report
  window on the Windows host), holds 2 s, raises after 3 s quiet. The client
  sends a `linkstats` report every 500 ms.
- **The browser** (Chrome) runs its own SCTP stack (dcsctp) behind a UDP
  socket whose receive buffer is small (see W1 bis: a 64 KB sink reproduces
  its drops).

## 2. Tools and method

- **Click → flag** (`frontend/js/stream/LatencyProbe.js`, host
  `backend/src/LatencyFlag.cpp`): the client clicks, the host raises a flag on
  screen, the client times until it sees it. The whole loop the player feels.
  Split by `inputstamp` into **up** (client → host injection), **rest**
  (everything after). ⚠️ Until `652fc726` (09/10) the probe measured itself:
  on the default renderer (Canvas2D, main thread) it read the flag's pixels
  after every draw and dated the flag after that read, ~4.5 ms per click
  (4.7 ms a frame on the 780M). Absolute click times before that are high by
  about that much; gaps between arms at the same frame rate stand. Comparisons
  across frame rates do not (240 fps was 4-10 ms worse than 120). See §3,
  09/10.
- **Content age** (`scripts/bench/content-age/`, `scroll.html?band=time`): the
  age of what is on screen, read from a time band. ⚠️ The probe that reads the
  band delayed the frames it read by 4-13 ms on the main thread until
  `b83f3dac` (03/10, moved to a worker); absolute ages before that are high by
  about that much (`f69ed657`, POC §6.1).
- **Per-frame log** (`frontend/js/stream/FrameLog.js`, POC U0.2 `ea3d313c`):
  every drawn frame's capture → arrival → decode → draw on the host's clock
  (via ping/pong). `e2e` = capture → draw; `down` = capture → last chunk
  arrived.
- **Relay frame log** (`relaylog=1`, `backend/src/streaming/RelayFrameLog.h`,
  `5ce2228c`): each video frame's way through the relay — capture on the
  host's steady clock, the decision (sent, backlog drop, gated, evicted), first
  and last fragment handed to the DataChannel, `bufferedAmount` before and
  after, usrsctp's retransmission counters, SCTP's smoothed RTT.
- **`flagpath.py`** joins the three, click by click, into legs that add up to
  the measured click: up, inject, raise (flag raised), toCap (to the capture
  that shows it), encode, send (sender queue), **net** (last fragment into the
  DataChannel → last chunk arrived on the client), decode, draw, detect.
  **inSctp** = net − srtt/2: the part of `net` spent before leaving usrsctp
  (an estimate: it assumes the air takes half the SCTP RTT).
- **usrsctp's global counters** (`mw::sctp::readCounters`,
  `SctpCounters.cpp`; process-wide, one session per worker): sent,
  retransmitted (fast, T3), fast-retransmit inside a recovery, SACKs, sends
  held by the window (`sctps_send_cwnd_avoid`), windows trimmed to max burst
  (`sctps_maxburstqueued`). The session ends with `SCTP this session: …` and
  `SCTP window this session: …`.
- **Kernel drops on the Mac client**: `netstat -s -p udp`, "dropped due to
  full socket buffers", read before and after each pass by `series.py`.
- **`udp_ref.py`**: a bare UDP ping to an echo on the client beside a pass,
  and video-shaped UDP bursts to a sink with a chosen receive buffer.
- **Series**: `scripts/bench/wifi/series.py` (one client's passes, alternated
  rounds), `report.py` (one row per pass), outputs in `bench-out/wifi/` and
  `bench-out/content-age/` (not in the repo).
- **Bench machines**: host DualRTX (Arc A380 renders the virtual display at
  240 Hz; RTX and an AMD iGPU also available). Clients: Mac M1 (Wi-Fi,
  120 Hz), N95 mini-PC (Wi-Fi, 60 Hz), UM790Pro under Windows (Ethernet 1 GbE,
  120 Hz, the witness), iPhone (Safari, Wi-Fi, by hand).
- Unless said, "two rounds" means the variants alternated A B A B (or A B C A
  B C), one pass each, the page scrolling at 240 fps, 60 clicks a pass; tables
  give the mean of the rounds.

## 3. Findings, by date

### 06/08/2026 — Periodic stutter on a Mac: AWDL, not the stream

Micro-stutters seen only on the Mac (Wi-Fi 5, host on Ethernet) came from
AirDrop/AWDL. `ping -i 0.1` Mac → host: one spike every 5 packets exactly, a
~500 ms period, 28 → 74 ms amplitude growing ~4 ms a cycle, on a 4.6 ms base.
AirDrop off → no stutter. Moonlight-Qt and Parsec stuttered the same way.
Lesson: a metronomic disturbance is the radio leaving the channel; congestion
is random. Ask for the ping before touching the code.

### 17/09/2026 — Corporate Wi-Fi: a freeze the stream never caught up

Heavy lag on a corporate Wi-Fi/firewall (Mac, iPhone; the same iPhone on 5G:
no freeze in 3 min). The stats showed ~20 ms latency, blind to the transport's
queue.
- After a timeout usrsctp drops to cwnd = 1 MTU, ssthresh = max(cwnd/2,
  4 MTU), then +1 MTU per RTT: the host's buffer (304 KB visible + 256 KB in
  usrsctp) drained at ~2 Mbit/s for 2 s on a 20 Mbit/s link.
- The video channel was ordered with `maxRetransmits 3`: everything in flight
  at the freeze was retransmitted in order when the link came back, before the
  keyframe; the client decoded second-old frames.
- The IDR cooldown doubled at each keyframe our own guard dropped.
- Fixed (`509bd9c2`, `9bb268d6`, `dfe4e0ee`, `92772341`, `80d73021`):
  `maxPacketLifeTime` 500 ms instead of a retransmission count, keyframe asked
  when the buffer drains, fast raise to the proven rate, "link freezes" line.
- A second blind spot: usrsctp's fixed 256 KiB hid 1 s of video at 2 Mbit/s
  (iPhone capture: "LINK QUEUE 933 ms, freezes 0"). "Fixed" by `fe359154`:
  send buffer = 100 ms of the set bitrate, 64-256 KiB. **⚠️ That fix never
  took effect: see 03/10/2026, the 256 KiB SO_SNDBUF.**

### 25/09/2026 — A click tail shaped like a T3 timeout

One click in ten ~205 ms late, the shape of a T3 retransmission timeout at
usrsctp's 200 ms minimum RTO: a lone flag frame whose last packet is lost has
nothing behind it to trigger a fast retransmit. Bench knobs added, never
settings: `MW_SCTP_RTO_MIN_MS`, `MW_SCTP_SACK_DELAY_MS` (`applySctpSettings`).
Not followed up with an A/B recorded here.

### 01-03/10/2026 — Loss lab and congestion modules (plan Punktfunk, A0)

`4815aabd`: bench keys `loss=` (video messages thrown away before SCTP),
`burst=`, `sctpcc=0..3` (usrsctp congestion control: RFC 2581, HSTCP, H-TCP,
RTCC), `flood=` (useless traffic on channel 3). RTCC (`sctpcc=3`), A/B
alternated on the N95 in Wi-Fi: retransmissions halved, click unchanged (88-94
against 88-100 ms). Set aside.

### 03/10/2026 — T7 (radios plan): the click's surplus is on the way down

| Client | Click → flag | Up | Rest | SCTP retransmissions |
|---|---|---|---|---|
| Mac M1, Wi-Fi, page at 240 | 71.4 | 2.6 | 69.2 | 0.6 % |
| N95, Wi-Fi, page at 240 | 88-100 | 4-8 | 80-90 | 0.2-0.5 % |
| UM790Pro Windows, Ethernet | 34.6 | 1.6 | 32.6 | 0 |
| Still screen (all) | 27-37 | 1.5-2.6 | 25-35 | ~0 |

(ms.) The click goes up fast; a bare UDP ping on the same Wi-Fi does its round
trip in 4.7 ms (p99 13.8). The host's small messages on the input channel took
12-37 ms at the median, 387 ms at p90 on the Mac. `bufferedAmount` reached
142 KB. Hypothesis then: Wi-Fi loses or reorders chunks, usrsctp's loss-based
window brakes, everything waits behind.

### 03/10/2026 — W0: the baseline (10:25-11:08)

Host Arc, Auto with detection, two rounds per cell (`report.py w0`), ms:

| Client | Content | Click | Up | Rest | Shown age | Frame age med / mean / p90 | Retr. | Messages (p90) | UDP ping (p99) |
|---|---|---|---|---|---|---|---|---|---|
| Mac M1, Wi-Fi | page at 240 | 71.1 | 3.3 | 67.1 | 37.9 | 20.7 / 33.1 / 63.2 | 0.72 % | 34.6 (225) | 4.6 (13.5) |
| | game 75-83 | 57.3 | 2.3 | 54.7 | 47.9 | 37.5 / 41.7 / 69.7 | 0.71 % | 68.7 (455) | 5.1 (12.3) |
| N95, Wi-Fi | page at 240 | 99.4 | 5.6 | 90.7 | 56.6 | 35.0 / 47.6 / 76.4 | 0.57 % | 34.7 (269) | |
| | game 75-83 | 93.5 | 5.9 | 85.1 | 54.7 | 38.5 / 47.9 / 84.1 | 0.21 % | 25.6 (67) | |
| UM790Pro, Ethernet | page at 240 | 33.6 | 1.7 | 31.9 | 24.0 | 12.6 / 13.0 / 17.2 | 0 | 4.1 (9) | |
| | game 75-83 | 34.2 | 1.6 | 31.8 | 28.2 | 16.6 / 17.5 / 25.6 | 0 | 5.2 (14) | |

- Wi-Fi's cost is in the tail: the median frame age on the Mac is barely
  above Ethernet's, the mean and p90 are not.
- 13-20 link freezes a pass on the Mac (longest 1.2-3.3 s), 1-3 on the N95,
  none on Ethernet. Frames dropped by the host: Mac 45-109/min, N95 30-39.
  `bufferedAmount` up to 1.3 MB on the Mac.
- The gate: after 250 ms of full queue the relay drops a delta; the Arc has no
  named drops (oneVPL, see below) and the Mac cannot decode through a hole, so
  it awaits a keyframe and drops every delta until then (`gatedDelta`,
  231-322 a pass on the Mac at T7).

### 03/10/2026 — W1: where the flag's frame waits (13:15-14:15)

`relaylog=1`, `flagpath.py`, ~230 clicks per client, medians in ms:

| Leg | Mac M1, Wi-Fi | N95, Wi-Fi | UM790Pro, Ethernet |
|---|---|---|---|
| **Click → flag** | **63.8** (mean 69.8) | **91.9** (99.1) | **36.8** (38.6) |
| up | 2.7 | 3.4 | 1.8 |
| injected → flag raised | 11.0 | 10.3 | 10.4 |
| raised → capture showing it | 4.3 | 8.3 | 4.7 |
| encode | 4.0 | 3.7 | 4.2 |
| sender queue | 0.1 | 0.2 | 0.1 |
| **net** (into DataChannel → arrived) | **26.6** | **34.2** | **6.9** |
| … of which above srtt/2 (`inSctp`) | 23.2 | 24.6 | 4.5 |
| SCTP srtt | 6 | 16 | 4 |
| decode | 5.5 | 3.7 | 5.0 |
| draw | 7.3 | 16.0 | 3.4 |

- The flag's frame waits inside usrsctp before leaving. The Mac's surplus over
  Ethernet (+27 ms) is +20 in `net`, of which +19 before leaving usrsctp; the
  air (srtt/2) takes +1.
- 650-760 "losses inside a recovery" a pass on the Mac, 120-130 on the N95, 0
  on Ethernet. `bufferedAmount` was empty for the flag's frame in 82 % of Mac
  clicks: the wait is in usrsctp's buffer, invisible to the host.
- Keyframe gate: 0.9 % of Mac clicks (143 ms each). Head-of-line blocking
  behind a retransmitted chunk: 9 % of Mac clicks, +8 ms of `net`, ~1 ms on
  average.
- **The radio alone loses nothing**: UDP bursts host → Mac, 45 Mbit/s at 120
  then 240 packets/s, 20 s each, no stream: 185,000 datagrams, 0 lost, 0
  reordered.
- `sctps_slowpath_sack` is useless for counting gap SACKs: with PR-SCTP almost
  every SACK takes the slow path. "Sends held by the window" are high
  everywhere, Ethernet included (28-31 K a pass).

### 03/10/2026 — W1 bis: the "losses" are the browser's full UDP socket (14:40-14:52)

Mac's kernel counter "dropped due to full socket buffers", read around each
pass; UDP bursts host → a sink on the Mac, 45 Mbit/s in packets of 120
frames/s, 15 s:

| Run | Kernel drops | SCTP retransmissions | Wait in usrsctp | Click | Messages (p90) |
|---|---|---|---|---|---|
| UDP burst, sink buffer 4 MB | 0 (1 lost of 70,200) | — | — | — | — |
| UDP burst, 256 KB | 0 (1 lost) | — | — | — | — |
| UDP burst, 64 KB | **1,073** (1.5 %) | — | — | — | — |
| Stream, page at 240, Auto (~35 Mbit/s) | **1,855** | 3,120 (0.60 %) | 27.7 ms | 74.0 ms | 53 (444) |
| Stream, game, Auto (~41 Mbit/s) | **2,303** | 3,960 (0.66 %) | 20.5 ms | 58.6 ms | 25 (338) |
| Stream, page at 240, **20 Mbit/s fixed** | **213** | 279 (0.15 %) | 9.1 ms | **48.5 ms** | 16.5 (18) |

- What SCTP repairs on the Mac's Wi-Fi is, for the most part, datagrams the
  Mac's kernel throws away because the receive buffer of Chrome's UDP socket
  overflows. Wi-Fi delivers in aggregates; a 64 KB buffer reproduces it, 256 KB
  does not.
- Each drop cuts usrsctp's window, and the frames wait. At a lower bitrate the
  drops fall ninefold, the wait in usrsctp from 28 to 9 ms, the click from 74
  to 48.5 ms.
- N95 (Windows client): "received errors" for UDP at 0 since boot, IPv4 and
  IPv6. Whether Windows counts a full socket there is unknown: inconclusive.
- Not investigated: Chrome's own counters (`webrtc-internals`), whether Chrome
  sets SO_RCVBUF on its SCTP socket and to what.

### 03/10/2026 — W2 A: pacing the host's sends (failed, kept as a bench key)

`pace=<n>` (`b2b52486`, `SendPacer.h`): a frame's chunks handed to SCTP at n
times the stream's bitrate, 16 KB at a time, with a high-resolution waitable
timer. Mac in Wi-Fi, two rounds:

| | Click | Kernel drops | Retr. | Frame age med / p90 | Dropped /min | `bufferedAmount` > 1 frame |
|---|---|---|---|---|---|---|
| no pacing | 74.9 ms | 2,115 | 0.74 % | 20.6 / 51 | 110 | 6.0 % |
| `pace=4` | 85.0 ms | 2,540 | 0.79 % | 23.8 / 60 | 75 | 6.8 % |
| `pace=2` | 79.7 ms | 2,065 | 0.70 % | 15.5 / 24 | 48 | 2.8 % |

- **Why it failed**: pacing does not bring the drops down. Chrome's socket
  overflows when Chrome reads late (at 42 Mbit/s a 64 KB buffer holds
  ~12 ms), not under the host's bursts. Only a lower bitrate did.
- N95: no measurable effect (0.40/0.33 % retransmissions without, 0.39/0.24 %
  with). Local witness: +0.6-0.8 ms of send time.
- `pace=2` halved the p90 of a frame's age with no click gain; combined later
  with B it added nothing.

### 03/10/2026 — W2 B: the rate follows SCTP's retransmissions (kept: the Windows default)

`retrcut=<‰>` (`e0324f4e`): the relay adds to each link report the share of
SCTP chunks sent again since the previous one (`LinkFeedback::retransPermille`);
at the threshold or above the governor cuts 20 %, and calls the link quiet
only under half of it. Why it was needed: the governor's delay rise is a
minimum over its window, and on the Mac a few frames always arrive fast while
the rest wait ~20 ms in usrsctp — the rise never reached 30 ms.

N95, Wi-Fi (18:03-18:32, two rounds): click 109.2 → 100.2 ms (p90 177 → 144),
`net` of the flag 40.7 → 34.8, all frames' way down 27.8 → 22.0 ms, received
11.0 → 10.6 Mbit/s, retransmissions 0.31 → 0.32 %.

Mac, Wi-Fi (22:29-22:47, two rounds):

| | Click (p90) | Kernel drops | Retr. | Frame age med / p90 | Messages p90 | Dropped /min | Received |
|---|---|---|---|---|---|---|---|
| without | 73.6 ms (112) | 1,016 | 0.62 % | 20.3 / 86 | ~330 ms | 182 | 23.1 Mbit/s |
| `retrcut=3` | **62.6 ms (86)** | **366** | 0.27 % | 14.8 / **29** | **35 ms** | **34** | 20.0 Mbit/s |
| `retrcut=3,pace=2` (1 pass) | 61.5 ms (86) | 513 | 0.34 % | 15.7 / 72 | 37 ms | 35 | 17.5 Mbit/s |

The governor cut 5-10 times a pass and came back 2-6 times to the last good
rate: it held ~20-30 Mbit/s of the 60 set. ~18 ms remained in usrsctp
(`inSctp` 17.9 against 4.5 on Ethernet).

Ethernet witness (UM790Pro, 23:25-23:46, two rounds): click 37.1 / 42.4 ms
without, 41.3 / 36.7 with (means 39.8 / 39.0). SCTP sent 0-8 chunks again in a
whole session. One cut: all 8 retransmissions of a session fell in one 500 ms
report window (at 30 Mbit/s, 3 ‰ is ~5 chunks), 40 → 32 Mbit/s for 3.5 s.
Judged acceptable; a floor in chunks would not have filtered it (8), "two
windows in a row" would, but would change what was measured on the Mac.

**Decision (Bruno, 03/10)**: the Windows host's default, `55dd9cde`
(`RateGovernor::kRetransCutPermille` = 3; `retrcut=0` turns it off). Linux and
macOS hosts unchanged; GameStream relays have no governor.

### 03/10/2026 — usrsctp's send buffer has always been 256 KiB

Read in libdatachannel (`src/impl/sctptransport.cpp`, the `SctpTransport`
constructor): after reading the `sctp_sendspace` sysctl, it raises
`SO_SNDBUF` to its largest message, `Configuration::maxMessageSize`
(`DEFAULT_LOCAL_MAX_MESSAGE_SIZE`, 256 KiB unset). usrsctp's `SO_SNDBUF`
setsockopt is `sbreserve(&so->so_snd, …)` (`user_socket.c`). So every session
has had **256 KiB, whatever the bitrate**: `SendBacklog::sendBufferBytesFor`
(100 ms of bitrate, 64-256 KiB, `fe359154`, 17/09) never took. At 20 Mbit/s
that hides ~100 ms, at 2 Mbit/s a second. The comment in `applySctpSettings`
says so since `dc7f9c72`; the product is unchanged. The only way down is a
smaller `maxMessageSize` on the peer connection, which also caps what either
side may send in one message (both ways: the clipboard can reach 256 KB).

usrsctp's send buffer holds both data not yet sent and data sent but not yet
acked (`total_output_queue_size` against `SCTP_SB_LIMIT_SND`), so it caps the
data in flight too.

### 03/10-04/10/2026 — W2 C: a small usrsctp buffer and the picture held (failed, kept as bench keys)

`sctpbuf=<KB>` (`dc7f9c72`): the real usrsctp buffer, through
`maxMessageSize`. `linkhold=<ms>` (`a511bdd3`; Windows host): the capture loop
holds its picture unencoded once the video channel's `bufferedAmount` has
stayed above zero that long, and sends the freshest once it drained — the
`cadence=host-guarded` hold, asked of the relay through
`Session::setLinkBusyProbe`. No frame thrown away, so no keyframe.

Ethernet (UM790Pro, 23:32-23:54):
- First version, holding at the first byte (`linkhold=1` as a switch), with
  `sctpbuf=48`: **87 fps instead of 223**. 44 % of frames overflowed usrsctp
  for a millisecond or two; held frames came out bigger (34 KB median against
  14) and overflowed again. Throughput unchanged (29 Mbit/s).
- Holding after a 4 ms backlog: 233 fps at 48 KiB, 225 at 32; frame age
  11.2 / 10.8 ms (unchanged); `net` 7.8 / 7.3 ms (unchanged).

Mac, Wi-Fi (04/10 23:56-00:17, two rounds, B already the default):

| | Click (p90) | Frame age med / p90 | fps | Repeats /min | Kernel drops | Retr. | Received | Held /s |
|---|---|---|---|---|---|---|---|---|
| B alone | **63.6 ms (78)** | 15.1 / 26 | 120 | 1,141 | 472 | 0.36 % | 21 Mbit/s | — |
| `sctpbuf=48,linkhold=4` | 83.1 ms (134) | 25.7 / 79 | 31 | 4,524 | 0 | 0 | 25 Mbit/s | 107 |
| `sctpbuf=32,linkhold=4` | 77.7 ms (141) | 30.8 / 81 | 31 | 4,765 | 0 | 0 | 17 Mbit/s | 78 |

- **Why it failed**: the ~18 ms "inside usrsctp" is not a queue of frames that
  could be cut. A byte stays ~16 ms in usrsctp's buffer between hand-over and
  ack on this Wi-Fi, even with no loss at all, so the buffer caps the
  throughput at about buffer / 16 ms (48 KiB → ~25 Mbit/s, 32 KiB →
  ~17 Mbit/s). With no loss B never cuts, the encoder aims at 30+ Mbit/s, the
  excess spills into `bufferedAmount` (up to 560 KB) and the hold turns it
  into lost frame rate. `inSctp` rose to 25-26 ms instead of falling.
- **0 kernel drops** at 48 and 32 KiB: with that little in flight, Chrome's
  64 KB socket never overflows. Confirms W1 bis (the drops come from what is in
  flight when Chrome reads late), at too high a price.

### 03/10/2026 — Ultra LAN POC (U0.2, U0.3): usrsctp holds the video at 120 fps

From the POC session (`docs/design/ultra-lan-poc.md` §6). Host DualRTX;
D = Auto with detection (the product), U = 120 fps asked, virtual display at
240 Hz, tearing. `relaylog=1` + `flagpath.py`; p90 of `inSctp` over every frame
of the clicks' minute:

| Client | Link | D | U (120 fps) | Ref. |
|---|---|---|---|---|
| N95 | Wi-Fi | 35-54 ms | 186-232 ms (`net` p90 204-263) | POC §6.3, `8fb7c380` |
| UM790Pro | Ethernet 1 GbE | 5-12 ms | 5-12 ms (`net` p90 7-15) | `0ac463e7` |
| Mac M1 | Wi-Fi | 50-67 ms | 55-79 ms (`net` p90 57-84) | `923c7c1b` |

- N95 in U: the air (half the SRTT) takes 24-28 ms at p90, no SCTP
  retransmission, the decoder adds ~15 ms (2-3 → 18-22 ms on the flag's
  frame). The congestion window holds the video at 120 fps, as in W1. Effects
  (20 passes, 15:57-16:50, `94990337`): link cuts of 3-16 s, rate down from 7
  to 5 Mbit/s, 41-82 fps delivered, shown age 130-320 ms (D: 45-58), more than
  one click in two without a flag (D: 83-94 ms). The detection in D tried
  116 fps and went back to 58 (decode queue filling, capture → paint
  > 189 ms). AV1 in U: 0.9-1.1 s decode, cuts up to 31 s.
- Mac holds 120 fps without dropping out: cuts frequent but short (7-14 a
  pass, longest 1.2-3.5 s), click 61-75 ms in D and U alike.
- UM790Pro on Ethernet: no cut, 24-47 Mbit/s; the detection climbs to
  230-240 fps and stays.
- iPhone (Safari, Wi-Fi, 21:26-21:37, `3b04ba8b`): network 4.5-17 ms (40 once,
  during a 368 ms freeze), 1-2 freezes of 0.33-0.73 s a pass. **"Frames
  dropped (jitter)" 38-47 % in both modes while "frames lost (network)" is
  0.00 %** — maybe the counter under Safari: to check. On iOS the `--dev` at
  its LAN address fails: Safari does not extend the page's certificate
  exception to the signalling WebSocket.
- A pass's end-of-pass "Measured" line read 42 ms while the pass median was
  315 ms: it is a sliding window. Use the per-frame log for pass statistics.

### 04/10/2026 — W2.5: usrsctp's max burst (a clear gain on the Mac)

`sctpburst=<n>` (`01717368`): usrsctp's `sctp_max_burst_default` sysctl, set
with libdatachannel's settings before the peer connection; libdatachannel's
own is 10 packets. Mac, Wi-Fi, 05:34-05:55, two alternated rounds, B already
the default (`build\` `01717368`). `inSctp` here is over every frame of the
clicks' minute (`inSAll`), the click's leg over the flags' frames:

| | Click (p90) | `net` of the flag | `inSctp` all frames | Frame age med / p90 | Repeats /min | Messages med / p90 | Kernel drops | Retr. | Trimmed to max burst |
|---|---|---|---|---|---|---|---|---|---|
| base (10) | 66.9 ms (98) | 20.5 ms | 15.5 ms | 14.7 / 25.4 | 1,169 | 24.5 / 37.3 | 422 | 0.31 % | 7,156-7,494 |
| `sctpburst=0` (no limit) | **58.5 ms (69)** | **14.2 ms** | **9.4 ms** | 12.2 / 17.5 | **417** | 23.6 / **25.4** | 547 | 0.42 % | 0 |
| `sctpburst=32` | 60.6 ms (76) | 20.6 ms | 11.8 ms | 13.4 / 21.5 | 780 | 24.4 / 35.7 | 409 | 0.29 % | 3,414-3,880 |

- **The max burst was a third of what remained in usrsctp.** With no limit,
  each frame spends ~6 ms less inside usrsctp, the click gains 8 ms at the
  median and ~30 ms at p90, the repeats fall to a third.
- "Sends held by the window" fell from ~19-21 K to ~3-4 K a pass: most of what
  that counter showed was the burst limit, not the congestion window.
- Kernel drops rose a little (422 → 547 on average, one pass at 639), and the
  retransmissions with them (0.31 → 0.42 %): a bigger burst lands on Chrome's
  socket at once. B absorbs it.
- Still ~9 ms in usrsctp against ~4.5 on Ethernet (W1): the SACK clock.

N95, Wi-Fi (10:01-10:30, two alternated rounds, same build): **no gain**.

| | Click (p90) | `net` / `inSctp` of the flag | `inSctp` all frames | Frame age med / p90 | Messages med | T3 |
|---|---|---|---|---|---|---|
| base (10) | 80.0 / 87.0 ms (146 / 152) | 22.4 / 14.8 ms | 12.0 ms | 24.6 / 86, 31.6 / 202 | 26.4 ms | 0, 1 |
| `sctpburst=0` | 80.2 / 95.5 ms (125 / 166) | 26.5 / 19.4 ms | 9.6 / 14.3 ms | 22.6 / 187, 19.9 / 76 | 17.6 ms | 1, 3 |

- At ~6 Mbit/s and 60 fps an N95 frame is ~10 packets, so a burst of 10
  rarely holds it back; the Mac's gain (120 fps, ~20 Mbit/s) does not carry
  over. The click moves within the N95's pass-to-pass noise (80-95 ms); the
  host's messages get faster (26 → 18 ms median).

Ethernet witness (UM790Pro, 10:44-11:05, two alternated rounds, same build,
with `sctpss=4` as a third arm): **no regression, a gain**.

| | Click | Frame age med / p90 | `inSctp` flag / all frames | fps | Repeats /min | Messages med / p90 | Sends held by the window |
|---|---|---|---|---|---|---|---|
| base (10) | 38.7 / 37.7 ms | 14.6 / 26.0, 11.4 / 16.2 | 4.1-4.8 / 2.8-3.3 ms | 120, 239 | 2,473, 462 | 5.6 / 14.5, 4.3 / 10.1 | ~29-30 K |
| `sctpburst=0` | 34.4 / 39.0 ms | 8.1 / 10.6, 8.7 / 10.8 | 2.1-2.7 / 0-0.7 ms | 239, 239 | 141, 198 | 3.3 / 6.3, 3.6 / 6.8 | 26 |
| `sctpss=4` | 36.8 / 38.7 ms | 9.9 / 13.5, 11.4 / 17.5 | 4.5-5.1 / 2.5-2.8 ms | 239, 239 | 621, 597 | 3.6 / 7.1, 4.5 / 11.6 | ~27 K |

- Even on Ethernet the burst limit cost 2-3 ms a frame inside usrsctp; with
  no limit almost nothing waits there. The first base pass stayed at 120 fps
  (Auto's own pick); the others ran at 239.
- **Decision (Bruno, 04/10 ~10:45): no max burst is the Windows native
  host's default, `2ef56bfe`** (`DataChannelRelay::kNativeSctpMaxBurst` = 0;
  `sctpburst=10` restores libdatachannel's). Native sessions now always hand
  the relay their link settings; GameStream relays and the Linux and macOS
  hosts keep 10.

### 04/10/2026 — W2.3: usrsctp's stream scheduler (no effect; one module breaks the association)

The host's small messages on the input channel (pongs, cursor, rumble) took
~25 ms round trip at the median and 34-53 at p90 on the Mac with B, against a
7 ms bare UDP ping. libdatachannel exposes no per-stream priority and keeps
the socket `SCTP_SS_VALUE` would need; the scheduler's module, though, is a
sysctl (`sctp_default_ss_module`) the socket copies when it is made.
`sctpss=<0..5>` (`0c21bd5a`) sets it right after the peer connection (whose
`usrsctp_init` resets the sysctls) and before the SCTP transport is made.
Mac, Wi-Fi, 06:00-06:16, two alternated rounds, B the default:

| | Click | Message round trip med / p90 |
|---|---|---|
| base (module 0) | 67.3 / 61.4 ms | 25.3 / 52.6, 24.5 / 33.9 |
| `sctpss=4` fair bandwidth (shortest pending message first) | 66.0 / 61.2 ms | 24.9 / 49.9, 24.6 / 38.9 |
| `sctpss=2` round robin by packet | — | — |

- **Fair bandwidth changes nothing**: the log confirms it was applied, and the
  messages kept their times. They do not wait in usrsctp's stream queues; the
  wait is elsewhere (in flight behind the video, the SACK clock). What did help
  them was W2.5's burst (p90 37 → 25 ms).
- **Round robin by packet breaks the association**, both rounds: the video
  channel opened, then the peer connection failed ~8 s later, with 98 and 360
  of ~5,200 chunks retransmitted and 1 T3 each. Never use module 2 against
  Chrome. Not investigated further (a guess: fragments of different streams'
  messages interleaved without I-DATA, which Chrome's dcsctp cannot
  reassemble).

### 04/10/2026 — POC Ultra: the iPad under RE9 (from session 9b)

Bruno's iPad, Safari, Wi-Fi, streaming the RTX's screen (NVENC) under RE9,
19:17-19:36; the `--dev` `ded56fc6`, joined through stream.dev, so with
`retrcut=3` and `sctpburst=0` as defaults (`24509762`, POC doc §6.3).
- Network leg of the latency detail: 13.8 ms (50 fps), 19.5 ms (62 fps),
  2.3 ms (26 fps). "Link queue": 11, 8, 3 ms. "Link freezes": 2 of 0.91 s,
  2 of 0.51 s, then none.
- **"Frames dropped (jitter)" 44-45 % at 50-62 fps against 2.7 % at 26 fps,
  with "Frames lost (network)" at 0.00 % everywhere** (iPhone, 03/10:
  38-47 %). The counter follows the received rate and the decoder (16-22 ms a
  frame at 50-62 fps, 8 ms at 26), not a loss on the link. To check in the
  counter's code.
- Safari on the iPad reported a refresh of 32 to 51 Hz with Low Power Mode
  off; Auto followed it (26 fps in pass 3) while the game presented ~65 fps.

### 04/10/2026 — B and W2.5 on the Linux native host (UM790Pro)

Host: the UM790Pro under Ubuntu 24.04, X11 session (the click's flag is X11
only on Linux), DEV `6ab77b3f` LAN only, KMS capture 1920×1080 at 60 Hz, the
bench page on its screen. Driven from DualRTX by `series.py --host um790pro`
(`8fd33518`, `1231e818`): the band is not read on a remote host. Two
alternated rounds per arm; Linux's own defaults: no retrcut, burst 10.

> **Correction (04/10/2026, 23:45): no client in this section was on Wi-Fi.**
> With `--host`, `pass.py` drove a kiosk of its own on DualRTX instead of the
> client `series.py` named (fixed in `09df1c3c`); the host's logs show
> 192.168.1.66 as the peer. The rows first labelled "N95, Wi-Fi" are DualRTX's
> Chrome decoding on the RTX 5060 Ti (`DISPLAY5`), the rows labelled
> "DualRTX Chrome, wired" the same machine decoding on its AMD iGPU
> (`DISPLAY9`). Both are Ethernet. The gains below are real for a wired
> client; the Linux host has **not** been measured with a Wi-Fi client.

Without clicks (19:45-20:36; the `input` group was not yet granted):

| Client | Arm | Frame age med / p90 | `inSctp` all frames | Messages med | Retr. |
|---|---|---|---|---|---|
| DualRTX, RTX (first: "N95") | base | 18.4 / 23.9, 18.8 / 23.3 | 9.3 / 9.7 ms | 4.4-5.0 | 0-0.02 % |
| | `retrcut=3` | 17.8 / 22.3, 18.7 / 23.0 | 9.1 / 9.8 ms | 4.4-5.0 | 0 % |
| | `sctpburst=0` | 13.3 / 17.6, 11.9 / 15.8 | 2.9 / 2.5 ms | 3.2 | 0.03-0.12 % |
| | both | 13.1 / 17.6, 13.2 / 17.4 | 3.1 / 3.2 ms | 3.3 | 0.01-0.13 % |
| DualRTX, AMD iGPU | base | 17.1 / 21.4, 21.0 / 35.5 | | 4.2, 8.3 | 0-0.01 % |
| | `retrcut=3` | 17.3 / 21.2, 21.6 / 37.9 | | 5.2, 8.7 | 0 % |
| | `sctpburst=0` | 11.9 / 18.1, 13.3 / 19.0 | | 3.3, 7.4 | 0-0.03 % |
| | both | 12.5 / 17.8, 12.9 / 18.5 | | 3.8, 7.5 | 0-0.01 % |

With clicks (20:38-21:01; bruno added to `input` for the series, removed after):

| Client | Arm | Click (p90) | `net` / `inSctp` of the flag | Frame age med |
|---|---|---|---|---|
| DualRTX, RTX (first: "N95") | base | 63.6 / 65.6 ms (86 / 92) | 13.9 / 11.0 ms | 20.6 / 21.0 |
| | `sctpburst=0` | 58.7 / 58.8 ms (80 / 81) | 6.1 / 2.6 ms | 14.4 / 14.7 |
| DualRTX, AMD iGPU | base | 86.5 / 87.2 ms (126 / 107) | 20.3 / 17.1 ms | 20.9 / 21.3 |
| | `sctpburst=0` | 78.4 / 78.4 ms (111 / 101) | 10.9 / 7.9 ms | 13.5 / 12.8 |

- **`sctpburst=0` gains on the Linux host too, for a wired client**: ~6.5 ms
  less inside usrsctp per frame, a frame ~6 ms younger, the click 6-8 ms
  faster. A few more retransmissions on the RTX client (to ~0.1 %) and, in
  one set, more frames dropped (71 → 128 a minute).
- **`retrcut=3` changes nothing there**: a wired link barely retransmits
  (0-0.1 %), unlike the Windows host to the Mac's Wi-Fi. Untested on Wi-Fi.
- On this host ~37 ms pass between the flag going up and the capture that
  shows it (`toCap`: KMS at 60 Hz plus the X11 flag window), against 3-4 ms on
  the Windows host: the largest share of a Linux click, and not the network.
- DualRTX reaches the UM790Pro through its Hyper-V switch (2-3 ms with
  spikes to 25 ms). Its AMD-iGPU client clicks ~23 ms slower than its RTX
  one (the screen and decoder behind each, not the link), so each counts
  only against its own base.
- **Decision (Bruno, 04/10 ~23:20): no max burst is the Linux native host's
  default too, `6a833826`** (`kNativeSctpMaxBurst` = 0 on Windows and Linux);
  `retrcut` stays off on Linux.

### 04/10/2026 — B and W2.5 on the macOS native host: started, not finished

Host: the Mac M1 Pro (Wi-Fi), DEV `0.3.1.g5de-dev` from the CI (`5de1e2af`),
ScreenCaptureKit, the bench page on its screen. The client was meant to be
the N95 in Wi-Fi; it was in fact DualRTX's Chrome on Ethernet, decoding on the
RTX (same bench-driver fault as the Linux section above, fixed in
`09df1c3c`), so only the host was on Wi-Fi. Two passes only before the screen
time Bruno granted ran out:

| Arm | Click (p90) | Frame age med / p90 | fps | Received | Dropped /min | Messages med |
|---|---|---|---|---|---|---|
| base | 70.5 ms (127) | 19.4 / 56.5 | 96 | 10.8 Mbit/s | 238 | 38.4 |
| `retrcut=3` | no click seen | 39.2 / 63.1 | 106 | 22.5 Mbit/s | 426 | 34.6 |

- In the `retrcut=3` pass the host saw and showed all 60 flags (its log) but
  the client found none: the flag sits at the bottom of the Mac's screen
  (792,1111 of 1800×1169), probably under the kiosk page that pass. Not
  understood yet.
- The following passes failed on the bench driver, not the host: it clicked
  a button labelled "Unlock", and the bench Chrome is in French
  ("Déverrouiller"); fixed in `d1900763`.
- Second slot (04/10, 23:27-23:39): one base pass (frame age 38.1 ms median,
  p90 45.8; the client again saw none of the 60 flags), then two passes found
  no host card on the client's page and the series was stopped once the
  driver fault came to light. Still to do: all four arms, two rounds, with
  the N95 really driven.


### 04/10/2026 — POC Ultra U0.4: Steam Remote Play, and the browser's present path (session ex-3b)

Steam Remote Play (client beta), host DualRTX streaming its primary screen,
the RTX's (NVENC for HEVC); client the UM790Pro under Windows on 1 GbE (780M
hardware decode, 1920×1080, automatic bitrate, quality modifier middle,
4:4:4 off), 22:10-23:28 (`e93407a7`, `9e1b8899`, POC doc §6.5). Method,
no camera (`scripts/bench/photon/`, `2e0a012b`): the host shows a full-screen
window that flips black/white on each mouse press, streamed as a non-Steam
game. The client injects a click at the stream window's centre and reads that
pixel back from its own composed desktop (GDI) until it flips. The sample
covers everything from the click's way up to the client's DWM composition,
leaving out the panel's scan-out. 60 clicks per pass, none missed; raw samples
in `bench-out/photon/*.json`.

| Pass | Codec | Low Latency Networking | Median | p90 | Min - max |
|---|---|---|---|---|---|
| 1 | HEVC | off | 49.9 ms | 58.9 ms | 32.6 - 67.2 |
| 2 | PyroWave | off | 42.7 ms | 59.0 ms | 32.7 - 66.6 |
| 3 | HEVC | off | 57.9 ms | 66.6 ms | 40.6 - 91.7 |
| 4 | PyroWave | off | 42.6 ms | 58.3 ms | 32.4 - 83.9 |
| 5 | HEVC | on | 58.6 ms | 83.2 ms | 41.3 - 375.8 |
| 6 | PyroWave | on | 49.7 ms | 59.1 ms | 32.9 - 92.3 |

- **Steam's "Low Latency Networking" makes both codecs worse** on 1 GbE:
  PyroWave +7 ms at the median; HEVC no better at the median, with a p90 of
  83 ms and one 376 ms outlier.
- PyroWave is steady from pass to pass (42.6-42.7 ms); Steam's HEVC moves by
  8 ms (49.9 → 57.9).
- **MoonlightWeb on the same pair, same tool** (23:22, `--dev` `ded56fc6`,
  the RTX screen in Auto, 120 fps, tearing, Chrome; defaults `retrcut=3`,
  `sctpburst=0`): median 58.2 ms, p90 75.0 ms (42-108), 60 of 60. A second pass
  is void (58 of 60 missed): most likely the bench's `latency_flag_enabled`
  overlay drew over the pixel read.
- **Correction (05/10, 00:10)**: a bench Chrome of the Wi-Fi session ran on
  DualRTX, on the RTX and DISPLAY5 (the streamed screen), from 22:40 to 23:00
  and from 23:27 (its `pass.py --host` fault, fixed in `09df1c3c`). Pass 1
  (HEVC, ended 22:56) is tainted by it; every other pass ran after 23:00. On
  the clean passes only: PyroWave 42.6-42.7 ms against HEVC 57.9 (58.6 with
  Low Latency Networking), 15 ms. MoonlightWeb's void second pass
  (23:26:40-23:27:41) met that Chrome's restart on the streamed screen, the
  likeliest cause.
- **The browser's present path**: MoonlightWeb's own click → flag on this pair
  read 33-43 ms (U0.3, the flag read in the canvas at draw). Read on the
  composed desktop it is 58 ms. The 15-25 ms between them are Chrome's
  compositing and the DWM's, a leg the network and the codec do not touch and
  a native client such as Steam does not have.

### 05/10/2026 — B and W2.5 on the Linux and macOS hosts, with the N95 really in Wi-Fi

Redone after the driver fault (`09df1c3c`); each pass's host log shows the
N95 (192.168.1.168) as the peer. Every arm explicit (`retrcut=0|3`,
`sctpburst=10|0`), two alternated rounds, `relaylog=1`. The N95 runs
~4-7 Mbit/s at ~50 fps, its link noisy: second rounds degraded on both hosts.

**Linux host** (UM790Pro, DEV `0.3.1.av1b-dev`, whose own default is already
burst 0).

Without clicks, GNOME Wayland (05:30-05:48; the `input` group and X11 need a
`sudo` the session was not yet allowed):

| Arm | Frame age med (p90) | Way down med | Repeats /min | Dropped /min |
|---|---|---|---|---|
| burst 10 | 39.4 / 39.4 (93 / 69) | 34.3 / 33.0 | 108 / 306 | 146 / 240 |
| `retrcut=3` | 37.0 / 40.4 (119 / 105) | 31.6 / 34.8 | 180 / 188 | 92 / 68 |
| `sctpburst=0` | 32.9 / 33.0 (60 / 85) | 27.6 / 28.5 | 593 / 797 | 264 / 262 |
| both | 35.0 / 33.9 (66 / 115) | 28.6 / 28.7 | 506 / 205 | 172 / 270 |

With clicks, GNOME Xorg and the `input` group (06:03-06:31, Bruno's two
exact permission rules, `um-x11-on.sh` / `um-x11-off.sh`). First round (the
second's link was worse: all frames' `net` 93-211 ms against 30-44):

| Arm | Click (p90) | `net` of the flag | `inSctp` flag / all frames |
|---|---|---|---|
| burst 10 | 138.7 (192) | 38.7 | 20.4 / 14.0 |
| `retrcut=3` | 158.0 (216) | 48.6 | 31.7 / 22.7 |
| `sctpburst=0` | 155.4 (184) | 41.8 | 20.3 / 13.1 |
| both | 144.2 (192) | 38.5 | 20.3 / 11.7 |

- The click's legs: up 18-28 ms, `toCap` ~38 (KMS at 60 Hz + the X11 flag
  window), encode ~5, `net` 38-56, decode on the N95 16-38, draw ~19.
  usrsctp is a small share; no arm moves the click outside the noise.
- Without clicks, no max burst gives a frame ~6 ms younger, with more repeats
  and drops. The default (`6a833826`) stays.

**macOS host** (the Mac M1 Pro, DEV `0.3.1.g5de-dev`, 06:43-07:13). Its bench
page now opens in a screen-sized window, not a kiosk (`ba3a7df7`). A
full-screen Chrome went to a Space of its own, and macOS blacks out the band
around the notch where the flag sits. The N95 read 0,0,0 there and every click
timed out. Before that, the night of 04/10, the session was locked: the
capture showed only the lock screen (a flat 47,90,148, ~1 Mbit/s).

| Arm | Click r1 / r2 (p90) | `net` of the flag | `inSctp` flag | `inSctp` all frames |
|---|---|---|---|---|
| burst 10 | 137.0 (175) / 129.0 (195) | 53.2 / 38.1 | 39.8 / 15.0 | 21.9 / 40.7 |
| `retrcut=3` | 140.5 (169) / failed | 52.3 | 39.4 | 21.1 |
| `sctpburst=0` | 141.3 (190) / 125.5 (182) | 37.3 / 37.9 | 24.0 / 19.9 | 13.1 / 12.5 |
| both | 125.4 (179) / 129.3 (168) | 37.6 / 39.1 | 21.2 / 24.4 | 10.1 / 9.8 |

(Round 2's burst-10 pass had a degraded link: frame age 146 ms, 2.7 Mbit/s.
Its `retrcut=3` pass lost the N95's Chrome.)

- **No max burst halves the time a frame spends in usrsctp on the macOS host**
  (~22 → ~11 ms over all frames, the flag's `net` 53 → 38 ms), as on the
  Windows host towards the Mac. The click's median moves within the N95's
  noise (125-141 ms): decode and draw on the N95 weigh more.
- `retrcut=3` alone: nothing.
- **Decision (Bruno, 05/10 ~10:30): no max burst is the macOS host's default
  too, `6194297c`** (`kNativeSctpMaxBurst` = 0 on every native host);
  `retrcut` stays off on macOS.

### 05/10/2026 — POC Ultra U1.2: the DataChannel's ceiling at intra-codec rates (session ex-3b)

Host DualRTX (`--dev` `66f71c56`, Windows defaults `sctpburst=0`,
`retrcut=3`) streaming a physical screen (DISPLAY1, the Arc's, 120 Hz) with
the bench page scrolling; client the UM790Pro under Windows, Chrome, 1 GbE.
Method: `ultra=synthetic:<KiB>` (`e8f02ce0`) sends that many KiB of
incompressible chunks after every video frame, on channel id 5, same SCTP
association as the video; the client (`mw_ultra_sink`, `5ee304da`) counts
what lands. Medians of the last 30 s, POC doc §6.8 (`039ad899`).

| KiB / frame | Asked at 60 / 120 fps | Got at 60 fps | Got at 120 fps | Extra delay p95 (60 / 120) | Video fps (60 / 120) |
|---|---|---|---|---|---|
| 40 | 20 / 39 Mbit/s | 19.4 | 37.7 | 77 / 16 ms | 60 / 111 |
| 200 | 98 / 197 | 19.2 | 98.9 | 1427 / 397 ms | 10 / 59 |
| 350 | 172 / 344 | 62.4 | 103.3 | 818 / 717 ms | 12 / 34 |
| 500 | 246 / 492 | 105.7 | 95.1 | 605 / 1320 ms | 25 / 21 |
| 1000 | 492 / 983 | 105.2 | 95.7 | 1508 / 2685 ms | 11 / 13 |
| 2000 | 983 / 1966 | 106.7 | 97.0 | 3121 / 3961 ms | 6 / 5 |

- **A ceiling of 95-107 Mbit/s on 1 GbE**, whatever the frame size or rate;
  past it the wait grows to seconds and **the video falls with it** (5-25
  fps): one association, one congestion window, whatever sender queue each
  channel has.
- Variants at 120 fps: unordered without retransmission 90.7 (350 KiB) / 63.2
  (1000 KiB) Mbit/s with losses; `sctpburst=10` 97.0 / 15.4 Mbit/s;
  `sctpbuf=1024` 64.5 / 49.5 Mbit/s and the video no longer shown. None
  raises the ceiling; `sctpburst=0`, the Windows default, is the better one.
- An out-of-range key (`sctpbuf=4096`; the key takes 24-1024) voids the whole
  `MW_NATIVE_TUNING`: two passes lost, set aside.
- Open: 200 KiB at 60 fps got 19 Mbit/s, far less than 350 KiB (62); where the
  ceiling sits (host send thread and libjuice, Chrome's SCTP receive, usrsctp)
  is U1.3's question.

### 05/10/2026 — The bench's "1 GbE" path has a Wi-Fi 7 hop (session ex-3b)

DualRTX and the UM790Pro are each wired, at 1 Gbit/s, to a Freebox repeater,
but the two repeaters (ground floor, second floor) talk to each other over
**Wi-Fi 7**. DualRTX also goes through a Hyper-V virtual switch
("vEthernet (LAN)"). Measured with a socket probe (TCP and paced UDP,
server DualRTX, client the UM790Pro under Windows; ICMP is blocked on the
UM790Pro, so the round trip is a UDP echo):

| Test | Result |
|---|---|
| UDP round trip, idle (64 B every 10 ms) | p50 2.6 ms, p90 3.2, p99 5.4-15 (a cable gives ~0.3) |
| TCP down, 1 stream / 4 streams, 10 s | 74 / 90 Mbit/s, 36-124 per second |
| UDP down paced at 50 / 100 Mbit/s | no loss; extra one-way delay p99 2.2 / 5.1 ms |
| UDP down paced at 150 Mbit/s | no loss; p90 +16 ms, p99 +47 ms |
| UDP down paced at 200 Mbit/s | 175 received, 3.7 % lost, p50 +158 ms |
| UDP down paced at 300-800 Mbit/s | 152-157 received, 48-77 % lost, +620-660 ms |

- **The U1.2 ceiling of 95-107 Mbit/s comes first from this path**, not from
  the DataChannel: TCP itself gets 74-90 Mbit/s, and the hop carries ~150
  Mbit/s at best before it queues and drops. Every "1 GbE, Ethernet" pass
  between these two machines (U0, U1.2, U3, Steam U0.4) ran over it.
- Next: Bruno puts the two PCs on one cable (evening of 05/10); the probe is
  replayed there, then U1.2. Before that, RTP against SCTP is measured on
  this Wi-Fi path on purpose, for its losses (POC plan U1.4).

### 05/10/2026 — What that hop means for the Wi-Fi plan's "Ethernet" witnesses

Every "Ethernet" or "wired" row of the Wi-Fi plan between DualRTX and the
UM790Pro ran over the path above, not a cable end to end:
- W0, W1 (the UM790Pro under Windows as the client of the DualRTX host);
- the W2 B and W2.5 witnesses (the same);
- the Linux host's 04/10 section (DualRTX's Chrome as the client of the
  UM790Pro).

What they show still holds for what they were: **a clean link**. SCTP
retransmitted 0-8 chunks a session there, against 0.2-0.6 % on the Mac's and
the N95's Wi-Fi, and nothing regressed on it. The absolute figures carry the
hop: ~2.6 ms of round trip instead of ~0.3, and a few ms of queue when the
stream bursts.
- The W1 reading "~4.5 ms in usrsctp on Ethernet" is ~4.5 ms over this path:
  part of it is the hop's round trip, which paces the SACKs.
- W2.5's gain there (frame age 13.0 → 8.4 ms, usrsctp 2-3 ms → almost
  nothing) is a gain on a short, lossless path. A cable may leave less to
  gain.
- Worth replaying on the cable Bruno lays on the evening of 05/10: one round
  of base / `sctpburst=0`, UM790Pro client, to anchor the wired figures.

### 05/10/2026 — POC Ultra U1.4: the video on an RTP track, against SCTP (session ex-3b)

Same path (the Wi-Fi 7 hop above), on purpose. Host DualRTX (`--dev`,
`e399d604`): the video on a send-only RTP track when `MW_RTP_VIDEO` names the
codec. Client the UM790Pro under Windows, Chrome 154 (`b2f6d264`), which takes
each frame off the track with an `RTCRtpScriptTransform`, before its own
decoder, into the DataChannel's decode path. POC doc §6.10.

- **Idle, RTP costs 5-10 ms** of host stamp → drawn on every codec and both
  host types. Native HEVC: 18.3-18.7 ms against 8.8-11.6 ms for SCTP; H.264
  23.8-26.4 against 15.3-16.0; AV1 37.5-38.9 against 32.9. Sunshine host:
  content age +6 ms on all three.
- **It is Chrome's wait, not the host or the link.**
  - The host sends 4 ms after capture either way (`sendFrame` 0.37 ms).
  - The worker → page hop is 0.2 ms.
  - Between the frame's last packet (`receiveTime` in its metadata) and the
    transform, Chrome holds it 7.5 ms median, p90 14, at most 17 ms, uniform
    over a 60 Hz period: a receive-side tick.
  - Neither `jitterBufferTarget = 0` nor `--disable-features=WebRtcMetronome`
    changes it.
- **Under an Ultra load, RTP wins clearly.** With `ultra=synthetic:250`
  (~122 Mbit/s at 60 fps) next to HEVC video, both transports deliver it all,
  with no loss:
  - RTP: Ultra's extra wait 15 / 25-30 ms (p50 / p95), video 20-21 ms host →
    drawn.
  - SCTP: Ultra's extra wait 35-52 / 62-90 ms, video 42-65 ms, because the
    video waits behind Ultra in the one association.
- No loss was seen on this hop, either way: RTP under loss is still to measure
  (NACK, the keyframe path).
- Open: where Chrome's receive tick comes from, and whether a page can avoid
  it. Answered in the next entry.

### 05/10/2026 — POC Ultra U1.4 ter: Chrome's 64 Hz metronome on received video frames (session ex-3b)

Same path and machines. Commits `36f1a492` and `c83acc30`. HEVC native host,
idle unless said.

**The tick is a 64 Hz grid.**
- In the worker, the times frames reach the transform lock onto a 15.625 ms
  period: phase coherence 0.98. The times their last packet came do not:
  0.01-0.07.
- Gaps between deliveries are 15.7 / 31 ms. The hold is 7.8 ms median, at
  most 17-18 ms.
- On the same socket, the DataChannel's frames show no lock (0.01-0.05). The
  wait is in the RTP video receive path, not in the network service or a
  Chrome-wide timer.

**Its source in Chromium.**
- `VideoMetronomeWorker` in
  `third_party/blink/renderer/platform/peerconnection/rtc_encoded_video_stream_transformer.cc`
  queues each received video frame. It hands the queue to the transform on the
  next tick of the decode metronome: `TimerBasedTickProvider`, `kDefaultPeriod
  = base::Hertz(64)`, ticks snapped to a fixed grid.
- History:
  - landed in January 2024 ("Align Encoded Transforms for Receiver Video frames
    to a metronome", crbug 1502070, to wake JS less in large calls);
  - on by default since April 2024;
  - its kill switch `RTCAlignReceivedEncodedVideoTransforms` was removed in
    October 2025.
- No flag turns it off in M154.
- **Audio frames are not held:** the audio transformer posts each frame at
  once.

**What did not help.**

| Try | Result |
|---|---|
| The legacy `createEncodedStreams` on the page thread | Same hold. The hop drops to 0.0 ms, the wait is before it. |
| `--disable-features=AlignWakeUps,AddTaskLeeway` with `--enable-features=LowerHighResolutionTimerThreshold` | Same hold |
| `--enable-features=VSyncDecoding` | Worse: ~50 ms hold, ~10 Hz ticks for this window |

**What works: the frames cut in Opus packets** (the `rtp_video` item `aroad`,
bench only).
- Each packet is an 8-byte header ('M', key flag, frame seq, index, count)
  plus up to 1100 bytes, on a send-only audio track. The transform worker puts
  the frames back together.
- The hold drops to 0.2 ms.

Host stamp → drawn, p50, one pass per cell unless noted:

| Codec | Video track | Audio road | SCTP |
|---|---|---|---|
| HEVC | 17.3 / 18.1 ms | 9.1 / 9.3 / 10.1 ms | 9.6 / 9.8 ms |
| H.264 | 23.7 ms | 14.6 ms | 14.8 ms |
| AV1 | 30.9 ms | 31.1 ms | 21.1 ms |

- AV1: the encoder held only ~40 fps, so these are noisy. To redo.
- No frame lost idle.

**Under an Ultra load** (`ultra=synthetic:250`, ~122 Mbit/s, 14:10-14:20,
iPhone stopped):

| Video / Ultra | Video p50 | Ultra extra wait p50 / p95 |
|---|---|---|
| Video track / video track | 21.4 ms | 16.8 / 41.4 ms |
| Audio road / video track | 11.6 ms | 16.4 / 35.5 ms |
| Audio road / audio road | 11.1 ms | 7.2 / 22.3 ms |
| SCTP / SCTP | 123-129 ms | 141-155 / 185-200 ms |

- The audio road has no retransmission: Chrome sends no NACK for it. In the
  2-minute pass with both on the audio road, the host got 10 IDR requests.
  Next: a NACK of our own, carrying the missing chunk indexes on the input
  channel.

**The hop is shared with the house.** At 13:27-13:55, a household iPhone (weak
signal) streamed video, ~11 Mbit/s on average, in bursts. Meanwhile:
- Ultra on RTP fell to 78-109 Mbit/s with 6-12 losses/s, and SCTP's video
  fell to 113-225 ms.
- NetProbe's paced UDP stayed clean to ~200 Mbit/s with or without it (198 vs
  193 Mbit/s).
- Once it stopped, Ultra on RTP was back to this morning's level: 122.9 Mbit/s,
  0 loss.

A clean paced probe does not clear the link: compare a burst load with and
without the suspect.

### 05/10/2026 — POC Ultra U1.4 quater: the audio road's own NACK (session ex-3b)

Same path, iPhone stopped, 14:35-15:10. Commit `a95e95d9`.

**How it works.**
- The transform worker asks for a chunk again as soon as it sees a hole in a
  frame's indexes, or when a newer frame starts while an older one is still
  missing chunks.
- The page sends the request on the input channel (`aroadnack`). The host
  resends from a per-track history of the last 60 frames.
- A complete video frame waits at most 15 ms for an older one, then goes out
  marked lost (the keyframe path).
- Losses are injected on the host with `MW_AROAD_DROP` (per mille), on first
  sends only.

**Results, HEVC idle.** Host stamp → drawn:

| Case | p50 | p90 | Draws/s | Frames given up |
|---|---|---|---|---|
| Audio road, no loss | 9.1-9.3 ms | 13.2 ms | 59.4-59.7 | — |
| Audio road, 1 % chunks dropped (~1,200 resent) | 9.7 ms | 14.5 ms | 59.9 | 0 |
| Audio road, 5 % dropped (~4,500 asked again) | 11.8 ms | 16.3 ms | 59.4 | 0 |
| SCTP, `loss=50` (5 % of messages thrown before SCTP) | 8.4 ms | 11.2 ms | 38 | — |

- With `loss=50`, the client sent 153 recovery requests in 2 minutes.
- SCTP's p50 counts only the frames that came through whole.

**Under Ultra 250**, with both video and Ultra on the audio road:
- Ultra's chunks were repaired (~600 resent).
- A resent video chunk queues behind Ultra's 250 KiB bursts on the Wi-Fi.
  It comes back past 15 ms, so the host still got 11 IDR requests in 2
  minutes.
- On the product path, Ultra replaces the video instead of riding next to it.

**AV1 redone** (two rounds), host stamp → drawn p50. The 21 ms SCTP figure of
U1.4 ter was noise.

| Road | Round 1 | Round 2 |
|---|---|---|
| Audio road | 30.0 ms | 18.0 ms |
| SCTP | 32.9 ms | 31.7 ms |
| Video track | 39.9 ms | 38.8 ms |

### 05/10/2026 — the audio road would ride in the Wi-Fi voice queue off Windows (session ex-3b)

Found by the audio + DSCP planning session. No bench.

- **libdatachannel marks by track type.** `Track::transportSend`
  (`third_party/libdatachannel/src/impl/track.cpp:215-220`) sets DSCP 46 (EF)
  on every packet of an "audio" track and 36 (AF42) on every other track,
  following RFC 8837.
- **The audio road is an audio track.** On a host where the mark reaches the
  wire (Linux, macOS), all the video on `aroad` would leave as EF. An access
  point that follows RFC 8325 maps EF to the WMM voice queue. That queue does
  not aggregate frames, so 20-150 Mbit/s of video there would crush its rate
  and starve the real voice traffic of the house.
- **Windows hosts are not affected.** libjuice sets no DSCP there, so the
  U1.4 ter and quater benches on DualRTX are not touched.
- **To fix before `aroad` reaches the product off Windows.** The video and
  Ultra packets of the audio road should carry a video mark (AF4x), and only
  the real audio track EF. The DSCP plan decides the marks.

### 05/10/2026 — W4: SCTP, the RTP video track and the audio road to a client in Wi-Fi (N95)

Wi-Fi plan, W4. The first measurement of the three video roads against a
client in Wi-Fi.
- **Setup.**
  - Host: DualRTX `--dev`, Windows native, Arc A380 encoder, build `build\` of
    18:56.
  - Page: served from the repo with `74ec6f9f` in it.
  - Client: N95 in Wi-Fi, Chrome 154, page at 58-59 Hz.
  - Arms: `MW_RTP_VIDEO` empty (SCTP, B and `sctpburst=0` the defaults),
    `native:h264+hevc` (RTP video track), `native:h264+hevc+aroad` (audio
    road). Each in H.264 and HEVC, two rounds, 20:15-21:52.
  - Measured: the click → flag, and each frame's age (host stamp → drawn,
    `e2e`).
- **The "loaded" arm did not load.** A fixed `--bitrate 30000` caps the rate
  but does not raise it: the scroll page at 59 fps asked for 1.2-2.3 Mbit/s
  in H.264 and 4-22 in HEVC. The four passes of each arm below are therefore
  read together, as four light-link passes. A real load needs heavier content.
- **One pass was hit and redone.** `sctp-h264-idle-r1` met 7 TDRs of the RTX
  (20:27:53-20:28:52, another session's encoder trial); it was redone as
  `r1b`. The Arc encoder was not touched.
- **H.264, medians of four passes:**

  | Road | Click → flag | Frame age (`e2e`) |
  |---|---|---|
  | Audio road | ~68 ms (61-74) | ~20 ms |
  | SCTP | ~72 ms (68-77) | ~21 ms |
  | RTP video track | ~78 ms (67-83) | ~28 ms |

  - The RTP video track's ~7 ms is Chrome's receive metronome (U1.4 ter),
    seen here in Wi-Fi too.
  - The audio road is level with SCTP on a light link, a little ahead at the
    click.
  - All three draw 46-51 frames a second, with 55-58 clicks of 60 measured.
- **HEVC (the N95 decodes it 20-40 ms slower than H.264, on every road):**
  - **SCTP keeps its Wi-Fi tail.** Frame age median 25-48 ms but mean 34-71
    and p90 65-172. Retransmissions 0.15-1.3 %, the wait in usrsctp's window
    up to 3.6 s a pass. Click ~104 ms.
  - **The RTP video track has no tail.** Age median 35-41, p90 60-96. But the
    click is slowest, ~123 ms (one of four passes had no valid click).
  - **The audio road breaks.** Only ~7 frames a second are drawn and 10-19
    clicks of 60 measured, though the frames it does draw are young (median
    22-31 ms, p90 37-63). Each lost chunk the NACK cannot save makes the page
    give the frame up and ask for a keyframe. Since `74ec6f9f` this holds even
    on a host that heals by invalidation, because the audio road's frames do
    not carry the host's frame number. An HEVC keyframe is many Opus packets,
    and on this link one of them is lost again. The H.264 frames, a few
    packets each, get through.
- **What it means for the product (to confirm on the Mac):**
  - On a light Wi-Fi link, the RTP video track costs the metronome's ~7 ms for
    nothing.
  - The audio road equals SCTP in H.264.
  - In HEVC, the audio road is not usable until a lost frame is healed by
    deltas: its header must carry the host's frame number, as planned by the
    POC session.
  - SCTP's advantage is only that nothing is ever lost. Its cost is the tail.
- **The same evening, the audio road healed by the host's frame number**
  (`1489e114` page, `14f57e0e` host, build `build\` of 22:06). The aroad
  header grew from 8 to 12 bytes and now carries the host's wire `frameId`.
  A frame the page gives up on becomes a `frameId` gap, which StreamView names
  to the host (`invalidateref`) and decodes on, with no keyframe. Two HEVC
  passes, 22:07-22:18, give-up still 15 ms:
  - click 89.5 ms (p90 127-129), 59 of 60 clicks both times;
  - 58 frames a second;
  - frame age median 21-22 ms, mean 26-31, p90 34-58;
  - 8 invalidations and 5 keyframe requests a pass.

  The SCTP witness of the same hour (HEVC): click 100.9 ms (p90 158), 49 of 60
  clicks, frame age median 36 ms, mean 84, p90 232, 0.9 % retransmitted.
  **With the frame number, the audio road beats SCTP in HEVC on Wi-Fi by ~11
  ms at the click and has none of its tail.** Not tried: a longer give-up
  (`mw_aroad_giveup=40`).
- **Open:** a real load (heavier content or another station).

### 05-06/10/2026 — W4 on the Mac: the three roads, before and after the POC's two fixes

Same host and method as the N95 entry above. Client: Léo's MacBook in Wi-Fi,
Chrome, the page at 240, HEVC, Auto bitrate (16-48 Mbit/s: a real load this
time). Kernel drops for a full socket buffer counted per pass (`netstat`).
The house link was busy that evening.
- **"Before"** (22:36-23:42, 3 passes per road):
  - build `build\` of 22:20 for the first five passes;
  - the last four on `73620029`, with the fixes turned off by their keys:
    `mw_rtp_stallwatch=0`, `aroadpace=0,retrcut=0`.
- **"After"** (23:42-00:24): `73620029`, built at 23:20. It adds two fixes:
  - the RTP track's stall watchdog: no frame for 250 ms → a forced IDR request;
  - the audio road paced at 3× what it carries, 50 Mbit/s at least, and its
    resends counted in `retransPermille`, so `retrcut` cuts the encoder.

  `aroad0` is the audio road with `aroadpace=0` (the resend cut kept).

| Road | Clicks of 60 | Click median | Frame age median | Kernel drops / pass |
|---|---|---|---|---|
| SCTP before | 58-59 | 74.5-81.5 ms (p90 85-175) | 13-14 ms | 254-354 |
| SCTP after | 59 | 71.4-75.2 ms (p90 86-92) | 13-15 ms | 309-431 |
| RTP track before | 0 / 54 / 54 | 91-93 ms | 24 ms | 0 |
| RTP track after | 59 / 56 / 48 | 91-99 ms | 24-25 ms | 0 |
| aroad before | 23 / 17 / 37 | 68-76 ms | 13-15 ms | 2,067-3,833 |
| aroad after, paced | 43 / 36 / 31 | 68-75 ms | 13-14 ms | 693-5,011 |
| aroad0 after | 48 / 53 / 44 | 64.5-67.6 ms | 13-15 ms | 802-4,266 |

- **The RTP track's stall watchdog works.** No pass stalled at 0 fps after it;
  before it, one in three did, when a loss hit at the wrong moment and the host
  ignored Chrome's PLI. The track costs ~10 ms of frame age, the receive
  metronome, and ~20 ms at the click against SCTP. It drops nothing in the
  kernel.
- **The audio road is the fastest when it gets through, and it floods the
  Mac's socket.** Its median click is 65-75 ms against SCTP's ~73. But Chrome's
  UDP socket on the Mac overflows by thousands of datagrams a pass, and 7-43
  clicks of 60 are lost.
  - The resend cut helps a little: more clicks, the encoder at ~21-25 Mbit/s.
  - The pacing does not help, and probably hurts: 693-5,011 drops against
    802-4,266 for `aroad0`.
  - This is W1 bis and W2 A again: the socket overflows when Chrome reads late,
    not under the host's bursts. A pacer at 3× and at least 50 Mbit/s stays
    far above what Chrome drains then.
- **SCTP is the only road on the Mac that loses no click.** Its congestion
  control is what keeps the socket's overflow to ~300 datagrams a pass. Its
  tail at the click was smaller after midnight (p90 86-92) than before
  (85-175); the house link was likely quieter.
- **What it means:**
  - On a Mac in Wi-Fi, no road beats SCTP yet.
  - The audio road would need real congestion control: a rate that follows
    the resends and the drops, not a fixed multiple of what it carries.
  - The RTP track will always carry Chrome's metronome.
  - On the N95, which drops nothing in the kernel, the audio road with the
    frame number already beats SCTP in HEVC (entry above). The client's socket
    makes the difference.
- **Open:** why the audio road loses clicks; whether the flag's frames are the
  ones lost to the socket. And a congestion-controlled audio road, measured on
  the Mac.

### 06/10/2026 — W4 on the Mac: the audio road with a capped bitrate

Bruno's question: with the bitrate capped, does the audio road still flood the
Mac? Same bench as the entry above (DualRTX `--dev` build of 23:20, the Mac's
Chrome in Wi-Fi, HEVC, page at 240, 60 clicks a pass). The audio road ran
without pacing (`aroadpace=0`, the default since aea2a192). `--bitrate` fixed
at 12000 (about half of last night's Auto) and 24000, two rounds, arms rotated.

| Pass | Kernel drops | Clicks of 60 | Click median (p90), ms |
|---|---|---|---|
| audio road, 12 Mbit/s | 0 / 0 | 56 / 57 | 59.3 (77.5) / 68.0 (84.8) |
| SCTP, 12 Mbit/s | 28 / 20 | 59 / 58 | 60.1 (83.0) / 69.8 (84.9) |
| audio road, 24 Mbit/s | 72 / 122 | 54 / 51 | 68.0 (85.5) / 68.0 (83.0) |
| SCTP, 24 Mbit/s | 288 / 29 | 58 / 59 | 68.6 (86.5) / 69.6 (89.8) |
| audio road, Auto (last night) | 802-4,266 | 44-53 | ~66.5 |

- **The cap removes the flood.** At 12 Mbit/s the audio road drops nothing in
  the Mac's socket, and at 24 Mbit/s it drops 72-122 against 800-4,300 at
  Auto. The page's Auto asks more than Chrome drains on the Mac when it reads
  late. A fixed cap, or a window, keeps the road under that.
- **It still misses clicks with zero drops.** 3-4 clicks of 60 lost at
  12 Mbit/s, 6-9 at 24, against 1-2 for SCTP. These losses are not in the
  socket: they are on the air. SCTP resends them in time; the audio road gives
  up after `mw_aroad_giveup` (15 ms) and heals by invalidation, so the frame
  carrying the flag can go missing.
- **Time at the click is the same.** The audio road's median lead over SCTP
  disappears once both are capped (59-68 ms each). Last night's lead at Auto
  (~66.5 against ~73) came with the flood.
- **What it means:**
  - A capped bitrate is a usable workaround on a Mac in Wi-Fi, but it does not
    make the audio road better than SCTP there.
  - Two levers are left for the audio road: a send window against the flood
    (POC session, 89adcb0f host side, key `aroadwin=<KiB>`, off by default)
    and a longer give-up for the losses on the air (`mw_aroad_giveup=40`).
    Both measured the same morning, below.

**Same morning: a longer give-up, then the send window.**

- **A longer give-up recovers nothing.** At 12 Mbit/s, `mw_aroad_giveup=40`
  caught 54 and 56 clicks of 60, against 56 and 57 at the default 15 ms
  (medians 59-63 ms). The missing clicks are not a repair that comes too late.
- **The send window** (POC session: host 89adcb0f, the page's acks 2c8ab800,
  key `aroadwin=<KiB>`): the host keeps at most that many bytes not yet
  acknowledged by the page. When the window stays full, it drops an older
  delta frame. `build\` of 05:31, Auto or 24 Mbit/s, two rounds. The page's
  counters are now kept by `pass.py` (5bd7b13b): frames given up (`lost`) and
  chunks NACKed.

| Window | Bitrate | Kernel drops | Clicks of 60 | Page lost / NACKed | Host frames dropped |
|---|---|---|---|---|---|
| none | Auto | 948 / 2,447 | 41 / 52 | 26 / 29, 829 / 1,634 | – |
| none | 24 Mbit/s | 878 / 238 | 49 / 54 | 32 / 11, 369 / 175 | – |
| 48 KiB | Auto | 0 / 157 | 12 / 12 | 67 / 51 | ≥389 / ≥230 |
| 48 KiB | 24 Mbit/s | 0 / 0 | 11 / 29 | 24 / 30 | ≥117 / ≥126 |
| 256 KiB | Auto | 1,012 | 45 | 23, 937 | 0 |
| 128 KiB | Auto | 733 | 53 | 23, 708 | 12 |

- **48 KiB cascades.** A full window drops a delta, the page forces an IDR,
  the IDR is bigger than the window, and it starts again. The host counts in
  flight everything not yet acknowledged, the ack path included (up to 5 ms
  or 16 KiB of throttle, the page thread, SCTP upstream). At ~15 ms of ack
  loop, 48 KiB caps the road near 26 Mbit/s. W1 bis's 48 KB was what sits in
  Chrome's socket, not what the host has in flight.
- **128-256 KiB stops the cascade but no longer protects the socket.** Kernel
  drops come back to the witness's level (733-1,012). No fixed host-side size
  both carries Auto's rate and keeps Chrome's socket short.
- **The clicks are lost to give-ups.** Without a window, the missing clicks
  follow the frames the page gives up (6-19 missing for 11-32 lost a pass),
  despite hundreds of NACKs. A chunk NACKed once is never asked again: if the
  NACK or the resend is lost, the frame waits until the give-up.
- **Next, on the POC side:** a second NACK after about one RTT for a chunk
  already asked; a window measured in what Chrome holds, or one that shrinks
  on the page's losses. Off by default until measured on the Mac.
- The witness passes at 24 Mbit/s dropped 238-878 here, against 72-122 an hour
  earlier on the older build: the Wi-Fi varies that much from pass to pass.

**Same morning: the audio road's repair (06:24-07:08).** The POC session
found why frames were given up. The page could only NACK a frame it had
opened, i.e. one with at least one chunk arrived. A small delta frame is 1-2
chunks: lose them on the air and the page sees only a hole in the sequence,
never asks for it, and gives the frame up. That also explains why a longer
give-up changed nothing. Counters added to `pass.py` (c8579820): `whole`
(whole frames asked) and `reasked`. The witness is localStorage
`mw_aroad_reask=0`.

- **v1** (host e023c404, page 41a3b301): every hole asked whole, every
  missing chunk asked again twice, 8 ms apart; give-up 30 ms.
- **v2** (host 9157fd29: resends capped at 20% of what the road sent over the
  last 100 ms, 16 KiB floor; page 079b3c1f): only a hole of 1-2 frames asked
  whole, once; a chunk asked again once, after 20 ms; give-up 40 ms.

| Arm | Bitrate | Kernel drops | Clicks of 60 | Median, ms | Lost / whole / reasked |
|---|---|---|---|---|---|
| v1 | 24 Mbit/s | 164 / 808 | 59 / 56 | 67.8 / 67.1 | 2 / 7 / 136, 4 / 6 / 863 |
| witness | 24 Mbit/s | 154 / 442 | 50 / 52 | 77.2 / 74.7 | 5, 11 |
| v1 | Auto | 14,053 / 14,890 | 52 / 54 | 74.4 / 65.6 | 66 / 461 / 10,810, 35 / 397 / 11,574 |
| witness | Auto | 1,829 / 3,670 | 50 / 30 | 66.7 / 63.9 | 40, 31 |
| v2 | Auto | 3,767 / 2,850 | 54 / 50 | 72.6 / 72.2 | 15 / 18 / 2,292, 17 / 16 / 1,872 |
| witness (v2 build) | Auto | 2,604 / 1,703 | 34 / 40 | 73.1 / 65.9 | 28, 84 |

- **The repair wins the clicks.** At 24 Mbit/s, 59 and 56 of 60, SCTP's
  level, against 50 and 52. At Auto, v2 catches 54 and 50 against 34 and 40.
  The witness's `lost` undercounts its missing clicks: a frame lost whole was
  never counted.
- **v1 storms at Auto.** Every resend lands in a socket that is already
  full, is dropped, and is asked again: ~14,000 kernel drops and ~23,000
  chunks resent a pass.
- **v2 stops the storm, but not all the cost.** The budget refused 2,774
  resends against 1,605 sent in its second pass. Kernel drops are still 1.45
  to 1.67 times the witness's.
- **What it means:** with v2, the audio road on a Mac in Wi-Fi gets close to
  SCTP's clicks (50-54 at Auto, 56-59 at 24 Mbit/s with v1), at ~66-73 ms. A
  smaller budget (10-15%) may trim the extra drops; it is to be measured at the
  next Mac slot.
- **Decision (POC session, 06/10):** v2 stays the default within the audio
  road, with no code change. It fails the rule set beforehand ("Auto drops no
  more than the witness": they are 1.45-1.67×) and is kept anyway. The clicks
  are what the user feels, the median does not move (~72 ms), and the audio
  road is not the product's default road. Open, at a later Mac slot: a budget
  of 10-15%, then the audio road with v2 against SCTP at Auto in the same
  session.

**Same day, 09:49-10:30: the resend budget at 10, 15 and 20%, against SCTP.**
A bench key sets the budget's share (`aroadbudget=<%>`, 210318fd; default 20).
All four arms ran at Auto, HEVC, page at 240, on build\ at 8ef302e3. That made
three rounds, with the order rotated, 12 passes, and all of them measured.
(`build-t7\` could not run them: the dev edition's virtual-display task only
elevates `build\MoonlightWeb.exe`, so its launches failed in `vdisplay`.)

| Arm | Clicks of 60 | Click median, ms | Kernel drops | Lost frames | Resent / refused |
|---|---|---|---|---|---|
| SCTP | 60 / 58 / 58 | 75.3 / 74.4 / 67.0 | 388 / 436 / 249 | – | – |
| aroad v2, 20% | 59 / 54 / 54 | 76.4 / 66.1 / 59.1 | 1,048 / 2,319 / 2,387 | 6 / 8 / 7 | 809 / 509, 1,402 / 1,836, 1,404 / 1,733 |
| aroad v2, 15% | 59 / 55 / 50 | 74.0 / 74.0 / 70.8 | 996 / 1,009 / 2,039 | 12 / 13 / 22 | 601 / 704, 603 / 783, 1,202 / 1,304 |
| aroad v2, 10% | 51 / 56 / 50 | 66.6 / 64.7 / 74.3 | 1,052 / 1,987 / 1,622 | 13 / 30 / 26 | 604 / 846, 1,002 / 1,873, 1,015 / 1,004 |

- **A smaller budget does not help.** The kernel drops stay within the
  pass-to-pass spread: 1,000-2,400 at every share. The frames lost double or
  triple below 20% (6-8 → 12-30). The clicks fall: 167 of 180 at 20%, 164 at
  15%, 157 at 10%. 20% stays.
- **SCTP beats the audio road here.** It catches 176 of 180 clicks against
  167, and its kernel drops are 4-6 times fewer (249-436). The click median is
  the same within the spread: 67-75 ms against 59-76 ms.
- **W4's answer for a Mac in Wi-Fi at Auto:** the video stays on SCTP. The
  audio road, even repaired, fills Chrome's socket more than SCTP does: SCTP's
  window holds the host back, while the audio road sends and then resends.
  Clicks are lost to that, and nothing is gained in latency. The audio road
  remains a POC option.

### 05/10/2026 — DSCP on the wire and on the air, and the host's share of a sound (audio + DSCP plan, D0 and A1)

Plan « le son et la priorité des paquets », session moonlight-web-da. Tools in
`scripts/bench/dscp/` and `scripts/bench/photon/` (README of each holds the
detail). Commits `d62b565b`, `edfbfe08`, `1489e114` (the host's audio log rode
in on a POC commit), `d9734962`.

**Who marks what today (code, 05/10).**
- libdatachannel stamps every packet: EF (46) for an "audio" track, AF42 (36)
  for any other track, AF11 (10) for every SCTP packet (`track.cpp:215-220`,
  `sctptransport.cpp:470-475`); libjuice applies it with `setsockopt(IP_TOS /
  IPV6_TCLASS)` before each send whose value changed.
- **On Windows libjuice gives up without trying** (`udp.c`, « IP_TOS has been
  intentionally broken on Windows »): the Windows host sends every packet with
  DSCP 0. Linux and macOS hosts already mark.
- One SCTP association carries one DSCP (usrsctp bundles chunks of several
  streams in a packet; RFC 8837 requires it): while the video rides SCTP, the
  host's small messages (force feedback, rumble, pongs) share its class.
- Chrome marks only RTP it sends (`networkPriority`); our page sends none, so
  its inputs, SACKs and RTCP leave as DF. Firefox and Safari mark nothing.
- libjuice's socket is IPv6 dual stack (IPv4 peers through v4-mapped
  addresses).

**Windows marks a user socket after all** (`win_dscp_probe.py` on DualRTX,
Windows 11 26200.9457, not elevated, no QoS policy; received by `tcpdump -v`
on the UM790Pro):

| Method | EF 46 asked | AF41 34 asked | CS6 48 asked |
|---|---|---|---|
| plain socket | 0 | 0 | 0 |
| `setsockopt(IP_TOS)` | 46 | 34 | refused (WSAEACCES) |
| `WSASendMsg` + `IP_TOS` per packet | 46 | 34 | refused |
| qWAVE `QOSAddSocketToFlow` (Voice) | 56 | 56 | 56 |
| qWAVE `QOSSetFlow(OutgoingDSCPValue)` | refused (error 5) | refused | refused |

- A patch of a few lines in libjuice would make the Windows host mark like the
  others. Bruno's decision (05/10): carry it in our copy, as the
  moonlight-common-c fork, and draft an upstream PR (not published without
  him). Still to check: the value changed before every packet on one socket,
  and which option marks a dual-stack socket's v4-mapped traffic (probe cases
  311-323).

**The path keeps the mark.** The DSCP crossed DualRTX → Freebox repeater → the
repeaters' Wi-Fi 7 link → repeater → UM790Pro unchanged (ping 2-5 ms: not
the cable).

**The Freebox's WMM** (`iw dev wlp3s0 scan`, UM790Pro's AX210): the box and
every repeater advertise the standard client parameters, no admission control:
VO CW 3-7 AIFSN 2 TXOP 1504 µs; VI CW 7-15 AIFSN 2 TXOP 3008 µs; BE CW
15-1023 AIFSN 3; BK CW 15-1023 AIFSN 7. 5 GHz channel 44 at 80 MHz, 2.4 GHz
channel 6, no 6 GHz.

**On the air** (AX210 in monitor mode, channel 44, passive): the N95
(`78:8a:86:09:57:5c`) sits on the far repeater (`3a:07:16:ec:68:b4`, -68 dBm
from the UM790Pro). The Windows host's stream reaches it in TID 0 (best
effort): 146 of 146 decodable frames. A household iPhone on the same repeater
sends and receives in TID 6 (voice). Reading the Freebox's DSCP → TID table
failed from there: marked UDP to the N95 (9 classes) gave almost no decodable
frame (the far repeater's frames to the N95 come at high rates, beamformed).
To do with a station near the capture.

**The host's own share of a sound** (A1, `audiolog=1` with the latency flag's
beep, `MW_LATENCY_FLAG_SOUND=click`; Windows native host DualRTX, two N95
passes in Wi-Fi, 59 and 60 clicks):
- every beep reached the capture; from the beep asked to the pacer tick of its
  first loud frame: **median 28.9 / 30.2 ms, p90 34.6 / 35.4 ms**;
- of that, ~12 ms are the render queue ahead of the beep (the beep's own
  output stream; a game's sound has its own), the rest the WASAPI loopback
  (delivered in 10 ms bursts) and the pacer (mean 0.84-1.08 frames queued);
- the relay adds 0.05-0.11 ms at the median (p99 0.6-2.5 ms, a max of 54-138
  ms once a pass), the RTP track takes the packet ~1.6 ms after the tick;
- at 22:20-22:33 the N95's Wi-Fi was heavily loaded (click 120-139 ms, frame
  age 220-570 ms): NetEq's buffer rose to a median of 344 / 416 ms (target
  400-436 ms), 0.5-1.2 % of the sound concealed, 142-303 packets lost.

**The audio jitter target is not a strict floor.** With `mw_audio_target=40`
on a local client (DualRTX, Arc display): Chrome reported a target of 40 ms
and a minimum of 20, and the buffer held 33-37 ms, no concealment. To settle
in A2 (default 60 against `off`, per client).

### 06/10/2026 — The Freebox's DSCP → Wi-Fi queue table: precedence behind the mesh link, nothing on the same box (audio + DSCP plan, D0)

Plan « le son et la priorité des paquets », D0.4, 05:10-05:15, a quiet Wi-Fi.
Tool: `scripts/bench/dscp/ap-table.sh` + `ap_table.py` (README).

**Method.** The UM790Pro's AX210 joins one access point (BSSID pinned, no IP
address) and counts the data frames it receives and sends per 802.11 TID
(mac80211's counters, `iw dev wlp3s0 station dump -v`). Raw marked frames
leave its own wired port for its own Wi-Fi MAC: 16 classes from DF to CS7, 300
frames each at 150/s. Every class landed 300 of 300 frames on one TID (a few
group-addressed frames of the house on the side, non-QoS). The other
direction, out of the Wi-Fi, is this kernel's own table: it gave exactly RFC
8325 (CS1 → 1, AF21 → 3, CS3-AF42 → 4, CS5 → 5, VA and EF → 6, CS6 and CS7 →
7), which proves the counters. A Windows station cannot do this: its Wi-Fi
stack hands frames up as plain Data, without the QoS field (pktmon on the N95).

**Down, the access point's choice:**

| DSCP | `07:1d:00`, the Freebox router: the UM790Pro's cable was on it (-26 dBm) | `ec:68:b4` (the N95's) and `e4:6f:44`, behind the Wi-Fi 7 link |
|---|---|---|
| DF 0, LE 1 | 0 (BE) | 0 (BE) |
| CS1 8, **AF11 10** | 0 (BE) | **1 (BK)** |
| CS2 16, AF21 18 | 0 (BE) | 2 (BK) |
| CS3 24, AF31 26 | 0 (BE) | 3 (BE) |
| CS4 32, AF41 34, AF42 36 | 0 (BE) | 4 (VI) |
| CS5 40, VA 44, **EF 46** | 0 (BE) | **5 (VI)** |
| CS6 48 | 0 (BE) | 6 (VO) |
| CS7 56 | 0 (BE) | 7 (VO) |

- **On the same box, wired port to Wi-Fi: the DSCP is ignored**, everything
  goes best effort.
- **Behind the mesh link: the old precedence rule**, TID = DSCP >> 3. Whether
  the far repeater classifies, or the mesh link carries the priority the
  entry box gave it, is not known; for a client behind the link the effect is
  the same.
- So, on this network:
  - **EF lands in VI**, the video's queue, not VO: audio marked EF does not
    pass the video (AF41/AF42 → TID 4, the same AC). Only CS6 or CS7 reach VO;
  - **AF11 lands in BK, below best effort**. libdatachannel marks every SCTP
    packet AF11, so a Linux or macOS host sends its SCTP video, inputs and
    messages to a client behind a far repeater in the background queue
    (AIFSN 7): it loses to any best-effort traffic of the house;
  - the audio road (`aroad`, an audio track, EF) from a Linux or macOS host
    lands in VI, not VO: the §6 worry about video in AC_VO does not hold here;
  - a Windows host (DSCP 0) is best effort everywhere. The W4 series all ran
    from DualRTX: their SCTP-against-aroad figures carry no queue difference.
- Open: which box DualRTX's cable hangs off (the stream's real path to the
  N95), and what the box itself does. Both ask for a station with an IP and a
  sender elsewhere on the LAN.

**Again from the bench's switch, 11:19-11:23** (the UM790Pro moved onto the
switch DualRTX hangs off, Ubuntu, `enp1s0`/`wlp2s0`; `07:1d:00` is the router,
whose MAC is the HESSID every BSS advertises):
- **`ec:68:b4`, the access point nearest the switch (-21 dBm), the N95's when
  it was in Wi-Fi: everything in TID 0 (BE)**, whatever the DSCP: the same
  signature as the router's own port this morning, so the switch hangs off
  this one;
- `07:1d:00`, the router, behind the Wi-Fi 7 link (-68 dBm): precedence again
  (CS1/AF11 → 1, CS2/AF21 → 2, CS3/AF31 → 3, AF4x → 4, CS5/VA/EF → 5, CS6 → 6,
  CS7 → 7; a weak link delayed some frames into the next class's window);
- `e4:6f:40` (the repeater the Freebox app lists at 192.168.1.121): too weak
  from there (-71 dBm, 2.4 GHz), the association dropped, no reading.
- **The model holds both ways: from a wired port to a Wi-Fi client of the
  same box, the DSCP is ignored; across the Wi-Fi 7 link, precedence.** For
  the bench: a Wi-Fi client on `ec:68` sees no effect of any mark, from any
  host on the switch (Linux and macOS hosts included: their SCTP is not in BK
  there); a client on the router or another box behind the link sees
  precedence, AF11 in BK included.

### 06/10/2026 — The sound: the 60 ms floor costs ~25 ms on the Mac, and the N95 never plays most beeps (audio + DSCP plan, A1 bis and A2 begun)

Windows native host DualRTX. Tools: `audio_summary.py` (content-age),
`sound_offset.py` (photon). Commit `911ab5c1`.

**The floor on the Mac** (Chrome in Wi-Fi, the W4 audio-road passes of session
25, 06:00-06:08, round 1 at the default target of 60 ms, round 2 with
`mw_audio_target=off`, plus two default passes of round 3):
- with the floor, NetEq held a median buffer of ~71 ms (passes: 40-79);
  without it, ~46 ms (38-61). NetEq alone targets 26-40 ms there;
- no more concealment without the floor (0-0.03 % against 0-0.07 %), no
  loss attributable to it;
- the buffer settles at some level early in a pass and stays there: NetEq
  only acts when it leaves a band around its target, so lowering the target
  lowers the band. Small sample, one road; A2 with Bruno to confirm.

**On the N95 the floor does nothing under load**: NetEq chose 240-320 ms by
itself (06:08-06:20, the house's Wi-Fi busy again), far above 60.

**Most beeps never reach the N95's output** (`MW_LATENCY_FLAG_SOUND=click`,
`audiolog=1`, two passes of 60 clicks):
- the host captured and sent all 60 beeps of each pass (60 loud bursts of 5
  frames in its audio log, all `sent`); the page counted 20 and 54 packets
  lost of ~10 600;
- the client played 19 (1 kHz tone) and 12 (white noise). Its WAV holds
  only those, at full level, digital silence between: the missing beeps are
  not quieter, they are not played. The first eight clicks of the tone pass
  were all heard, then most went missing as the buffer grew;
- white noise fares no better, and beeps went missing in seconds where NetEq
  neither cut nor concealed: its time stretching is not the cause;
- heard beeps trail their flag by a median of 283 ms (tone, 15 pairs) and
  439 ms (noise, 8 pairs), close to NetEq's buffer;
- next: the page now logs NetEq's discarded packets, flushes and played
  energy, to place the loss inside or after the browser; then a calm link.

**A2 with Bruno, 09:04-09:45** (Windows native host DualRTX, a quiet morning;
`b0a5ef01`):
- **Mac, Chrome in Wi-Fi**, four passes alternated: with the 60 ms floor the
  buffer held 73.2 and 72.3 ms; without it (`mw_audio_target=off`) 30.3 and
  40.6 ms. **The floor costs ~37 ms on every sound there**, and removing it
  brought no concealment and no loss (0-0.02 %, 0-1 packet per pass). NetEq's
  own target: 20-40 ms;
- **iPhone, Safari 26.5 on iOS 18.7** (driven by `safaridriver` over USB from
  the Mac, `safari_audio.py`): **no `jitterBufferTarget` in this Safari**, so
  the page's floor never applied. NetEq targets 20 ms and holds 39 ms, jitter
  1 ms, no loss, no concealment (81 s);
- Safari on the Mac: not measured (its "Allow remote automation" setting is
  off);
- with this morning's 06:00-06:08 passes of session 25 and the local client
  of A0 (target 40 → 33-37 ms), the A2 gate's answer: on a Chromium client
  with a quiet link the floor sits well above what NetEq wants, and lowering
  it is worth 25-37 ms per sound; under load (the N95) NetEq goes above the
  floor by itself and the floor does nothing; Safari ignores it.

**A3, 13:38-14:31** (Bruno's go after A2; Windows native host DualRTX; the
N95 now on the bench switch's cable; one Mac pass that overlapped a stream of
the production instance, 13:48-13:51, set aside and run again):
- **L2, the Opus frame (`audioframe=`, `ced222aa`), Mac's Chrome in a quiet
  Wi-Fi.** With the page's floor (60 ms) the buffer hides the frame: 47-79
  ms whatever it is. Without the floor, NetEq held 31 ms for 5 ms frames, 29
  for 10 ms, 20 (p90 30) for 20 ms; adding the frame's own wait before it
  leaves, about 36, 39 and 40 ms. Half or a quarter of the packets (8600 /
  4300 / 2150 per pass), and no loss or concealment in any. **5 ms stays the
  best on a quiet link**; the case for longer frames is a loaded Wi-Fi, not
  measured yet;
- **L1 on the N95 by cable: NetEq chooses 77-140 ms by itself** (jitter 3-4
  ms), above the floor, so the floor changes nothing there (buffer 89-101 ms
  without it, 92-125 with it, concealment 0.07-0.19 % either way). On this
  small CPU the playout side, not the link, seems to set NetEq's target: the
  floor's cost depends on the client;
- **A1 ter by cable**: 43 of 60 beeps played (12-19 in Wi-Fi), NetEq
  discarded no packet (`packetsDiscarded` 0) and concealed 0.15 %: the
  missing beeps are not packets NetEq threw away. Chrome reports
  `totalAudioEnergy` 0 on this receiver even with beeps playing, so that
  counter cannot place the loss; the flag was not found in the window this
  time (0 flags), so no sound − picture offset. Next: count the listener's
  own loopback gaps and Chrome's playout stats (`media-playout`), to tell a
  sound the client never played from one the listener did not catch.

### 06/10/2026 — POC Ultra on a real cable: SCTP, the RTP track and the audio road (session 9b)

Windows native host DualRTX; the UM790Pro as a Windows client, alone on
DualRTX's switch by cable (1 Gbit/s), no Wi-Fi hop this time (see the 05/10
entry on the "1 GbE" path). 14:34-15:30, 23 passes. Detail and tables:
`ultra-lan-poc.md` §6.16, commit `6d8ce138`.

- **The path (NetProbe):** UDP round trip p50 0.38 ms (2.6 ms through the
  Wi-Fi 7 hop); TCP 948 Mbit/s; paced UDP lossless up to 500 Mbit/s (extra
  delay p99 ≤ 2.8 ms), 0.4 % loss at 800. DualRTX's Hyper-V vEthernet does not
  matter. The 150 Mbit/s ceiling seen before came from the Wi-Fi hop.
- **The DataChannel at high rate** (synthetic source, 120 fps): the SCTP
  association carries ~530 Mbit/s (1,000 and 2,000 KiB frames); past that, the
  video sharing the association waits hundreds of ms. One pass at 350 KiB
  collapsed to 85 Mbit/s although it asked for less than 1,000 KiB carried:
  a single outlier, to be run again before reading anything into it.
  `sctpburst=10` at 1,000 KiB: 513 Mbit/s against 528 at the default
  (`sctpburst=0`, default since 04/10): no gain on cable.
- **HEVC, median content age** (two rounds each):

  | | SCTP | RTP video track | Audio road |
  |---|---|---|---|
  | HEVC alone | 23.1 / 23.2 ms | 32.9 / 31.6 ms | 23.3 / 23.1 ms |
  | with Ultra's 123 Mbit/s alongside | 22.1-23.3 ms | 31.3 / 31.6 ms | 22.5 ms |

  On cable SCTP no longer suffers when Ultra shares the link (125 ms in Wi-Fi);
  the audio road matches it; the RTP track keeps its 9-10 ms hold (Chrome's
  metronome, see U1.4 ter).
- **5 % loss injected (HEVC):** the audio road loses nothing (58.7 frames
  drawn/s, 29/30 clicks, +1.3 ms age for its repair); SCTP drops to 31.9
  frames drawn/s and 20/30 clicks, as in Wi-Fi.
- **For the Wi-Fi plan:** these are the SCTP passes at today's defaults
  (`sctpburst=0`); relay log
  `bench-out/content-age/u14c-um-smoke-hevc-sctp-sctp-r{0,1}-v120-client-r0.relay.csv`.
  With W4 on the Mac (above): the audio road is the better road on a clean or
  lossy cable, but in a busy Wi-Fi at Auto it fills the client's receive queue
  and SCTP wins. The choice of road depends on the link, not on one winner.

### 09/10/2026 — The click probe measured itself: what it changes above (from the session « Capture et Attente »)

No bench here; the fix and its numbers are in `652fc726` (not pushed).

- **The bias.** While a click waits for its flag, the default renderer
  (Canvas2D on the main thread) read the three flag pixels after every draw:
  three `getImageData` calls, 4.7 ms a frame on the 780M against 0.2 without
  the probe. The flag was dated after that read, so every click carried ~4.5 ms
  of the probe. At 240 fps the frames behind also piled up (decode waiting
  7.7 ms instead of 0.6), so 240 fps looked 4-10 ms slower than 120.
- **What still holds in this file.** Every A/B above compares arms at the same
  frame rate, on the same client and renderer, in the same session: the gaps
  stand. That covers W2 B (`retrcut`), W2.5 (`sctpburst`), the W4 roads and
  the resend budget on the Mac (page at 240 for every arm), and the N95's
  passes.
- **What does not.**
  - Absolute click times (e.g. "~73 ms on the Mac") are high by about the
    probe's cost, which was only measured on the 780M; the Mac's and the N95's
    are not known.
  - Click times compared across clients or frame rates (the Mac at 240 against
    the UM790Pro at 120, Wi-Fi against Ethernet witnesses) carry different
    biases.
  - Frame ages and content ages are not dated by this probe; only the frames
    drawn while a click waited could have been slowed at 240 fps.
- **The RTP video track** still pays Chrome's 64 Hz metronome (U1.4 ter) on
  top; the product's video is on SCTP.
- **First passes with the fixed probe** (session « Capture et Attente »,
  09/10 08:11, Ethernet, relayed by session 59):
  - SCTP: click 11.5-11.8 ms median. The RTP track's 26-27 ms was taken with
    the old probe, so the two are not comparable;
  - the comparable figure is capture → screen: 3.2-3.6 ms on SCTP against
    11.7-12 ms on the RTP track. **The metronome alone costs ~8 ms a frame**,
    in line with U1.4 ter's 7.8 ms hold, and it backs W4's verdict against
    the RTP track;
  - "Auto" detected at 240 fps saves the host 3.9 ms between the click and
    the capture;
  - at 240 fps on the 780M the probe still reads for 3.4-3.9 ms: judge a gain
    from the per-leg breakdown, not from the click alone.

### 09/10/2026 — One LAN session in nine went through the router's hairpin (POC Ultra, relayed by session 59)

DualRTX `--dev` (98276780) → the UM790Pro under Windows, by cable, 120 fps,
nine passes, 19:29-19:49. Log:
`bench-out/content-age/u14ux-um-smoke-hevc-sctp-hs-r1-v240-client-r0.server.log`.

- **Eight passes chose the direct IPv6 pair; one chose the hairpin.** Selected
  pair (line 582): local `prflx 82.67.150.202:46102` → remote
  `prflx 192.168.1.254:62318`, i.e. through the Freebox's NAT and back.
- **Why it can win.** With UPnP, the host relays a copy of each IPv4 host
  candidate as `82.67.150.202 46102 typ host` (the media slot's router hole),
  **at the same priority as the LAN candidate it copies** (e.g. candidate 6:
  `192.168.1.66 48550` and `82.67.150.202 46102`, both 2114976511). Copies
  are also made for the virtual adapters (192.168.56.1, 172.20.240.1,
  172.29.128.1). ICE then has equal-priority pairs and takes whichever check
  succeeds first: a race.
- **Cost:** the sync round trip went from 0.5 to 4.7 ms, and capture → draw
  gained ~2 ms. Every packet crosses the router twice.
- **Open, not changed:** give the public copy a lower priority than the LAN
  host candidate (an srflx-like priority), so a LAN client always prefers the
  direct pair while a remote client still finds the hole. Until then, a LAN
  bench pass should log its selected pair and be set aside if it hairpinned.
- **Done the same evening** (Bruno's go via session 59). The copy now carries
  a server-reflexive type preference (100 instead of 126, the low 24 bits
  kept): 2114976511 → 1678768895 (`RelayBase::emitLocalCandidate`,
  `IcePriority.h`). Bench, 20:41-21:06: `build\` with the change, one `--dev`
  per pass, ten short passes (`still`) to the UM790Pro under Windows by cable
  (`series.py um --prefix ice`). The log shows each copy at its new priority;
  **10 of 10 chose the direct IPv6 pair, 0 the hairpin**. The `--dev` held no
  UDP port in 3478-3481. Limit: the priority orders the browser's checks, but
  libjuice, the controlling agent, nominates on its own pair priorities, so
  this narrows the race rather than closing it by construction; at the old 1
  in 9, ten clean passes are encouraging, not proof.

### 10/10/2026 — What POC Ultra's close says about the road (cable, from `ultra-lan-poc.md` §6.26-6.43)

DualRTX (NVENC) → the UM790Pro under Windows (780M), 1 Gbit/s cable, 1080p at
120 fps, the fixed probe, then the client's own clicks (§6.38). No new bench
here: the POC's numbers, read for the network.

- **The product's HEVC gains nothing off SCTP on cable** (§6.26): click 10.5 ms
  on SCTP against 11.7 on the audio road, capture → draw 3.5 ms on both. On a
  near-still scene an HEVC frame is ~180 bytes. SCTP's way down for it: 0.41 /
  1.01 ms (p50 / p90), relay → drawn 1.11 ms (§6.42).
- **The audio road only gets big frames there sooner**: for PyroWave's 177 KB
  frames, 0.7 ms (§6.26), then ~1 ms with the nudge (§6.29: relay → arrival
  3.9-4.0 → 3.0-3.1 ms).
- **On the audio road the host pays per packet** (§6.41): ~12.4 µs whatever
  its size (three copies, an `srtp_protect`, a `sendto`), and Chrome receives
  at the pace of the host's send loop. 1,400-byte packets instead of 1,100:
  −0.43 ms of send, −0.41 ms of way down, −0.86 ms click → screen. 1,400 is the
  ceiling in IPv6 under a 1,500-byte MTU; PyroWave's default only (`3ed245ea`).
- **Not done: SCTP's packet size.** SCTP runs at libdatachannel's default MTU,
  1,280, i.e. 1,172-byte SCTP packets (`sctptransport.cpp`, path-MTU discovery
  off), against 1,400 on the audio road. A bigger MTU means fewer packets a
  frame (less per-packet cost on the host and, in Wi-Fi, fewer packets to
  SACK), but it needs a path that holds 1,500 bytes: LAN only, not the
  internet or a mesh VPN at 1,280. Open (§6).
- **For the road choice:** with W4 on the Mac (SCTP loses fewer clicks in a
  busy Wi-Fi), the video stays on SCTP on cable and in Wi-Fi. The audio road
  is for a very heavy flow (Ultra) or a video that must not wait behind one.

### 10/10/2026 — W2 A, C and W2.3 with a Windows client in Wi-Fi (UM790Pro)

Bruno's go via session 59. DualRTX `--dev` (`build\` of 16:48, `5060bb35`'s
code, Arc, the page at 240, Auto with detection, `relaylog=1`, B and W2.5 at
their defaults) → the UM790Pro under Windows **on its Wi-Fi**, its cable
disabled for the series (Killer AX1675x, `OctoPowerWifi7`, 5 GHz channel 44,
802.11ax, 1,201 Mbit/s, 99 % signal from the nearby repeater). 17:04-17:45, 12
passes: four arms × three rounds, order turned (`bench-out/wifi/w2-um-wifi-run.sh`,
`series.py um --prefix w2um`, `MW_UM_IP`). Every pass took the direct IPv6
pair to the Wi-Fi address; the fixed probe (`652fc726`). Means of three passes:

| | Click (p90) | Frame age mean / p90 | Way down | Retr. | Refreshes with no new picture /min | Message p90 | Mbit/s |
|---|---|---|---|---|---|---|---|
| base | 27.9 ms (43) | 8.4 / 11.7 ms | 6.9 ms | 0.06 % | 908 | 6.0 ms | 28.6 |
| A, `pace=2` | 27.6 ms (45) | 11.7 / 20.7 ms | 8.7 ms | 0.04 % | 1,931 | 3.9 ms | 25.2 |
| C, `sctpbuf=48,linkhold=4` | 29.7 ms (39) | 9.6 / 14.0 ms | 7.6 ms | 0.00 % | 1,332 | 12.3 ms | 28.7 |
| W2.3, `sctpss=4` | 29.8 ms (40) | 8.6 / 11.6 ms | 7.2 ms | 0.03 % | 862 | 18.5 ms | 28.5 |

- **No arm beats the product.** The click moves by ±2 ms between arms, inside
  its pass-to-pass spread (25-33 ms). One pass per arm ended at 120 fps
  instead of 240 (Auto), so the arms stay balanced; those passes carry the
  worst p90s.
- **A and C age the picture**: pacing adds 3.3 ms to the mean frame age and
  ~9 to its p90; C's small buffer adds 1.2 ms. W2.3 is the base.
- **This link is clean**: at most 0.1 % of chunks resent (the Mac 0.3-0.7 %), 0
  T3. The three keys were built for a lossy, overflowing Wi-Fi; on a clean
  one they cannot gain and A and C still cost.
- **Windows counted nothing**: 0 UDP "Receive Errors" (v4 + v6) in any pass,
  as on the N95. On a link this clean, no overflow was expected anyway, so this
  does not settle the open question below.
- Verdict: unchanged, the three keys stay bench-only.

## 4. The model so far (04/10/2026)

What the measurements support, in order of the path:

1. **The way up is not the problem** on Wi-Fi: 2-6 ms.
2. **The radio alone does not lose**: 0 of 185,000 UDP datagrams at 45 Mbit/s.
3. **The client's kernel does**: Chrome's UDP socket has a small receive buffer
   (a 64 KB sink reproduces it; 256 KB does not) and overflows when Chrome
   reads late while Wi-Fi delivers in aggregates. 1,000-2,300 drops a 2-minute
   pass at 35-42 Mbit/s, ~200-470 at 20 Mbit/s, 0 when less than ~48 KB can be
   in flight.
4. **SCTP reads each drop as congestion**: fast retransmit, the window halves,
   the frames behind wait in usrsctp's (256 KiB, invisible) buffer.
5. **Lowering the bitrate when SCTP retransmits** (B) removes most of it:
   −11 ms at the click on the Mac, the video's tail ÷3, the host's messages
   from ~330 to 35 ms at p90.
6. **What remains (~18 ms of `inSctp` on the Mac with B, 4.5 on Ethernet)
   is not a queue of frames.** A byte stays ~16 ms in usrsctp between
   hand-over and ack even without loss. Candidates: the SACK clock
   (Wi-Fi aggregation, the client's delayed SACK) and usrsctp's max burst of
   10 packets per send opportunity (a frame of 20-35 packets needs 2-4 SACK
   round trips: 8-9 ms each in Wi-Fi, 3-4 on Ethernet).
7. **The max burst was ~6 of those ~15 ms** (W2.5, 04/10): with no limit a
   frame spends ~9 ms in usrsctp on the Mac, the click gains 8 ms at the
   median and ~30 at p90; on Ethernet the wait there falls to almost nothing.
   The Windows native host's default since `2ef56bfe`. What remains on the
   Mac is the SACK clock of the Wi-Fi link.

## 5. What was tried and failed

| When | What | Result | Why |
|---|---|---|---|
| before 09/2026 | Unordered video channel | removed | reordering looked like holes, each asked for an IDR |
| 17/09 | Send buffer sized at 100 ms of bitrate | never took | libdatachannel raises it to 256 KiB (found 03/10) |
| 01-03/10 | RTCC congestion module (`sctpcc=3`) | no click gain | retransmissions halved, the wait did not move |
| 03/10 | Pacing the host's chunks (`pace=`) | no gain, `pace=4` worse; 10/10 on a clean Windows Wi-Fi: frame age +3.3 ms | the socket overflows when Chrome reads late, not under the host's bursts |
| 03-04/10 | Small usrsctp buffer + picture held (`sctpbuf=`, `linkhold=`) | worse: fps ÷4, +14-20 ms click; 10/10 on a clean Windows Wi-Fi: frame age +1.2 ms | the buffer caps throughput at ~buffer / 16 ms; the wait is in-flight time, not a queue |
| 04/10 | usrsctp's fair-bandwidth stream scheduler for the host's messages (`sctpss=4`) | no effect (Mac 04/10, Windows Wi-Fi 10/10) | the messages do not wait in usrsctp's stream queues |
| 04/10 | usrsctp's round-robin-by-packet scheduler (`sctpss=2`) | breaks the association in ~8 s | not investigated; never against Chrome |
| 29/09 | Named drops on oneVPL (Arc) | forbidden | a long-term-reference repair during an intra-refresh wave hangs Intel's HEVC encoder (bench §8n.30) |

## 6. Open questions

- What sets the ~16 ms a byte stays unacked on the Mac's Wi-Fi with no loss:
  the client's SACK policy (dcsctp), Wi-Fi aggregation, usrsctp's max burst?
  W2.5 took ~6 ms off with the burst; ~9 ms remain against ~4.5 on Ethernet.
- Whether the slightly higher kernel drops with no max burst (422 → 547 a
  pass on the Mac) or the N95's extra T3 timeouts (1 and 3 against 0 and 1)
  matter in the field. Ethernet gained (04/10).
- Whether GameStream sessions (Sunshine through the same relay) gain the same
  from `sctpburst=0`: not measured, so not changed. The Linux and macOS native
  hosts were measured with the N95 in Wi-Fi on 05/10 (§3): usrsctp's wait
  halves on macOS, the click stays in the N95's noise on both. Not yet
  measured: a Mac client in Wi-Fi against them.
- What receive buffer Chrome gives its UDP socket on macOS and on Windows, and
  whether a page can influence it (it cannot directly). Whether Windows counts
  a full-socket drop anywhere (the N95 showed 0 "received errors", the
  UM790Pro on a clean Wi-Fi too, 10/10; neither link was shown to overflow).
- SCTP's MTU: libdatachannel's default 1,280 (1,172-byte SCTP packets)
  against 1,400 on the audio road, whose host pays ~12.4 µs a packet (POC
  Ultra §6.41). Fewer, bigger packets on a LAN that holds 1,500: less host
  cost, fewer packets to SACK in Wi-Fi? Never tried (10/10 entry above).
- Whether the N95's SCTP losses are the same mechanism (pacing changed nothing
  there; no kernel counter).
- Why "two windows in a row" or a minimum count was not needed for
  `retrcut=3`: one false cut in four Ethernet passes; worth watching in the
  field.
- Safari's "frames dropped (jitter)" at 38-47 % with no network loss.
- The host's messages on the input channel still wait behind the video
  (~25 ms median, ~25 p90 with B and `sctpburst=0` on the Mac, against a 7 ms
  UDP ping). Not in usrsctp's stream queues (W2.3). In flight behind video
  chunks, at the AP or in the client's reassembly? A separate association or
  an unordered small-message channel would tell. U1.4 (05/10, §3) points the
  same way: with the video on an RTP track, the video no longer waited behind
  Ultra's SCTP load (20 ms against 42-65). If the video leaves SCTP, the
  inputs and the host's messages have the association to themselves. If it
  stays, the next try is a second PeerConnection for them alone. QUIC for the
  inputs only was weighed and set aside (05/10). U1.4 ter (§3): a received RTP
  video track waits for Chrome's 15.625 ms metronome, while audio tracks and
  DataChannels do not. The same frames on an Opus track came out at 11.1 ms
  under the Ultra load, against 21.4 on the video track and 123-129 on SCTP.
  Leaving SCTP therefore does not have to cost the metronome's ~8 ms.
  U1.4 quater (§3): the audio road repairs its own losses by NACK. With 5 %
  of chunks dropped, no frame was lost and the video was 2.5 ms later.
- The Wi-Fi 7 hop of the POC benches is shared with the house: from 13:27 to
  13:55 on 05/10 an iPhone with a weak signal streaming video in bursts took
  Ultra on RTP from 122 down to 78-109 Mbit/s, with losses, while the paced
  UDP probe stayed clean up to ~200 Mbit/s. Is a dip like that the other
  station's airtime, or Ultra's own burstiness? Bench results on that hop need
  their time of day.
- RTO minimum (200 ms) and the lone-frame T3 tail (25/09): never A/B'd.
- DSCP/WMM marking of the video, from the host (W2 item 5 of the plan): not
  tried. It would come for free on the audio road, though (§3,
  05/10): libdatachannel marks every audio track EF (DSCP 46). A Linux or
  macOS host would then put the video in the Wi-Fi voice queue (AC_VO), while
  on Windows libjuice marks nothing. To measure before that road ships: does
  AC_VO shorten the video's wait, and does a video-sized flow in it starve the
  house's other stations, or the AP's own policing?
  Update (05/10, §3 « DSCP on the wire and on the air »): Windows does mark a
  user socket (EF, AF41; not CS6), only libjuice does not try; the Freebox's
  repeaters keep the mark; their DSCP → Wi-Fi queue table is still unread.
  Update (06/10, §3 « The Freebox's DSCP → Wi-Fi queue table »): read. Behind
  the mesh link, precedence (EF → VI, AF11 → BK, CS6 → VO); on the wired
  port's own box, everything BE (checked from the router's port and from the
  bench switch's box, `ec:68`). The audio road would ride VI, not VO, and a
  Linux or macOS host's SCTP rides BK, but only for a client behind the link.

## 7. Knobs (bench keys, off by default unless said)

Link keys go in `MW_NATIVE_TUNING` / `--tuning` of `local_matrix.py`
(`backend/src/streaming/NativeBench.cpp`):

| Key | What | Since |
|---|---|---|
| `relaylog=1` | the relay's per-frame CSV | `5ce2228c` |
| `loss=`, `burst=` | video messages thrown away before SCTP | `4815aabd` |
| `sctpcc=0..3` | usrsctp congestion module | `4815aabd` |
| `flood=`, `floodsize=`, `floodchannel=` | useless traffic on channel 3 | |
| `pace=<n>`, `paceburst=<KB>` | pacing of a frame's chunks | `b2b52486` |
| `retrcut=<‰>` | governor cuts on SCTP retransmissions; **3 by default on Windows**, 0 off | `e0324f4e`, `55dd9cde` |
| `sctpbuf=<KB>` | usrsctp's real send buffer (via `maxMessageSize`) | `dc7f9c72` |
| `linkhold=<ms>` | hold the picture after a backlog that long (Windows) | `dc7f9c72`, `a511bdd3` |
| `sctpburst=<n>` | usrsctp's max burst in packets, 0 no limit; **0 by default on the Windows native host**, 10 elsewhere | `01717368`, `2ef56bfe` |
| `sctpss=0..5` | usrsctp's stream scheduler (4 = fair bandwidth) | `0c21bd5a` |
| `namedrops=0\|1` | name the relay's dropped delta to the encoder | `25bf8c48` |
| `audiolog=1` | each audio packet's way through the host (pacer tick and queue, peak, relay thread, RTP track), `relay-audio-*.csv` | `1489e114` |
| `audioframe=5\|10\|20` | the native host's Opus frame in ms (5 the product): half or a quarter of the packets, 5 or 15 ms more before each | `ced222aa` |
| `aroadpace=<x>`, `aroadwin=<KiB>` | the audio road's pacing and send window (both off by default) | `aea2a192`, `89adcb0f` |
| `aroadbudget=<%>` | the audio road's resend budget, share of what it sent over 100 ms (default 20) | `210318fd` |

Environment: `MW_SCTP_RTO_MIN_MS`, `MW_SCTP_SACK_DELAY_MS`;
`MW_LATENCY_FLAG_SOUND=click|tick` (the server's latency flag also beeps,
`d9734962`). Page (localStorage): `mw_audio_target=<ms>|off` (`edfbfe08`);
`mwAudio.csv()` per second of NetEq, saved by `age.py` as `<tag>.audio.csv`.
