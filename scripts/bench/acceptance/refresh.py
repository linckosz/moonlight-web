"""Re-probe the fleet and rewrite chapter 1's access rows.

Run this last, just before the report. What the report says of each machine's
access (its LAN address, whether a rendezvous address is known and on which
server) has to be read at the end — after every pass, every restart and every
reinstall — not at the moment an install happened to finish. The rendezvous
address itself and a PIN are never written here: they live in the memory of the
run that uses them (fleet.rendezvous_url, fleet.mint_pin).

It keeps each machine's install/update steps as they were; only what the
machine says about itself right now is replaced.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fleet  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PATH = os.path.join(REPO, "bench-out", "acceptance", "results", "chapter1.jsonl")


def main():
    rows = []
    with open(PATH, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))

    latest = {}
    for rec in rows:
        latest[rec.get("machine")] = rec

    for mid, rec in latest.items():
        if mid not in fleet.MACHINES:
            continue
        print("  probing %s…" % mid, flush=True)
        try:
            probe = fleet.probe(mid)
        except Exception as e:
            print("    unreachable: %s" % e)
            continue
        native = probe.get("native") or {}
        rec["afterUpdate"] = probe
        # Rows written before these rules held carried both: not kept.
        rec.pop("rendezvousUrl", None)
        rec.pop("pin", None)
        rec["rendezvousKnown"] = bool((probe.get("internet") or {}).get("rendezvousKnown"))
        rec["rendezvousHost"] = fleet.rendezvous_host(probe)
        rec["lanUrl"] = fleet.lan_url(mid, probe)
        rec["ports"] = {"http": probe.get("httpPort"), "https": probe.get("httpsPort"),
                        "listening": probe.get("listening")}
        rec["status"] = "ok" if native.get("available") else "failed"
        if native.get("available"):
            rec["reason"] = ""
        else:
            rec["reason"] = (native.get("reason") or native.get("error")
                             or "the native host did not answer at the end of the run")
        version = ""
        for part in (probe.get("versionLine") or "").split("Version:")[1:]:
            version = part.strip()
        print("    %-10s native=%s autostart=%s rendezvous=%s v=%s"
              % (mid, native.get("available"), probe.get("autostart"),
                 rec["rendezvousKnown"], version[:22]))

    with open(PATH, "a", encoding="utf-8") as f:
        for rec in latest.values():
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    print("rewrote %d access rows" % len(latest))


if __name__ == "__main__":
    main()
