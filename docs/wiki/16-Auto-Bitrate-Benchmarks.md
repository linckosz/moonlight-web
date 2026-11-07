[← Client Presentation Benchmarks](15-Client-Presentation-Benchmarks.md) · **Auto Bitrate Benchmarks**

---

# 16. Auto Bitrate Benchmarks — picture quality and latency against bitrate

This chapter records the measurements made in November 2026 to set the **Auto** bitrate (`computeAutoBitrate()` in `frontend/js/util/AutoBitrate.js`). Until then Auto was a single rule: 20 Mbit/s at 1920×1080, 60 fps, scaled by pixel count and frame rate (so 9 Mbit/s at 720p60), ×1.5 for HDR or 4:4:4, ×0.5 on a touch device. The question was simple: **how much bitrate does the picture actually need**, for which content, and what does a higher bitrate cost in latency?

As in [chapter 15](15-Client-Presentation-Benchmarks.md), the numbers hold for the setups described. Read them as **curves within a series**: where quality stops improving, and how fast it falls below that.

## 16.1 How things were measured

### Content

The host shows `scripts/bench/bitrate/content/bench.html`, a page that draws one of several scenes, frame by frame, at the stream's exact resolution (device pixel ratio 1, no scaling):

| Scene | What it is | Why |
|---|---|---|
| **Still** | *Junk Shop* (a detailed 3D render), shown for 3 s | the first frame (a scene change, coded with no history) and the settled picture (after the encoder has refined it) |
| **Scroll** | a captured search-results page (text and thumbnails), scrolling continuously at 0.5 screen height per second, for 5 s | the hard case for a desktop stream: fine text moving every frame |
| **Scroll / stop** | the same page, scrolling 0.5 s then stopping 0.5 s, for 6 s | the common reading pattern: the picture has to sharpen again after each move |
| **Gears** *(main bench)* | a second detailed 3D render, 3 s | a still with other textures than *Junk Shop* |
| **Web page scroll** *(main bench)* | a text-heavy web page, laid out for each stream size, scrolling like the search page, 5 s | dense small text; the page differs between sizes, so this scene compares bitrates within a combination, not across them |
| **4K photo** *(main bench, 2160p only)* | a native 3840×2160 picture, 3 s | the two renders above are 2560×1440 sources, upscaled at 2160p |

Between scenes the page shows mid grey for 1 s. Every frame carries a **barcode** in its bottom-left corner: scene id (3 bits), frame number (13 bits), 8 check bits, over a complemented row. The page advances with the clock, not with `requestAnimationFrame` (Chrome on Windows runs rAF at the primary display's rate), so frame *n* of a scene is always the same picture at the same scroll position, whatever the bitrate.

### Recording

The client is a stock MoonlightWeb page (`webrtc-dc-udp`, WebCodecs decode, Canvas2D, video worker off, Enhancer off, SDR 4:2:0). A bench-only script injected over the DevTools protocol (`tap.js`) wraps `VideoDecoder`:

- it copies **every encoded chunk** the client hands to the decoder;
- it copies a few **decoded frames** (`VideoFrame.copyTo`, the hardware decoder's own planes), every 200th frame.

The recording starts on a keyframe the client asks for **after** the bench page is on the host's screen, and stops before the page closes. Three guards keep anything else out of a recording:

1. the bench window is kept on top of every other window, without taking the focus;
2. a watch polls the host's virtual screen during the whole stream and stops the run on any other window (class and program only are logged);
3. every saved frame must carry a valid barcode, checked in the page and again offline; a run where one does not is wiped at once.

### Scoring

The recorded stream is decoded offline by ffmpeg 9.0 and every frame is matched to its reference by its barcode. The reference is the same page frame rendered from the source pictures. All rows above the barcode band are scored:

- **VMAF NEG** (`vmaf_v0.6.1neg`) with motion forced to 0: the published model saturates at 100 on a scroll, where its motion feature dominates;
- **PSNR-Y**, **SSIM**, **MS-SSIM** (luma);
- LPIPS and FLIP are left for later.

The decoded hardware frames are compared to the ffmpeg frames at the same index (see §16.7).

### Machines

| Arm | Host (encoder) | Client (decoder) |
|---|---|---|
| A | bench-desk: native host, NVENC on an RTX 5060 Ti, virtual display | UM790 Pro: Chrome 156, Radeon 780M hardware decode |
| B | UM790 Pro: release v0.4.1, AMF on the Radeon 780M, virtual display | bench-desk: Chrome 156, RTX 5060 Ti hardware decode |
| Main bench | bench-desk: native host, NVENC on an RTX 5060 Ti, virtual display | bench-desk: Chrome 156 on the same machine, hardware decode |

Arms A and B on a 1 Gbit/s cable LAN; the main bench over the machine's own network stack (the scores do not depend on the client: they come from ffmpeg's decoding of the received stream). HEVC, constant settings except the bitrate (`stream_bitrate_auto: false`).

The combinations were chosen from the anonymous usage census: 1920×1080 at 60 fps (the most used by far) and 1280×720 at 60 fps for arms A and B; the main bench (§16.6) covers ten HEVC combinations taken in order of use, plus H.264 at 1080p60 and 720p60.

## 16.2 Arm A — NVENC, 1080p60

![1080p60 HEVC, NVENC](../screenshots/bitrate/a-1080p60.png)

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Still: first frame VMAF | Still: settled VMAF | Still: settled PSNR-Y | Scroll VMAF | Scroll PSNR-Y | Scroll MS-SSIM | Scroll/stop VMAF |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.5 | 1.02 | 7.5 | 84.9 | 39.99 | 57.7 | 25.09 | 0.9264 | 73.7 |
| 2 | 1.25 | 7.5 | 89.0 | 41.72 | 66.3 | 26.64 | 0.9456 | 79.8 |
| 3 | 1.89 | 11.9 | 92.0 | 43.59 | 77.0 | 29.27 | 0.9676 | 85.6 |
| 4 | 2.73 | 20.6 | 93.5 | 44.94 | 85.1 | 32.55 | 0.9816 | 89.2 |
| 6 | 4.02 | 34.4 | 94.0 | 45.65 | 89.7 | 35.88 | 0.9894 | 92.1 |
| 8 | 4.64 | 44.6 | 94.2 | 45.86 | 91.5 | 37.15 | 0.9923 | 93.3 |
| 10 | 5.52 | 51.5 | 94.3 | 45.97 | 92.7 | 38.48 | 0.9942 | 94.0 |
| 12 | 6.54 | 58.2 | 94.3 | 46.02 | 93.5 | 39.59 | 0.9955 | 94.5 |
| 15 | 8.02 | 64.6 | 94.3 | 46.12 | 94.3 | 41.01 | 0.9967 | 94.9 |
| 20 | 10.20 | 72.2 | 94.4 | 46.22 | 94.9 | 42.21 | 0.9975 | 95.3 |
| 30 | 14.38 | 81.0 | 94.3 | 46.31 | 95.5 | 43.75 | 0.9984 | 95.7 |

Reading:

- **Still picture:** the settled picture stops improving at **4–6 Mbit/s** (VMAF 93.5–94.0, then flat at 94.3). The first frame after a scene change is the costly one: its quality rises all the way to 30 Mbit/s, because the encoder's rate control spreads a keyframe-sized change over the frames that follow.
- **Scrolling text:** the curve rises steadily to 20 Mbit/s and keeps rising at 30 (PSNR-Y +1.5 dB). This is the content that justifies a high bitrate.
- **Scroll / stop:** in between; the picture sharpens back within the 0.5 s pauses.
- **The encoder does not use the bitrate it is given on this content.** While scrolling, NVENC spends about half the target (10.2 Mbit/s at a 20 Mbit/s target, 14.4 at 30).

## 16.3 Arm A — NVENC, 720p60

![720p60 HEVC, NVENC](../screenshots/bitrate/a-720p60.png)

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Still: first frame VMAF | Still: settled VMAF | Still: settled PSNR-Y | Scroll VMAF | Scroll PSNR-Y | Scroll MS-SSIM | Scroll/stop VMAF |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.75 | 0.47 | 5.3 | 83.3 | 38.60 | 49.3 | 23.07 | 0.9103 | 62.8 |
| 1 | 0.61 | 5.3 | 88.1 | 40.53 | 58.0 | 24.39 | 0.9321 | 71.4 |
| 1.5 | 0.93 | 7.7 | 91.7 | 42.85 | 71.3 | 26.99 | 0.9598 | 81.7 |
| 2 | 1.28 | 13.8 | 93.2 | 44.24 | 78.7 | 29.18 | 0.9732 | 86.7 |
| 3 | 1.87 | 28.2 | 93.9 | 45.17 | 86.3 | 32.66 | 0.9855 | 90.8 |
| 4 | 2.53 | 36.0 | 94.0 | 45.34 | 90.0 | 35.33 | 0.9913 | 92.6 |
| 5 | 3.09 | 42.9 | 94.1 | 45.43 | 91.8 | 37.42 | 0.9936 | 93.4 |
| 6 | 3.46 | 50.3 | 94.2 | 45.60 | 93.1 | 39.27 | 0.9954 | 94.0 |
| 8 | 3.75 | 58.8 | 94.3 | 45.72 | 94.2 | 41.17 | 0.9969 | 94.6 |
| 10 | 3.86 | 65.6 | 94.2 | 45.77 | 94.7 | 42.13 | 0.9975 | 95.1 |
| 15 | 4.03 | 76.2 | 94.2 | 45.89 | 95.6 | 43.48 | 0.9987 | 95.6 |

Reading:

- **Still picture:** settled at **3 Mbit/s** (VMAF 93.9).
- **Scrolling text:** most of the gain is reached at **6–8 Mbit/s** (VMAF 93.1–94.2); today's Auto (9 Mbit/s) sits on the plateau.
- **Above 8 Mbit/s the encoder stops spending:** 3.75 Mbit/s used while scrolling at an 8 Mbit/s target, 4.03 at 15.

## 16.4 Latency against bitrate

Arm A's setup, without `tap.js` (nothing copied in the client page). The host shows the repository's `scripts/bench/content/scroll.html?band=time` (text scrolling at 600 px/s, every frame stamped with the host clock), and the client page reads the stamp of every frame it draws (`mwContentAge`, calibrated on the host clock as in `scripts/bench/content-age/age.py`). Five bitrates per combination, two passes of 210 s each, the second in reverse order, so each pass covers a full phase cycle between the free-running displays.

- **Host capture:** the picture's age when the host captured it.
- **Decoded:** its age when the client finished decoding it.
- **Shown:** its age while it is on the client's screen, sampled at every refresh (120 Hz).

1920×1080, 60 fps:

| Set (Mbit/s) | Passes | Shown: median (ms) | Shown: p99 (ms) | Decoded: median (ms) | Host capture: median (ms) |
|---:|---:|---:|---:|---:|---:|
| 4 | 2 | 25.1 (24.7 / 25.6) | 37.0 | 15.9 | 7.0 |
| 8 | 2 | 23.2 (19.1 / 27.4) | 32.8 | 15.0 | 8.1 |
| 12 | 2 | 22.9 (24.0 / 21.8) | 32.0 | 14.9 | 9.8 |
| 20 | 2 | 22.5 (24.9 / 20.2) | 31.8 | 14.4 | 9.5 |
| 30 | 2 | 25.7 (27.2 / 24.3) | 34.7 | 17.6 | 10.9 |

1280×720, 60 fps:

| Set (Mbit/s) | Passes | Shown: median (ms) | Shown: p99 (ms) | Decoded: median (ms) | Host capture: median (ms) |
|---:|---:|---:|---:|---:|---:|
| 2 | 2 | 22.2 (18.1 / 26.4) | 31.9 | 13.9 | 7.1 |
| 4 | 2 | 23.9 (27.2 / 20.6) | 33.9 | 15.7 | 8.9 |
| 6 | 2 | 20.5 (20.5 / 20.6) | 30.9 | 11.2 | 6.8 |
| 9 | 2 | 22.9 (22.8 / 23.0) | 34.1 | 14.4 | 7.7 |
| 15 | 2 | 22.6 (22.9 / 22.2) | 33.6 | 14.0 | 7.5 |

Reading: **no effect of the bitrate on latency is measurable on a cable LAN.** The medians move by up to 8 ms between two passes at the same bitrate, more than between bitrates, and in no order. Between 2 and 30 Mbit/s, encoding, sending and decoding a larger frame costs well under a millisecond here. The bitrate matters for latency only when the link is too slow for it (for example Wi-Fi shared with other traffic), which these measurements do not cover.

## 16.5 Arm B — AMF on the Radeon 780M

The same bench in the other direction: the UM790 Pro runs the v0.4.1 release as the host (AMF), the bench-desk's Chrome is the client. Same scenes, same bitrates.

![1080p60 HEVC, NVENC against AMF, set bitrate](../screenshots/bitrate/ab-1080p60.png)

![720p60 HEVC, NVENC against AMF, set bitrate](../screenshots/bitrate/ab-720p60.png)

AMF, 1080p60:

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Still: first frame VMAF | Still: settled VMAF | Still: settled PSNR-Y | Scroll VMAF | Scroll PSNR-Y | Scroll MS-SSIM | Scroll/stop VMAF |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.5 | 1.79 | 31.5 | 93.0 | 44.26 | 82.9 | 31.47 | 0.9775 | 86.9 |
| 2 | 2.34 | 41.9 | 93.0 | 44.29 | 86.2 | 33.24 | 0.9833 | 89.2 |
| 3 | 3.43 | 54.9 | 93.0 | 44.33 | 89.9 | 35.88 | 0.9897 | 92.2 |
| 4 | 4.45 | 63.4 | 93.0 | 44.34 | 91.3 | 37.31 | 0.9921 | 92.8 |
| 6 | 6.49 | 73.2 | 93.0 | 44.40 | 93.4 | 39.72 | 0.9954 | 94.0 |
| 8 | 8.50 | 78.9 | 93.0 | 44.50 | 94.3 | 41.22 | 0.9968 | 94.6 |
| 10 | 10.47 | 82.5 | 93.1 | 44.58 | 94.6 | 41.82 | 0.9973 | 94.8 |
| 12 | 12.48 | 84.8 | 93.0 | 44.63 | 95.1 | 42.64 | 0.9979 | 95.1 |
| 15 | 15.02 | 87.3 | 93.1 | 44.75 | 95.4 | 43.56 | 0.9984 | 95.4 |
| 20 | 16.18 | 89.7 | 93.2 | 44.91 | 95.5 | 43.88 | 0.9986 | 95.6 |
| 30 | 16.70 | 92.0 | 93.7 | 45.70 | 95.7 | 44.10 | 0.9988 | 95.7 |

AMF, 720p60:

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Still: first frame VMAF | Still: settled VMAF | Still: settled PSNR-Y | Scroll VMAF | Scroll PSNR-Y | Scroll MS-SSIM | Scroll/stop VMAF |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.75 | 0.95 | 25.8 | 92.9 | 43.68 | 83.5 | 31.28 | 0.9797 | 86.1 |
| 1 | 1.24 | 33.4 | 92.6 | 41.83 | 86.6 | 32.94 | 0.9847 | 88.6 |
| 1.5 | 1.86 | 45.6 | 92.7 | 41.84 | 90.2 | 36.01 | 0.9905 | 91.2 |
| 2 | 2.24 | 55.7 | 93.0 | 43.83 | 92.1 | 39.07 | 0.9935 | 92.7 |
| 3 | 2.62 | 67.5 | 93.0 | 43.88 | 93.6 | 41.06 | 0.9960 | 94.0 |
| 4 | 2.72 | 74.4 | 93.0 | 43.96 | 94.3 | 41.85 | 0.9971 | 94.5 |
| 5 | 2.67 | 78.8 | 92.9 | 44.02 | 94.7 | 42.26 | 0.9977 | 94.8 |
| 6 | 2.82 | 82.0 | 93.0 | 44.12 | 95.0 | 42.59 | 0.9981 | 95.0 |
| 8 | 2.73 | 86.0 | 93.1 | 44.23 | 95.2 | 42.81 | 0.9985 | 95.3 |
| 10 | 2.76 | 88.3 | 93.0 | 44.32 | 95.3 | 42.96 | 0.9987 | 95.4 |
| 15 | 2.74 | 91.2 | 93.3 | 44.80 | 95.5 | 43.30 | 0.9990 | 95.6 |

**AMF spends the bitrate it is given; NVENC spends about half of it.** Plotted against the bitrate each scene actually used, the two encoders are close:

![1080p60 HEVC, against the bitrate used](../screenshots/bitrate/ab-1080p60-used.png)

![720p60 HEVC, against the bitrate used](../screenshots/bitrate/ab-720p60-used.png)

Reading:

- **1080p60:** from about 3 Mbit/s used upwards, NVENC and AMF are within about one VMAF point on the scrolling scenes; below that, AMF is ahead.
- **720p60:** AMF is ahead on the scrolling scenes at equal used bitrate, but stops spending at about 2.7 Mbit/s whatever the target (NVENC: about 4 Mbit/s).
- **Still picture:** AMF settles at VMAF 93.0 from the lowest bitrate on and stays there; NVENC goes up to 94.3. AMF's first frame after a scene change is much better at low bitrates (VMAF 31 against 8 at 1.5 Mbit/s in 1080p60).
- For the Auto rule this means that **one target bitrate does not give the same stream on every host**: the same setting costs AMF about twice what it costs NVENC.


## 16.6 Main bench — ten combinations, NVENC

The bench-desk alone (host and client on the same machine, see §16.1), the five scenes of the main bench, and for every combination **11 bitrates from ×0.075 to ×1.5 of today's Auto**, so the curves of different sizes can be laid side by side. All HEVC, NVENC.

### What each combination needs

The target bitrate (Mbit/s) at which a scene first reaches a VMAF NEG level, read on the curves (interpolated between the two measured points around it, on a log scale). For scale: today's Auto gives VMAF NEG 95.3 on the scrolling search page at 1080p60.

| Combination | Auto today | *Junk Shop* settled ≥ 93.5 | Search page scroll ≥ 93 | Search page scroll ≥ 95 | Scroll / stop ≥ 95 |
|---|---:|---:|---:|---:|---:|
| 1080p60 | 20 | 4.1 | 8.5 | 17.3 | 15.9 |
| 720p60 | 9 | 2.5 | 4.4 | 8.8 | 8.7 |
| 1366×768, 60 fps | 10 | 2.7 | 3.6 | 8.7 | 9.5 |
| 1080p120 | 40 | 7.2 | 7.4 | 16.3 | 17.6 |
| 720p30 | 4 | 1.2 | 3.8 | > 6.0 | > 6.0 |
| 1440p60 | 36 | 5.9 | 11.9 | 28.4 | 24.4 |
| 1920×1200, 60 fps | 22 | 4.3 | 7.9 | 17.1 | 18.1 |
| 1080p30 | 10 | 2.2 | 6.4 | 13.7 | 13.0 |
| 1440p120 | 71 | 11.5 | 12.1 | 23.6 | 25.6 |

Reading:

- **The frame rate barely matters.** At 1080p, the search page reaches VMAF 95 at 13.7 Mbit/s at 30 fps, 17.3 at 60 and 16.3 at 120; at 1440p, 28.4 at 60 and 23.6 at 120. The page scrolls at the same speed in pixels per second at every frame rate, so at 120 fps each frame moves half as far and costs less. A fit over the combinations gives, for VMAF 95, *needed bitrate ∝ pixels^0.8 × fps^0.05*. Today's Auto is linear in both: it gives twice the 60 fps value at 120 fps, where nothing is gained, and half of it at 30 fps, where the picture falls short (720p30: VMAF 95 is not reached even at ×1.5 of Auto).
- **The pixel count matters less than linearly** (exponent about 0.8): 1440p60 needs 1.6 times the bitrate of 1080p60 for the same quality, not 1.78.
- **Still pictures are cheap.** A settled still reaches its plateau at 1–12 Mbit/s depending on the combination, well below any scrolling scene. The first frame after a scene change is the exception: its quality rises with the bitrate all the way up, but it lasts a few frames (three to four at today's Auto in 1080p60).
- **The web page scene** is laid out for each size and is not comparable across combinations; at 1080p60 it is the hardest content measured (VMAF 90 at 16 Mbit/s).
- These are NVENC figures. AMF spends the whole target (§16.5), so the same target costs an AMF host about twice the bandwidth for a similar picture.

### Per combination

### 1080p60

![1080p60, NVENC](../screenshots/bitrate/m-1920x1080p60-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.5 | 0.93 | 7.5 | 84.7 | 80.7 | 64.3 | 72.8 | 44.6 | 26.24 |
| 2 | 1.22 | 7.4 | 89.2 | 89.7 | 74.1 | 78.6 | 51.9 | 28.41 |
| 3 | 1.63 | 11.9 | 92.0 | 92.9 | 82.4 | 85.4 | 64.0 | 31.01 |
| 4 | 2.29 | 21.1 | 93.5 | 94.1 | 88.2 | 88.7 | 72.2 | 34.23 |
| 6 | 3.56 | 34.4 | 94.0 | 94.6 | 91.6 | 91.8 | 80.3 | 37.53 |
| 8 | 4.33 | 44.8 | 94.2 | 94.7 | 92.8 | 93.2 | 83.2 | 38.85 |
| 10 | 4.71 | 52.1 | 94.2 | 94.7 | 93.5 | 94.3 | 87.3 | 39.64 |
| 12 | 5.67 | 58.2 | 94.3 | 94.8 | 94.1 | 94.5 | 87.9 | 40.79 |
| 15 | 6.96 | 64.9 | 94.3 | 94.8 | 94.7 | 94.9 | 89.7 | 41.98 |
| 20 | 9.36 | 72.3 | 94.3 | 94.8 | 95.3 | 95.4 | 90.8 | 43.53 |
| 30 | 12.91 | 81.0 | 94.3 | 94.8 | 95.7 | 95.8 | 93.1 | 44.28 |

</details>

### 720p60

![720p60, NVENC](../screenshots/bitrate/m-1280x720p60-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.68 | 0.39 | 5.3 | 78.1 | 75.3 | 51.4 | 58.7 | 39.9 | 23.27 |
| 0.9 | 0.50 | 5.3 | 84.7 | 79.2 | 59.1 | 67.3 | 50.6 | 24.54 |
| 1.4 | 0.83 | 7.5 | 91.6 | 91.6 | 74.9 | 80.4 | 69.5 | 27.95 |
| 1.8 | 1.08 | 11.1 | 92.6 | 92.9 | 81.8 | 85.1 | 78.1 | 30.16 |
| 2.7 | 1.58 | 23.4 | 93.7 | 94.2 | 88.6 | 89.9 | 86.7 | 34.16 |
| 3.6 | 2.15 | 32.5 | 94.0 | 94.4 | 91.6 | 92.3 | 90.4 | 37.46 |
| 4.5 | 2.63 | 39.0 | 94.1 | 94.6 | 93.1 | 92.9 | 92.5 | 40.02 |
| 5.4 | 2.87 | 46.4 | 94.2 | 94.6 | 94.1 | 94.0 | 92.9 | 41.64 |
| 6.8 | 2.96 | 53.9 | 94.2 | 94.7 | 94.5 | 94.2 | 94.2 | 42.38 |
| 9 | 3.02 | 62.8 | 94.3 | 94.7 | 95.0 | 95.1 | 94.5 | 43.21 |
| 14 | 3.07 | 74.6 | 94.3 | 94.8 | 95.6 | 95.5 | 95.3 | 43.93 |

</details>

### 1366×768, 60 fps

![1366×768, 60 fps, NVENC](../screenshots/bitrate/m-1366x768p60-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.75 | 0.49 | 5.8 | 78.2 | 76.8 | 60.5 | 71.3 | 47.5 | 24.91 |
| 1 | 0.66 | 5.8 | 85.6 | 81.1 | 70.5 | 77.5 | 58.2 | 26.92 |
| 1.5 | 1.01 | 5.8 | 91.1 | 91.8 | 82.3 | 84.5 | 72.2 | 30.55 |
| 2 | 1.43 | 13.0 | 92.8 | 93.4 | 88.4 | 87.9 | 79.7 | 34.20 |
| 3 | 2.16 | 24.8 | 93.7 | 94.3 | 92.2 | 91.3 | 88.4 | 38.36 |
| 4 | 2.60 | 33.6 | 94.1 | 94.5 | 93.5 | 92.7 | 91.0 | 40.83 |
| 5 | 2.79 | 41.3 | 94.1 | 94.6 | 94.3 | 93.6 | 92.0 | 42.32 |
| 6 | 2.91 | 48.1 | 94.2 | 94.7 | 94.4 | 94.0 | 92.9 | 42.60 |
| 7.5 | 2.94 | 54.7 | 94.2 | 94.7 | 94.8 | 94.6 | 93.5 | 43.09 |
| 10 | 2.96 | 63.7 | 94.3 | 94.8 | 95.2 | 95.1 | 94.0 | 43.58 |
| 15 | 2.97 | 74.1 | 94.3 | 94.8 | 95.6 | 95.6 | 94.5 | 44.08 |

</details>

### 1080p120

![1080p120, NVENC](../screenshots/bitrate/m-1920x1080p120-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 3 | 2.32 | 9.1 | 89.9 | 90.9 | 83.4 | 84.6 | 73.4 | 32.09 |
| 4 | 2.71 | 10.5 | 91.3 | 92.2 | 87.3 | 88.2 | 80.8 | 34.02 |
| 6 | 3.97 | 14.2 | 93.0 | 93.8 | 91.8 | 91.1 | 88.6 | 38.25 |
| 8 | 4.79 | 24.7 | 93.8 | 94.4 | 93.5 | 92.8 | 91.5 | 40.61 |
| 12 | 5.90 | 42.0 | 94.1 | 94.7 | 94.5 | 94.2 | 93.6 | 42.78 |
| 16 | 6.25 | 50.3 | 94.2 | 94.7 | 95.0 | 94.9 | 94.1 | 43.65 |
| 20 | 6.26 | 58.0 | 94.3 | 94.8 | 95.2 | 95.2 | 94.6 | 43.92 |
| 24 | 6.26 | 65.9 | 94.3 | 94.8 | 95.4 | 95.3 | 94.9 | 44.09 |
| 30 | 6.26 | 72.0 | 94.3 | 94.8 | 95.6 | 95.5 | 95.1 | 44.31 |
| 40 | 6.24 | 79.2 | 94.3 | 94.8 | 95.7 | 95.8 | 95.3 | 44.41 |
| 60 | 6.25 | 84.8 | 94.3 | 94.8 | 95.9 | 95.8 | 95.4 | 44.62 |

</details>

### 720p30

![720p30, NVENC](../screenshots/bitrate/m-1280x720p30-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.3 | 0.18 | 5.3 | 71.4 | 63.5 | 42.1 | 44.9 | 16.4 | 21.94 |
| 0.4 | 0.25 | 5.3 | 81.3 | 76.6 | 45.2 | 51.2 | 25.7 | 22.45 |
| 0.6 | 0.39 | 5.3 | 89.8 | 85.6 | 56.1 | 65.5 | 38.4 | 24.17 |
| 0.8 | 0.54 | 8.1 | 91.5 | 90.4 | 65.9 | 72.7 | 52.5 | 25.93 |
| 1.2 | 0.80 | 21.4 | 93.7 | 94.2 | 78.0 | 81.5 | 69.7 | 29.07 |
| 1.6 | 1.04 | 28.4 | 93.8 | 94.4 | 83.5 | 85.9 | 81.3 | 31.20 |
| 2 | 1.30 | 36.0 | 94.0 | 94.5 | 87.2 | 88.5 | 86.0 | 33.48 |
| 2.4 | 1.52 | 41.5 | 94.2 | 94.6 | 89.3 | 89.7 | 88.1 | 35.24 |
| 3 | 1.93 | 50.3 | 94.2 | 94.7 | 91.3 | 91.5 | 90.9 | 37.58 |
| 4 | 2.27 | 58.8 | 94.3 | 94.7 | 93.4 | 93.3 | 92.7 | 40.13 |
| 6 | 2.44 | 70.3 | 94.3 | 94.8 | 94.3 | 94.4 | 94.5 | 41.92 |

</details>

### 1440p60

![1440p60, NVENC](../screenshots/bitrate/m-2560x1440p60-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2.7 | 1.66 | 9.4 | 90.4 | 90.7 | 66.2 | 74.7 | 49.4 | 27.39 |
| 3.6 | 2.29 | 11.6 | 91.8 | 92.3 | 75.2 | 81.1 | 63.3 | 29.62 |
| 5.4 | 3.63 | 24.5 | 93.3 | 94.0 | 86.6 | 87.4 | 78.9 | 34.78 |
| 7.2 | 4.90 | 32.2 | 93.9 | 94.4 | 89.7 | 90.5 | 85.0 | 37.62 |
| 11 | 6.46 | 46.0 | 94.3 | 94.7 | 92.6 | 93.0 | 90.7 | 41.18 |
| 14 | 6.99 | 52.7 | 94.3 | 94.8 | 93.9 | 93.7 | 92.3 | 43.03 |
| 18 | 7.29 | 61.5 | 94.3 | 94.8 | 94.4 | 94.4 | 93.5 | 43.60 |
| 22 | 7.43 | 67.2 | 94.4 | 94.9 | 94.7 | 94.8 | 94.2 | 43.73 |
| 27 | 7.42 | 72.3 | 94.4 | 94.9 | 94.9 | 95.2 | 94.6 | 44.08 |
| 36 | 7.54 | 78.5 | 94.3 | 94.9 | 95.3 | 95.4 | 95.0 | 44.45 |
| 54 | 7.38 | 85.4 | 94.4 | 94.9 | 95.5 | 95.7 | 95.4 | 44.77 |

</details>

### 1920×1200, 60 fps

![1920×1200, 60 fps, NVENC](../screenshots/bitrate/m-1920x1200p60-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.6 | 1.12 | 7.7 | 85.3 | 81.6 | 66.0 | 70.5 | 40.6 | 26.67 |
| 2.2 | 1.43 | 7.7 | 89.7 | 90.5 | 72.9 | 79.1 | 57.2 | 28.28 |
| 3.3 | 2.14 | 12.2 | 92.4 | 93.3 | 83.4 | 85.9 | 71.5 | 31.78 |
| 4.4 | 2.98 | 22.8 | 93.6 | 94.2 | 88.5 | 88.8 | 79.6 | 35.15 |
| 6.6 | 4.45 | 35.9 | 94.2 | 94.7 | 91.9 | 92.0 | 88.5 | 39.14 |
| 8.8 | 5.24 | 45.6 | 94.3 | 94.7 | 93.6 | 93.1 | 90.5 | 41.51 |
| 11 | 5.73 | 52.6 | 94.3 | 94.8 | 94.2 | 94.4 | 92.6 | 42.71 |
| 13 | 5.86 | 58.8 | 94.4 | 94.8 | 94.5 | 94.4 | 93.0 | 43.13 |
| 16 | 5.98 | 64.7 | 94.4 | 94.9 | 94.9 | 94.8 | 93.8 | 43.63 |
| 22 | 6.03 | 73.5 | 94.4 | 94.9 | 95.3 | 95.3 | 94.7 | 44.04 |
| 33 | 6.08 | 81.4 | 94.4 | 94.8 | 95.6 | 95.7 | 95.1 | 44.55 |

</details>

### 1080p30

![1080p30, NVENC](../screenshots/bitrate/m-1920x1080p30-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.75 | 0.52 | 7.5 | 83.9 | 80.2 | 51.1 | 60.3 | 34.1 | 24.00 |
| 1 | 0.72 | 7.4 | 88.8 | 87.4 | 61.4 | 68.4 | 44.5 | 25.78 |
| 1.5 | 1.08 | 11.9 | 91.9 | 92.7 | 72.7 | 77.1 | 61.4 | 28.22 |
| 2 | 1.46 | 21.1 | 93.4 | 94.1 | 79.9 | 82.0 | 69.1 | 30.47 |
| 3 | 2.21 | 34.4 | 94.1 | 94.6 | 86.8 | 87.3 | 82.6 | 34.06 |
| 4 | 2.96 | 44.8 | 94.2 | 94.7 | 89.7 | 90.2 | 86.9 | 36.67 |
| 5 | 3.65 | 52.1 | 94.2 | 94.7 | 91.4 | 91.7 | 89.5 | 38.78 |
| 6 | 4.11 | 58.2 | 94.3 | 94.8 | 92.6 | 92.8 | 90.8 | 40.37 |
| 7.5 | 4.50 | 64.9 | 94.3 | 94.8 | 94.1 | 93.9 | 92.2 | 42.34 |
| 10 | 4.65 | 72.3 | 94.3 | 94.8 | 94.3 | 94.5 | 93.3 | 42.72 |
| 15 | 4.82 | 81.0 | 94.3 | 94.8 | 95.2 | 95.3 | 94.4 | 43.89 |

</details>

### 1440p120

![1440p120, NVENC](../screenshots/bitrate/m-2560x1440p120-hevc.png)

<details><summary>Table</summary>

VMAF NEG unless stated.

| Set (Mbit/s) | Used while scrolling (Mbit/s) | Junk Shop: first frame | Junk Shop: settled | Gears: settled | Search page scroll | Scroll/stop | Web page scroll | Search page scroll PSNR-Y |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 5.3 | 3.53 | 12.2 | 91.4 | 91.6 | 85.7 | 86.1 | 76.5 | 33.80 |
| 7.1 | 4.17 | 12.3 | 92.1 | 92.9 | 88.5 | 89.3 | 83.3 | 35.69 |
| 11 | 5.93 | 27.8 | 93.4 | 94.1 | 92.5 | 92.5 | 90.0 | 39.82 |
| 14 | 7.38 | 35.6 | 94.0 | 94.5 | 93.6 | 93.7 | 92.0 | 41.90 |
| 21 | 8.56 | 47.7 | 94.3 | 94.7 | 94.9 | 94.8 | 94.2 | 44.19 |
| 28 | 8.69 | 59.4 | 94.3 | 94.8 | 95.1 | 95.1 | 94.6 | 44.45 |
| 36 | 8.64 | 66.8 | 94.4 | 94.8 | 95.4 | 95.5 | 95.1 | 44.74 |
| 43 | 8.79 | 72.8 | 94.4 | 94.9 | 95.6 | 95.6 | 95.2 | 45.00 |
| 53 | 8.68 | 77.4 | 94.4 | 94.8 | 95.6 | 95.7 | 95.5 | 44.99 |
| 71 | 8.65 | 81.9 | 94.4 | 94.8 | 95.8 | 95.9 | 95.6 | 45.14 |
| 110 | 8.92 | 88.8 | 94.4 | 94.8 | 95.9 | 96.0 | 95.8 | 45.28 |

</details>


### Not measured yet

- **2160p60:** recorded at all 11 bitrates; the scoring was interrupted (the machine ran short of memory) and is to be finished.
- **H.264** (1080p60, 720p60): only the 1.5 Mbit/s point of 1080p60 was recorded before the bench stopped. Whether H.264 needs a higher Auto than HEVC is still open.

## 16.7 Hardware decode against the reference decoder

Every 200th decoded frame is copied from the client's hardware decoder and compared, byte for byte, with ffmpeg's decoding of the same recorded stream.

- **AMF streams (arm B), decoded by the RTX 5060 Ti:** identical in all 22 runs.
- **NVENC streams, decoded by the Radeon 780M (arm A):** identical in 8 of the 22 runs. In the other 14, one frame of the scrolling scene differs (frame 500, plus 700 or 900 once): up to 21 luma levels, on up to 0.7 % of the luma bytes, a few chroma bytes.
- **NVENC streams, decoded by an RTX on the bench-desk (main bench, §16.6):** the same pattern. The differing frames are always in a scrolling scene, never in a still one, never at the lowest bitrates; up to 36 luma levels, on 0.01–8 % of the luma bytes.

Two different hardware decoders disagree with ffmpeg on NVENC streams only, in scrolling content only, so the cause lies between what NVENC writes and how ffmpeg 9.0 reads it, not in one decoder. Which side follows the HEVC specification has not been checked (the HM reference decoder would settle it). The scores in this chapter are ffmpeg's; on the sampled frames, the difference is far below what moves a VMAF mean.

## 16.8 What this means for Auto

- On a cable LAN the bitrate costs no latency (§16.4): the reason to keep Auto low is the network's capacity (Wi-Fi, other traffic), not the delay.
- The quality needed for text that moves sets the bitrate; still pictures need far less.
- **The rule should scale with the pixel count to a power below 1 and hardly with the frame rate.** Today's linear rule is about right at 1080p60, too high at 120 fps (×2.5 at 1080p120, ×3 at 1440p120 for the same picture) and too low at 30 fps.
- The same target is spent differently by different encoders (NVENC about half, AMF all of it), so a target picked on one encoder is not a quality level on another.

The new rule itself is decided separately; this chapter keeps the measurements it rests on.



## 16.9 Reproducing

Tools: `scripts/bench/bitrate/` (see its `README.md`). The source pictures are not in the repository (the search page capture is not ours to publish); `prep.py` builds the scene pictures for every resolution from local files.

---

[← Client Presentation Benchmarks](15-Client-Presentation-Benchmarks.md) · [Home](Home.md)
