[← Frontend](04-Frontend.md) · **Streaming & Transports** · [Next: Security →](06-Security.md)

---

# 5. Streaming & Transports

This chapter explains how video, audio and input actually travel between Sunshine, the MoonlightWeb server and the browser — the heart of the project, and where most of the hard-won engineering lives.

## 5.1 The five transport modes

The same Sunshine-side stream (RTSP/RTP/ENet handled by `moonlight-common-c`) can be relayed to the browser in five ways:

| Mode | Backend relay | Browser path | Video sink | Codecs | Notes |
|---|---|---|---|---|---|
| `webrtc-dc-udp` | `DataChannelRelay` | SCTP DataChannels over UDP ICE | canvas (WebCodecs) | H.264/HEVC/AV1 | Default. Lowest latency with full codec choice |
| `webrtc-dc-tcp` | `DataChannelRelay` | same, ICE-TCP candidates | canvas | H.264/HEVC/AV1 | UDP-hostile networks |
| `webrtc-media-udp` | `MediaTrackRelay` | native RTP tracks | `<video>` (browser decoder) | **H.264 only** | Browser-managed jitter/FEC/PLC; true-HDR-capable sink |
| `webrtc-media-tcp` | `MediaTrackRelay` | same, ICE-TCP | `<video>` | H.264 only | |
| `wss` | `StreamRelay` | one WebSocket (TLS) | canvas | H.264/HEVC/AV1 | Always works (it rides the HTTPS port); worst latency profile |

### The fallback chain

`TransportPriorities::orderedTransports()` defines the auto order:

- **Video Enhancement OFF**: `webrtc-dc-udp` → `webrtc-dc-tcp` → `webrtc-media-udp` → `webrtc-media-tcp` → `wss`
- **Video Enhancement ON** (canvas required for WebGPU): `webrtc-dc-udp` → `webrtc-dc-tcp` → `wss` → `webrtc-media-udp` → `webrtc-media-tcp` (media kept only as a last resort, streaming *without* enhancement)

Rules applied when building the chain (`main.cpp`):

- HEVC/AV1 requests **skip webrtc-media** (H.264-only) — or, if a media mode is explicitly forced, the codec is overridden to H.264 and the response flags `codecOverridden` + `originalCodec`.
- Host codec support is checked against the canonical `SCM_MASK_*` values from `Limelight.h` (the GFE-era literals are wrong for Sunshine).
- A forced `-udp` mode promotes its `-tcp` sibling to second place (same family before switching).

**The browser owns the loop**: the `/start` response is sent *before* ICE connects, so only the client can observe a connection failure. The full `transport_chain` and current `transport_index` are echoed in the response; on failure the frontend relaunches with `transport_index + 1`. Manual transport selection precedes automatic fallback.

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
- **Codec bootstrap**: SPS/PPS(/VPS) are parsed browser-side (`Mp4Muxer.js` / `Av1Utils.js`) to build exact codec strings for `VideoDecoder.configure`; 4:4:4 chroma is opt-in and only offered for profiles the selected codec/browser advertises via the codec masks.
- **Debug**: `MW_FRAME_DUMP=1` dumps raw frames on both WS and WebRTC paths.

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
   DC/WSS: ordered audio channel → WebCodecs AudioDecoder → AudioWorklet (jitter buffer + WSOLA)
   media : native RTP Opus track → browser (FEC/PLC) → <audio>
```

- 5.1/stereo Opus; SDP is rewritten with `stereo=1` (browsers answer mono by default). Mobile clients may request 10 ms low-bandwidth frames (`low_audio`).
- The AudioWorklet jitter buffer adapts between ~60 and ~160 ms; WSOLA time-stretch (default on) absorbs clock drift pitch-free.
- `mute_host_audio` maps to GameStream `localAudioPlayMode` (host speakers muted by default while streaming).
- **Never apply gain in JS** — the historical "60% volume" issue was Sunshine's virtual sink volume on the host (host loopback is post-volume and volume keys act on it mid-stream); `HostAudioSink` normalizes it.

## 5.5 Input path

```
Browser events (kbd/mouse/touch/gamepad/clipboard)
  ─JSON over input DC (or WSS)─► backend InputEncoder (binary Limelight packets)
  ─InputCrypto AES-128-GCM─► EnetControlStream ─ENet─► Sunshine
```

- Mouse: relative (pointer-lock) or absolute; scroll, buttons. Touch: trackpad translation (1/2/3-finger gestures). Keyboard: raw key events, Escape is a normal key. Gamepads: `gamepadconnect`/`gamepad`/`gamepaddisconnect` snapshots + rumble back-channel.
- **Lock-key sync is host-authoritative**: on the first keydown the client sends `locksync`; the backend reads the *real* host state (`GetKeyState`, gated to host==backend) and reconciles — never a blind NumLock tap. The sync runs synchronously on the relay thread to preserve input ordering.
- Clipboard: bidirectional text via `paste` events + `ClipboardBridge` (host==backend only).

## 5.6 Session lifecycle & teardown discipline

Ordering rules that prevent whole bug classes (crashes, 504s, zombie sessions):

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
| Windows Schannel can't load ACME PEM keys | Force Qt TLS backend to OpenSSL at boot | `main.cpp` |
| mDNS port 5353 conflicts with Sunshine on the same machine | Never bind 5353 at boot; ephemeral scans only | `ComputerManager` |
| Virtual adapters (VPN) captured as mDNS source → 502 | `chooseBestMdnsAddress` picks the default-route subnet | `ComputerManager` |
| Sunshine HTTP wedges if polled during a stream | Poll suspension predicate while any relay is alive | `main.cpp` |
| Router-owned external port breaks WS URLs | Signaling URL anchored on `window.location.host` | frontend |
| GFE-era codec masks wrong for Sunshine | Use `SCM_MASK_*` from Limelight.h | `main.cpp` |
| Cross-clock latency display freezes on offset error | Latency = sum of measured legs only | `StreamView` stats |
| AV1 negotiation gaps | Forced AV1→H.264 fallback | `Session.cpp` |
| `qWarning() << socket` on a freed QTcpSocket crashed at double-`/quit` | Never stream QObject pointers into logs after teardown; minidump handler added | `HttpServer` / `CrashHandler` |

---

[← Frontend](04-Frontend.md) · [Home](Home.md) · [Next: Security →](06-Security.md)
