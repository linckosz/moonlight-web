# The bench of Auto's learned ceiling

A browser starts every native host at 250 M pixels a second and earns its way up,
one rung at a time, by holding a stream that really moved
(`frontend/js/util/AutoCeiling.js`, `frontend/js/stream/AutoCeilingMeter.js`).
This bench answers two questions no unit test can:

- Are the thresholds right: does a healthy machine climb, and a machine at its
  limit stop or come back down?
- Does the whole path work on real streams: the key, the launch line, the stats
  card's row, the virtual display made at the learned size?

```bash
python -m unittest test_journal                          # no machine, no browser

set MW_BENCH_LOCAL_PORTS=18080,18443                     # the --dev instance
set MW_BENCH_CLIENT_POS=-2560,0                          # a screen the stream does not capture
set MW_BENCH_CLIENT_LUID=0,12345                         # the GPU driving it, not the encoder's

python ceiling.py rung --rung 330 --tag r330 --tile "Display 1" --gdi DISPLAY1 --plan
python ceiling.py rung --rung 330 --tag r330 --tile "Display 1" --gdi DISPLAY1
python ceiling.py rung --rung 700 --tag r700 --target vdisplay --screen 3840x2160
python ceiling.py climb --tag climb --target vdisplay --screen 3840x2160
```

Results go to `bench-out/auto-ceiling/<tag>.json`, a picture of the card beside
them. The exit code says whether the pass was clean. `climb` takes `--deadline HH:MM`
(no launch starts that could end after it) and `--rung R` (go on from a rung reached
before). This bench reaches the host at its own address, unless `--rendezvous` says
otherwise: through the rendezvous, a machine's memory is
`mw_auto_ceiling:<identifier>`, not `mw_auto_ceiling`.

## What a pass does

1. Reads `mw_auto_ceiling`, `mw_enhancer_rung`, `mw_autostep` and
   `mw_autostep_failed`, and clears the last three: a memory left by another pass
   changes what is measured. The ceiling's key is then prepared (`--rung`), cleared,
   or left as it is (`--keep`).
2. Sets `mw_auto_learn` (`--learn`, 1 by default), Auto and Auto, and leaves every
   other setting at the product's default.
3. Launches by the tile's **name**, puts the moving picture on that tile's screen,
   and reads the card every 20 s.
4. Stops, reads the memories again, and judges the launch with `journal.py`.

Everything the verdict uses comes from what the page itself prints
(`[MW] Auto ceiling: …`) and the key it writes. `test_journal.py` reads the page's
source, so a sentence changed there fails here.

## Which pass proves what

| Pass | What it says |
|---|---|
| `rung --rung R` | R runs, is asked for, holds, earns the next rung. A stream that is not held back by R cannot earn anything: the verdict says so. |
| `climb` | The real way up: each launch earns the next, with nothing prepared. At least five launches from the base to the top, a little over 5 min each. |
| `rung --keep` | A client on another machine, going on from what it learned. |
| `--expect silent` | A TV, Power Saving, the switch at 0: no line, no key. |
| `--learn log --expect journal` | The lines are there, the key is not. |
| `--expect sick` | A client that cannot hold its rung: the signal at the rung, one rung down in the key, the failure counted, the entry closed. The pass ends at the signal. |
| `--load-gpu` | A client made ill on this machine, with `--expect sick`. |
| `--client-port` + `--expect sick` | A weak machine at a rung it asks for and cannot hold: no load tool, the machine is its own. The rung must be one its screen can ask for: 2160p60 is 498 M, so 500 on a 60 Hz screen, 1000 on a 120 Hz one (with `--screen 3840x2160`). `--client-url` is the host's LAN address: the fleet knows this desk by its loopback, and a pass that would send the other machine there stops at once. |
| `--enhance` | The Enhancer on, algorithm Auto: the rung holds with MWSR running. Every reading keeps the card's Enhancer row; MWSR never seen (nor idle at 1:1, where the stream fills the screen) is a defect. MWSR only works when the stream is smaller than the screen: on a 1440p120 desk, 330 M (2204×1240) is the rung that makes it work. |
| `--autostep 0` | The same stream without the cadence's steps. By default a step kept to the end counts for the rung, and nothing that happens on it counts against. |
| any active rung | What the browser was asked (`decodingInfo`) is in the results: the guard says nothing of a "yes", and a browser never asked is a defect. |
| `--refuse-above M` | The browser says no above M: the launch comes down a rung, nothing is written. The answer is the bench's own, the path is the real one. |
| `--rendezvous` | The page goes through the staging server and keeps to its machine's key: the plain key stays empty, and an entry put there for another machine stays as it was. |

`--rendezvous` needs an instance with a line, so no `MW_LAN_ONLY`: that also opens it
to the Internet and, unless `upnp_enabled` is false in its settings, writes to the
router's table. It is an exception to the bench's rule, to be granted each time.

## Four limits to keep in mind

**Two identical GPUs are one to the load tool.** `mw-gpu-load` folds boards with the
same name and memory into one, and refuses the LUID of the other: on such a machine it
reaches the first board only, so `--load-gpu` cannot make a client ill on the second.
Loading the board that encodes is another test, a game saturating the host: the
stream falls, nothing counts for or against the rung.

**No 4K screen.** A desk at 1440p120 cannot ask for more than 442 M. `--screen`
tells the page its screen is bigger, so that it asks for more: decoding and rendering
are real, presenting stays at the true size. A client on a virtual 4K screen is not
the answer: the virtual display driver renders all its screens on one GPU, and the
client would decode on the GPU that encodes.

**A window that does not move proves nothing.** A still desktop sends almost no
frames. The default page changes every frame, and still has text a fifth of the
time less than a game would.

**Every launch at another size may turn the virtual display over.** On a desk where
a physical screen has left the desktop on such a switch, read
`scripts/bench/monitors.ps1` before and after a series. The pass says when a screen
is gone. The virtual display's settings file can also gain a mode: put it back.

## After the switch goes on by default

An Auto/Auto launch may climb on its own from then on. A bench that needs a known
size names it, or sets `mw_auto_learn=0`. `acceptance/drive.py` does that for every
pass (`Driver.auto_learn`, `"0"` unless told otherwise), and this bench is the one that
says otherwise.
