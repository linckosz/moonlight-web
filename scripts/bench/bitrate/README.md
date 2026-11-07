# The Auto bitrate bench

How much bitrate does the picture need? This bench streams a known page at a
list of bitrates, records exactly what the client received, and scores it
against the page's own frames. The results and how to read them are in the
wiki, chapter 16 (`docs/wiki/16-Auto-Bitrate-Benchmarks.md`).

## Pieces

| File | Role |
|---|---|
| `content/bench.html` | The page the host shows: still pictures and scrolling pages, drawn frame by frame at the stream's size, a barcode in every frame |
| `prep.py` | Builds the scene pictures for each stream size (`content/src/<W>x<H>/`) |
| `tap.js` | Injected in the client page: copies every encoded chunk and a few decoded frames, from a keyframe asked once the page is up |
| `collector.py` | Receives what `tap.js` sends at the end of a run (loopback only) |
| `driver.py` | One combination, a list of bitrates: settings, stream, content window, recording, checks |
| `main_bench.py` | Every combination, 11 bitrates each, from ×0.075 to ×1.5 of today's Auto |
| `latency.py` | Content age (host capture → client screen) at a few bitrates |
| `score.py`, `verify.py`, `score_all.py`, `score_bg.py` | ffmpeg decode, VMAF NEG / PSNR / SSIM / MS-SSIM per frame, hardware frames against ffmpeg; `score_bg.py` runs it all at Idle priority with no window |
| `selftest.py` | Offline check of the scoring on a synthetic x265 run |
| `plot.py`, `plot_main.py`, `wikitable.py`, `ltable.py`, `synth.py` | Curves, wiki tables, and the bitrate each quality target needs |

## Guards

The stream captures a virtual display that becomes the primary screen, so
anything that opens on the desk during a run would be recorded. Three guards:

1. the content window is kept topmost without taking the focus;
2. a watch lists the windows on the virtual screen during the whole stream
   (class and program only) and stops the run on any other window; the run is
   wiped and redone a minute later, three times at most
   (`--allow-nvidia-overlay` and `--allow-handwriting-canvas` accept two known
   transparent system windows);
3. every frame copied must carry a valid barcode, checked in the page and
   again offline.

No script clicks or types on the desk, and `nowin.py` keeps every child
process from opening a console window. A file named `PAUSE` next to the
scripts stops the bench and the scoring.

## Running it

Needs: a `--dev` instance with `--autostart` on the encoding machine, a client
Chrome with its own profile and a DevTools port, Python with numpy and Pillow,
ffmpeg with libvmaf, matplotlib for the plots.

```bash
set MW_BENCH_HOST_URL=https://<host>:8443/
set MW_BENCH_PAGES=<folder with the page captures>      # google-1440.png, web-*.png
set MW_BENCH_CP4K=<a native 3840x2160 still>
python prep.py 1920x1080 1280x720
python -m http.server 18470 --bind 127.0.0.1 --directory content
python collector.py runs/M
python main_bench.py --deadline 06:00                    # or driver.py for one combination
python score_bg.py runs/M
python plot_main.py && python synth.py --md
```

The page captures are not in the repository: a search results page is not
ours to publish.
