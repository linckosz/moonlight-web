"""Run the fleet half (run-fleet.ps1) against one machine of the fleet, its PIN handed over in memory.

    python scripts/bench/fleet-run.py --machine <id> [-- <run-fleet.ps1 arguments>]
    python scripts/bench/fleet-run.py --machine mw-um790pro -- -Label um790pro -Codecs hevc,h264

<id> is the machine's id in acceptance/hosts.local.json. Without -AppUrl, the
address is the one the machine answers on (fleet.lan_url).

The PIN is minted on the machine by fleet.mint_pin, kept in this process's
memory, and written to run-fleet.ps1's standard input by subprocess: it never
goes through a standard output, a file, a command line or the environment,
so neither a terminal nor a tool that captures output ever holds it. What is
shown is run-fleet.ps1's own output, and from this launcher yes/no only.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "acceptance"))
import fleet  # noqa: E402

RUN_FLEET = os.path.join(HERE, "run-fleet.ps1")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--machine", required=True, help="its id in acceptance/hosts.local.json")
    ap.add_argument("rest", nargs=argparse.REMAINDER, help="-- then run-fleet.ps1's arguments")
    a = ap.parse_args(argv)
    rest = a.rest[1:] if a.rest[:1] == ["--"] else a.rest
    if a.machine not in fleet.MACHINES:
        print("no machine %r in hosts.local.json" % a.machine, flush=True)
        return 2
    probe = fleet.probe(a.machine)
    if not any(x.lower() == "-appurl" for x in rest):
        rest = rest + ["-AppUrl", fleet.lan_url(a.machine, probe)]
    pin = fleet.mint_pin(a.machine, probe)
    print("PIN minted on %s: %s" % (a.machine, bool(pin)), flush=True)
    if not pin:
        return 1
    try:
        done = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                               "-File", RUN_FLEET] + rest,
                              input=pin + "\n", text=True)
    finally:
        pin = None
    return done.returncode


if __name__ == "__main__":
    sys.exit(main())
