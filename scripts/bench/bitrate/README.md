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
| `fmtref.py` | References past SDR 4:2:0: full chroma (4:4:4) and BT.2020 PQ (HDR), with the highlights of the `hdrpeak` scenes |
| `hdrcal.py` | Reads back the calibration bands of an HDR run: how much light the stream really carried, and the white level the scoring applies (`cal.json`) |
| `factors.py`, `hfactor.py` | H.264, 4:4:4 and HDR against HEVC 4:2:0; HDR with highlights against the same renders in SDR |
| `test_hdrpeak.py`, `test_fmt_hdr.py`, `test_dimguard.py` | Offline checks: the HDR scoring and calibration on x265 encodes, the dimmed-screen guard on synthetic and recorded frames |

## Guards

The stream captures a virtual display that becomes the primary screen, so
anything that opens on the desk during a run would be recorded. Four guards:

1. the content window is kept topmost without taking the focus;
2. a watch lists the windows on the virtual screen during the whole stream
   (class and program only) and stops the run on any other window; the run is
   wiped and redone a minute later, three times at most
   (`--allow-nvidia-overlay` and `--allow-handwriting-canvas` accept two known
   transparent system windows);
3. every frame copied must carry a valid barcode, checked in the page and
   again offline;
4. in SDR, the barcode's white cells must come out white (luma 225 or more):
   a system prompt that the window watch does not see can dim the whole
   screen behind it, and such a run is wiped and redone like a covered one.

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

`--content-luid` (or `MW_BENCH_CONTENT_LUID`) pins the content Chrome to the
GPU that encodes, by its adapter LUID.

### HDR with highlights

The host's virtual display in HDR mode, the client asking for HDR. The
`hdrpeak` scenes draw the renders into an extended-range canvas with their
highlights at `--hdr-peak` times the SDR white; `--hdr-gain` brings Chrome's
canvas white onto the desktop's SDR white (`hdrcal.py` measures it on the
30 Mbit/s run). The `rendersdr` scenes are the same renders in SDR, the
baseline.

```bash
python driver.py --geo 1920x1080 --fps 60 --codec hevc --kbps 30000 --out runs/H --arm H \
  --scenes hdrpeak --fmt hdr --hdr-peak 3 --hdr-gain 2.783 ...
python hdrcal.py runs/H/1920x1080p60-hevc-hdr-30000
python driver.py ... --kbps 1500,2000,3000,4000,6000,8000,10000,12000,15000,20000 --out runs/H ...
python driver.py ... --kbps 1500,...,30000 --out runs/HS --arm HS --scenes rendersdr
python score_all.py runs/H runs/HS && python hfactor.py
```
