# The bench campaign — protocol, thresholds, and what to do when it breaks

> The harness is `scripts/bench/`, the command is `/bench`, and the output is
> `bench-out/report.html` — **local, never committed**.
>
> `docs/bench-native-host.md` is the *journal* of past encoder campaigns and
> their verdicts. This file is the *method*: how a campaign is run, what it
> measures, and what each failure means. The two are meant to be read together.

## 1. Why a campaign rather than a measurement

Every measurement this project has published so far was built by hand in a
session scratchpad and lost with it — thirty files including the reference clip,
the kiosk driver and the click-to-photon runner, rebuilt from memory each time.
The consequences were not theoretical: two campaigns using different content are
not comparable, and §7 of `docs/design/glass-to-glass.md` (the table of
readings) is still empty.

A campaign is therefore three things at once: a **fleet discovery** that adapts
to whatever answers today, a **fixed experiment design** so two campaigns can be
compared, and a **report that proposes a fix for every anomaly** rather than a
table somebody still has to interpret.

## 2. The experiment design: a star, plus a sweep

The full product — codec × enhancer × 4:4:4 × HDR × mute × six modes × the
fleet — is four hundred passes. The star design answers the same questions in
twelve:

**Reference point**, identical on every machine:

| | |
|---|---|
| Resolution / cadence | 1080p, 60 fps |
| Codec | HEVC |
| Enhancer | off |
| Chroma | 4:2:0 |
| Dynamic range | SDR |
| Host audio | muted |
| Bitrate, aspect | automatic |
| Content | the Call of Duty clip, **looped** |

**The star** — exactly one factor moves at a time: codec (`h264`, `av1`),
enhancer on, 4:4:4 on, HDR on, mute off.

**The sweep** — resolution and cadence, everything else at the reference:
720p60, 720p120, 1080p120, 1440p60, 1440p120.

**The reference is replayed at the head and the tail.** If the two disagree by
more than 15 %, the machine was not in a steady state and nothing inside that
matrix means anything. That check is worth more than any single number in it.

Cross-combinations are run **only on doubt** — when a factor moves something and
the explanation needs a second factor to be pinned down.

## 3. What is skipped, and what is only observed

A codec the host cannot encode is skipped, with its reason recorded. Everything
else is **run and then compared against what was negotiated**, because the three
most valuable findings a campaign can make are all silent fallbacks:

- 4:4:4 quietly becoming 4:2:0 (decided per codec: NVENC does H.264 and HEVC,
  never AV1; AMF and oneVPL not at all),
- HDR quietly becoming SDR,
- HEVC quietly becoming H.264 (MediaTrack transports carry H.264 only; a
  fallback to H.264 also forces `hdr_enabled` and `chroma_444_enabled` off).

A capability table would hide all three, and capability tables lie: on the
reference bench `/api/native/status` reports **60 Hz for a display Windows
drives at 164 Hz**, and advertises **AV1 on an AMF GPU that then refuses to
initialise it**. Nothing is skipped on the strength of a declared capability
except the codec list itself.

## 4. The two instruments

**`--native-bench`** (host side, encoder only, into a sink — no network, no
browser). One CSV row per frame:
`frame,keyframe,captured,bytes,avg_qp,t0_present_us,…,acquire_us,convert_us,encode_us,host_total_us`.
It runs on every platform, a pass costs ten seconds, and it is the only way to
separate the encoder from everything else. Statistics are computed **from the
CSV**, never scraped from the summary text: the columns are a contract, the
prose is not.

**The real stream through a browser** — click-to-photon plus the client legs.
This is the only instrument that measures what a player feels.

## 5. Click to photon

The host raises a blue/white/red flag at the top of **every** monitor for 100 ms
whenever a click is *injected*; the browser sends a click, stamps it, and
watches the decoded picture for the flag. Since 08/09/2026 this works on:

| Host | Click source | Overlay | Needs |
|---|---|---|---|
| Windows | `WH_MOUSE_LL`, flag `LLMHF_INJECTED` | layered topmost window | a desktop session |
| macOS | listen-only `CGEventTap`, `kCGEventSourceStateID` | `NSWindow` at shielding level | **Input Monitoring** granted to the binary |
| Linux | `/dev/input/event*`, virtual devices only | override-redirect X windows | an **X11 session**, and the user in the `input` group |

It is no longer restricted to debug builds: the compile-time gate was removed
because a real debug build is ten times slower in the convert stage, so the only
way to measure was a Release tree carrying `QT_DEBUG` — a recipe no CI binary
could satisfy. The runtime guard was always the real one: `latency_flag_enabled`
defaults to false and is writable **from localhost only**.

**Wolf is grey by design**: it injects into the uinput of a container with its
own compositor, so no flag placed on the host is in its picture. A Wayland
session is grey too — no client may draw above everything else, and a flag that
is sometimes in the picture is worse than no flag.

Protocol for one series, all of it load-bearing:

1. park the host pointer on the inert click target — an injected click that
   activates another window makes the page hidden, freezes rAF and hangs the
   probe **forever**;
2. `Page.bringToFront`, then assert the page is visible **and** fullscreen;
3. one warm-up click, discarded (the first click after a launch is always cold,
   35–40 ms against a 27 ms median);
4. ten clicks, 1.5 s apart.

A bimodal distribution is **normal**: it is the capture cadence, the flag
falling either side of the next deadline. Read the histogram, not just the
median.

## 6. Metrics collected

| Source | What it gives |
|---|---|
| `mwLatencyResults` | click→photon: n, median, p90, p99, min, max, and `reason`/`saw`/`via` on every discarded sample |
| `[perf]` console line, once a second (`mw_perf_diag=1`) | fps in/out, drops by cause, queue depths, draw submit vs wait, p99 of every leg — host and client |
| the stats overlay | resolution, decoded fps, Mbps, **negotiated** codec, the enhancer actually running, transport, losses, recoveries |
| DataChannel `{"type":"stats"}` | `hostRttMs`, `decodeLatencyUs`, `hostProcMs`, `bpDrops` |
| the `/start` reply | negotiated `videoCodec`, `yuv444`, `native_encoder`, `ref_invalidation`, `latency_flag`, `codecOverridden` |
| the host log | `[Session] Per-request streaming settings:` — the proof a setting was taken |
| `--native-bench` CSV | encode mean/p95/p99, KB per frame, average QP, achieved capture rate |

Two the overlay does not give and the campaign adds: **presented over decoded**
(a pipeline presenting two thirds of what it decodes still reads "60 fps"), and
**IDR frames requested over 60 s** — one too many means reference invalidation
did not work.

## 7. Flags and thresholds

| | |
|---|---|
| 🟩 green | within threshold |
| 🟨 yellow | works, but off the mark, or a deliberate downgrade |
| 🟥 red | does not work: no picture, wrong colours, a crash, a setting ignored |
| ⬜ grey | not applicable — **always with its reason** |
| ⬛ black | not run (the machine was not there today) |

Real hardware, LAN, 1080p60: click→photon median ≤ 45 ms · p90 ≤ 80 ms ·
presented/decoded ≥ 98 % · network loss < 0.5 % · unsolicited IDR = 0 · launch to
first picture < 4 s.

**On a VM, a machine with no GPU, or a software encoder — bench-arm, bench-vm,
Wolf on Bazzite, the Debian client VM — latency is never a failure criterion.**
Those cells are grey or yellow, never red. What is validated there is that it
works: the picture arrives, the colours are right, input passes, the sound
mutes.

## 8. Running one

```powershell
# 0. once: put the reference clip in the cache (it is NOT in the repository)
scripts\bench\fetch-content.ps1 -From <path to cod.webm>

# 1. what is here today
scripts\bench\discover.ps1

# 2. the matrix that would be run, without running it
scripts\bench\run-campaign.ps1 -PlanOnly -Display 0

# 3. the encoder half
scripts\bench\run-campaign.ps1 -Display 0 -EncoderOnly

# 4. the browser half, piece by piece (see the skill for the order)
scripts\bench\kiosk.ps1 -Url <app url> -DebugPort 9333 -X .. -Y .. -W .. -H ..
scripts\bench\click-target.ps1 -X .. -Y ..
python scripts\bench\cdp.py settings '{"video_codec":"hevc"}'
python scripts\bench\cdp.py launch "Display 1"
python scripts\bench\cdp.py fullscreen
python scripts\bench\cdp.py stats
scripts\bench\probe-run.ps1 -Label ref-head

# 5. the report
python scripts\bench\report.py      # -> bench-out\report.html
```

`-NoKiosk` measures whatever is already on the display: useful to exercise the
machinery on a machine somebody is sitting in front of, but the numbers are not
comparable with a real campaign.

## 9. The playbook — what each failure means

### Reaching the fleet

- **SSH works, the service port does not.** The "allow this app?" dialog opened
  unwitnessed in the console session and Windows made **two Block rules** for the
  binary. A block beats any port rule.
  `Get-NetFirewallRule -Direction Inbound -Enabled True -Action Block`, remove
  them, add a `-Program` rule.
- **The address answers but the machine behaves wrongly.** Two routers hand out
  `10.0.0.0/24` here (gateways `.254` and `.1`). Compare gateways; a Windows
  target replying `TTL=64` is a different device.
- **`--native-probe` says "no interactive desktop session".** SSH lands in
  session 0. Anything touching the display goes through a scheduled task with
  `-LogonType Interactive`.
- **Quotes eaten over SSH.** Write the command to a file, `scp` it, run it there;
  LF and UTF-8 **without** BOM.
- **The bench-mini is on the wrong OS.** One at a time, and only Bruno reboots it:
  Ubuntu is the Wolf and Linux-native bench, Windows is MultiSeat and AMF. That
  chapter goes black and the campaign carries on.

### Launching, and the client

- **A click on a tile launches nothing.** Reload the page between two streams,
  click by **coordinates** (never by element), twice if the first only hovers.
- **"Streaming your own PC?"** — click `.self-stream-go`, or the card stays stuck
  in `app-card--launching` and every later click is ignored.
- **The dev instance is empty.** It always starts empty: pair each host again,
  and get a PIN from `POST /api/admin/pin/generate`.
- **The JavaScript does not match the code.** Purge the `mw-shell` service
  worker cache before concluding anything.
- **Never measure presentation through the Chrome extension.** It emulates a
  fixed viewport, `requestFullscreen` is refused to its clicks, and its tab
  stays hidden. A dedicated Chrome driven over CDP is the only way.

### The probe

- **Every click times out.** Read `saw` and `via` on the samples: a flag absent
  from the picture, the wrong surface sampled, and a surface never drawn all
  report `timeout` and have nothing to do with each other.
- **The probe never returns.** An injected click activated another window → the
  page went hidden → rAF froze. That is what `click-target.ps1` exists for.
- **The first sample is 35–40 ms.** Normal. That is the warm-up click, discarded.
- **`fromMarkMs == latencyMs`.** rAF is frozen (hidden tab): the measurement
  still stands, the camera pairing does not.

### Picture and codec

- **Black picture, "NOT a keyframe! Cannot extract SPS/PPS".** The parameters are
  not in the **buffered** keyframe; check `descLen` on `Configuring VideoDecoder`.
- **Green picture.** HEVC 4:4:4 **10-bit**: Chrome on Windows decodes it and
  cannot present it. 4:4:4 in SDR only.
- **Washed-out HDR.** An SDR client screen (Chrome tone-maps it itself and proves
  nothing), or missing signal information; check for `hvc1.2.*` and `hdr=true`.
- **AV1 accumulating seconds of delay.** Software decoding on the client. Grey,
  "compatibility", not red.

### Network and quality

- **Quality degrades on its own.** The congestion ladder; look for
  `[MW] Standby stream aborted` or `dual_unavailable` in the console.
- **No "jitter reserve" line.** Normal — the pacer is opt-in (`mw_pacing=1`).
- **Frame gaps.** Look for "reference invalidated … healing with a delta" and
  **zero IDR**. One IDR too many means the invalidation failed.

### Sound

- **The mute cannot be proved.** Only the host's own output level proves it. On a
  **Linux host `mute_host_audio` is ignored in silence** — a known yellow, not a
  red.

### Performance

- **Absurd numbers on a VM or a software encoder.** Out of scope: grey, never
  red.

## 10. Where things live

| | |
|---|---|
| Harness | `scripts/bench/` (committed) |
| Fleet table | `scripts/bench/hosts.json` — hints only, `discover.ps1` probes everything |
| Raw results | `scripts/bench/results/` (ignored) |
| Report | `bench-out/report.html` (ignored) |
| Reference clip | `~/.mw-bench/content/cod.webm` — **outside the repository**, 263 MB |
| Past verdicts | `docs/bench-native-host.md` |
| Probe internals | `docs/design/glass-to-glass.md` §5 bis |

## 11. Two tiers: the campaign of record, and the diagnosis loop

A number is only as trustworthy as the binary that produced it, and the binary a
user installs is not the one a developer builds. The bench therefore has two
tiers, and **the report says which one it was** — mixing them in silence is what
makes two campaigns incomparable.

| | Campaign of record | Diagnosis |
|---|---|---|
| Binary | the **CI artifact**, installed as a user installs it | the local `build\` |
| Answers | what a user actually gets | what a change did |
| Report card | green | **yellow**, "not a campaign of record" |

`run-campaign.ps1` writes `results/provenance.json` — the binary's path, a digest
of the file, its version and the digest of the clip — and the report prints it
above everything else. The **digest, not the version string**: the displayed
version comes from a CMake cache and has been seen surviving a rebuild, so it
does not prove which code is running.

### Why the artifact, concretely

Three gaps a local build cannot close, all of them already known:

- **Windows ARM64** — the cross-compiled OpenSSL is broken (every client TLS
  handshake crashes in `libssl!tls_parse_all_extensions`). Only the CI's native
  `windows-arm64` job produces a sound binary.
- **macOS** — the `.pkg` cannot be assembled on the bench: `ibtool` needs a full
  Xcode and the bench has only the Command Line Tools. The signing identity and
  the TCC behaviour of the shipped app are also not those of a hand-built one.
- **Linux** — the bench builds against Qt 6.6.3, the CI against 6.11.

And it is a chapter 0 consequence that this is possible at all: until the flag
left `QT_DEBUG`, a release binary could not be measured for click-to-photon.

### Getting the artifacts, without cutting a release

No tag is needed. `ci.yml` runs the full packaging as its last stage, and **a
manual run on a branch only uploads workflow artifacts** — the version is
`<last tag>-<3-char sha>`, nothing is published.

1. Push the commits (Bruno's gesture; `ci.yml` has no branch push trigger on
   purpose, so the multi-platform matrix is never spent by accident).
2. Run `ci.yml` manually on `main` — or `release.yml` directly, which takes a
   `platform` input (`all`, `windows-x64`, `windows-arm64`, `linux`, `macos`)
   when only one bench needs refreshing.
3. Collect what each bench needs:

| Artifact | For |
|---|---|
| `MoonlightWeb-windows-x64-v<ver>` | the Inno installer — installs like a user |
| `unsigned-payload-x64` | **the bare `MoonlightWeb.exe`** — the encoder half without touching an existing install |
| `MoonlightWeb-windows-arm64-v<ver>` | bench-arm, the only sound ARM64 build |
| `moonlightweb-linux-x64-v<ver>` | `.deb` / `.rpm` / AppImage |
| `moonlightweb-macos-arm64-v<ver>` | the `.pkg` |

4. `run-campaign.ps1 -Exe <path to the artifact's exe>`; the report will say
   `CI artifact` in green.

### When to stay local

The loop through CI is long: push, a multi-platform build, a reinstall on five
machines, a replay. When the campaign **finds** something, diagnose it against
the local build — that is what the yellow tier is for — and only re-run the
campaign of record once the fix has landed and been packaged.
