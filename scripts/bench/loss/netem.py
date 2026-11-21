#!/usr/bin/env python3
"""tc netem on a Linux CLIENT, applied to a MoonlightWeb stream only.

Shapes the UDP between this machine and the host's media ports (48550-48573,
one per stream slot, and a DEV edition's 48574-48597; together inside
48544-48599), IPv4 and IPv6 alike: ICE picks
whichever pair answers first, and on the bench LAN that was IPv6 (a filter on
the host's IPv4 address shaped nothing, 28/09/2026). SSH is TCP: untouched.

  - downlink (host -> client): through ifb0 — one-way delay, losses, rate;
  - uplink (client -> host): the same one-way delay, so `--rtt` is a round trip.

Losses are random (Bernoulli), or in bursts when `--burst` > 1: netem's
Gilbert-Elliott model with the mean burst length asked and the same average
loss (p = r·L/(1-L), r = 1/burst; every packet lost in the bad state).

    sudo -n is used for tc; run as a user with passwordless sudo for tc.

    netem.py on --rtt 30 --loss 1 [--burst 4] [--rate 30000] [--limit 200]
    netem.py change --rtt 80 --loss 0.3      (the qdiscs must be on)
    netem.py off
    netem.py show

As a module: setup(), shape(rtt_ms, loss_pct, burst, rate_kbit), teardown(),
shaped_packets().
"""
import argparse
import re
import subprocess
import sys

# 48544-48599 as three u32 port masks: every slot's media port, release
# (48550-48573) and DEV (48574-48597).
PORT_BLOCKS = ("48544 0xffe0", "48576 0xfff0", "48592 0xfff8")
LIMIT = 200  # netem's queue: about 45 ms at 30 Mbit/s — a buffer, not a bottomless one


def sh(cmd):
    r = subprocess.run(["sudo", "-n", "sh", "-c", cmd], capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def default_interface():
    """The interface of the default route (the UM790Pro's NIC was renamed by a
    GPU added on its M.2 port: enp1s0 became enp2s0 on 01/10/2026)."""
    out = subprocess.run(["ip", "route", "show", "default"], capture_output=True,
                         text=True).stdout
    m = re.search(r"\bdev (\S+)", out)
    if not m:
        raise SystemExit("no default route: name the interface with --if")
    return m.group(1)


def teardown(iface=None):
    iface = iface or default_interface()
    sh("tc qdisc del dev %s root 2>/dev/null; tc qdisc del dev %s ingress 2>/dev/null; "
       "tc qdisc del dev ifb0 root 2>/dev/null; true" % (iface, iface))


def setup(iface=None, limit=LIMIT):
    iface = iface or default_interface()
    teardown(iface)
    down = []
    up = []
    for block in PORT_BLOCKS:
        down += [
            "tc filter add dev %s parent ffff: protocol ip u32 match ip protocol 17 0xff "
            "match ip sport %s action mirred egress redirect dev ifb0" % (iface, block),
            "tc filter add dev %s parent ffff: protocol ipv6 u32 match ip6 protocol 17 0xff "
            "match ip6 sport %s action mirred egress redirect dev ifb0" % (iface, block),
        ]
        up += [
            "tc filter add dev %s parent 1: protocol ip u32 match ip protocol 17 0xff "
            "match ip dport %s flowid 1:3" % (iface, block),
            "tc filter add dev %s parent 1: protocol ipv6 u32 match ip6 protocol 17 0xff "
            "match ip6 dport %s flowid 1:3" % (iface, block),
        ]
    for cmd in [
        "modprobe ifb numifbs=1",
        "ip link set dev ifb0 up",
        "tc qdisc add dev %s handle ffff: ingress" % iface,
    ] + down + [
        "tc qdisc add dev ifb0 root handle 1: netem delay 0ms limit %d" % limit,
        "tc qdisc add dev %s root handle 1: prio" % iface,
    ] + up + [
        "tc qdisc add dev %s parent 1:3 handle 30: netem delay 0ms" % iface,
    ]:
        rc, out = sh(cmd)
        if rc != 0:
            teardown(iface)
            raise SystemExit("netem setup failed at %r: %s" % (cmd, out))


def loss_clause(loss_pct, burst):
    if loss_pct <= 0:
        return ""
    if burst <= 1:
        return " loss %s%%" % loss_pct
    # Gilbert-Elliott: stationary loss L = p / (p + r), mean burst 1 / r.
    loss = loss_pct / 100.0
    r = 1.0 / burst
    p = r * loss / (1.0 - loss)
    return " loss gemodel %.4f%% %.4f%% 100%% 0%%" % (100 * p, 100 * r)


def shape(rtt_ms, loss_pct=0.0, burst=1, rate_kbit=0, iface=None, limit=LIMIT):
    """Change the shaping in place (setup() first). Returns (ok, message)."""
    iface = iface or default_interface()
    one_way = rtt_ms / 2.0
    rate = " rate %dkbit" % rate_kbit if rate_kbit else ""
    rc1, o1 = sh("tc qdisc change dev ifb0 root handle 1: netem delay %.1fms%s%s limit %d"
                 % (one_way, loss_clause(loss_pct, burst), rate, limit))
    rc2, o2 = sh("tc qdisc change dev %s parent 1:3 handle 30: netem delay %.1fms"
                 % (iface, one_way))
    return rc1 == 0 and rc2 == 0, (o1 + " " + o2).strip()


def qdisc_stats():
    return sh("tc -s qdisc show dev ifb0")[1]


def shaped_packets():
    """Packets netem has passed on the downlink so far."""
    m = re.search(r"Sent \d+ bytes (\d+) pkt", qdisc_stats())
    return int(m.group(1)) if m else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("op", choices=["on", "change", "off", "show"])
    ap.add_argument("--rtt", type=float, default=0, help="added round trip, ms")
    ap.add_argument("--loss", type=float, default=0, help="downlink loss, %%")
    ap.add_argument("--burst", type=float, default=1, help="mean loss burst, packets")
    ap.add_argument("--rate", type=int, default=0, help="downlink rate, kbit/s (0: none)")
    ap.add_argument("--limit", type=int, default=LIMIT)
    ap.add_argument("--if", dest="iface", default=None)
    a = ap.parse_args()
    if a.op == "off":
        teardown(a.iface)
        print("netem off")
        return
    if a.op == "show":
        print(qdisc_stats())
        return
    if a.op == "on":
        setup(a.iface, a.limit)
    ok, msg = shape(a.rtt, a.loss, a.burst, a.rate, a.iface, a.limit)
    print(("shaped: rtt %g ms, loss %g %%, burst %g, rate %s" %
           (a.rtt, a.loss, a.burst, a.rate or "-")) if ok else "shape failed: " + msg)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
