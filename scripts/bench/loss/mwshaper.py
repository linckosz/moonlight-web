"""Host-side link shaper: tc netem's gestures on a Windows host (DualRTX).

A Windows, Mac, iPhone or Android client has no netem, so the link is shaped
here, on the host, with WinDivert (a signed packet-diversion driver, loaded
while this runs): every UDP packet to or from the host's media ports
(48550-48573, one per stream slot, and a DEV edition's 48574-48597) passes
through this process.

  cut <ms>                       drop everything, both ways, for <ms> (self-expiring)
  rate <kbit> <ms> [n]           the downlink (host -> client) through a <kbit>
                                 bottleneck with a queue of n packets (default 200,
                                 netem's limit in the Linux runs) for <ms>; tail drop
                                 beyond it. The queue is flushed in order when the
                                 rate ends.
  loss <pct> [burst] [ms] [dir]  random losses, <pct> % of the packets, for <ms>
                                 (default: an hour). burst > 1: Gilbert-Elliott, the
                                 same average loss in bursts of that mean length
                                 (p = r·L/(1-L), r = 1/burst). dir: down (default),
                                 up or both.
  delay <ms> [ms_up] [for_ms]    one-way delay added downlink (and uplink: ms_up,
                                 default the same — <ms> + <ms_up> is the round
                                 trip added), for <for_ms> (default: an hour).
                                 Order is kept: a packet never overtakes one queued
                                 before it.
  clear                          end whatever is on
  addremote <ip> ...             shape these client addresses too (with --remote)
  stats                          counters as JSON
  quit                           flush, close, exit

Commands are lines on a TCP socket bound to the loopback (shapectl.py); each
gets one JSON line back. Every shaping state expires by itself, so a
controller that dies cannot leave the stream shaped for good; and closing the
WinDivert handle (this process ending, however) puts the traffic back on its
normal path. With no command for --idle-exit seconds, it exits.

    py mwshaper.py [--port 47250] [--remote ip,ip,...] [--log file]
                   [--windivert <dir holding WinDivert.dll and WinDivert64.sys>]

--remote restricts the shaping to those client addresses (IPv4 or IPv6, as the
host sees them); any other traffic on the media ports is passed untouched and
counted apart. ICE may first go out through the router (a hairpin): add the
router's address (192.168.1.254 on the bench LAN) to --remote. Needs an
elevated process (WinDivertOpen); the loopback is never seen, so the shaped
client is another machine. `sc stop WinDivert`, elevated, unloads the driver
at the end of a bench.

Pure Python over ctypes: about 15 000 packets/s before this process becomes
the bottleneck — a stream, not a flood at 100 Mbit/s.
"""
import argparse
import ctypes
import ipaddress
import json
import os
import random
import socket
import struct
import sys
import threading
import time
from collections import deque
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
PORT_LO, PORT_HI = 48550, 48597  # release slots, then a DEV edition's
FILTER = ("udp and not loopback and ((outbound and udp.SrcPort >= %d and udp.SrcPort <= %d) or "
          "(inbound and udp.DstPort >= %d and udp.DstPort <= %d))" % (PORT_LO, PORT_HI, PORT_LO, PORT_HI))
ADDR_LEN = 80  # sizeof(WINDIVERT_ADDRESS) in 2.x
MTU_MAX = 40 + 0xFFFF
INVALID_HANDLE = ctypes.c_void_p(-1).value
HOUR_MS = 3600 * 1000.0

LOG = None


def log(msg):
    line = "%s %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    if LOG:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")


class Divert:
    def __init__(self, filt, dll_dir):
        os.add_dll_directory(dll_dir)
        self.dll = ctypes.WinDLL(os.path.join(dll_dir, "WinDivert.dll"), use_last_error=True)
        self.dll.WinDivertOpen.restype = wintypes.HANDLE
        self.dll.WinDivertOpen.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int16, ctypes.c_uint64]
        self.dll.WinDivertRecv.restype = wintypes.BOOL
        self.dll.WinDivertRecv.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_uint,
                                           ctypes.POINTER(ctypes.c_uint), ctypes.c_void_p]
        self.dll.WinDivertSend.restype = wintypes.BOOL
        self.dll.WinDivertSend.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_uint,
                                           ctypes.POINTER(ctypes.c_uint), ctypes.c_void_p]
        self.dll.WinDivertClose.restype = wintypes.BOOL
        self.dll.WinDivertClose.argtypes = [wintypes.HANDLE]
        self.dll.WinDivertShutdown.restype = wintypes.BOOL
        self.dll.WinDivertShutdown.argtypes = [wintypes.HANDLE, ctypes.c_int]
        self.dll.WinDivertSetParam.restype = wintypes.BOOL
        self.dll.WinDivertSetParam.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_uint64]
        h = self.dll.WinDivertOpen(filt.encode(), 0, 0, 0)  # NETWORK layer, priority 0, divert
        if h is None or h == INVALID_HANDLE:
            raise OSError("WinDivertOpen failed: error %d" % ctypes.get_last_error())
        self.h = h
        # Room for a stall's worth of packets while this process is busy.
        self.dll.WinDivertSetParam(h, 0, 8192)       # QUEUE_LENGTH
        self.dll.WinDivertSetParam(h, 1, 2000)       # QUEUE_TIME ms
        self.dll.WinDivertSetParam(h, 2, 16 << 20)   # QUEUE_SIZE bytes
        self.buf = ctypes.create_string_buffer(MTU_MAX)
        self.addr = ctypes.create_string_buffer(ADDR_LEN)
        self.rlen = ctypes.c_uint(0)
        self.send_lock = threading.Lock()

    def recv(self):
        if not self.dll.WinDivertRecv(self.h, self.buf, MTU_MAX, ctypes.byref(self.rlen), self.addr):
            return None, None
        # string_at copies the packet only, not the 64 KiB buffer around it.
        return ctypes.string_at(self.buf, self.rlen.value), self.addr.raw

    def send(self, pkt, addr):
        n = ctypes.c_uint(0)
        with self.send_lock:
            return bool(self.dll.WinDivertSend(self.h, pkt, len(pkt), ctypes.byref(n), addr))

    def close(self):
        self.dll.WinDivertShutdown(self.h, 3)  # BOTH
        self.dll.WinDivertClose(self.h)


def outbound(addr):
    return bool((struct.unpack_from("<I", addr, 8)[0] >> 17) & 1)


def remote_of(pkt, out):
    v = pkt[0] >> 4
    if v == 4:
        return ipaddress.IPv4Address(pkt[16:20] if out else pkt[12:16])
    if v == 6:
        return ipaddress.IPv6Address(pkt[24:40] if out else pkt[8:24])
    return None


class LossModel:
    """Bernoulli, or Gilbert-Elliott in bursts of a mean length, on a fixed seed."""

    def __init__(self, pct, burst, seed):
        self.loss = max(0.0, min(1.0, pct / 100.0))
        self.burst = max(1.0, burst)
        self.rng = random.Random(seed)
        self.bad = False
        if self.burst > 1 and self.loss < 1:
            self.r = 1.0 / self.burst
            self.p = self.r * self.loss / (1.0 - self.loss)

    def drop(self):
        if self.loss <= 0:
            return False
        if self.burst <= 1:
            return self.rng.random() < self.loss
        # Leave or enter the bad state first; every packet in it is lost.
        if self.bad:
            if self.rng.random() < self.r:
                self.bad = False
        elif self.rng.random() < self.p:
            self.bad = True
        return self.bad


class Shaper:
    def __init__(self, divert, remotes):
        self.d = divert
        self.remotes = remotes  # None = every remote
        self.lock = threading.Condition()
        self.cut_until = 0.0
        self.rate_bps = 0.0
        self.rate_until = 0.0
        self.limit = 200
        self.loss = {"down": None, "up": None}
        self.loss_until = 0.0
        self.delay_s = {"down": 0.0, "up": 0.0}
        self.delay_until = 0.0
        # (send_at, pkt, addr), in order per direction.
        self.queues = {"down": deque(), "up": deque()}
        self.next_free = 0.0
        self.c = {k: 0 for k in ("down", "up", "downBytes", "cutDown", "cutUp", "tailDrop",
                                 "lossDown", "lossUp", "delayed", "queued", "queueMax", "other",
                                 "sendErr", "flushed")}
        self.seen = {}
        self.stopping = False
        self.last_cmd = time.monotonic()

    # ── the packet path ──────────────────────────────────────────────────
    def rx_loop(self):
        while not self.stopping:
            pkt, addr = self.d.recv()
            if pkt is None:
                if self.stopping:
                    return
                continue
            out = outbound(addr)
            rem = remote_of(pkt, out)
            key = str(rem)
            self.seen[key] = self.seen.get(key, 0) + 1
            if self.remotes is not None and rem not in self.remotes:
                self.c["other"] += 1
                self._send(pkt, addr)
                continue
            direction = "down" if out else "up"
            now = time.monotonic()
            with self.lock:
                if out:
                    self.c["down"] += 1
                    self.c["downBytes"] += len(pkt)
                else:
                    self.c["up"] += 1
                if now < self.cut_until:
                    self.c["cutDown" if out else "cutUp"] += 1
                    continue
                model = self.loss[direction] if now < self.loss_until else None
                if model is not None and model.drop():
                    self.c["lossDown" if out else "lossUp"] += 1
                    continue
                queue = self.queues[direction]
                send_at = now
                rating = out and now < self.rate_until
                if rating:
                    if len(queue) >= self.limit:
                        self.c["tailDrop"] += 1
                        continue
                    start = max(now, self.next_free)
                    self.next_free = start + len(pkt) * 8.0 / self.rate_bps
                    send_at = self.next_free
                    self.c["queued"] += 1
                if now < self.delay_until and self.delay_s[direction] > 0:
                    send_at += self.delay_s[direction]
                    self.c["delayed"] += 1
                if send_at > now or queue:
                    # Behind whatever is already waiting: nothing overtakes.
                    if queue and send_at < queue[-1][0]:
                        send_at = queue[-1][0]
                    queue.append((send_at, pkt, addr))
                    if len(queue) > self.c["queueMax"]:
                        self.c["queueMax"] = len(queue)
                    self.lock.notify()
                    continue
            self._send(pkt, addr)

    def _send(self, pkt, addr):
        if not self.d.send(pkt, addr):
            self.c["sendErr"] += 1

    def tx_loop(self):
        while not self.stopping:
            with self.lock:
                while not (self.queues["down"] or self.queues["up"]) and not self.stopping:
                    self.lock.wait(0.5)
                if self.stopping:
                    return
                now = time.monotonic()
                batch = []
                for direction, queue in self.queues.items():
                    # A rate that ended with no delay behind it: its queue leaves
                    # now, in order (the shaper's behaviour before delay existed).
                    if direction == "down" and now >= self.rate_until and \
                            not (now < self.delay_until and self.delay_s["down"] > 0):
                        if queue:
                            self.c["flushed"] += len(queue)
                            batch.extend(queue)
                            queue.clear()
                            self.next_free = 0.0
                        continue
                    while queue and queue[0][0] <= now:
                        batch.append(queue.popleft())
                if not batch:
                    heads = [q[0][0] for q in self.queues.values() if q]
                    wait = (min(heads) - now) if heads else 0.5
                    self.lock.wait(max(0.0005, wait))
                    continue
            for _, pkt, addr in batch:
                self._send(pkt, addr)

    # ── control ──────────────────────────────────────────────────────────
    def command(self, line):
        self.last_cmd = time.monotonic()
        parts = line.split()
        if not parts:
            return {"ok": False, "err": "empty"}
        op, now = parts[0], time.monotonic()
        with self.lock:
            if op == "cut":
                ms = float(parts[1])
                self.cut_until = now + ms / 1000.0
                return {"ok": True, "cut_ms": ms}
            if op == "rate":
                kbit, ms = float(parts[1]), float(parts[2])
                if len(parts) > 3:
                    self.limit = int(parts[3])
                self.rate_bps = kbit * 1000.0
                self.rate_until = now + ms / 1000.0
                self.next_free = now
                self.lock.notify()
                return {"ok": True, "kbit": kbit, "ms": ms, "limit": self.limit}
            if op == "loss":
                pct = float(parts[1])
                burst = float(parts[2]) if len(parts) > 2 else 1.0
                ms = float(parts[3]) if len(parts) > 3 else HOUR_MS
                direction = parts[4] if len(parts) > 4 else "down"
                if direction not in ("down", "up", "both"):
                    return {"ok": False, "err": "dir is down, up or both"}
                seed = 0x5EED
                self.loss = {"down": None, "up": None}
                for d in (("down", "up") if direction == "both" else (direction,)):
                    self.loss[d] = LossModel(pct, burst, seed)
                    seed += 1
                self.loss_until = now + ms / 1000.0
                return {"ok": True, "loss_pct": pct, "burst": burst, "ms": ms, "dir": direction}
            if op == "delay":
                down = float(parts[1])
                up = float(parts[2]) if len(parts) > 2 else down
                ms = float(parts[3]) if len(parts) > 3 else HOUR_MS
                self.delay_s = {"down": down / 1000.0, "up": up / 1000.0}
                self.delay_until = now + ms / 1000.0
                self.lock.notify()
                return {"ok": True, "down_ms": down, "up_ms": up, "ms": ms}
            if op == "clear":
                self.cut_until = 0.0
                self.rate_until = 0.0
                self.loss_until = 0.0
                self.delay_until = 0.0
                self.lock.notify()
                return {"ok": True}
            if op == "addremote":
                if self.remotes is None:
                    return {"ok": False, "err": "no remote filter: every remote is shaped already"}
                for x in parts[1:]:
                    self.remotes.add(ipaddress.ip_address(x))
                return {"ok": True, "remotes": sorted(map(str, self.remotes))}
            if op == "stats":
                out = dict(self.c)
                out["queueNow"] = {d: len(q) for d, q in self.queues.items()}
                out["cutting"] = now < self.cut_until
                out["rating"] = now < self.rate_until
                out["losing"] = now < self.loss_until
                out["delaying"] = now < self.delay_until
                seen = dict(self.seen)  # the receive thread adds to it unlocked
                out["remotes"] = dict(sorted(seen.items(), key=lambda kv: -kv[1])[:6])
                return out
            if op == "quit":
                self.cut_until = 0.0
                self.rate_until = 0.0
                self.loss_until = 0.0
                self.delay_until = 0.0
                self.lock.notify()
                return {"ok": True, "bye": True}
        return {"ok": False, "err": "unknown command %r" % op}


def serve(shaper, port, idle_exit):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    srv.settimeout(1.0)
    log("control on 127.0.0.1:%d" % port)
    while True:
        if time.monotonic() - shaper.last_cmd > idle_exit:
            log("no command for %d s: exiting" % idle_exit)
            return
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        with conn:
            conn.settimeout(30)
            f = conn.makefile("rw", encoding="utf-8", newline="\n")
            try:
                for line in f:
                    line = line.strip()
                    try:
                        reply = shaper.command(line)
                    except Exception as e:  # a malformed command, answered
                        reply = {"ok": False, "err": str(e)}
                    if line.split()[:1] != ["stats"]:
                        log("cmd %-24s -> %s" % (line, json.dumps(reply)))
                    f.write(json.dumps(reply) + "\n")
                    f.flush()
                    if reply.get("bye"):
                        return
            except (OSError, ValueError):
                pass


def main():
    global LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=47250)
    ap.add_argument("--remote", default="")
    ap.add_argument("--log", default="")
    ap.add_argument("--idle-exit", type=int, default=1800)
    ap.add_argument("--windivert", default=os.environ.get(
        "MW_WINDIVERT_DIR", os.path.join(HERE, "WinDivert-2.2.2-A", "x64")))
    a = ap.parse_args()
    LOG = a.log or None
    ctypes.windll.winmm.timeBeginPeriod(1)
    remotes = None
    if a.remote:
        remotes = {ipaddress.ip_address(x.strip()) for x in a.remote.split(",") if x.strip()}
    try:
        d = Divert(FILTER, a.windivert)
    except Exception as e:
        log("FATAL %s" % e)
        sys.exit(1)
    log("WinDivert open: %s | remotes %s" % (FILTER, sorted(map(str, remotes)) if remotes else "all"))
    sh = Shaper(d, remotes)
    rx = threading.Thread(target=sh.rx_loop, daemon=True)
    tx = threading.Thread(target=sh.tx_loop, daemon=True)
    rx.start()
    tx.start()
    try:
        serve(sh, a.port, a.idle_exit)
    finally:
        with sh.lock:
            sh.cut_until = sh.rate_until = sh.loss_until = sh.delay_until = 0.0
            batch = []
            for q in sh.queues.values():
                batch.extend(q)
                q.clear()
        for _, pkt, addr in sorted(batch, key=lambda item: item[0]):
            sh._send(pkt, addr)
        sh.stopping = True
        with sh.lock:
            sh.lock.notify_all()
        d.close()
        log("closed; final %s" % json.dumps(sh.c))


if __name__ == "__main__":
    main()
