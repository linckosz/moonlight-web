[← Backend](03-Backend.md) · **Frontend** · [Next: Streaming & Transports →](05-Streaming-and-Transports.md)

---

# 4. Frontend (Vanilla JS)

The frontend is a **framework-less, build-less** web app served directly by the backend. ES6 modules provide structure; there is no bundler, no transpiler, no `node_modules` at runtime. Tooling (Prettier, ESLint, Vitest, tsc in advisory `checkJs` mode) exists only for development.

## 4.1 File architecture

```
frontend/
├── index.html                  # single page shell: header + #main-content + footer, PWA meta
├── manifest.webmanifest        # PWA manifest (installable, icons)
├── version.json                # deploy version — VersionGuard polls it to force-reload stale PWAs
├── css/
│   ├── tokens.css              # design tokens (Cyberpunk-2077-inspired theme)
│   ├── base.css / layout.css / components.css / stream.css
│   └── views/                  # per-view styles: admin, apps, hosts, login, settings
├── js/
│   ├── app.js                  # entry point, navigation, view/overlay orchestration
│   ├── api/
│   │   ├── BackendClient.js    # REST client (auth-aware: reloads on 401)
│   │   ├── WebRtcDataChannel.js# webrtc-dc transport client (frame assembly, IDR, stats)
│   │   └── WebRtcMedia.js      # webrtc-media transport client (RTP tracks, <video>)
│   ├── audio/
│   │   ├── AudioPipeline.js    # WebCodecs Opus decode → AudioWorklet playback
│   │   ├── audio-processor.js  # AudioWorkletProcessor: adaptive jitter buffer + WSOLA
│   │   ├── audio-decode-worker.js / opusWasm.js  # WASM Opus fallback decode path
│   │   └── iosAudioUnlock.js   # iOS silent-switch workaround (looping silent <audio>)
│   ├── stream/
│   │   ├── VideoDecodeWorker.js# opt-in OffscreenCanvas decode+render worker (mw_video_worker=1)
│   │   ├── JitterController.js # adaptive jitterBufferTarget (webrtc-media), AIMD control law
│   │   ├── GamepadManager.js   # Gamepad API → input DC (standard mapping, rumble)
│   │   └── renderers/          # VideoRenderer base + WebGpu / Canvas2D / VideoElement + factory
│   ├── ui/
│   │   ├── HostListView.js     # main view: host boxes with inline app grids
│   │   ├── StreamView.js       # the streaming overlay (largest module: decode, render, input, stats)
│   │   ├── StreamViewKeyboard.js / StreamViewTouch.js / StreamViewFullscreen.js
│   │   ├── AdminView.js / SettingsView.js / SetupView.js / LoginView.js
│   │   ├── PairDialog.js / Toast.js / icons.js
│   ├── models/                 # Host.js, App.js
│   ├── util/                   # Mp4Muxer (NAL/avcC/hvcC/codec strings), Av1Utils (OBU parse),
│   │                           # SdpUtils (forceOpusStereo), BrowserDetect, VersionGuard, escapeHtml
│   ├── i18n/i18n.js            # homemade runtime i18n (en/fr/zh, Tolgee-compatible JSON)
│   └── vendor/opus-decoder.min.js
├── locales/{en,fr,zh}.json     # ~279 keys, nested namespaces
├── test/                       # Vitest suites (jsdom) for the pure-logic modules
└── package.json                # dev tooling only (prettier, eslint, vitest, typescript)
```

## 4.2 Navigation model

From the `app.js` header comment — one main view plus overlays:

- **Main view** (with a history entry): `hosts` at `/` — host boxes carry their own app grids; apps launch directly from a host box.
- **Overlays** (no persistent history entry, guard `pushState`):
  - `admin` → URL `/admin` (survives refresh)
  - `settings` → URL `/settings`
  - `streaming` → no URL change (fullscreen `StreamView`)
- Switching overlays uses `replaceState`; the Back button/gesture closes an overlay instead of leaving the app.
- Guard states protect against accidental back-navigation during a stream.

Auth flow: `LoginView` is shown when the backend answers 401 (PIN entry or certificate-file upload); `BackendClient` force-reloads the page whenever a session expires or is revoked mid-use. On first run (macOS/Linux), `/setup` renders `SetupView` (config → live progress checklist → done).

## 4.3 StreamView — the streaming overlay

`ui/StreamView.js` owns the whole in-stream experience:

- **Transport client** — instantiates `WebRtcDataChannel` or `WebRtcMedia` per the `transport_chain` echoed by `/start`, and walks the chain (relaunch with `transport_index+1`) when a transport fails to establish. WSS mode reuses the DC framing over one socket.
- **Decode** — WebCodecs `VideoDecoder` fed Annex-B access units; `Mp4Muxer.js` extracts SPS/PPS(/VPS), builds `avcC`/`hvcC` descriptions and codec strings; `Av1Utils.js` does the AV1 sequence-header equivalent. An opt-in worker (`mw_video_worker=1`) moves decode+render to an OffscreenCanvas worker.
- **Render** — via the renderer abstraction (next section), sized with HiDPI awareness, fullscreen through `StreamViewFullscreen` (header button + CSS fallback for iOS).
- **Input** — keyboard (`StreamViewKeyboard`: Escape forwarded as a key; shortcuts on `Ctrl/Cmd+Alt+Shift` combos), mouse (pointer-lock "gaming mode" vs absolute; `_mouseFocused` model), touch trackpad (`StreamViewTouch`), gamepads (`GamepadManager`, polled per frame, change-only snapshots, rumble feedback), virtual keyboard on mobile (input-event diffing with a sentinel — the only reliable capture across iOS/Gboard), clipboard `paste` events.
- **Stats overlay** — FPS, bitrate, decode/render timings, RTT; **latency is displayed as a sum of measured legs** (host processing + ENet RTT/2 + client pipeline per frame) — never a cross-machine clock offset (which freezes on the offset error).
- **Degradation & resilience** — IDR request throttling with exponential backoff, frame-gap detection via `frameId`, decoder-error fallback, session teardown ordering (`webrtc.close()` **before** HTTP `/quit`; `_closed`/`_stopping` guards; 10 s grace period after a WS close when ICE is still connected — first launches often reconnect).

## 4.4 Renderers — why canvas *and* video

`stream/renderers/createRenderer.js` picks among three sinks behind one `VideoRenderer` interface:

| Renderer | Sink | When |
|---|---|---|
| `WebGpuRenderer` | `<canvas>` (WebGPU) | Preferred everywhere. Enables **Video Enhancement**: pass 0 blits `importExternalTexture(frame)` to a sampleable texture, then SGSRv1 (1 pass) or FSR1 (EASU+RCAS, 2 passes) upscale/sharpen to the canvas. `draw()` **must await `onSubmittedWorkDone()`** — without GPU backpressure the queue backlogs and latency explodes. |
| `Canvas2DRenderer` | `<canvas>` (2D) | Transparent fallback when WebGPU is unavailable or init fails (the WebGPU probe runs `requestAdapter/requestDevice` *before* `getContext('webgpu')`, leaving the canvas clean for 2D). |
| `VideoElementRenderer` | `<video>` via `MediaStreamTrackGenerator` | The **HDR path**. Canvas sampling (`importExternalTexture`/`drawImage`) is SDR-referred and tone-maps PQ away; a `<video>` element presents decoded frames in their native BT.2020+PQ color space and lets the browser composite HDR correctly. Same WebCodecs decode, only the sink swaps. |

The **webrtc-media transport** natively renders into a `<video>` element (RTP → browser decoder), which is why it is both the lowest-CPU path and incompatible with WebGPU enhancement — the transport chain reorders accordingly (see [Transports](05-Streaming-and-Transports.md)).

## 4.5 Audio pipeline

- moonlight-common-c delivers **encoded Opus** (never PCM); the backend forwards those bytes verbatim on every transport, so decode happens client-side.
- `AudioPipeline` decodes with the WebCodecs `AudioDecoder` (WASM `opus-decoder` fallback in a worker) and transfers Float32 PCM to `audio-processor.js` (AudioWorklet, real-time thread).
- The worklet implements an **adaptive jitter buffer** (~60 ms base, grows to ~160 ms on underruns/near-underruns, decays slowly) and optional **WSOLA time-stretch** (`audio_time_stretch`, default on) to absorb clock drift without latency.
- Stereo is forced in SDP (`stereo=1` via `SdpUtils.forceOpusStereo`) — browsers default their Opus answer to mono. **No gain is ever applied in JS** (volume issues are host-side sink issues).
- On the **webrtc-media** transports audio is a native RTP Opus track (browser-managed FEC/PLC — fixed micro-dropouts); WSS keeps the AudioPipeline path.
- iOS: sound on the built-in speaker with the silent switch on requires a looping silent `<audio>` element (`iosAudioUnlock.js`) to escape the *ambient* audio category.

## 4.6 i18n

`js/i18n/i18n.js` is a homemade ~zero-dependency runtime: loads `/locales/<lang>.json` (English always loaded as fallback), `t(key, vars)` with `{{var}}` interpolation, `applyDOM()` for `[data-i18n]`/`[data-i18n-attr]` static markup, language persisted in `localStorage` (`mw-lang`). The JSON is Tolgee-compatible (a self-hosted Tolgee docker-compose lives in `tolgee/`); `frontend/scripts/check-i18n.cjs` validates catalog completeness. See `docs/i18n.md`.

## 4.7 Quality tooling

| Tool | Role | Gate |
|---|---|---|
| Prettier + ESLint | format + lint (`npm run check`) | CI-blocking |
| Vitest (jsdom) | unit tests in `frontend/test/` | CI-blocking, **70% coverage gate scoped to pure-logic modules** |
| TypeScript (`tsconfig.json`, `checkJs`) | static analysis over JSDoc | advisory |
| `VersionGuard` | runtime: force-reloads a stale PWA after a deploy (never during a stream) | — |

---

[← Backend](03-Backend.md) · [Home](Home.md) · [Next: Streaming & Transports →](05-Streaming-and-Transports.md)
