[← Auto Bitrate Benchmarks](16-Auto-Bitrate-Benchmarks.md) · **Learned Auto Ceiling**

---

# 17. Learned Auto Ceiling — how many pixels a second Auto asks of each machine

When the resolution or the frame rate is left on **Auto**, the client asks for no more than a number of pixels a second. Until v0.4.2 that number was one budget for every machine, `PIXEL_RATE_BUDGET` in `frontend/js/util/StreamResolution.js`: 250 million, 1920×1080 at 120 fps. It protects an Intel N95, and costs a PC with a dedicated GPU, which could stream 4K at 120 fps.

Since v0.4.2 each browser **learns its own ceiling**, one rung at a time, by holding streams that really moved. A stream that does not hold costs a rung. The browser's own answer (`MediaCapabilities.decodingInfo`) stays as a guard on every launch.

| File | Role |
|---|---|
| `frontend/js/util/AutoCeiling.js` | The rungs, the per-device top, the memory in `localStorage`, the switch |
| `frontend/js/stream/AutoCeilingMeter.js` | Watches one stream: does it prove its rung, or is it sick? Decides nothing about the size |
| `frontend/js/stream/AutoCeilingSession.js` | One launch's dealings with the ceiling: read once at launch, the guard, what the meter says written to the memory, the console journal |
| `frontend/js/app.js` (`_startAutoCeiling`, `_guardAutoCeiling`) | Wiring: the device, the codec, the memory of this machine, the guard at launch |

## 17.1 Who learns

- **The native host only.** Sunshine, Apollo and Wolf cannot report their display's size and stream at most 1080p (see `stream_resolution` in [Settings Reference](07-Settings-Reference.md)), so there is nothing to climb to.
- **Only what Auto chose.** A launch where the viewer named both the resolution and the frame rate proves nothing and is not limited by the ceiling.
- **Per device:**

| Device | Highest rung |
|---|---|
| Computer | 1000 M (3840×2160 at 120 fps) |
| Phone or tablet (`IS_HANDHELD`) | 450 M (2560×1440 at 120 fps) |
| TV, a device of at most 1 GB (`IS_LOW_MEMORY`), or Power Saving on | 250 M: these learn nothing and stay on the base budget |

## 17.2 The rungs

`CEILING_RUNGS`, in pixels a second. No step is more than ×1.43, so a rung lost never costs much:

| Rung | Example of what it allows (16:9, 120 fps) |
|---|---|
| 250 M (base) | 1920×1080 (249 M) |
| 330 M | 2204×1240 (328 M) |
| 450 M | 2560×1440 (442 M) |
| 500 M | 2716×1528 (498 M), or 3840×2160 at 60 fps (498 M) |
| 700 M | 3214×1808 (697 M) |
| 1000 M | 3840×2160 (995 M) |

Above the base budget, the tallest box Auto asks for is 2160 lines (`AUTO_RAISED_MAX_HEIGHT`) instead of 1440: the budget alone then decides the size. The budget still pushes the Auto resolution down first (never under 1080 lines), then the Auto frame rate (never under 60 fps), as at the base budget.

## 17.3 Climbing

A still desktop sends few frames: no drop, no queue, a stream that looks healthy and proves nothing. So only **loaded time** counts towards a rung:

- at least 90 % of the frame rate asked for arrives, at the size asked for (at least 70 % of its area, and more than the rung below allows);
- not in the first 10 s (start-up traffic), nor while the tab is hidden, during a transition or a share (`neutral` ticks);
- the stream asked for more than the rung below allows, and no more than 2 % over its own rung (`provesRung`): a 720p stream says nothing of a rung of 450 M.

With that:

- **5 minutes** of loaded time at a rung on trial **confirms** it (`held`).
- When the ceiling limited the launch (the next rung would have streamed more: neither the screen nor the decoder stood first), **5 minutes** of loaded time **earns the next rung** (`earned`). It serves the **next launch**, never this one: resizing a virtual display mid-stream can kill a game.
- A step of the frame-rate stepper above the stream's own rate counts **for** the rung, never against it: what goes wrong up there is the step's, and the stepper takes it back. The stepper is bounded at twice the rung, and at most the top rung (`stepCeiling`).
- A rung earned is **on trial** until a stream has held it. A rung on trial writes nothing to the Enhancer's memory: a step down would follow the viewer back to a size where it means nothing.

## 17.4 Falling

One signal is enough to call a stream sick, and it stays sick to its end (`AutoCeilingMeter`):

| Signal | What it is |
|---|---|
| `drops` | frames thrown away for want of room (`queueFull`, `backpressure`) over 0.5 % of the frames, weighed over at least 60 s and 600 frames |
| `decode_rate` | the decode-rate governor asks the host for fewer frames |
| `congestion` | two sustained congestions in one stream (one proves nothing: a Wi-Fi does it with the machine at rest) |
| `decoder_rebuilt` | the decoder was torn down and rebuilt |
| `host_behind` | the host's display presents at the rate (≥ 90 %), yet under 85 % of it arrives, over 60 s |
| `decoder_silent`, `renderer_failed`, `enhancer`, `codec_fallback` | reported by the view: a silent decoder, a lost renderer, the Enhancer stepping down, a codec fallback (except a fallback for opaque HDR frames, which says nothing of the machine) |
| `worker_crash` | the host's stream worker crashed (see below) |

A sick stream that ran at its rung costs **one rung**, never under the base budget. The failure is counted at the rung that failed, and regaining it takes twice the hold for each failure there: 10, then 20, then 40 minutes at most (`HOLD_MAX_MS`). A rung that holds forgets its failures, and every failure is forgotten after **7 days** (`FAIL_TTL_MS`): a memory that keeps one bad session must not stay stuck.

Three other cases:

- **A stream that never came back.** When a stream proves its rung, the memory marks it open, and closes it when the stream ends. A page hidden or left closes it too: a tab killed from the background is no crash. Still open at the next launch: the browser or the machine crashed, and that launch starts one rung down.
- **The host refused the launch.** A rung on trial takes the blame: one rung down, and the launch is tried again. A rung already held is not at fault.
- **The host crashed** (`worker_crash`, from v0.4.4). A host short of memory crashes at any rung, so its crash says little of this machine. The first crash at a rung is only remembered, for 7 days. A second crash at the same rung, in another stream, costs the rung like any failure, and the count starts over. A host that really cannot carry a rung still brings its client down.

## 17.5 The guard: the browser's own answer

Before each launch above the base budget, the client asks the browser whether it can decode what the launch would stream (`decoderRefuses()` in `frontend/js/util/DecoderLimit.js`, `MediaCapabilities.decodingInfo`), the bitrate included, within 300 ms in all (`AUTO_CEILING_GUARD_MS`). On an outright "no", **this launch** comes down a rung and asks again. What was learned stays: the browser may say otherwise tomorrow. Any other answer, or no answer in time, changes nothing.

No codec stored in the settings counts as HEVC, as for the decoder's own limits.

## 17.6 What the viewer sees

- **Stats card:** under *Resolution*, a line **Auto** with the size Auto chose and the rung's state: `1440p120, learned` or `1440p120, on trial`. Only when Auto chose at least one half of the pair and the ceiling is above the base budget; otherwise no line.
- **Settings:** the preview of the Auto resolution uses the ceiling learned from the host launched last (`previewCeiling`), under what this device may reach.

## 17.7 Storage, the switch and the journal

Nothing goes to the server. The memory is one `localStorage` entry:

- `mw_auto_ceiling` on a direct address, where the origin is the machine;
- `mw_auto_ceiling:<rendezvous identifier>` through the rendezvous, where every machine shares one origin and every native host answers to one uuid (`autoCeilingKey`, `tunnelHostId()`).

Inside, one entry per host and per codec (holding 4K120 in HEVC says nothing of H.264), the 32 used last (`ENTRIES_KEPT`):

```json
{
  "v": 1,
  "last": "<host uuid>|hevc",
  "hosts": {
    "<host uuid>|hevc": {
      "ceiling": 450000000,
      "trial": false,
      "openAt": 0,
      "fails": { "500000000": { "n": 1, "at": 1794009600000 } },
      "crashes": { "450000000": 1794010200000 },
      "at": 1794010200000
    }
  }
}
```

`crashes` is only there while a first host crash is remembered. An entry written by hand is read snapped down to a rung, which is how the bench prepares one.

**The switch** is `mw_auto_learn` in `localStorage`:

| Value | Effect |
|---|---|
| absent, or `1` | learns (the default since v0.4.2) |
| `log` | measures and writes in the console what it would do; Auto stays on the base budget and nothing is stored |
| `0` | off: Auto stays on the base budget and nothing is measured |

Benches that need a fixed size set `mw_auto_learn=0` (`Driver.auto_learn` in `scripts/bench/acceptance/drive.py`) or name the size: once learning is on, a bench in Auto/Auto may climb without warning.

**The journal.** Every decision goes to the browser console, so that a descent can always be read back:

```
[MW] Auto ceiling: 450 M on hevc, on trial, asking 442 M, held back by it
[MW] Auto ceiling: 450 M held, 300 s loaded
[MW] Auto ceiling: 500 M earned for the next launch, 300 s loaded
[MW] Auto ceiling: sick (decode_rate), 12 s loaded, at 500 M — down to 450 M
[MW] Auto ceiling: sick (worker_crash), 40 s loaded, at 450 M — the host's first crash at it, not counted
[MW] Auto ceiling: the browser refuses 498 M — this launch runs at 450 M
[MW] Auto ceiling: the last stream at 450 M never came back — down to 330 M
[MW] Auto ceiling: stream over, 610 s loaded at 450 M
```

## 17.8 Benchmarks (November 2026)

The bench tool is `scripts/bench/auto-ceiling/` (`ceiling.py`, see its [README](../../scripts/bench/auto-ceiling/README.md)). It drives a stock client page over the DevTools protocol, prepares the memory before a run, can tell the page it has a 3840×2160 screen (`--screen`) so that a 1440p display can prove the top rungs, reads the journal back, and judges each run (`--expect hold | sick | journal | silent | any`). The content is a page that keeps moving, never a still desktop.

### Climbing to the top

**bench-desk** (native host, two RTX 5060 Ti: one encodes, the other decodes in Chrome on the same PC), HEVC, Canvas2D, `webrtc-dc-udp`, virtual display:

| Launch | Rung | Result |
|---|---|---|
| 1 to 6 | 250 → 330 → 450 → 500 → 700 → 1000 M | each launch earned the next rung after 313 s of loaded time, with no sign of sickness |
| 6 | 1000 M | held at 3840×2160, 120 fps, 135 Mbit/s; encoder at most 63 % busy, decoder at most 33 % |

Launches 1 to 4 ran mostly on a 240 fps step of the stepper, counted for the rung as designed.

### Falling on a congested link

A laptop (Apple M3) over Wi-Fi, from bench-desk: the first launch, 2224×1440 at 60 fps, earned 330 M after 354 s. The next one, 2904×1880 at 60 fps and 79 Mbit/s, fell sick (`congestion`) after 178 s: the host was dropping frames because the link did not drain. Back to 250 M, with one failure at 330 M (regaining it takes 10 minutes).

### Falling on a weak client

An **Intel N95** mini PC as the client (Chrome, 60 Hz screen, three virtual displays active, so a weaker client than a bare N95), the page told of a 3840×2160 screen, bench-desk as the host. `decodingInfo` answered *supported, smooth, power-efficient* at every rung: only the learning stopped it.

| Rung | Stream | Frames a second | Result |
|---|---|---|---|
| 500 M | 3840×2160 at 59 fps | 18 | `decode_rate` → 450 M |
| 450 M | 3668×2064 | 16–22 | sick → 330 M |
| 330 M | 3142×1768 | 20–28 | sick → 250 M |
| 250 M | 2560×1440 | 30–38 | sick, stays at 250 M: no failure counted at the base budget |

The memory ended at 250 M, with one failure at 330, 450 and 500 M.

An earlier attempt at 500 M fell for another reason: the bench machine ran out of memory because of unrelated processes, and the host's stream worker crashed. The client lost a rung for a fault that was not its own. Hence the rule of §17.4 for host crashes.

### Guard, journal, switch, rendezvous

- **Guard:** a real `decodingInfo` answer (HEVC, yes) at 450 M; a refusal simulated by the bench above 400 M brought the launch down to 330 M while the memory stayed at 450 M.
- **Journal only** and **off:** nothing stored, Auto on the base budget, the journal written or silent as expected.
- **Through the rendezvous:** a first launch at 1920×1080 earned 330 M after 301 s; the next one ran at 2204×1240 on trial. The memory was written under the machine's own key, and the key of the direct address stayed empty.

### With the Enhancer on

bench-desk again, the client's screen at 2560×1440 and 120 Hz, the Enhancer on (algorithm Auto, WebGL):

| Rung | Stream | Result |
|---|---|---|
| 330 M | 2204×1240, upscaled by MWSR to the screen | held for 300 s of loaded time, earned 450 M; MWSR never stepped down |
| 450 M | 2560×1440, the screen's own size | held for 301 s, learned |

### A mini PC as the client

A **UM790 Pro** (Radeon 780M) as the client, Chrome on a 1440p screen at 120 Hz, the page told of a 3840×2160 screen, bench-desk as the host, HEVC. `decodingInfo` answered *supported, smooth* at each active rung.

| Rung | Stream | Result |
|---|---|---|
| 250 M | 1920×1080 at 120 fps | held for 300 s, earned 330 M |
| 330 M | 2204×1240 | held for 300 s, earned 450 M |
| 450 M | 2560×1440 | held for 301 s, earned 500 M |

The stepper took each launch to 240 fps, and the 780M kept up.

### A TV as the client

An Android TV with 1 GB of memory, in its own browser, bench-desk as the host: a 150 s stream at 1280×720 and 30 fps. Nothing was stored and the journal stayed silent: a TV stays on the base budget, as §17.1 says.

### Not measured yet

- the UM790 Pro above 450 M, and the Enhancer above 450 M (where the stream fills a 1440p screen, there is nothing left to upscale).
