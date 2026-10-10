# Click → photon without a camera (POC Ultra U0.4)

The same measure for any streaming client, MoonlightWeb or Steam Remote Play
(HEVC or PyroWave): a click on the client, timed until its answer shows on the
client's own screen.

- `flag-window.ps1`, on the **host**: a window over a whole screen that flips
  black ↔ white on every mouse button press. Optional `-Log` writes each
  flip's host time.
- `click-photon.ps1`, on the **client**: finds the streaming client's window by
  process (`streaming_client` for Steam, `chrome` with `-Title` for
  MoonlightWeb), clicks at its centre (SendInput) and reads that pixel back
  from the composed desktop until it flips. 60 clicks, 700 ms apart by
  default; median, p90, misses, every sample in `-Out <json>`.

What a sample includes: the click's way up, the flip on the host, capture,
encode, network, decode, the client's present and the DWM's composition, plus
up to one pixel read. A `GetPixel` on the screen waits for the DWM's next
composition (8.3 ms at 120 Hz, measured on the UM790Pro on 10/10/2026), so a
change is seen within one composition: an onset is a composition's time, give
or take under a millisecond. It leaves out the screen's scan-out and
response, the same for every client on one screen. GDI reads the DWM's last
composed frame: right for a windowed client, unproven for an exclusive
full-screen one. Keep the client windowed.

## Steam Remote Play (U0.4)

1. Both machines: Steam → Settings → Interface → Client Beta Participation →
   "Steam Beta Update", restart Steam. The same Steam account on both.
2. Host: add `flag-window.ps1` as a non-Steam game. Target `powershell.exe`,
   launch options `-NoProfile -ExecutionPolicy Bypass -File "<path>\flag-window.ps1"`.
3. Client: Settings → Remote Play → Advanced Client Options → Pyrowave on
   (off for HEVC), and "Display performance information" for Steam's own
   readings. Stream the flag "game" from the client's library.
4. Client: `powershell -NoProfile -File click-photon.ps1 -Process streaming_client -Out steam-pyrowave-1.json`,
   mouse untouched. Alternate HEVC and PyroWave, two runs each.
5. For MoonlightWeb on the same pair: stream the host's screen with the flag
   window on it, then `click-photon.ps1 -Process chrome -Title MoonlightWeb`.

The plan's gate: if native PyroWave does not beat Steam's HEVC by 2 ms or
more on NVIDIA, a browser port will not.

## Click → sound (plan audio + DSCP, A1)

`click-sound.cpp` (`build-click-sound.bat` → `mw-click-sound.exe`, static, no
install) times the sound the way `click-photon.ps1` times the picture, on a
Windows client, with no camera and no microphone.

- **Host**: the latency flag on (`latency_flag_enabled`), and the server
  started with `MW_LATENCY_FLAG_SOUND=click`. Each injected click then also
  plays 20 ms of 1 kHz on the host's default output (`LatencyBeep`), the one
  the stream captures. `tick` instead: beep and flag together every 500 ms,
  without a click. The server's log dates each beep (asked, and into the output
  buffer behind how many queued frames) on the relay's steady clock; with
  `audiolog=1` the beep shows in the host's own capture as the `peak` column.
- **Client**: the page plays sound (not a `--mute-audio` kiosk), the cursor
  rests on the stream, then `mw-click-sound --clicks 30 --window MoonlightWeb --out a.json`.
  It clicks (SendInput), hears its own output through WASAPI loopback (each
  packet carries the QPC time of its first sample) and watches one pixel of the
  composed desktop for the host's flag: `--flag X,Y` names a point of its left,
  pure blue band, or `--window TITLE` finds the stream's window and looks for
  the flag in it by its colours (a run of pure blue, then white, then red),
  wherever the page puts the stream (tabs, toolbars, letterboxing).
  Blue, not a change of brightness: a bench page scrolling under the flag is
  never that blue. Per click: click → sound, click → flag, sound − flag (the
  lip-sync offset). `--tick SECS` sends no click and pairs each beep it hears
  with the nearest flag: the offset alone, against a host in `tick`, or while
  something else clicks (a `series.py` pass, whose page clicks raise the flag
  and the beep).
- `--center` puts the cursor in the middle of `--window`'s client area first;
  `--warmup N` clicks N times before the measured clicks (a page that captures
  the pointer takes its first click for that). Each click's line carries its
  QPC time in µs (`click 1 at <µs> ... flag <ms>`): with the page's time origin
  on QPC (`MW_BENCH_TICKS_ORIGIN=1`) and its frame log, each click is followed
  from the client's SendInput to its flag on the screen (POC Ultra U3.7, the end
  of the chain: clicks from the client's own OS, the page's probe idle, so no
  canvas readback sits on the frame it measures). Without a beep on the host,
  `--timeout 600` keeps an 800 ms interval: each click waits that long for a
  sound that never comes.
- Over ssh, `run-click-sound.ps1 -Tag t -Arguments '--tick 120 --window MoonlightWeb'`
  runs it in the console session (a scheduled task), where the desktop, the
  clicks and the user's audio are.
- The loopback hears the stream with the output muted: the bench needs no
  audible sound (checked on DualRTX, 05/10/2026).
- `--wav FILE` keeps everything the client played (mono, 16-bit, silences
  filled on the QPC clock; `wav starts at <µs>` is printed), to see what became
  of a beep the threshold did not catch.
- On the N95 in loaded Wi-Fi (05/10/2026), 17 beeps of 60 were heard, at full
  level when heard, while NetEq held 380 ms: a pure tone may be what its time
  stretching shortens when it catches up. `MW_LATENCY_BEEP=noise` on the host
  plays white noise instead, to compare. 06/10: no better (tone 19 of 60,
  noise 12 of 60, NetEq at 300-330 ms), while the host captured and sent all
  60 and the client's WAV holds only the heard ones, at full level: the
  missing beeps are not played at all. The page now logs the packets NetEq
  discarded, its flushes and the energy it played (`mwAudio.csv()`), to place
  the loss.
- `sound_offset.py <listener output>` pairs the streamed `flag`/`beep` onsets
  again with a wider window (-100 to +900 ms): the tool's own ±250 ms misses
  a sound that trails its picture by a grown jitter buffer.
- **10/10/2026: those missing beeps were the listener's.** Its 20 ms
  loopback buffer overran on the N95 and one WAV write a sample starved its
  thread: it lost 106-137 s of output in 5 minutes. With 200 ms, one write a
  packet and a time-critical capture thread, it heard 60/60 and 58/60 beeps,
  with no discontinuity. Check the `loopback:` line of every run: a
  discontinuity means the tool, not the client, lost sound.
- Where a missing beep went (10/10/2026): every run prints `gap <µs> <len>`
  (no loopback packet for over 2 ms: no stream rendering, an output stopped on
  silence) and `silent <µs> <len>` (packets the mixer flagged as digital
  silence, as between beeps), and
  counts the audio engine's discontinuities and skipped frames.
  `sound_offset.py` puts each flag without a beep at its flag plus the median
  offset and says whether it fell in a gap, in a silent run, or in sound the
  browser played as silence. The page's `mwAudio.csv()` adds Chrome's
  `media-playout` side: the ms the device took each second, those the browser
  made up at the output, and the output's delay.
- **Counted**: the click's send to this machine's mixer (the loopback tap): the
  way up, the host's input, its beep through its mixer and loopback capture,
  the pacer, Opus, the relay, the network, the browser's jitter buffer and
  audio output. **Not counted**: the DAC, the speakers, a Bluetooth headset
  (100-200 ms more with SBC/AAC).
- Never on the host itself: a client there plays the stream back into the
  capture.
