#!/usr/bin/env python3
"""A real, reproducible load beside the stream, and what it costs the house (D1).

Plan « le son et la priorité des paquets (DSCP/WMM) », piste 2, D1. The stream
competes on the Wi-Fi with what the rest of the house does; to tell what a
DSCP class buys, that competition has to be the same from pass to pass, and
its own loss has to be measured too: the stream's gain may be the house's
harm.

    wired side:  python3 load.py serve [--port 47990] [--bind ADDR]
    Wi-Fi side:  python3 load.py run --server ADDR --profile video|bulk|call|idle
                     [--dir down|up] [--mbps M] [--period S] [--secs S]
                     [--tos DF|AF41|EF|<0-63>] [--ping HZ] [--bind ADDR] [--out file.json]

The generator runs on a second Wi-Fi station of the stream client's access
point (--bind its Wi-Fi address to keep it off a wired port); the server on a
wired machine. --dir down loads the access point's queues toward the
station, as the stream's own downlink does; up loads the air from the
station. Profiles:
- video: an iPhone watching a video. Segments of --mbps x --period (default
  11 Mbit/s x 4 s, 5.5 MB), each fetched over TCP as fast as the link goes,
  one every --period: bursts at the link's speed, which the stream sees
  (`wifi-contention-devices`), not a paced flow, which it does not.
- bulk: one TCP transfer for the whole run (a download, a backup).
- call: a video call, paced UDP both ways (--mbps each way, default 3, 30
  frames a second); mark it EF to compete with the stream's sound.
- idle: the ping alone.
Every run also pings the server (--ping, 50 a second by default) in the same
class: the generator's own latency. --tos marks every socket of the run, on
both sides (the server marks what it sends back as asked).

What it prints and saves (--out): per video segment its time and Mbit/s; the
bulk's Mbit/s per second; the call's loss and late datagrams each way; the
ping's round trips. Wall-clock start times, to line up with a bench pass.
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

CLASSES = {"DF": 0, "CS0": 0, "LE": 1, "CS1": 8, "AF11": 10, "AF21": 18, "CS2": 16,
           "AF41": 34, "AF42": 36, "CS5": 40, "VA": 44, "EF": 46, "CS6": 48, "CS7": 56}
CHUNK = 64 * 1024
FRAME = 1200  # a call datagram


def dscp_of(text):
    t = str(text).upper()
    if t in CLASSES:
        return CLASSES[t]
    v = int(t)
    if not 0 <= v <= 63:
        raise ValueError("DSCP out of range: %s" % text)
    return v


def mark(sock, dscp):
    """Sets the socket's DSCP; False where the system refuses it."""
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, dscp << 2)
        return True
    except OSError:
        return False


def quantile(values, q):
    v = sorted(values)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))] if v else None


def recv_exact(conn, n):
    got = 0
    while got < n:
        data = conn.recv(min(CHUNK, n - got))
        if not data:
            raise ConnectionError("closed after %d of %d bytes" % (got, n))
        got += len(data)


def read_line(conn):
    line = b""
    while not line.endswith(b"\n"):
        c = conn.recv(1)
        if not c:
            return None
        line += c
    return line


# ── server ────────────────────────────────────────────────────────────────


def serve_tcp(conn):
    """Requests on one connection, one JSON line each: down N (send N bytes),
    up N (read N, answer OK); a bulk asks a huge N and hangs up when done."""
    pad = bytes(CHUNK)
    try:
        while True:
            line = read_line(conn)
            if line is None:
                return
            req = json.loads(line)
            mark(conn, int(req.get("tos", 0)))
            n = int(req["bytes"])
            if req["op"] == "down":
                left = n
                while left > 0:
                    k = min(CHUNK, left)
                    conn.sendall(pad[:k])
                    left -= k
            else:
                recv_exact(conn, n)
                conn.sendall(b"OK\n")
    except (ConnectionError, OSError, ValueError):
        pass
    finally:
        conn.close()


class CallCounter:
    """What arrives of one call's datagrams: lost, and late (overtaken)."""

    def __init__(self):
        self.got, self.max_seq, self.late, self.dup = 0, -1, 0, 0
        self.seen = set()

    def add(self, seq):
        self.got += 1
        if seq in self.seen:
            self.dup += 1
            return
        self.seen.add(seq)
        if seq < self.max_seq:
            self.late += 1
        else:
            self.max_seq = seq

    def report(self, sent):
        return {"sent": sent, "got": self.got, "lost": sent - len(self.seen),
                "late": self.late, "dup": self.dup}


def send_call(sock, addr, sid, mbps, fps, secs, dscp, lock=None):
    """Paced call frames to @p addr: mbps/fps worth of FRAME-byte datagrams
    back to back, fps times a second. Returns the datagrams sent."""
    per = max(1, int(mbps * 1e6 / 8 / fps / FRAME))
    pad = bytes(FRAME - 9)
    seq = 0
    t0 = time.perf_counter()
    nxt = t0
    while time.perf_counter() - t0 < secs:
        for _ in range(per):
            data = b"C" + struct.pack("!II", sid, seq) + pad
            if lock:
                with lock:
                    mark(sock, dscp)
                    sock.sendto(data, addr)
            else:
                sock.sendto(data, addr)
            seq += 1
        nxt += 1.0 / fps
        while time.perf_counter() < nxt:
            time.sleep(0.0005)
    return seq


def serve(port, bind):
    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp.bind((bind, port))
    tcp.listen(16)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    udp.bind((bind, port))
    lock = threading.Lock()  # one DSCP at a time on the shared UDP socket
    calls = {}

    def accept_loop():
        while True:
            conn, _ = tcp.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=serve_tcp, args=(conn,), daemon=True).start()

    threading.Thread(target=accept_loop, daemon=True).start()
    print("load server on %s:%d (TCP and UDP)" % (bind or "*", port), flush=True)
    while True:
        data, addr = udp.recvfrom(65536)
        kind = data[:1]
        if kind == b"P" and len(data) >= 2:
            # A ping: back as it came, in the class it names.
            with lock:
                mark(udp, data[1])
                udp.sendto(data, addr)
        elif kind == b"C" and len(data) >= 9:
            sid, seq = struct.unpack("!II", data[1:9])
            calls.setdefault(sid, CallCounter()).add(seq)
        elif kind == b"R" and len(data) >= 9:
            sid, sent = struct.unpack("!II", data[1:9])
            rep = calls.pop(sid, CallCounter()).report(sent)
            udp.sendto(b"R" + json.dumps(rep).encode(), addr)
        elif kind == b"D":
            # The call's way down, asked by the station: paced to its socket.
            req = json.loads(data[1:])
            args = (udp, addr, int(req["sid"]), float(req["mbps"]), float(req["fps"]),
                    float(req["secs"]), int(req["tos"]), lock)

            def down(a=args):
                n = send_call(*a)
                with lock:
                    udp.sendto(b"N" + struct.pack("!II", a[2], n), a[1])

            threading.Thread(target=down, daemon=True).start()


# ── generator ─────────────────────────────────────────────────────────────


def connect(server, port, bind, dscp):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if bind:
        s.bind((bind, 0))
    marked = mark(s, dscp)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.connect((server, port))
    return s, marked


def run_video(a, dscp, stop, out):
    seg = int(a.mbps * 1e6 / 8 * a.period)
    s, out["marked"] = connect(a.server, a.port, a.bind, dscp)
    t_end = time.perf_counter() + a.secs
    nxt = time.perf_counter()
    pad = bytes(CHUNK)
    while time.perf_counter() < t_end and not stop.is_set():
        t0 = time.perf_counter()
        wall = time.time()
        s.sendall((json.dumps({"op": a.dir, "bytes": seg, "tos": dscp}) + "\n").encode())
        if a.dir == "down":
            recv_exact(s, seg)
        else:
            left = seg
            while left > 0:
                k = min(CHUNK, left)
                s.sendall(pad[:k])
                left -= k
            read_line(s)
        dt = time.perf_counter() - t0
        out["segments"].append({"wall": round(wall, 3), "secs": round(dt, 4),
                                "mbps": round(seg * 8 / dt / 1e6, 2)})
        print("segment %d: %.1f MB in %.2f s, %.1f Mbit/s" % (
            len(out["segments"]), seg / 1e6, dt, seg * 8 / dt / 1e6), flush=True)
        nxt += a.period
        while time.perf_counter() < min(nxt, t_end) and not stop.is_set():
            time.sleep(0.01)
    s.close()


def run_bulk(a, dscp, stop, out):
    s, out["marked"] = connect(a.server, a.port, a.bind, dscp)
    huge = 1 << 40
    s.sendall((json.dumps({"op": a.dir, "bytes": huge, "tos": dscp}) + "\n").encode())
    s.settimeout(0.5)
    t0 = time.perf_counter()
    sec_start, sec_bytes = t0, 0
    pad = bytes(CHUNK)
    while time.perf_counter() - t0 < a.secs and not stop.is_set():
        try:
            if a.dir == "down":
                data = s.recv(CHUNK)
                if not data:
                    break
                sec_bytes += len(data)
            else:
                sec_bytes += s.send(pad)
        except socket.timeout:
            pass
        now = time.perf_counter()
        if now - sec_start >= 1.0:
            out["perSecond"].append(round(sec_bytes * 8 / (now - sec_start) / 1e6, 2))
            sec_start, sec_bytes = now, 0
    s.close()


def run_call(a, dscp, stop, out):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    u.bind((a.bind or "", 0))
    out["marked"] = mark(u, dscp)
    sid = int(time.time() * 1000) & 0x7FFFFFFF
    fps = 30.0
    u.sendto(b"D" + json.dumps({"sid": sid, "mbps": a.mbps, "fps": fps, "secs": a.secs,
                                "tos": dscp}).encode(), (a.server, a.port))
    down = CallCounter()
    down_sent = [None]
    done = threading.Event()

    def receive():
        u.settimeout(0.5)
        while down_sent[0] is None:
            try:
                data, _ = u.recvfrom(65536)
            except socket.timeout:
                if stop.is_set() or done.is_set():
                    return
                continue
            if data[:1] == b"C":
                down.add(struct.unpack("!II", data[1:9])[1])
            elif data[:1] == b"N":
                down_sent[0] = struct.unpack("!II", data[1:9])[1]

    rx = threading.Thread(target=receive, daemon=True)
    rx.start()
    sent = send_call(u, (a.server, a.port), sid, a.mbps, fps, a.secs, dscp)
    rx.join(timeout=5)
    done.set()
    rx.join()
    out["callDown"] = down.report(down_sent[0] if down_sent[0] is not None else down.max_seq + 1)
    u.settimeout(2)
    for _ in range(3):
        u.sendto(b"R" + struct.pack("!II", sid, sent), (a.server, a.port))
        try:
            while True:
                data, _ = u.recvfrom(65536)
                if data[:1] == b"R":
                    out["callUp"] = json.loads(data[1:])
                    break
            break
        except socket.timeout:
            continue
    for way in ("callUp", "callDown"):
        r = out.get(way) or {}
        print("call %s: %s sent, %s lost, %s late" % (way[4:].lower(), r.get("sent"),
                                                      r.get("lost"), r.get("late")), flush=True)


def run_ping(a, dscp, stop, out):
    """Small datagrams at --ping a second, each answered by the server in the
    same class. A receiving thread blocks on the socket, so a round trip is
    stamped when it lands, not at the sender's next poll."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind((a.bind or "", 0))
    marked = mark(u, dscp)
    if out["marked"] is None:
        out["marked"] = marked
    u.settimeout(0.2)
    sent = {}
    lock = threading.Lock()
    t0 = time.perf_counter()
    done = threading.Event()

    def receive():
        while not done.is_set():
            try:
                data, _ = u.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:  # Windows: ICMP port unreachable, no server yet
                continue
            now = time.perf_counter()
            k = struct.unpack("!I", data[2:6])[0]
            with lock:
                at = sent.pop(k, None)
            if at is not None:
                out["ping"].append((round((at - t0) * 1000, 1), round((now - at) * 1000, 3)))

    rx = threading.Thread(target=receive, daemon=True)
    rx.start()
    period = 1.0 / a.ping
    seq = 0
    nxt = t0
    end = t0 + a.secs
    while time.perf_counter() < end and not stop.is_set():
        now = time.perf_counter()
        if now >= nxt:
            with lock:
                sent[seq] = time.perf_counter()
            u.sendto(b"P" + bytes([dscp]) + struct.pack("!I", seq) + bytes(40), (a.server, a.port))
            seq += 1
            nxt += period
        time.sleep(max(0.0, min(nxt - time.perf_counter(), 0.005)))
    time.sleep(2)  # the last answers; later ones count as lost
    done.set()
    rx.join()
    out["pingSent"] = seq


def run(a):
    dscp = dscp_of(a.tos)
    out = {"profile": a.profile, "dir": a.dir, "mbps": a.mbps, "period": a.period,
           "secs": a.secs, "dscp": dscp, "startWall": round(time.time(), 3),
           "segments": [], "perSecond": [], "ping": [], "marked": None}
    stop = threading.Event()
    work = {"video": run_video, "bulk": run_bulk, "call": run_call}.get(a.profile)
    pinger = None
    if a.ping > 0 and work:
        # The ping in a process of its own: in this one, the load's loops
        # hold Python's lock and would stretch its round trips (p99 +20 ms
        # on loopback beside a call).
        fd, ping_out = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        cmd = [sys.executable, os.path.abspath(__file__), "run", "--server", a.server,
               "--port", str(a.port), "--profile", "idle", "--secs", str(a.secs),
               "--tos", str(dscp), "--ping", str(a.ping), "--out", ping_out, "--quiet"]
        if a.bind:
            cmd += ["--bind", a.bind]
        pinger = subprocess.Popen(cmd)
    try:
        if work:
            work(a, dscp, stop, out)
        elif a.ping > 0:
            run_ping(a, dscp, stop, out)
        else:
            time.sleep(a.secs)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        if pinger:
            try:
                pinger.wait(timeout=a.secs + 15)
                with open(ping_out) as f:
                    got = json.load(f)
                out["ping"], out["pingSent"] = got["ping"], got.get("pingSent", 0)
            except (subprocess.TimeoutExpired, OSError, ValueError):
                pinger.kill()
            finally:
                os.remove(ping_out)
    rtts = [r for _, r in out["ping"]]
    out["pingSummary"] = {"sent": out.get("pingSent", 0), "back": len(rtts),
                          "median": quantile(rtts, 0.5), "p90": quantile(rtts, 0.9),
                          "p99": quantile(rtts, 0.99), "max": max(rtts) if rtts else None}
    if a.quiet:
        if a.out:
            with open(a.out, "w") as f:
                json.dump(out, f)
        return out
    if out["segments"]:
        m = [s["mbps"] for s in out["segments"]]
        print("video %s: %d segments, Mbit/s median %s, min %s" % (
            a.dir, len(m), quantile(m, 0.5), min(m)), flush=True)
    if out["perSecond"]:
        print("bulk %s: Mbit/s median %s, min %s" % (
            a.dir, quantile(out["perSecond"], 0.5), min(out["perSecond"])), flush=True)
    p = out["pingSummary"]
    print("ping %s/s in DSCP %d: %s/%s back, rtt median %s ms p90 %s p99 %s max %s; marked: %s" % (
        a.ping, dscp, p["back"], p["sent"], p["median"], p["p90"], p["p99"], p["max"],
        out["marked"]), flush=True)
    if a.out:
        with open(a.out, "w") as f:
            json.dump(out, f)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sv = sub.add_parser("serve")
    sv.add_argument("--port", type=int, default=47990)
    sv.add_argument("--bind", default="")
    rn = sub.add_parser("run")
    rn.add_argument("--server", required=True)
    rn.add_argument("--port", type=int, default=47990)
    rn.add_argument("--bind", default="")
    rn.add_argument("--profile", choices=("video", "bulk", "call", "idle"), default="video")
    rn.add_argument("--dir", choices=("down", "up"), default="down")
    rn.add_argument("--mbps", type=float, default=None)
    rn.add_argument("--period", type=float, default=4.0)
    rn.add_argument("--secs", type=float, default=60.0)
    rn.add_argument("--tos", default="DF")
    rn.add_argument("--ping", type=float, default=50.0)
    rn.add_argument("--out", default="")
    rn.add_argument("--quiet", action="store_true", help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.cmd == "serve":
        serve(a.port, a.bind)
        return
    if a.mbps is None:
        a.mbps = 3.0 if a.profile == "call" else 11.0
    run(a)


if __name__ == "__main__":
    sys.exit(main())
