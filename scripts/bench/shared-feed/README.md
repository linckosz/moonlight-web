# The owner under guests: what a guest costs the owner of a native host

Plan « flux commun des invités » (S0 and S9, bench §8q). One owner stream on a
display of the native host, then invited guests joining it through the share
board's slots 2-4 — the invitation's link, its PIN, the Join button, as a
person does — in a schedule of windows. `--guests 0,1,3,0,3,1,0` replays the
reference at the end, so a drift of the machine itself shows.

```bash
# --dev instance on 18080/18443, launched elevated (REALTIME, as the service's
# worker runs); three screens, three GPUs: encoder, owner's client, guests'.
set MW_BENCH_LOCAL_PORTS=18080,18443
python owner_load.py --tag s0-rtx --display-gpu RTX --client-gpu AMD --guest-gpu Arc
python summary.py ../../../bench-out/shared-feed/s0-*.json
```

- The owner's Chrome decodes on `--client-gpu`, from its screen; the guests'
  three windows on `--guest-gpu`'s screen. Neither is the encoder's GPU
  (docs/bench-campaign.md §4). The captured screen shows `content/scroll.html`.
  `--client-screen` / `--guest-screen` (`\\.\DISPLAY5`) pick another screen of
  that GPU than its first; a guest window that would leave it stops the run.
- A remote host (`--host-url`, `--fleet-id`) is reached over the LAN; its PIN is
  minted over SSH by fleet.py. Its workers are not counted. Its bench page is
  put on its screen apart.

## What a window reads

| field | from |
|---|---|
| owner's host stages (acquire, convert, encode, queue, send, total) | the `stats` messages the host sends the owner's page every second, caught by `hook.js` (a prototype patch: nothing in the app changes); per second: n, mean, p50, p95, p99, max |
| E2E, frame rate | the owner's overlay |
| sent | frames the stats windows closed, over their seconds: what the host really sent |
| workers, sessions | the --dev instance's stream workers, and the native sessions their logs say are running (resolution, codec, intra-refresh) |
| NVENC sessions | `nvidia-smi` (an NVIDIA encoder) |
| video engine load | WMI's GPU engine counters, every `Video…` engine type of the encoder's adapter (Intel's encoder shows as Video Decode) |
| guests' stages | the same hook in each guest's page: what their own workers encode |

The overlay's end-to-end is a rolling figure and moves with the client; the
host's own stages are the measure of what a guest costs the owner on the host.

`--feed-heights 1080,720,1440` has the owner pick the guests' picture before
each window (a feed with guests on it is rebuilt under them), and
`--no-hevc-slots 4` gives that guest a Chrome without HEVC (the whole feed goes
H.264).

## The hard cases (S9, bench §8q.5)

| script | what it checks |
|---|---|
| `hard_cases.py --server-log <log> --vdd DISPLAYn` | a lone guest leaving and coming back (no relaunch), the captured virtual display changing mode (one rebuild, the guests follow its shape — `display-mode.ps1`, never a physical screen), the feed's worker killed (elevated), a guest without HEVC (one switch to H.264) |
| `throttled_guest.py --shaper-dir <dir>` | a guest on the N95, in Wi-Fi, shaped below the feed's floor (WinDivert, elevated): it alone drops pictures, the feed stays at its floor, the local guests keep 60 per second — counted where the page draws them |
| `share_nonreg.py --server-log <log> --host <uuid>` | a share from a Sunshine or Wolf host as before: the three quality buttons, the guest's own session, no shared feed |
| `vd_cold.py [--second] [--owner-other]` | a guest opening an invitation on "MoonlightWeb Virtual Display" with nobody streaming: the display comes on, and goes off after them |

Each of them switches the primary screen or elevates something: say so to
whoever sits at the machine first. After a pass on a virtual display,
`monitors.ps1` and `vdd_settings.xml` must be as they were.
