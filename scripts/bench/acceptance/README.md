# The acceptance run — can this build be deployed?

`/bench` next door answers *how fast is this pipeline*. This answers a different
question, the one asked the day before a release: **does this build install,
update and stream, on every machine we have?**

It is a go/no-go, not a measurement. Click-to-photon is deliberately out of
scope — the only number kept is the total latency the overlay shows, because
that is the number a user reads.

## Running one

```bash
python run.py --plan-only                              # both matrices, nothing runs
python run.py --chapter 1 --package <dir of artifacts> # install + update everywhere
python run.py --chapter 2                              # DualRTX host, DualRTX client
python run.py --chapter 3                              # every other host
python report.py --version <v> --commit <sha> --run <id>
```

Restrict with `--only <machine>` (repeatable) and `--pass <id>`. A pass replayed
later supersedes the earlier one in the report, so a single failure can be
re-run on its own.

Get the artifacts without cutting a release:

```bash
gh workflow run ci.yml --ref main -f channel=dev
gh run download <id> -n MoonlightWebDev-windows-x64-v<ver> -D <dir>
```

`channel=dev` is what makes it the **MoonlightWebDev** edition: its own name,
its own AppId, ports 48080/48443, and a rendezvous line on `stream.dev`. It
installs beside a production MoonlightWeb without touching it.

## The pieces

| File | What it is |
|---|---|
| `matrix.json` | the two matrices and the base config, as data |
| `fleet.py` | how to reach each machine, and what it says about itself |
| `install.py` | uninstall / install / update, one recipe per OS |
| `drive.py` | one streaming pass, over DevTools, on one CDP connection |
| `run.py` | the orchestrator, and the verdict rules |
| `gpu_load.py` | the `"load": "gpu"` passes: `mw-gpu-load` on the encoder's own GPU, calibrated before the stream (see `backend/native-host/tools/gpu-load/README.md`) |
| `report.py` | the HTML page and the PDF |
| `refresh.py` | re-read the access rows just before the report |
| `restore.py` | give the machines back: drop the bench task, put production back |

`drive.py` imports `../cdp.py` rather than copying it: the click-by-coordinates
rule and the bounded timeouts stay in one place. `cdp.py` remains the tool for
driving a pass **by hand**.

## Where the fleet is declared

`fleet.py` says what each machine **is** — its OS, its architecture, whether its
latency means anything. It never says where the machine lives or how to log into
it: addresses, accounts and passwords are read from
`../hosts.local.json`, which git ignores, exactly as `discover.ps1` does it.
A machine with no entry there is simply absent from the run.

Put nothing identifying in this directory. It is committed.

## Three things that are not obvious

**The port is discovered, never assumed.** 48080/48443 are defaults. A
`settings.json` decides, a silent uninstall *keeps* that `settings.json`, and a
fresh install therefore inherits whatever was there — the dev instance on this
desk answers on 18080/18443. `fleet.probe()` asks the process which ports it
holds and then asks each of them; `lan_url()` re-checks the HTTPS one from the
measuring station, because that is the machine that has to reach it.

**`/api/admin/*` needs the per-run key.** The server mints one at start and
hands it out on `GET /api/admin/token`; without the `X-MW-Admin-Key` header the
PIN route answers 403 even on loopback. That key is a CSRF barrier, not a
credential, and it dies with the process.

**The Sunshine target is anchored on `isLocalHost`.** A bench client paired with
the whole fleet sees seventeen host cards. Picking "the Sunshine" by position
would just as happily pick a Wolf on another machine: the pass would run, the
numbers would be real, and they would be about the wrong computer.

**Some settings live on the host, not in the browser.** A pass's `settings`
reach the client's localStorage and nothing else. What the host itself decides
— the native engine's video pipeline (`native_video_pipeline`), for one — goes
in the pass's `hostSettings`: `run.py` writes it through the host's own
loopback API (`fleet.host_settings`, with the per-run admin key) before the
stream starts, and writes the previous values back once the pass is over, fail
or not. A key the host refuses fails the pass rather than letting it measure
the wrong thing; a key the host does not list is not "restored" to anything.

```json
{"id": "pipeline-d3d12", "factor": "D3D12 pipeline",
 "hostSettings": {"native_video_pipeline": "d3d12"}}
```

**Auto does not learn on the bench.** `drive.py` writes `mw_auto_learn=0` before every
launch: an Auto/Auto pass streams at the base ceiling, whatever this browser learned
from the passes before it. `Driver(auto_learn=None)` leaves the page's own default;
`scripts/bench/auto-ceiling/` is the bench of the ceiling itself.

## What the colours mean

🟩 clean · 🟨 works but off the mark · 🟥 fails · ⬜ not applicable, **always with
its reason** · ⬛ not run.

On a VM, a machine with no GPU or a software encoder — `mw-arm`, `mw-debian` —
latency is **never** a failure criterion. Those cells are grey or yellow, never
red. What is validated there is that it works at all.

## The report keeps the names, never the way in

`report.py` here is not `../report.py`, which redacts machine names too — a
go/no-go that will not say which machine failed is not worth reading, so the
names, the screens and the numbers all stay.

What it does **not** print is the pair that opens a host: the rendezvous
address and the PIN. Those are handed to the reader in the session that
produced the page. A report is a thing one forwards; a PIN is not. The access
table says only whether each machine answered, so a row still proves the probe
reached it.

It is written under `bench-out/`, which `.gitignore` covers.
**Never commit `bench-out/`.**
