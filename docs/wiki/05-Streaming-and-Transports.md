[← Frontend](04-Frontend.md) · **Streaming & Transports** · [Next: Security →](06-Security.md)

---

# 5. Streaming & Transports

This chapter explains how video, audio and input actually travel between Sunshine, the MoonlightWeb server and the browser — the heart of the project, and where most of the hard-won engineering lives.

## 5.1 The five transport modes

The same Sunshine-side stream (RTSP/RTP/ENet handled by `moonlight-common-c`) can be relayed to the browser in five ways:

| Mode | Backend relay | Browser path | Video sink | Codecs | Notes |
|---|---|---|---|---|---|
| `webrtc-dc-udp` | `DataChannelRelay` | video+input SCTP DataChannels **+ RTP audio track**, UDP ICE | canvas (WebCodecs) | H.264/HEVC/AV1 | Default. Lowest latency with full codec choice |
| `webrtc-dc-tcp` | `DataChannelRelay` | same, ICE-TCP candidates | canvas | H.264/HEVC/AV1 | UDP-hostile networks |
| `webrtc-media-udp` | `MediaTrackRelay` | RTP video+audio tracks, input DataChannel | `<video>` (browser decoder) | **H.264 only** | Browser-managed jitter/FEC/PLC; true-HDR-capable sink |
| `webrtc-media-tcp` | `MediaTrackRelay` | same, ICE-TCP | `<video>` | H.264 only | |
| `wss` | `StreamRelay` | one WebSocket (TLS), video+audio+input multiplexed | canvas | H.264/HEVC/AV1 | Always works (it rides the HTTPS port); worst latency profile |

The two WebRTC families differ only in the **video** path: `-dc` fragments access units over SCTP for WebCodecs, `-media` sends a real RTP video track. Audio is an RTP Opus track in both; input is always the `input` DataChannel. `-udp` vs `-tcp` changes nothing but the ICE candidate types (`enableIceTcp`).

### The fallback chain

`TransportPriorities::orderedTransports()` defines the auto order:

- **Video Enhancement OFF**: `webrtc-dc-udp` → `webrtc-dc-tcp` → `webrtc-media-udp` → `webrtc-media-tcp` → `wss`
- **Video Enhancement ON** (canvas required for WebGPU): `webrtc-dc-udp` → `webrtc-dc-tcp` → `wss` → `webrtc-media-udp` → `webrtc-media-tcp` (media kept only as a last resort, streaming *without* enhancement)

Rules applied when building the chain (`main.cpp`):

- HEVC/AV1 requests **skip webrtc-media** (H.264-only) — or, if a media mode is explicitly forced, the codec is overridden to H.264 and the response flags `codecOverridden` + `originalCodec`.
- Host codec support is checked against the canonical `SCM_MASK_*` values from `Limelight.h` (the GFE-era literals are wrong for Sunshine).
- A forced `-udp` mode promotes its `-tcp` sibling to second place (same family before switching).

**The browser owns the loop**: the `/start` response is sent *before* ICE connects, so only the client can observe a connection failure. The full `transport_chain` and current `transport_index` are echoed in the response; on failure the frontend relaunches with `transport_index + 1`. Manual transport selection precedes automatic fallback.

**Only a transport failure costs a rung.** A `/start` that never yields a session — timeout, 502, Sunshine refusal — is a verdict on the host, not on the transport: nothing was offered, no ICE was gathered. It is retried once on the *same* index and then reported as such (`transport.launchFailed`). Charging it to the chain empties the ladder against an unresponsive host and ends on a "no transport works" that is simply false — the 2026-08-07 incident, where a wedged Sunshine consumed all three rungs in 60 s without a single ICE attempt. The worker's own launch deadline (`NvHTTP::LAUNCH_TIMEOUT_MS`, 20 s) is deliberately shorter than the browser's `/start` timeout (25 s) so the failure is *reported* rather than the worker being killed mid-request by the relaunch that takes its slot.

## 5.2 Video path

```
Sunshine GPU encoder ─RTP/UDP─► moonlight-common-c ─decoded access units─► MoonlightShim
    ─► Relay (DC: FrameSender thread / media: RTP packetizer / WSS: WS frames)
    ─► Browser: reassemble → WebCodecs VideoDecoder → renderer (WebGPU/2D canvas or <video>)
```

Key mechanics (all present in the code — regressions here are the most expensive class of bug):

- **Frame framing (DC/WSS)**: each access unit is fragmented into DC messages with a header carrying a `frameId`. The video DC is **ordered** with `maxRetransmits=3`; the frontend detects `frameId` gaps and requests an IDR rather than reordering (a frontend reorder buffer was tried and removed — it causes IDR floods and latency).
- **IDR discipline**: IDR requests are throttled/coalesced backend-side (250–500 ms cooldown, sticky `m_AwaitingIdr`), and both sides apply **exponential backoff** under congestion — otherwise mobile networks enter an IDR spiral.
- **Backpressure everywhere**: 256 KB DC high-watermark (keyframes exempt), frontend consults `decodeQueueSize` before `decode()`, bounded worker→main signal queue (`m_PendingVideoFrames`, decremented on consume), WebGPU `draw()` awaits `onSubmittedWorkDone()`.
- **webrtc-media specifics**: no browser PLI reaches the backend, so a **proactive IDR every 250 ms** runs until the client confirms; RTP timestamps are derived from real capture times (a synthetic 60 fps clock broke frame pacing); packets are sent from the capture thread; `playoutDelayHint`/`jitterBufferTarget` is set on the `RTCRtpReceiver` (not the element), driven adaptively by `JitterController` (AIMD; a non-zero target also re-arms backend NACK).
- **DataChannel presentation pacing**: the DC/WebCodecs path has no dejitter buffer of its own — it presents on decode and drops to the freshest frame, so link jitter becomes "repeat + skip" judder (a visible 2–3 frame hitch at unchanged mean RTT). `FramePacer` (**opt-in**, `mw_pacing=1` — the project rule is freshest-frame-first: on real Wi-Fi, power-save delivers frames in 20-40ms bursts, so the p95-sized reserve legitimately sits near its cap and the latency it buys back is the project's priority) rebuilds the reserve from the `backendTs` capture stamps: it tracks the minimum transit delay, sizes the reserve at p95 of the per-frame excess, and stamps each decoded frame with a presentation deadline (rise immediately, decay slowly, hard cap 25 ms — deliberately tight: past ~1.5 frames the added lag hurts a shooter more than the judder it removes). At target 0 — the clean-link state — the behaviour is bit-for-bit the old immediate path. **The quantile is the whole control law**: it covers the tail and deliberately lets outliers hitch. The underrun fast-path (`noteUnderrun`) may only bring the target to that same quantile sooner, never past it, and must not touch the decay clock — a version that did both shipped briefly and pinned the reserve at the 25 ms cap on every non-loopback link (24.6 ms held against a 2.9 ms measured tail), because a few percent of late frames bump faster than the decay sheds and reset the decay timer on the way. The hold shows up in the existing render-queue latency leg, so it is never hidden from the overlay. Applies to both the worker and main-thread paths; **not** to webrtc-media, where the browser's own buffer does the job (see `JitterController` above).
- **Codec bootstrap**: SPS/PPS(/VPS) are parsed browser-side (`Mp4Muxer.js` / `Av1Utils.js`) to build exact codec strings for `VideoDecoder.configure`; 4:4:4 chroma is opt-in and only offered for profiles the selected codec/browser advertises via the codec masks.

### Why canvas *and* `<video>`?

- **Canvas (WebCodecs decode)** gives frame-level latency control, codec freedom (HEVC/AV1), custom rendering (WebGPU upscaling/sharpening) and precise stats. It is the default sink.
- **`<video>`** is used in two situations: (a) the *webrtc-media* transport, where the browser owns decode and dejitter entirely; (b) the `VideoElementRenderer` sink on the DC transport, because canvas sampling is SDR-referred — only a `<video>` element presents BT.2020+PQ frames as real HDR.

## 5.3 HDR — support and limitations

- The stream can be encoded HDR10 (HEVC/AV1, `hdr_enabled`); decoded `VideoFrame`s carry the bt2020/PQ `VideoColorSpace`.
- **Limitation**: WebGPU/Canvas2D can only output SDR color spaces (srgb/display-p3). `importExternalTexture`/`drawImage` tone-map PQ→SDR *before* the app sees the pixels, so HDR through the canvas path always looks washed out. Chrome historically double-converted, making naïve canvas-HDR attempts worse.
- **Current routing** (`docs/` + memory of `hdr-routing`): true HDR requires the **`<video>` sink** (VideoElementRenderer or webrtc-media). When the Enhancer is ON, or the display is SDR (`dynamic-range` media query), an **ACES tone-map of the P010 signal** is applied automatically in the canvas path; `mw_hdr_tonemap=1/0` (localStorage) overrides.
- Practical consequence: **HDR and Video Enhancement are mutually exclusive** — enhancement needs canvas, real HDR needs `<video>`.

## 5.4 Audio path

```
Sunshine ─RTP─► moonlight-common-c (Opus packets, NOT PCM) ─► relay
   webrtc-dc / webrtc-media: native RTP Opus track → browser (jitter buffer + FEC/PLC) → <audio>
   wss                     : Opus over the WebSocket → WebCodecs AudioDecoder → AudioWorklet (jitter buffer + WSOLA)
```

- **Both WebRTC transports carry audio as a real media track, never a DataChannel** — `m=audio` sendonly, Opus PT 111, `OpusRtpPacketizer` + `RtcpNackResponder(64)`, played from `pc.ontrack` on an `<audio>` element. `DataChannelRelay` and `MediaTrackRelay` are identical here. The ordered audio DataChannel this replaced head-of-line-blocked on packet loss (periodic ~0.5 s dropouts); the browser's NetEq conceals the loss instead. SCTP id 1 stays reserved-but-unused from that era.
- RTP timestamps advance by the negotiated Opus frame size (`audioSamplesPerFrame()`), never by arrival time — an arrival-time clock makes NetEq time-stretch and the audio turns robotic.
- 5.1/stereo Opus; SDP carries `stereo=1;sprop-stereo=1` (without it libwebrtc instantiates a **mono** decoder and downmixes (L+R)/2 — ~6 dB quieter than moonlight-qt, stereo image lost). Mobile clients may request 10 ms low-bandwidth frames (`low_audio`).
- **`wss` only**: `AudioPipeline` decodes Opus with the WebCodecs `AudioDecoder` (WASM `opus-decoder` worker fallback) and feeds `audio-processor.js`. That AudioWorklet jitter buffer adapts between ~60 and ~160 ms; WSOLA time-stretch (default on) absorbs clock drift pitch-free.
- `mute_host_audio` maps to GameStream `localAudioPlayMode` (host speakers muted by default while streaming).
- **Never apply gain in JS** — the historical "60% volume" issue was Sunshine's virtual sink volume on the host (host loopback is post-volume and volume keys act on it mid-stream); `HostAudioSink` normalizes it.

## 5.5 Input path

```
Browser events (kbd/mouse/touch/gamepad/clipboard)
  ─JSON over input DC (or WSS)─► backend InputEncoder (binary Limelight packets)
  ─InputCrypto AES-128-GCM─► EnetControlStream ─ENet─► Sunshine
```

- **Scroll is counted in WHEEL_DELTA units (120 to a notch)**, never in browser pixels — the client normalizes `deltaY`/`deltaX` (deltaMode-aware) and carries the fractional remainder between events, because the backend parses the value with `QJsonValue::toInt()`, which returns 0 for a non-integer. Windows hosts consume the exact amount and scroll smoothly; **Linux hosts throw away anything under a notch** — Sunshine hands the value to inputtino, which emits `REL_WHEEL = amount / 120` with no accumulator, so only the `REL_WHEEL_HI_RES` companion event can save a trackpad or touch drag, and X11 sessions routinely ignore it. `serverinfo` carries no OS field, so the hosts that need help cannot be told apart: `MoonlightShim` therefore **always** holds back sub-notch amounts and emits whole notches (`quantizeScroll`, one carry per axis, alive for the session) — the same thing Sunshine's own `high_resolution_scrolling = disabled` does, applied for every host and covering every client at once. Windows hosts lose nothing: the carry keeps the leftovers instead of the host.
- Mouse: relative (pointer-lock) or absolute; scroll (vertical + horizontal — the latter is a Sunshine-only extension, silently ignored by GFE hosts), buttons. Touch: trackpad translation (1/2/3-finger gestures), or absolute touch-screen mapping when `touch_screen` is on. Keyboard: raw key events, Escape is a normal key. Gamepads: `gamepadconnect`/`gamepad`/`gamepaddisconnect` snapshots + rumble back-channel.
- **Lock-key sync is host-authoritative**: on the first keydown the client sends `locksync`; the backend reads the *real* host state (`GetKeyState`, gated to host==backend) and reconciles — never a blind NumLock tap. The sync runs synchronously on the relay thread to preserve input ordering. When the host is remote its state is unreadable, and the fallback is each lock's hardware default — **NumLock on**, Caps and Scroll off. That default is load-bearing on Linux hosts: Sunshine turns VK_NUMPAD1 into evdev `KEY_KP1`, which xkb reads as End whenever the host's NumLock is off, so presuming "off" tapped an already-aligned host and left the guest with a cursor pad instead of a numpad. Windows hosts are immune — Sunshine injects the VK there and never consults the lock state.
- **A stall must not stretch a keypress** — the input channel is ordered+reliable, so a freeze does not lose events, it *delays* them: a press that already landed stays down for the whole stall while its release waits in the SCTP queue, and guest typematic turns one keystroke into dozens (2 s freeze ≈ 45 `r`). Nothing the client sends during the stall can fix that (the correction rides the same blocked queue), so the host side runs a **dead-man switch** (`MoonlightShim`): every inbound message refreshes a liveness timestamp, the client heartbeats its full held-input state every 100 ms while anything is held, and silence releases what is still down — 250 ms for ordinary keys (before the ~500 ms typematic delay, so no repeat is ever generated), 3 s for `hold`-flagged inputs (WASD/ZQSD — the same physical codes — plus mouse buttons and gamepads in gaming mode, so a hiccup does not stop a player mid-stride). Each heartbeat is also a resync: anything released mid-stall but genuinely still held comes straight back. Older clients that never send `inputstate` never arm the watchdog.
- Clipboard: bidirectional text via `paste` events + `ClipboardBridge` (host==backend only).

## 5.6 Session lifecycle & teardown discipline

### Stream worker processes & the two slots

`stream_worker_enabled` (default **on**) runs every session in a `MoonlightWeb --stream-worker` **child process** (`StreamWorkerHost` parent-side, `worker/StreamWorkerMain.cpp` child-side; JSON lines over stdin/stdout). Since `moonlight-common-c` is process-global, one process = one session — so child processes are what buy **two concurrent slots** (plus crash isolation: a relay crash no longer takes the web server down).

| Slot | Signaling / relay ports | Browser path | Role |
|---|---|---|---|
| 0 | 48001 / 48002 | `/ws`, `/ws/stream` | The live stream. |
| 1 | 48011 / 48012 | `/ws1`, `/ws1/stream` | The **standby** leg of seamless quality switching. |

**Seamless switching** (quality step, HDR/codec change, congestion degradation): the frontend launches the *other* slot with `{standby:true, session_slot}` and its own derived `client_uniqueid`, keeps the live stream playing, and swaps the display only on the standby's **first decoded frame** — the user never sees a loader. A standby launch never takes over slot 0; it joins the already-running Sunshine app (`preferResume`).

Rules specific to the dual path:

- **A standby launch is the capability probe.** Hosts that refuse a second concurrent session are remembered per host (`dual_supported:false` / `status:"dual_unavailable"` in the `/start` response) and the frontend reverts to the plain relaunch (quit, then launch again). Heuristic backstop: slot 0 dying within ~3 s of a standby launch means the host took over instead of adding a session.
- **Always retire, never plain-quit, when a twin stream exists.** The frontend's `quit({retire:true})` sends `keep_host_session:true`, which disconnects the leg *without* a Sunshine `/cancel`. Both legs share one Sunshine app: a `/cancel` kills the surviving leg too and cascades into `Failed to decrypt RTSP` on it. This applies to the standby swap **and** to the plain relaunch path.
- `/quit` with `session_slot` tears down that slot only; without it, every slot owned by the caller's `client_uniqueid`.
- The parent tracks which uniqueids still have a live Sunshine app (workers are fresh processes with no in-process resume hint), so a relaunch goes straight to `/resume` — Sunshine rejects `/launch` while an app is running.
- Set `stream_worker_enabled:false` to fall back to the in-process, single-stream mode; the ordering rules below then apply in-process and are also what the worker executes internally.

### Ordering rules

These prevent whole bug classes (crashes, 504s, zombie sessions):

1. Frontend quits by calling `webrtc.close()` **before** HTTP `/quit` (a `_stopping` guard silences `onerror`; `close()` uses its own `_closed` guard).
2. Backend `/quit` and all teardown paths stop the **shim first** (`stopConnection()` — moonlight stops calling back), then `relay->stop()`, then `deleteLater()`.
3. `/quit` is ownership-guarded by `client_uniqueid` — a stale quit from a taken-over client cannot kill the new owner's stream.
4. Relay teardown lambdas live in `qApp` context in `main.cpp` (the ephemeral `StreamSession` self-deletes once streaming starts, so it cannot own teardown).
5. A new `/start` **defers** until the previous relay graph emits `destroyed()` (frees the fixed signaling port + the moonlight singleton).
6. Take-over never `/cancel`s Sunshine — the newcomer `/resume`s the same session.
7. Revoking a streaming auth session triggers the same teardown plus a keyed Sunshine quit.

## 5.7 Notable workarounds catalog

| Problem | Workaround | Where |
|---|---|---|
| Browsers answer Opus as mono | Force `stereo=1` in SDP | `SdpUtils.js` |
| No PLI from browsers on media tracks | Proactive IDR every 250 ms via DC | `MediaTrackRelay` |
| iOS mute switch silences streaming audio | Looping silent `<audio>` unlock | `iosAudioUnlock.js` |
| Chrome swallows `cursor:none` set while unfocused (double cursor) | Toggle default→none on refocus | `StreamView` |
| iOS/Gboard virtual keyboards send unreliable key events | Diff `input` events against a sentinel | `StreamViewTouch` / mobile keyboard |
| Windows Schannel can't load PEM keys | Force Qt TLS backend to OpenSSL at boot | `main.cpp` |
| mDNS port 5353 conflicts with Sunshine on the same machine | Never bind 5353 at boot; ephemeral scans only | `ComputerManager` |
| Virtual adapters (VPN) captured as mDNS source → 502 | `chooseBestMdnsAddress` picks the default-route subnet | `ComputerManager` |
| Sunshine HTTP wedges if polled during a stream | Poll suspension predicate while any relay is alive | `main.cpp` |
| Router-owned external port breaks WS URLs | Signaling URL anchored on `window.location.host` | frontend |
| GFE-era codec masks wrong for Sunshine | Use `SCM_MASK_*` from Limelight.h | `main.cpp` |
| Cross-clock latency display freezes on offset error | Latency = sum of measured legs only | `StreamView` stats |
| AV1 negotiation gaps | Forced AV1→H.264 fallback | `Session.cpp` |
| `qWarning() << socket` on a freed QTcpSocket crashed at double-`/quit` | Never stream QObject pointers into logs after teardown; minidump handler added | `HttpServer` / `CrashHandler` |
| Qt strips request-side `Connection: close` and pools the TLS socket ~120 s, wedging Sunshine's single-threaded HTTPS (47984) — other instances/clients get 502 timeouts | `clearConnectionCache()` after **every** Sunshine HTTPS request (applist, pair-check, box art, launch) | `ComputerManager` / `Session` |

---

[← Frontend](04-Frontend.md) · [Home](Home.md) · [Next: Security →](06-Security.md)
