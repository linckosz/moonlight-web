[← Conclusion](14-Conclusion.md) · **Client Presentation Benchmarks**

---

# 15. Client Presentation Benchmarks — renderer, Enhancer, HDR, 4:4:4

This chapter records the measurements that fixed the client-side defaults in September 2026: which surface presents the frame (Canvas2D, WebGL2, WebGPU, `<video>`), on which thread, which upscaler the Enhancer runs, how HDR is routed, and what 4:4:4 chroma can and cannot do in a browser. Every default named here was chosen against these numbers, not against a guess. Where a choice was *not* the fastest option, the reason is stated.

The numbers are reproducible only on the setups described; treat them as **ranking within a series**, never as absolute latencies of the product. The absolute values include the host's capture and encode, the network, the decode and the presentation, and they moved by a factor of four between a windowed test and a fullscreen one on the same machine.

## 15.1 How things were measured

Two instruments, both in the repo.

**Pipeline statistics** (the in-stream overlay, `PipelineDiag`): per frame, the time from the last fragment's arrival to `decode()` ("Handoff" — this is where a worker hand-over shows), the decoder's own time, the render call, and for WebGPU the wait on `onSubmittedWorkDone()`. Reported as average and p99. This is cheap and always available, but it stops **before the compositor**: it cannot see the vsync wait of a `<video>` element or the extra hop a WebGPU canvas may pay on the way to the screen.

**Click-to-photon probe** (debug builds only): the host draws a small flag on the primary screen the instant a mouse click is seen (`LatencyFlag.cpp`, a low-level mouse hook); the client, on the same machine, samples the presented pixels of the stream (`frontend/js/stream/LatencyProbe.js`, `mwLatency.run(clicks, spacingMs)` in the console) and measures click → flag visible in the stream. Ten clicks after one warm-up click, median and p90. The first click after a launch is always cold (35–40 ms) and is discarded. This measures the **whole chain including presentation**, which is exactly what the pipeline stats miss — it is what settled the `<video>` question. Two constraints learned the hard way: the tab must be visible (the probe runs on `requestAnimationFrame`), and a canvas presented with `desynchronized: true` reads back empty through `drawImage`, so the probe asks the renderer for its pixels instead (`probePixels`; on an `rgba16float` HDR canvas it decodes half floats).

Machines: **DualRTX** (Windows 11, 2× RTX 5060 Ti 16 GB, Chrome 152, M27Q 1440p 165 Hz), a MacBook (Chrome 126, macOS 10.15) for the Mac series, an iPhone for nothing in this chapter. Hosts: Sunshine on DualRTX, or the native host (`mw-native-host`, NVENC or AMF) on the same machine.

## 15.2 Decision 1 — Canvas2D on the main thread is the default

The starting point was a side-by-side by the author on a Mac, LAN, Sunshine stream: moonlight-qt ≈ 30 ms measured, MoonlightWeb 43.8 ms. The overlay attributed 22.8 ms of it to Handoff and 10.1 ms to WebGPU rendering. On Windows the same worker cost 0.1 ms of Handoff.

Windows matrix (Sunshine, local loop, low-motion content, ms, average render):

| Worker | Renderer | Render (avg) | Note |
|---|---|---|---|
| on | WebGPU | 12.2 | |
| on | Canvas2D | 8.1 | |
| off | WebGPU | 12.0 | render p99 17 ms = one vsync |
| off | Canvas2D | 8.2 | |

Mac, worker off + Canvas2D: 9.7 ms render, author's verdict "better than moonlight-qt". A later check (worker **on** but Canvas2D forced) showed Handoff 0.2 ms and render 0.2 ms: the macOS blockage is **WebGPU inside a worker**, not the worker itself.

Decisions:

- **Canvas2D by default.** WebGPU is only created when something needs it: an upscaler that has no other home, or HDR.
- **Video worker `auto` resolves to off everywhere.** It gains nothing measurable and becomes toxic on macOS as soon as WebGPU is involved. `video_worker: on` remains as an explicit option.
- **Enhancer off by default**, for the settings page and for guests alike: the plain Canvas2D path is the fastest first impression. (The backend's `videoEnhancement()` still answers `on` for a settings file that never stored the key; the frontend seeds and sends `off`.)

## 15.3 Decision 2 — upscalers without WebGPU

With WebGPU out of the default path, the Enhancer needed a home that is not WebGPU. `WebGlRenderer.js` (WebGL2, `desynchronized`, gather emulated with four `texelFetch`, no GPU wait after the draw) carries three ports: **FSR1** (EASU + RCAS, two passes), **SGSR1** (one pass), **NIS** (NVIDIA Image Scaling 1.0.3, MIT; the compute kernel ported to a fragment shader, its 64-phase filter banks in a 4×64 RGBA32F texture). A fourth entry, **Smooth2D**, is Canvas2D drawn at display size with `imageSmoothingQuality: 'high'`. Every algorithm also exists on WebGPU, so each pair can be compared like for like.

Render cost (overlay, Display 3, 1080p stream into a 1372×887 window — a *reduction*, ms avg / p99): Smooth2D 0.4 / 1.0 · FSR1 WebGL2 0.3 / 1.4 · SGSR1 WebGL2 0.4 / 1.4; the WebGPU pass-through on the same machine was 4.6 / 8.0.

Click-to-photon, **windowed** (native host AMF HEVC 720p165 → 1372×887, scale ×1.07, ms median / p90):

| Mode | Median | p90 |
|---|---|---|
| Off (Canvas2D) | 11.6 | 13.6 |
| SGSR1 WebGL2 | 13.9 | 15.6 |
| SGSR1 WebGPU | 15.7 | 18.5 |
| NIS WebGL2 | 15.1 | 16.9 |
| NIS WebGPU | 14.9 | 18.4 |
| FSR1 WebGL2 | 14.7 | 16.2 |
| FSR1 WebGPU | 14.0 | 17.5 |

Reading: any upscaler costs 2–4 ms of median on this machine; WebGL2 wins mostly at the p90 (1–2 ms) over its WebGPU twin.

Click-to-photon, **fullscreen** (same host, 720p → 2560×1440 = ×2, tab visible, the stream filming itself so the pipeline is loaded at 165 fps continuously, ms median / p90):

| Mode | Median | p90 |
|---|---|---|
| Off (Canvas2D) | 30.3 | 36.8 |
| Smooth2D | 35.0 | 48.3 |
| SGSR1 WebGL2 | 34.2 | 46.8 |
| SGSR1 WebGPU | 27.1 | 42.1 |
| NIS WebGL2 | 37.2 | 47.1 |
| NIS WebGPU | 26.8 | 36.6 |
| FSR1 WebGL2 | 34.6 | 41.2 |
| FSR1 WebGPU | 29.5 | 36.4 |

The hierarchy **inverts** fullscreen: WebGPU beats WebGL2 on all three pairs by 5–10 ms of median, and Canvas2D loses its lead. The dispersion of a fullscreen series is 20–40 ms on ten clicks, so this is a trend to confirm on real game content, not a verdict. It is the reason the WebGPU flavours are kept and reachable (the debug menu, and every HDR path).

Downscaling quality was checked separately (1440p and 2160p streams into a 2018×1135 canvas, ×0.79 and ×0.53): Off, Smooth2D, SGSR1, NIS and FSR1 all render tab-bar and taskbar text cleanly, no aliasing, no ringing. NIS outside its SDK range ([0.5; 1] for downscale) still renders correctly and logs one warning.

## 15.4 Decision 3 — one checkbox, "auto" picks the algorithm

In production the Enhancer is a single checkbox; the algorithm is always `auto`. The dropdown (Auto · Canvas2D · Video · Smooth2D · SGSR1 · NIS · FSR1, each in its WebGL2 and WebGPU flavour) exists **only in debug builds**; a Release build honours a value planted in `localStorage['mw-streaming-settings'].video_enhancement_algo`, nothing more.

`auto` resolves (`pickAutoEnhancer` in `BrowserDetect.js`):

| Client | SDR, Enhancer off | SDR, Enhancer on | HDR, Enhancer off | HDR, Enhancer on |
|---|---|---|---|---|
| Desktop | Canvas2D | FSR1 on WebGL2 | WebGPU linear (or `<video>`, see 15.5) | FSR1 on WebGPU |
| Mobile / tablet | Canvas2D | SGSR1 on WebGL2 | WebGPU linear (or `<video>`) | FSR1 on WebGPU |

Why FSR1 on desktop: in the windowed series it was the cheapest of the three WebGL2 upscalers at the median and second at the p90, and its two-pass sharpening gives the most legible text in the side-by-side captures. Why SGSR1 on mobile: one pass, the lightest shader, on GPUs where the difference between one and two passes is not noise. Why never WebGL2 for HDR: Chrome has no HDR surface for WebGL2 or Canvas2D (`drawingBufferStorage` exists, `configureHighDynamicRange` does not; an HDR frame imported into either is tone-mapped by the browser), so with HDR on, each WebGL2 algorithm is mapped to its WebGPU twin and Smooth2D becomes a plain WebGPU blit. The quality ladder degrades `fsr1 → nis → sgsr → off` on resolution steps.

## 15.5 Decision 4 — HDR routing

**The constraint that shapes everything**: on any canvas, `importExternalTexture` (WebGPU) and `drawImage` (Canvas2D) tone-map a PQ frame to SDR **on import**, before the application sees a pixel. The only way to keep the HDR signal on a canvas is to read the raw planes with `VideoFrame.copyTo()` and do the PQ math in the shader — and `copyTo` works only on **software-decoded** frames. Chrome ships a software decoder for AV1 (dav1d), none for HEVC. So true HDR on WebGPU means **AV1, software decoded**; true HDR with HEVC means the `<video>` element.

Four routes, chosen from the codec and the display (`_hdrMode` in `StreamView.js`; the OS toggle is live through `matchMedia('(dynamic-range: high)')`):

| Display | Codec | Mode | What happens |
|---|---|---|---|
| HDR | AV1 | `linear` | `rgba16float` canvas in extended tone-mapping mode, PQ → scene light with 203 nits = 1.0, BT.2020 → P3; the Enhancer runs in HDR on the float intermediates. True HDR. |
| HDR | HEVC | `sink` | `<video>` via `MediaStreamTrackGenerator`, hardware decode, true HDR, **no Enhancer** (the overlay says "OFF (HDR via video)"). |
| SDR | AV1 | `tonemap` | ACES HDR→SDR in the renderer's first pass, then the Enhancer on a normal SDR canvas. |
| SDR | HEVC | `browser` | Plain blit, the browser's own tone-map on import. The least good picture, and the only one left. |

The HDR checkbox is offered only when the client can honour it: HDR display **and** a WebGPU adapter **and** a 10-bit decoder (`hdrClientCapability()`); otherwise it is greyed with the reason ("HDR is not active on this device" or "browser cannot present HDR"), and the launch does not send `hdr_enabled`. `mw_hdr_request=1` in localStorage bypasses the guard for experiments; `mw_hdr_tonemap=1/0` forces or disables the readback.

Overlay measurements (Sunshine DualRTX 1080p60, M27Q in Windows HDR; render p99 / GPU wait / decode p99, ms):

| Configuration | Render p99 | GPU wait | Decode p99 |
|---|---|---|---|
| SDR Off, Canvas2D | 1.1 | 0.7 | 1 |
| SDR Auto, FSR1 WebGL2 | 1.0 | 0.5 | 2 |
| HDR Off, `linear` (AV1 software) | 24 | 8.5 | 31 |
| HDR Auto, FSR1 `linear` | 26 | 10 | 31 |
| HDR Off, `<video>` (HEVC hardware) | 0.3 | — | 4 |
| HDR→SDR, ACES Off | 25 | 8 | 32 |
| HDR→SDR, ACES + FSR1 | 26 | 9.5 | 31 |

On these numbers alone the `<video>` sink looks like the obvious HDR default (2.6 ms of visible latency against 18). It was the wrong conclusion, because the overlay stops before the compositor. Click-to-photon, same day, windowed 996×935 on the M27Q, 1080p60 20 Mbps stream of a near-static 1440p desktop (ms median / p90):

| Series | Configuration | Median | p90 |
|---|---|---|---|
| SDR, native host, HEVC 4:4:4 | Off, Canvas2D | 54.4 | 59.7 |
| | replayed at the end of the series | 54.3 | 54.5 |
| | Auto, FSR1 WebGL2 | 35.6 | 42.1 |
| | `<video>` sink | 90.0 | 104.5 |
| HDR, Sunshine, M27Q in HDR | HEVC 4:2:0, `<video>` sink | 79.9 | 101.4 |
| | AV1, WebGPU `linear`, Enhancer off | 45.6 | 48.8 |
| | AV1, FSR1 `linear` | 44.4 | 50.5 |
| | AV1, ACES Off (`mw_hdr_tonemap=1`) | 45.8 | 49.4 |
| | AV1, ACES + FSR1 | 50.0 | 53.4 |

Verdicts:

1. **The `<video>` element is the slowest presenter everywhere**, by 35–45 ms: it presents on the compositor's vsync and behind the generator's own queue. It replaces nothing. HDR Off stays WebGPU `linear` where AV1 is available; `<video>` remains the route for HEVC HDR because it is the only one that is HDR at all.
2. **HDR on WebGPU costs about 10 ms more than SDR on WebGL2**, software AV1 decode included. That is the price of true HDR in a browser today; nothing in the client can lower it until Chrome lets a hardware-decoded HDR frame reach a shader untouched.
3. **FSR1 does not tone-map** and does not need to: in `linear` mode it runs on scene-referred floats and the picture is right; ACES is required only when the display is SDR.
4. Canvas2D Off measured **slower** than FSR1 WebGL2 in this windowed SDR series (54 vs 36 ms), stable across two runs. It contradicts the earlier windowed series in 15.3 (Canvas2D fastest) and was not pursued: the perimeter was closed by the author after these runs. Anyone reopening it should measure fullscreen first.

Seen in passing: Sunshine streams an HDR desktop **washed out** when the session is SDR; the native host does not encode HDR yet (planned: FP16 capture → P010 → HEVC Main10 / AV1 10-bit), so the "HDR host, SDR client" case is real only on Sunshine.

## 15.6 Decision 5 — 4:4:4 chroma

Host side, 4:4:4 is decided **per codec** (`GpuInfo::codecs444`): NVENC answers H.264 and HEVC, never AV1 (no 4:4:4 in the AV1 SDK); AMF and oneVPL answer nothing. The selector takes the first codec the client offers that carries 4:4:4; if none does, it streams the first common codec in 4:2:0 and logs it. Before this, a 4:4:4 request on an AMD host killed the session at encoder init.

Client side, measured on Chrome 152 / Windows with an RTX 5060 Ti:

| Stream | Decoder | Result |
|---|---|---|
| HEVC 4:4:4 8-bit (`hvc1.4.158`) | hardware | Correct picture; the whole SDR series in 15.5 ran on it. |
| HEVC 4:4:4 10-bit (`hvc1.4.156`) | hardware, accepted | **Green screen** in `<video>` and in Canvas2D alike: the frames decode but cannot be presented. |
| AV1 4:4:4 | — | Not encodable on NVENC. |

Also validated end to end from the Mac (Chrome 126): `isConfigSupported` true for the RExt profile, frames delivered in BGRA. A bug found on the way and fixed: the frontend took profile 4 (RExt) for Main10 and configured an 8-bit 4:4:4 stream as BT.2020 + PQ, over-exposing the picture; the SPS bit depth now decides.

Decision: **4:4:4 is SDR-only.** HDR stays 4:2:0. Text legibility is the whole point of 4:4:4 (desktop use, code, spreadsheets), and it is not worth a 10-bit path that renders green on the most common client.

## 15.7 What ships, in one table

| Setting | Default | Chosen because |
|---|---|---|
| Renderer, SDR, Enhancer off | Canvas2D, main thread | 15.2: fastest first impression on Mac and Windows; no WebGPU device created. |
| Video worker `auto` | off | 15.2: no gain; WebGPU-in-worker is the macOS pathology. |
| Enhancer | off | 15.2; opt-in, one checkbox in production. |
| Enhancer `auto`, desktop | FSR1 on WebGL2 | 15.3–15.4: cheapest WebGL2 upscaler at the median, best text. |
| Enhancer `auto`, mobile/tablet | SGSR1 on WebGL2 | 15.4: one pass. |
| HDR checkbox | offered only if display + WebGPU + 10-bit decode | 15.5: an HDR stream a client cannot present is worse than SDR. |
| HDR presenter | WebGPU `linear` (AV1) / `<video>` (HEVC) | 15.5: `<video>` is 35–45 ms slower; kept only where it is the sole HDR route. |
| HDR on an SDR display | ACES in the shader (AV1) / browser tone-map (HEVC) | 15.5. |
| 4:4:4 | SDR only, per-codec on the host | 15.6: 10-bit 4:4:4 renders green on Chrome/Windows. |

Open, not planned: the Canvas2D-vs-FSR1 anomaly of 15.5 item 4; fullscreen WebGPU advantage of 15.3 to be confirmed on game content; the backend's `on` default for a never-stored `video_enhancement` key.

---

[← Conclusion](14-Conclusion.md) · [Home](Home.md)
