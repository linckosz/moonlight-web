# Loss bench — `scripts/bench/loss/`

Tools to put a MoonlightWeb stream through a lossy link and measure what the
transport carries under it. Written for the FEC chapter of plan « Idées
Punktfunk » (phase A0, 01/10/2026); they were out of the repository before
(`bench-out\d3d12v2\inet`, `c925b`, `c925w`).

Nothing here ships. The host-side keys below are bench keys of a native
session, off unless `MW_NATIVE_TUNING` or `native_tuning` (settings.json) names
them.

## The three ways to lose packets

| tool | where | what it does | what SCTP sees |
|---|---|---|---|
| `netem.py` | a Linux **client** (the UM790Pro) | delay, random or bursty losses, rate — on the UDP of the host's media ports only (48550-48573, and a DEV edition's 48574-48597; IPv4 and IPv6) | everything: a real lossy link |
| `mwshaper.py` + `shapectl.py` | the Windows **host** (DualRTX), elevated | the same gestures with WinDivert, for any client — a phone included | everything |
| `loss=<‰>[,burst=<n>]` | the host's relay | video messages thrown away before SCTP is handed them, on a fixed seed | **nothing**: the receiver's half alone, its congestion window untouched |

### netem (`netem.py`)

    sudo-capable user on the client:
    python3 netem.py on --rtt 30 --loss 1 [--burst 4] [--rate 30000] [--limit 200]
    python3 netem.py change --rtt 80 --loss 0.3
    python3 netem.py off | show

- `--rtt` is the round trip added: half on the downlink (through `ifb0`), half
  on the uplink.
- `--burst` > 1: Gilbert-Elliott, same average loss in bursts of that mean
  length.
- ⚠️ netem's `limit` counts the packets in its delay line. At 14 000 packets/s
  and 40 ms one way, the default 200 drops by itself: `flood_run.py` uses 20 000
  (a delay line, not a bottleneck).
- The interface is the default route's: the UM790Pro's NIC was renamed
  `enp1s0` → `enp2s0` on 01/10/2026 (a GPU on its M.2 port).
- ICE takes IPv6 when it can: the filters match the ports, never an address.

### The WinDivert shaper (`mwshaper.py`, `shapectl.py`)

    elevated, on the host:
    py mwshaper.py --remote <client ip>,192.168.1.254 [--log shaper.log] [--windivert <dir>]
    py shapectl.py loss 1 4            (1 %, bursts of 4, downlink, an hour)
    py shapectl.py delay 15 15         (+30 ms round trip)
    py shapectl.py rate 8000 30000 200 | cut 500 | clear | stats | quit

- WinDivert 2.2.2 (`WinDivert-2.2.2-A`, sha256 of the zip
  `63CB41763BB4B20F600B6DE04E991A9C2BE73279E317D4D82F237B150C5F3F15`, from
  github.com/basil00/WinDivert releases) is not in the repository: point
  `--windivert` (or `MW_WINDIVERT_DIR`) at its `x64` folder.
- ICE may first go out through the router (a hairpin): put the router
  (192.168.1.254 on the bench LAN) in `--remote` too.
- It never sees the loopback: the client must be another machine.
- `Start-Process -Verb RunAs` ignores `-WorkingDirectory`: give the script by
  its absolute path. At the end: `quit`, then `sc stop WinDivert` elevated.
- Pure Python: about 15 000 packets/s before it is the bottleneck.

## The host's bench keys (native session)

In `MW_NATIVE_TUNING` or `native_tuning` (settings.json, read at each
`/start`; `set_tuning.py <settings.json> [<spec>]` writes it atomically):

| key | effect |
|---|---|
| `loss=<‰>` | video messages thrown away before SCTP, per thousand, fixed seed |
| `burst=<n>` | each such loss takes n messages in a row |
| `sctpcc=0..3` | usrsctp's congestion control: RFC 2581 (its own), HSTCP, H-TCP, RTCC |
| `flood=<kbps>` / `flood=max` | messages of no use on DataChannel id 3, paced or as fast as SCTP takes them (SctpFlood.h) |
| `floodsize=<bytes>` | their size (default 1100) |
| `floodchannel=fec\|video` | unordered with no retransmission (default), or the video channel's own reliability |

The browser counts the flood only when its localStorage holds `mw_flood`
(`fec` or `video`, FloodCounter.js): `[MW-FLOOD] {…}` once a second in the
console, `globalThis.__mwFlood` for a driver.

## Runs

- `client-chrome.sh` — the measuring Chrome on the Linux client, DevTools on
  9222. `--password-store=basic` is required (the GNOME keyring trap).
- `flood_run.py` — phases of netem shaping against a flooding host; one JSON
  line per phase: delivered kbps, messages/s, the counter's loss, extra delay
  p50/p95, Chrome's main-thread busy share (CDP `TaskDuration`).
- `flood_summary.py` — those lines as a table (`--markdown` for the docs).
- `video_run.py` — today's video channel under the same phases, as the viewer
  sees it: the host shows `scroll.html?band=1`, a sampler on the stream's
  canvas counts damaged pictures and freezes (gaps over 100 ms); the host's
  native tuning picks the recovery (default, `namedrops=0`, `dpb=1`).
- `rs-bench.html` — the client decoder micro-bench: Reed-Solomon GF(2⁸) as
  nanors writes it, pure JavaScript against hand-assembled WebAssembly SIMD;
  `?auto=1`, results in the page and in `globalThis.__rsBench`.

The first results — the SCTP ceiling under losses per congestion module, the
largest single-packet message, the main thread against the message rate, the
decoder — are in `docs/bench-native-host.md` §8r.
