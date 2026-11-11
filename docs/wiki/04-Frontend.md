[← Backend](03-Backend.md) · **Frontend** · [Next: Streaming & Transports →](05-Streaming-and-Transports.md)

---

# 4. Frontend (Vanilla JS)

The frontend is a **framework-less, build-less** web app served directly by the backend. ES6 modules provide structure; there is no bundler, no transpiler, no `node_modules` at runtime. Tooling (Prettier, ESLint, Vitest, tsc in advisory `checkJs` mode) exists only for development.

## 4.1 File architecture

```
frontend/
├── index.html                  # single page shell: header + #main-content + footer, PWA meta
├── manifest.webmanifest        # PWA manifest (installable, icons)
│                               # /version.json is synthesised by the backend from
│                               # the app version — VersionGuard polls it to force-reload stale PWAs
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
│   │   ├── VideoDecodeWorker.js# OffscreenCanvas decode+render worker (video_worker: auto|on|off)
│   │   ├── JitterController.js # adaptive jitterBufferTarget (webrtc-media), AIMD control law
│   │   ├── FramePacer.js      # adaptive presentation reserve (DataChannel paths, mw_pacing)
│   │   ├── GamepadManager.js   # Gamepad API → input DC, read at 250 Hz (mapped pads, single-pad guests, rumble)
│   │   ├── gamepadMapping.js   # non-standard pads: user / built-in / Android / SDL DB resolution, device kind, remap capture
│   │   ├── gamepadDb.js        # GENERATED from SDL_GameControllerDB (npm run gamepad-db), loaded on demand
│   │   ├── TouchPadState.js    # on-screen controller: what is held, stick maths, layout, its settings (no DOM)
│   │   ├── LatencyProbe.js     # click→photon probe (debug builds, mwLatency.run())
│   │   └── renderers/          # VideoRenderer base + Canvas2D / WebGl / WebGpu / VideoElement + factory
│   ├── ui/
│   │   ├── HostListView.js     # main view: host boxes with inline app grids
│   │   ├── StreamView.js       # the streaming overlay (largest module: decode, render, input, stats)
│   │   ├── StreamViewKeyboard.js / StreamViewTouch.js / StreamViewFullscreen.js
│   │   ├── StreamViewTouchPad.js / TouchPadView.js  # on-screen controller: when it is offered and shown; its layer
│   │   ├── AdminView.js / SettingsView.js / SetupView.js / LoginView.js
│   │   ├── GamepadRemapDialog.js / GamepadArt.js / DeviceArt.js  # Test & Remap: wizard, drawn pad, radio, flight stick, wheel
│   │   ├── PairDialog.js / Toast.js / icons.js
│   ├── models/                 # Host.js, App.js
│   ├── util/                   # Mp4Muxer (NAL/avcC/hvcC/codec strings), Av1Utils (OBU parse),
│   │                           # SdpUtils (forceOpusStereo), AutoBitrate (recommended kbps),
│   │                           # BrowserDetect, VersionGuard, escapeHtml
│   ├── i18n/i18n.js            # homemade runtime i18n (en/fr/zh, Tolgee-compatible JSON)
│   └── vendor/opus-decoder.min.js
├── locales/{en,fr,zh}.json     # ~1,000 keys, nested namespaces
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

- **Transport client** — instantiates `WebRtcDataChannel` or `WebRtcMedia` per the `transport_chain` echoed by `/start`, and walks the chain (relaunch with `transport_index+1`) when a transport fails to establish — but *only* then: a launch that never returns a session is retried on the same index instead of costing a rung ([Streaming §5.1](05-Streaming-and-Transports.md)). WSS mode reuses the DC framing over one socket.
- **Two live views at once** — `app.js` owns a second `StreamView` (the standby stream slot) during a **seamless quality transition**: a quality/codec/transport change launches the other slot, and the display, input and audio are handed over on its first decoded frame, never through a loader. The outgoing view is retired with `quit({retire:true})` so the shared Sunshine app survives ([Streaming §5.6](05-Streaming-and-Transports.md#56-session-lifecycle--teardown-discipline)). The promoted view mounts its **own** `ShareMenu` in `activate()` — the retiring view carries its header away, and an owner who lost the Share button would be down to the host list's kebab to manage guests mid-stream.
- **Sharing** (`ui/ShareBoard.js`, launched by `ui/ShareMenu.js`) — one overlay, four rows (the owner and the three guests), reached from two places: the **Share** button in the stream header, and the **Share** entry in a host's kebab, where nothing is streaming and the board picks the app the invitation leads to. `ShareMenu` is only the button and its guest count. The board's overlay is listed in `StreamView.isLocalKeyboardTarget` — without that, typing a guest's name over a running stream would go to the host instead. While `hasActiveShare()` holds, the automatic quality ladder is suspended ([Backend §3.3](03-Backend.md)) and **Stop** first asks Leave (keep the app and the guests running, `keep_host_session`) or Stop everything. On a native host the board also carries the **guests' picture** — 720p, 1080p or 1440p, one for every row (`POST /api/share/feed`) — since its guests watch one shared feed; `PlayerJoinView` then states that height instead of offering its three quality buttons, and tests HEVC before joining (`hevcClientDecodes`, `util/BrowserDetect.js`): a browser without it joins in H.264 at once. A guest page already on the feed that gets a `feedcodec` notice (another guest made the feed H.264) leaves and rejoins through the codec fallback (`StreamView._followFeedCodec`).
- **Decode** — WebCodecs `VideoDecoder` fed Annex-B access units; `Mp4Muxer.js` extracts SPS/PPS(/VPS), builds `avcC`/`hvcC` descriptions and codec strings; `Av1Utils.js` does the AV1 sequence-header equivalent. Decode+render move to an OffscreenCanvas worker only when `video_worker` is `on` (`auto` resolves to off: measured no gain, and WebGPU inside a worker costs 22 ms of hand-off on macOS — [ch. 15](15-Client-Presentation-Benchmarks.md#152-decision-1--canvas2d-on-the-main-thread-is-the-default)).
- **Render** — via the renderer abstraction (next section), sized with HiDPI awareness, fullscreen through `StreamViewFullscreen` (header button + CSS fallback for iOS).
- **Input** — keyboard (`StreamViewKeyboard`: Escape forwarded as a key; shortcuts on `Ctrl+Alt+Shift` combos (`Cmd+Option+Ctrl` on a Mac) — `Q` quit, `X` fullscreen, `Z` release, `M` mouse mode, `I` stats overlay, `S` sound off/on (a toast says which, since fullscreen hides the volume control), `←`/`→` **move the host one desktop over**, translated into the chord that host's own OS uses for desktops (`HostOsProbe` tells us which), never forwarded as the arrow itself). The cheat-sheet itself lives in `util/shortcutsHelp.js`, shown twice: the slide that flashes over the stream for 5s, and the **last section of the settings page** — same rows, same device rule (touch → gestures, keyboard otherwise), so what the settings page teaches is what the stream obeys, mouse (pointer-lock "gaming mode" vs absolute; `_mouseFocused` model), touch (`StreamViewTouch`: trackpad model by default — 1 finger moves the cursor, 2 scroll, a 3-finger horizontal swipe at base zoom changes desktop on the host; **touch-screen mode** instead places the cursor where you touch and turns the 1-finger drag into an all-directions scroll, long press still grabs for drag & hold), gamepads (`GamepadManager`, polled every 4 ms, change-only snapshots, rumble feedback; a pad without a standard layout is read through `gamepadMapping.js` — the user's own layout, Chrome Android's pre-sorted one, or SDL_GameControllerDB — and one nothing maps is offered the remap wizard, `GamepadRemapDialog` over the SVG pad of `GamepadArt`; a share guest forwards one pad, as controller 0; on a touch screen, an on-screen controller, see [below](#on-screen-controller)), virtual keyboard on mobile (input-event diffing with a sentinel — the only reliable capture across iOS/Gboard; a letter or a digit typed while a modifier of its bar is latched (Shift, Ctrl, Alt or Win) goes as a key with its character, `chord: true`, so the host presses its own key for it — Ctrl+z is Z on an AZERTY host too), clipboard `paste` events.
- **Stats overlay** — FPS, bitrate, decode/render timings, RTT; **latency is displayed as a sum of measured legs** (host processing + ENet RTT/2 + client pipeline per frame) — never a cross-machine clock offset (which freezes on the offset error). Frame loss is split the way moonlight-qt splits it, because the two have different fixes: **network** (`frameId` gaps — frames that never arrived, i.e. packet loss, each costing an IDR round trip) vs **jitter** (frames that arrived and decoded, then lost their turn at the render stage — uneven arrival, not loss; this is what the `FramePacer` reserve drives down). Both are DataChannel/WSS only.
- **Degradation & resilience** — IDR request throttling with exponential backoff, frame-gap detection via `frameId`, decoder-error fallback, session teardown ordering (`webrtc.close()` **before** HTTP `/quit`; `_closed`/`_stopping` guards; 10 s grace period after a WS close when ICE is still connected — first launches often reconnect).

## 4.4 Renderers — why canvas *and* video

`stream/renderers/createRenderer.js` picks among four sinks behind one `VideoRenderer` interface. The choice is made in `StreamView` from three inputs — Enhancer on/off, HDR on/off, codec — and every default below was measured ([ch. 15](15-Client-Presentation-Benchmarks.md)):

| Renderer | Sink | When |
|---|---|---|
| `Canvas2DRenderer` | `<canvas>` (2D, `desynchronized`) | **The default**: SDR, Enhancer off (except on an entry-level Mali GPU, which presents on WebGPU: 53-55 frames a second of a 1080p stream where Canvas2D gave 30 on a Mali-G57). The fastest first impression on Mac and Windows, no GPU device created. Also the fallback when WebGL2/WebGPU are unavailable or fail to init (the WebGPU probe runs `requestAdapter/requestDevice` *before* `getContext('webgpu')`, leaving the canvas clean for 2D). |
| `WebGlRenderer` | `<canvas>` (WebGL2, `desynchronized`) | **SDR + Enhancer**: MWSR, MoonlightWeb's own (1 pass — `auto` everywhere but iPhone, iPad and the entry-level Malis), or FSR1 (EASU+RCAS, 2 passes), SGSR1 (1 pass) or NIS, ported to fragment shaders; no GPU wait after the draw. |
| `WebGpuRenderer` | `<canvas>` (WebGPU) | **Every HDR path** (Chrome has no HDR surface for WebGL2 or Canvas2D), MWSR on iPhone and iPad (WebKit's WebGL2 upload costs milliseconds of main thread per frame), and the WebGPU twins of the other upscalers (debug menu). Pass 0 imports the frame (`importExternalTexture`, or `copyTo` of the raw PQ planes for HDR), then the upscaler. `draw()` **must await `onSubmittedWorkDone()`** — without GPU backpressure the queue backlogs and latency explodes. |
| `VideoElementRenderer` | `<video>` via `MediaStreamTrackGenerator` | **HDR with the Enhancer off**, and every stream on a TV (hardware decode, no software path for `copyTo`). The slowest presenter measured (+35–45 ms click-to-photon: compositor vsync plus the generator's queue), meant only where it is the sole HDR route. **Known gap, to re-measure**: on Chromium, AV1 on an HDR display with the Enhancer off goes through it too (since `8093839b`), where the 04/09 click-to-photon run found the WebGPU `linear` path about 34 ms faster ([ch. 15 §15.5](15-Client-Presentation-Benchmarks.md#155-decision-4--hdr-routing)). Same WebCodecs decode, only the sink swaps. |

The **webrtc-media transport** natively renders into a `<video>` element (RTP → browser decoder), which is why it is both the lowest-CPU path and incompatible with WebGPU enhancement — the transport chain reorders accordingly (see [Transports](05-Streaming-and-Transports.md)).

## 4.5 Audio pipeline

- moonlight-common-c delivers **encoded Opus** (never PCM); the backend forwards those bytes verbatim on every transport, so decode happens client-side.
- On **both WebRTC transport families** (`webrtc-dc` *and* `webrtc-media`) audio is a native RTP Opus track handed to an `<audio>` element via `pc.ontrack` — the browser owns the jitter buffer, FEC and PLC (this is what fixed the periodic micro-dropouts of the old ordered audio DataChannel). Nothing below applies there.
- **`wss` only**: `AudioPipeline` decodes with the WebCodecs `AudioDecoder` (WASM `opus-decoder` fallback in a worker) and transfers Float32 PCM to `audio-processor.js` (AudioWorklet, real-time thread).
- That worklet implements an **adaptive jitter buffer** (~60 ms base, grows to ~160 ms on underruns/near-underruns, decays slowly) and optional **WSOLA time-stretch** (`audio_time_stretch`, default on) to absorb clock drift without latency.
- Stereo is forced in SDP (`stereo=1` via `SdpUtils.forceOpusStereo`) — browsers default their Opus answer to mono. The header's volume control (`audio/streamVolume.js`) sets the level; nothing else touches it (volume issues are host-side sink issues).
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

## 4.8 Controller compatibility

How a pad is read (`gamepadMapping.js`, first match wins): the user's own layout → the browser's `standard` mapping → a profile built in for a device SDL does not know (desktop, by USB vendor:product: EdgeTX/OpenTX radios, Logitech's G923 for Xbox) → Chrome Android's pre-sorted layout → SDL_GameControllerDB (desktop, by USB vendor:product) → nothing, and the remap wizard is offered. A pad Windows reports under a generic name ("HID-compliant game controller", e.g. an Xbox pad over Bluetooth) is named from its USB ids.

**Tested on 24/09/2026** — Windows 11, Chrome, Settings → Controllers then a stream, buttons, sticks and triggers checked; Firefox used as a second opinion on the failing cases.

| Controller | Connection / mode | Driver model | Result |
|---|---|---|---|
| Xbox One S Controller | Bluetooth | HID + XInput layer | ✅ |
| Switch Pro Controller | USB‑C | Nintendo HID protocol | ✅ |
| Switch Pro Controller | Bluetooth | Nintendo HID protocol | ❌ |
| GameSir X2 Lightning | — | — | ✅ |
| 8BitDo SN30 Pro | USB‑C (Xbox 360) | XInput | ✅ |
| 8BitDo SN30 Pro | Bluetooth, Start+X (Xbox One S) | HID + XInput layer | ✅ |
| 8BitDo SN30 Pro | Bluetooth, Start+A (PS4) | DInput (HID) | ✅ |
| 8BitDo SN30 Pro | Bluetooth, Start+B (8BitDo) | DInput (HID) | ✅ |
| 8BitDo SN30 Pro | Bluetooth, Start+Y (Switch Pro) | Nintendo HID protocol | ❌ |

**The Switch protocol over Bluetooth fails in the browser, not in MoonlightWeb** — the genuine Pro Controller and the 8BitDo in Switch mode alike. Chrome drives Nintendo pads through its own driver, which must initialize the pad first; over Bluetooth on Windows that fails, and the pad never reaches `navigator.getGamepads()` (a gamepad tester page shows nothing either, `chrome://device-log` stays empty, closing Steam changes nothing). Firefox does list it (`057e-2009-Wireless Gamepad`), buttons working but not the sticks. Over USB the same pad works. Rewriting the Switch protocol (WebHID) was ruled out: use USB, or another mode of the pad — XInput first, for rumble and no layout to guess.

To tell whether a failing pad is ours or the browser's: open a gamepad tester page in the same browser. If it sees nothing, neither can we. If it sees nothing while Windows does (`joy.cpl`), restart the browser: a Chrome left open for days, its update pending, once stopped seeing every HID device (01/10/2026).

### RC radios, flight sticks and wheels

They reach the game as every pad does, as an **Xbox 360 controller**: two sticks of 16 bits, two triggers of 8 bits (256 steps), 15 buttons and the d-pad. What a device has beyond that is not sent, and no force feedback comes back. Sending the device itself, every axis and force feedback included, is [§4.9](#49-wheels-and-radios-as-themselves).

| Device | Connection / mode | Laid out by | Read on |
|---|---|---|---|
| EdgeTX and OpenTX radios: Radiomaster TX12, TX16S, Zorro, Boxer, Pocket, MT12; Jumper; BetaFPV LiteRadio 3 Pro; FrSky and Flysky under EdgeTX | USB, "USB Joystick (HID)", **Classic** mode (the default) | a **built-in profile**, on plug-in (`1209:4f54`) | Radiomaster TX12, EdgeTX, Windows 11 + Chrome 154, 01/10/2026 |
| Logitech G923 for Xbox One and PC | USB, **G HUB** running on the client (it puts the wheel in its PC mode, `046d:c26e`) | a **built-in profile**, on plug-in | Windows 11 + Chrome 154, 02/10/2026 |
| Logitech G29 and G923 for PlayStation | USB (G29: selector on PS3) | the wizard, once, in a wheel's order (recognized as a wheel) | — |
| Flight sticks and HOTAS, other wheels and pedals; Ethos and DJI radios; EdgeTX in "Advanced" USB mode | USB | the wizard, once | — |

**The radio profile** reads Mode 2 in the default AETR channel order:

- yaw (CH4) → left stick X, throttle (CH3) → left stick Y, throttle up = stick up; roll (CH1) → right stick X, pitch (CH2) → right stick Y;
- CH5 → LT and CH6 → RT, over the whole axis: a switch at −100, 0, +100 gives 0, 128, 255. **A channel the radio's model leaves unmixed sits at 0, a trigger half pressed**: mix CH5 and CH6 to switches in the model, or lay them out again in the wizard;
- CH9-16 (the radio's buttons 0-7, pressed when the channel is above 0) → A, B, X, Y, LB, RB, Back, Start. CH7-8 go nowhere: an Xbox pad has six analog channels;
- Mode 1, another channel order or the Advanced mode: the wizard.

**A switch reaches the computer only through a channel.** A radio sends its channels and nothing else: a switch or a button its model does not mix onto a channel sends nothing, and neither the wizard nor any driver on the client can see it. A model made for the USB link often mixes the sticks only (CH1-4). Mix the rest in the model's **Mixes** page:

| Channel | Source, for instance | The game receives |
|---|---|---|
| CH5 | a 2-position switch (arming) | LT: 0 or full |
| CH6 | a 3-position switch (flight mode) | RT: 0, half or full |
| CH9 to CH16 | a switch or a button each | A, B, X, Y, LB, RB, Back, Start, pressed above 0 |

For a radio, **Test** says where its switches are read, and the wizard says which channel to mix a switch on when it stays silent for 4 s.

**The G923 for Xbox profile:**

- the rim → left stick X; accelerator → RT, brake → LT;
- right paddle → RB, left paddle → LB; A, B, X, Y, View → Back, Menu → Start, LSB, RSB, the Xbox button and the d-pad, where an Xbox pad has them;
- the clutch, + and −, the dial and its Enter, a shifter's gears go nowhere: an Xbox pad has no third trigger and no button left.

**Plugged in without G HUB, the G923 for Xbox stays an Xbox device** (`046d:c26d`) that neither the browser nor `joy.cpl` lists: install G HUB on the client.

**The device's kind** (gamepad, RC radio, flight stick, wheel) comes from the user's choice, then the USB ids, then the name (`edgetx`, `hotas`, `rudder`, `wheel`, `fanatec`…), a gamepad otherwise. Settings → Controllers shows it as an icon. **Test** and **Remap** draw that device, each control tagged with what the game gets (LT, A, LS…): a radio's gimbals and switches, a flight stick's grip, twist and throttle, a wheel's rim, paddles and pedals (the clutch greyed: it has no place on an Xbox pad). The drawn rim turns as far as the wheel on G HUB's default 900°; on a narrower range it turns further. The wizard asks in the device's words and order (yaw, throttle, roll, pitch; steering, accelerator, brake, paddles). The dialog's **Type** selector corrects a wrong guess, and is saved on its own.

**An axis without a spring** (a throttle, a pedal) rests at an end stop. The wizard reads its direction from that rest, not from the move, so a throttle parked down never comes out inverted. The rest is taken when the wizard starts: start it with the throttle down.

**Settings in the game**, so that it passes the device's travel through untouched:

| Game | Settings |
|---|---|
| Liftoff | calibrate the "Xbox 360" controller in its input settings, dead zones at 0 |
| Assetto Corsa Competizione | steering filter 0, speed sensitivity 0, steering linearity 1 |
| Automobilista 2 | controller filtering off |
| Forza Motorsport | steering "Simulation", axis dead zone inside 0, outside 100 |

**On the client, Logitech G HUB** — where the wheel is plugged in; on the host it never sees the wheel. Set the wheel's rotation angle there (the game sees a stick: the rim's full turn is the stick's full travel, which the game turns into the car's full lock — set the car's lock there, and the rim turns as the car's does), and turn on the centering spring: no force feedback comes back from the game, and without the spring the rim stays where it was left.

**What the game receives** shows on a Windows host in `joy.cpl`: a "Controller (XBOX 360 For Windows)", whose axes move with the rim, the pedals or the radio's sticks.

**Limits.** If the link goes silent for 3 s, or the wizard opens mid-game, sticks go back to the center and triggers to 0: a drone's throttle drops to mid-stick and the flight is lost, though arming on CH5 falls back too, which disarms; a wheel's pedals are released. A user's layout belongs to one browser (`localStorage`); built-in profiles need nothing.

### On-screen controller

An Xbox pad drawn over the stream (`TouchPadView`), for a viewer with fingers and no controller. It is one more source of `GamepadManager` (`attachVirtual`), not a sender of its own, so it gets the manager's anti-spam, repeat and pause for free. The host sees a controller like any other, and nothing changes on the server. Touches are sent at the touch event, without waiting for the 4 ms read.

**When it is offered.** `touchEnabled()` (`BrowserDetect`): a touch screen the OS has on, read from `maxTouchPoints` and `(any-pointer: coarse)` and followed live, not the type of device. Never on a TV. **Settings → On-screen controller** is there only in that case: Auto, Always or Never, size 80-140 %, opacity 20-100 %, vibration, and what a finger does outside the controls (mouse or nothing). These are per-device fields (`touch_pad_*`, [§7.4](07-Settings-Reference.md#74-browser-side-preferences-localstorage)).

**Auto** plugs it in at the start with no fine pointer and no pad connected, hidden until the first press, and unplugs it when a real pad arrives, which then takes controller 0 rather than player 2. **Always** plugs it in and shows it.

**Shown and plugged in are two things.** Hiding leaves it plugged in, because a game that loses its controller pauses. The header's button, the round button between View and Menu, or a 4-finger tap (3 fingers remain the keyboard) show or hide it. Plugged in by a press with no real pad forwarded, it gives way to one as Auto does; plugged in beside one, it is a second controller on purpose and stays (not for a guest, who has one place). A press of 600 ms on either button unplugs it. With it hidden in fullscreen, where there is no header, a small handle stays at the top of the picture. It hides during the soft keyboard and any dialog that listens to the pads, and everything held is released at that moment.

**Layout** (`layoutTouchPad`, pure). The places and proportions of GeForce NOW's controller, buttons at 15.8 % of the screen's short side, capped at 66 px, with the cross and the face buttons a little tighter: buttons high and outside, sticks low and inside, L3 and R3 in the bottom corners. It keeps out of the safe areas. Upright, View, the toggle and Menu go to the top of the screen, and the rest sits right above the sticks.

**Fingers.** Each is followed by its identifier and hit-tested against the layout, since a touch event stays with the element it began on: a finger can slide from A to B, or round the cross through its diagonals. The cross answers on a disc, so a corner of its box under a stick's ring remains the stick's. Its fingers are hidden from the trackpad's `touches` (`withoutOwnFingers`), or two thumbs and a third finger on the picture would read as a three-finger gesture.

**Sticks float.** A stick starts anywhere in its side's zone (42 % of the width, what the buttons leave). Its ring lands under the thumb and follows when the thumb goes past its radius, so turning back acts at once. Dead zone 8 %. Releasing eases what is drawn back in 0.25 s; the axes are at zero the moment the finger lifts. A double tap holds L3 or R3, on top of their buttons.

**Menu and Guide.** Start goes on release of Menu, not on press, and a press held 500 ms is Guide instead: otherwise reaching Guide would pause the game first.

**Vibration** (`navigator.vibrate`, Chrome on Android; iOS has none): a short tick on a press, and the game's rumble as pulses in proportion to its level, a phone's motor being all or nothing.

**A guest** has no settings page. The controller is the first entry of the pad picker in their dialog, even with no real pad to list, and their choice between it and a real pad is kept in `mw_player_prefs` for the next visit. They need the invitation's gamepad right, and it takes their one place, controller 0.

**Limit.** With it on screen, a real mouse does not click under the sticks' zones (84 % of the width). Auto does not show it to a viewer with a fine pointer.

## 4.9 Wheels and radios as themselves

**« As itself »** (Settings → Controllers, on a wheel's, radio's or stick's row) sends the real device instead of an Xbox pad: the page reads it through WebHID and **MoonlightWeb's own host** recreates it, under its own name and USB ids, every axis and button (the G923's clutch, the TX12's eight channels). Design and benches: [`hid-passthrough-study.md`](../design/hid-passthrough-study.md).

**The client** needs Chrome or Edge on a computer, nothing installed. The first switch-on opens the browser's chooser, narrowed to that model; afterwards it is a plain switch. Logitech G HUB may stay open on the client: the page and G HUB share the wheel. A wheel that speaks USB PID (Moza, Simucube, Fanatec…) gets its force feedback from a Mac or Linux client only: Chrome on Windows describes its reports without the PID collections that name them, so from a Windows client it reaches the game without force feedback.

**The host** decides:

| Host | As itself | Force feedback |
|---|---|---|
| **Linux**, MoonlightWeb's package | out of the box (`uhid`; `uinput` for a wheel with force feedback) | yes: G920, G923 for Xbox (Logitech HID++); HID PID wheels written, not yet tried on one |
| **Windows** (x64) | with the **"MoonlightWeb Virtual HID"** driver, which the installer sets up (below) | yes, same wheels (`pid.dll`) |
| **macOS** | no: macOS lets no app recreate a HID device without a DriverKit entitlement from Apple | — |
| Sunshine, Wolf, Apollo | no: the mapping (an Xbox 360 pad) | rumble only |

Where the host cannot, the switch opens a short explanation instead of the chooser, and the device keeps reaching the game as an Xbox pad. Logitech's G29, G27 and G923 for PlayStation are recreated, but their older force feedback commands (lg4ff) are not played yet. G HUB on the host never sees the wheel, recreated or not.

**Between two streams**, a Windows host keeps the recreated device for 10 minutes, at rest, still answering the game: a game left running keeps its wheel, and its force feedback, when the next stream sends the same device (same model, same browser-side description). The next page first receives the effects the game had set up, then the game's updates. A stream that does not send the device within 30 s lets it go; unplugging it, or switching **As itself** off, removes it at once. A game started before the wheel was ever there still needs one restart to get force feedback. Each stream has a worker process of its own: the device goes from the worker to the server and on to the next worker by its driver handle (`HidParking`, `HidParkingTransfer`). A Linux host removes the device with the stream, as before.

**Medal on a Windows host**: Medal (the clip recorder) opens the wheel when it notices a game starting, and resets its force feedback as it does. When Medal starts after the game has set its force feedback up, the game keeps its wheel and pedals but never sends force feedback again (ACC, bench of 11/10, `pid.dll`). Start Medal before the game, as it does when it starts with Windows; if the force is gone, restart the game. The loss is in `pid.dll`, inside the game: a USB PID wheel plugged into the PC likely meets it too (not tried).

### Windows: the driver

The installer sets **"MoonlightWeb Virtual HID"** up on x64 PCs, on install and on every update; uninstalling MoonlightWeb removes it, unless the other edition (DEV or release) still uses it. Nothing to do by hand: no test mode, Secure Boot stays on, anti-cheats see an ordinary driver.

It is a user-mode driver (UMDF 2, over Windows' own Virtual HID Framework) signed with **MoonlightWeb's own certificate** ("MoonlightWeb Virtual HID", code signing only, not a certificate authority) rather than a paid one. Windows takes such a package only from a publisher it trusts, so the installer's elevated step:

1. checks the three files of the package against SHA-256 hashes compiled into `MoonlightWeb.exe`, and the catalog's signer against the certificate's pinned thumbprint;
2. puts that certificate in the machine's **Root** and **Trusted Publishers** stores, installs the driver, then **takes the certificate out of both**: once installed, Windows loads the driver without it.

So no machine keeps our certificate, and it never vouches for anything but this install. On Windows on ARM the switch explains that the host cannot take the device: the driver is x64 only.

**Developers**: `backend\native-host\drivers\vhid\build.ps1` builds the package (Visual Studio Build Tools, the Windows Driver Kit from NuGet) and test-signs it by default; `install.ps1` puts a test-signed package on a machine in test mode (`bcdedit /set testsigning on`).

---

[← Backend](03-Backend.md) · [Home](Home.md) · [Next: Streaming & Transports →](05-Streaming-and-Transports.md)
