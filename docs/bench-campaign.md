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

## 4. The three instruments

**`--native-bench`** (host side, encoder only, into a sink — no network, no
browser). One CSV row per frame:
`frame,keyframe,captured,bytes,avg_qp,t0_present_us,…,acquire_us,convert_us,encode_us,host_total_us`.
It runs on every platform, a pass costs ten seconds, and it is the only way to
separate the encoder from everything else. Statistics are computed **from the
CSV**, never scraped from the summary text: the columns are a contract, the
prose is not.

**The real stream through a browser** — click-to-photon plus the client legs.
This is the only instrument that measures what a player feels.

**The keyboard check** (§5 bis) — the odd one out: it does not measure how fast
anything came back, it measures whether the right thing happened at all. A
stream can be perfect on both other instruments and still put the wrong weapon
in the player's hands.

## 5. Click to photon

The host raises a blue/white/red flag at the top of **every** monitor for 100 ms
whenever a click is *injected*; the browser sends a click, stamps it, and
watches the decoded picture for the flag. Since 08/09/2026 this works on:

| Host | Click source | Overlay | Needs |
|---|---|---|---|
| Windows | `WH_MOUSE_LL`, flag `LLMHF_INJECTED` | layered topmost window | a desktop session, and a **physical** screen |
| macOS | listen-only `CGEventTap`, `kCGEventSourceStateID` | `NSWindow` at shielding level | **Input Monitoring** granted to the binary |
| Linux | `/dev/input/event*`, virtual devices only | override-redirect X windows | an **X11 session**, and the user in the `input` group |

It is no longer restricted to debug builds: the compile-time gate was removed
because a real debug build is ten times slower in the convert stage, so the only
way to measure was a Release tree carrying `QT_DEBUG` — a recipe no CI binary
could satisfy. The runtime guard was always the real one: `latency_flag_enabled`
defaults to false. It is a **file-only** setting — no switch in the UI, no write
route — so arming a bench machine means editing its `settings.json` and
restarting the server. Nothing on a player's machine can turn it on by accident.

**A virtual display cannot carry the flag.** Measured 09/09/2026 and worth the
paragraph, because everything about it says the opposite is true. On a VDD or a
dummy plug the overlay is created, the hook fires, the log says the click was
injected — and the bands are never painted. Content Chrome puts on that same
screen captures perfectly, so the encoder half looks flawless while every single
click-to-photon sample comes back `timeout` with three grey pixels where the
flag should be. On a physical screen the same code reads `0,0,255` /
`255,255,255` / `255,0,0` exactly.

Capture a physical screen for this chapter. `run-campaign.ps1` defaults to the
primary one and warns when the display it is about to measure looks virtual;
when only a virtual screen is available, the probe's cases are **grey with that
reason**, never red. The same goes for a very small screen: on 800×600 the flag
is 96×30 px, which does not survive a downscale to 720p.

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

## 5 bis. Key to interpretation

A keystroke has **two jobs that fail separately**, and no single verdict can
cover both:

| | |
|---|---|
| **Note** | the character a text field shows — what a typist means by the key working |
| **CS** | the physical key a game reading the raw keyboard sees, named by its US label — weapon slots 1‑5, and everything the player moves with |

They pull in opposite directions, which is exactly why both are measured.
Injecting the character as Unicode types it perfectly and **no game ever knows
a key was pressed**. Resolving the character on the host's own layout keeps the
key real — but on an AZERTY client the key that carries `&` is the US `7`, so
weapon slot 1 answers as slot 7, and the `z` a player presses to walk forward
is pressed as the US `Z` and walks sideways.

That trade is the finding this instrument exists to produce. It is not a crash
and it is not a regression: it is `keyboard_layout_fidelity` doing precisely
what it was built to do, at a price that has to be paid on purpose rather than
discovered in a match.

**How it runs.** `keyboard-check.ps1` types a fixed table of keys through a live
stream and reads the host's own `[KBD]` lines back:

```powershell
# needs "keyboard_debug": true on the host — see below
.\keyboard-check.ps1 -Profiles fr-azerty,us-qwerty
```

Three things make it a measurement rather than an anecdote:

- **The client layout is simulated, not installed.** `cdp.py keys` dispatches
  the *position* and the *character* independently, which is the whole shape of
  the bug class: a keystroke goes wrong exactly when those two stop agreeing.
  An AZERTY client replays identically from a machine running anything, with
  nobody logging out to switch a keyboard. Its one limit is **AltGr** — the CDP
  modifier bitmask has no bit for it and `StreamView.clientChar` reads it
  through `getModifierState` — so no table lists an AltGr key.
- **The host says which key it pressed; the bench decides whether that was the
  right one.** The log line names the key it resolved to, in US labels. Only
  the bench knows which position was pressed at the other end, so only the
  bench can compare them — which is what turns "the host pressed US 7" into
  "weapon slot 1 is broken".
- **The tables carry a `role`.** A wrong physical key on `Digit1` or `KeyW` is
  red; the same fault on a punctuation key nobody games with is a yellow line.
  `scripts/bench/keyboard/*.json` — add a layout by adding a file.

**Arming it.** `keyboard_debug` is an instrument, not a preference: it is
**not seeded** into `settings.json` and there is no UI for it. Add the key by
hand, and the *next stream* picks it up — the worker reads the file when it
starts, so no restart is needed. `-WriteSettings` lets the script add and remove
it itself, restoring the file byte for byte on the way out; without that switch
it refuses to touch a production configuration and prints the line to add.

**Where the lines are.** In the **worker** log —
`moonlightweb-worker-<pid>.log`, not `moonlightweb.log`. Keys are handled by
the process that owns the session, and in worker mode (the default) that is not
the server answering the API. Anything an input or capture path reads has to be
read again on that side of the fork; the latency flag has the same shape, and
so did the bug where the diagnostic was armed only in the parent.

**What good looks like.** `us-qwerty` is the control: every character already
sits where the protocol assumes, so both verdicts must come back OK on every
host. A KO there is a harness fault or a host whose own layout is not US —
never a layout-fidelity bug.

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
| the worker log's `[KBD]` lines (`keyboard_debug`) | how the host resolved each key: the character a text field gets, and the physical key a game sees |

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

# 4 bis. the keyboard check, once per host — needs "keyboard_debug": true
scripts\bench\keyboard-check.ps1 -Profiles fr-azerty,us-qwerty

# 4 ter. give the machine back to whoever is sitting at it
scripts\bench\kiosk-close.ps1

# 5. the report
python scripts\bench\report.py      # -> bench-out\report.html
```

### Getting the screen back

The two kiosks are borderless `--kiosk` Chromes pinned `HWND_TOPMOST`. That is
deliberate — anything else on the captured display is what the encoder would be
measuring — but it means they have no close button, and `Alt+F4` closes the
window that holds the *focus*, which is behind the overlay. Somebody sitting at
the machine used to have no way to get rid of the video at all.

Three ways out, in order of what they survive:

| | |
|---|---|
| `Ctrl+Alt+Shift+M` | un-pins every bench kiosk and minimises it — the campaign's Chrome stays alive and the pass can go on |
| `Ctrl+Alt+Shift+Q` | closes the kiosks and their helpers |
| `scripts\bench\kiosk-close.ps1` | the same, from a shell — works when the hotkey watchdog is dead or its combination is already taken |

The hotkeys are system-wide (`RegisterHotKey`), so they work whatever holds the
focus. `kiosk-hotkeys.ps1` registers them; `kiosk.ps1` starts it on the first
kiosk, and it exits ten seconds after the last one goes. Three modifiers on
purpose: a streamed game presses `Q`, `M`, `Ctrl+Q` and `Alt+M`, and the content
page has no key handler at all — a keystroke arriving from the stream must never
be able to stop a bench mid-pass. Whether the registration succeeded is written
to `%TEMP%\mw-kiosk-hotkeys.log`, because the watchdog runs hidden and a warning
it prints to a console goes nowhere.

`run-browser.ps1` owns the kiosks' lifetime and closes them on its way out — the
end of the matrix, a `throw`, or an operator saying stop. `-KeepKiosks` leaves
them up for inspection.

`-NoKiosk` measures whatever is already on the display: useful to exercise the
machinery on a machine somebody is sitting in front of, but the numbers are not
comparable with a real campaign.

### What the report may say about the fleet

The report is local — `bench-out/` is ignored — but "local" is a policy, not a
property. A report is one self-contained HTML file, which is exactly the shape of
thing that ends up attached to an issue or pasted into a release note. So it is
**scrubbed on the way out**, by default:

| Removed | Kept |
|---|---|
| addresses (v4, v6, MAC) | `127.0.0.1` — half the advice is unreadable without it |
| machine names, labels, SSH aliases | the OS, the backends, the encoder, the GPU |
| the account name, home directories | the rest of the path |
| rendezvous host and id, UUIDs, any hex run of 32+ | the 16-character **binary digest** — it identifies a build, which is the opposite of confidential |

The fleet is renumbered `host-1 … host-N` in the order of `hosts.json`, plus
`this machine`: decipherable by whoever owns the fleet, and by nobody else. A
banner at the top of the report says which of the two versions it is.

`python scripts\bench\report.py --no-redact` writes the raw one, for reading
alone at one's desk. It carries a red banner and should not leave the machine.

The scrubbing is applied in `report.py` at the single escaper every string goes
through, not at each interpolation site. That is deliberate: a rule that has to
be remembered forty times is forgotten on the forty-first, and the string that
leaks is always the one nobody thought carried an address — a driver's error
message, a path in the provenance card, a note written by `discover.ps1`.

### Where the fleet is declared

`hosts.json` is committed and carries the **shape** of a fleet: the ports a
backend answers on, the fields a machine entry may have, and the one machine
every campaign has — the one it runs on, under the id `local`.

**Your machines are declared in `hosts.local.json`**, which is **git-ignored**.
`discover.ps1` merges it on the `id` key: `local` completes the built-in entry,
any other id adds a machine. Copy `hosts.local.example.json` to start one.

That split is not only about credentials. A bench fleet is a description of one
room — these GPUs, that dual boot, a mini PC on a shelf. It is true there and
false everywhere else, so a repository is the wrong place for it: somebody
cloning this harness wants the machinery, never someone else's hardware.

Prefer an `sshAlias` pointing at `~/.ssh/config` over a password in a file,
wherever the machine allows it.

## 9. The playbook — what each failure means

### Before anything: is the kiosk on the captured screen?

- **Every pass reports ~0,2 fps of capture, 14 KB a frame, QP 10.** The kiosk is
  not where the encoder is looking. `run-campaign.ps1` prints the rectangle and
  names the monitor it chose, e.g. `DISPLAY43`; check that line
  before reading a single number.
- **`kiosk window never appeared`.** Chrome started and exited 0 without a
  window, which is what it does when `--user-data-dir` points somewhere it does
  not own. Look at the profile path the script actually passed.
- **The reference clip vanished when the client kiosk came up.** Two kiosks
  sharing one Chrome profile: the launch kills every Chrome of its profile.
  `-DebugPort` now picks the profile, so the two roles no longer collide.

### Launching a pass

- **The tile is clicked and nothing starts, no error.** Streaming this machine
  to itself asks "Streaming your own PC?" first; the card stays in
  `app-card--launching` until `.self-stream-go` — labelled **"Stream anyway 🚀"**,
  emoji included — is clicked. Every later click is swallowed until then.
- **The wrong host launched.** The native host names its tiles after the
  display — "Display 1", "Display 3" — not "Desktop". Several paired hosts each
  own a "Desktop" tile, and the first one on the page wins.

### Reaching the fleet

- **SSH works, the service port does not.** The "allow this app?" dialog opened
  unwitnessed in the console session and Windows made **two Block rules** for the
  binary. A block beats any port rule.
  `Get-NetFirewallRule -Direction Inbound -Enabled True -Action Block`, remove
  them, add a `-Program` rule.
- **The address answers but the machine behaves wrongly.** Two routers handing
  out the same private range is enough to make one address mean two machines.
  Compare gateways; a Windows target replying `TTL=64` is a different device.
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

### The keyboard

- *Not one `[KBD]` line while keys were typed* → look in the **worker** log,
  not `moonlightweb.log`: keys are handled by the process that owns the
  session. Then check `keyboard_debug` in the `settings.json` the host really
  reads — a `--dev` instance has its own.
- *The verdicts are all about the wrong key* → the table went out of step
  because one entry logged nothing. Only **printable** keys produce a line by
  design (an arrow, an F-key, a modifier depends on no layout and would be
  noise), so a non-printable entry in a table shifts everything after it. The
  script says `log out of step` rather than reporting a shifted table.
- *`Note` OK and `CS` KO on a movement or digit key* → not a bug: layout
  fidelity resolved the character on the host's layout and pressed the key that
  carries it, which is a different key. This is the trade §5 bis exists
  to surface, and the campaign's job is to report it, not to fix it on the spot.
- *`Note` KO reading `only if the host runs the client's layout`* → an unknown,
  not a failure. A Sunshine host cannot be asked what layout it runs; only the
  MoonlightWeb host can answer, and it does, on the line that follows.
- *`us-qwerty` comes back with any KO at all* → suspect the harness or a host
  whose own layout is not US before suspecting the feature. That profile is the
  control precisely because nothing in it diverges.
- *An AltGr character has to be checked* → it cannot be, from here: CDP has no
  AltGr modifier bit. Type it by hand on a real keyboard and read the log.

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
| The two halves | `run-campaign.ps1` (inventory + encoder) then `run-browser.ps1` (the same matrix through a real stream) |
| Fleet shape and ports | `scripts/bench/hosts.json` — no fleet, just the shape |
| Your fleet | `scripts/bench/hosts.local.json` (**ignored**), shape in `hosts.local.example.json` |
| Raw results | `scripts/bench/results/` (ignored) — unscrubbed, unlike the report |
| Report | `bench-out/report.html` (ignored) |
| Reference clip | `~/.mw-bench/content/cod.webm` — **outside the repository**, 263 MB |
| Past verdicts | `docs/bench-native-host.md` |
| Keyboard check | `keyboard-check.ps1` + the layout tables in `scripts/bench/keyboard/` |
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
| `unsigned-payload-x64` | **the bare `MoonlightWeb.exe`** — the encoder half without touching an existing install. Bare means bare: see below |
| `MoonlightWeb-windows-arm64-v<ver>` | bench-arm, the only sound ARM64 build |
| `moonlightweb-linux-x64-v<ver>` | `.deb` / `.rpm` / AppImage |
| `moonlightweb-macos-arm64-v<ver>` | the `.pkg` |

4. `run-campaign.ps1 -Exe <path to the artifact's exe>`; the report will say
   `CI artifact` in green.

### The bare exe is not a payload

`unsigned-payload-x64` is one file. Run it on its own and it dies with
`0xC0000135` — no Qt beside it, and the bench's own Qt does not satisfy it,
because CI does not build against the version installed here. It needs a
directory around it, and that directory has to be assembled with care:

- **The Qt runtime and OpenSSL** can be borrowed from an existing install —
  copy the install directory, then drop the artifact's exe into the copy. Copy
  it: writing into `C:\Program Files\MoonlightWeb` is exactly the "installed as
  a user installs it" tier, and it takes the production service down with it.
- **`frontend/` must come from the same commit as the exe.** This is the one
  that bites, because nothing announces it. A borrowed install brings the
  frontend of *its own* version, the server serves it happily, and the campaign
  measures a client that may be months older than the binary. On 09/09 that
  cost an evening: the backend answered `latency_flag: true`, the 0.2.4
  frontend it was serving carried no probe at all, and every click-to-photon
  reading came back a timeout that read like a broken pipeline.
  Stage it the way CI does — `cmake --install <build> --prefix <dir>` — and take
  `<dir>/frontend`. Never copy `frontend/` straight from the checkout: the
  install step is what applies the excludes.

Two checks before trusting a payload, worth the ten seconds:

```powershell
& <payload>\MoonlightWeb.exe --help                    # the exe runs at all
Select-String -Path <payload>\frontend\js\ui\StreamView.js -Pattern mwLatency -Quiet
```

### When to stay local

The loop through CI is long: push, a multi-platform build, a reinstall on five
machines, a replay. When the campaign **finds** something, diagnose it against
the local build — that is what the yellow tier is for — and only re-run the
campaign of record once the fix has landed and been packaged.
