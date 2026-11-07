"""Scores one recorded run (one bitrate) of the Auto-bitrate bench.

    py -E -X utf8 score.py <run dir> [--threads 8] [--lpips-every 0]

<run dir> holds what tap.js flushed (meta.json, bitstream.bin) and the driver's
run.json: {"W", "H", "fps", "kbps", "codec", "scenes": [{"id", "mode", "src", "step", "pause"}]}.

1. the bitstream is decoded again by ffmpeg (bit-exact: the same pictures the
   client's hardware decoder showed, see verify.py);
2. each decoded frame's barcode gives its scene and frame number, hence its
   reference, computed from the scene's source file (common.ref);
3. VMAF NEG, PSNR-Y, SSIM and MS-SSIM per frame, in one libvmaf pass, on the
   rows above the barcode, the reference converted to 4:2:0 BT.709 limited like
   cmp_lab.vmaf_neg, the decoded frame as it came out of the decoder;
4. LPIPS on every Nth frame (--lpips-every, 0 = none), on RGB (fixed BT.709).
Writes frames.csv (one line per decoded frame) and summary.json.
"""
import argparse
import csv
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as cm  # noqa: E402

CONTENT = os.path.join(cm.HERE, "content")


def score_piped(run, stream, w, h, rows, scenes, srcs, threads):
    """One pass, nothing on disk but vmaf.json: ffmpeg decodes into this process; each frame
    with a valid barcode goes, barcode rows cut, on top of its reference (converted by a second
    ffmpeg exactly as ref_to_yuv does) in one yuv420p picture of 2 x rows; libvmaf crops the two
    halves apart (even offsets on 4:2:0: plain copies) and scores them as before.
    Returns (frames from vmaf.json, picked, counts)."""
    import queue
    import threading
    lav = ("[0:v]split[a][b];[a]crop=%d:%d:0:0[d];[b]crop=%d:%d:0:%d[r];"
           "[d][r]libvmaf=n_threads=%d"
           ":model='version=vmaf_v0.6.1neg\\:name=neg|version=vmaf_v0.6.1neg\\:name=negnm\\:motion.motion_force_zero=true'"
           ":feature=name=psnr|name=float_ssim|name=float_ms_ssim:log_fmt=json:log_path=vmaf.json"
           % (w, rows, w, rows, rows, threads))
    vj = os.path.join(run, "vmaf.json")
    if os.path.exists(vj):
        os.remove(vj)
    pv = subprocess.Popen([cm.FFMPEG, "-hide_banner", "-nostats", "-loglevel", "error",
                           "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "%dx%d" % (w, 2 * rows), "-r", "60",
                           "-i", "pipe:0", "-lavfi", lav, "-f", "null", "-"],
                          cwd=run, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    pr = subprocess.Popen([cm.FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                           "-s", "%dx%d" % (w, rows), "-i", "pipe:0",
                           "-vf", "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
                           "-f", "rawvideo", "pipe:1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
    picked = []
    counts = {"decoded": 0, "unreadable": 0, "grey": 0, "duplicates": 0}
    q = queue.Queue()  # unbounded, but held back by the reference ffmpeg's pipe: a few frames at most
    err = []

    def feed():
        # decode, read the barcode, send the reference to its converter, queue the decoded planes
        try:
            seen = set()
            for i, buf in cm.decode_iter(stream, w, h):
                counts["decoded"] = i + 1
                y = np.asarray(buf[:w * h]).reshape(h, w)
                v = cm.read_bar(y)
                if v is None:
                    counts["unreadable"] += 1
                    continue
                sid, n = cm.split(v)
                if sid == 0 or sid not in scenes:
                    counts["grey"] += 1
                    continue
                if (sid, n) in seen:
                    counts["duplicates"] += 1  # the host captured the same page frame twice: scored once
                    continue
                seen.add((sid, n))
                picked.append((i, sid, n))
                uv = np.asarray(buf[w * h:]).reshape(h // 2, w // 2, 2)[:rows // 2]
                q.put(y[:rows].tobytes() + np.ascontiguousarray(uv[:, :, 0]).tobytes()
                      + np.ascontiguousarray(uv[:, :, 1]).tobytes())
                pr.stdin.write(np.ascontiguousarray(cm.ref(scenes[sid], srcs[sid], w, h, n)[:rows]).tobytes())
        except BaseException as e:  # noqa: BLE001  re-raised in the main thread
            err.append(e)
        finally:
            q.put(None)
            try:
                pr.stdin.close()
            except OSError:
                pass

    t = threading.Thread(target=feed, daemon=True)
    t.start()
    ys, cs = w * rows, (w // 2) * (rows // 2)
    nfr = 0
    while True:
        d = q.get()
        if d is None:
            break
        r = cm.read_exact(pr.stdout, ys + 2 * cs)
        if r is None:
            raise SystemExit("reference converter ended early")
        pv.stdin.write(d[:ys] + r[:ys] + d[ys:ys + cs] + r[ys:ys + cs] + d[ys + cs:] + r[ys + cs:])
        nfr += 1
    t.join()
    pv.stdin.close()
    verr = pv.stderr.read().decode("utf-8", "replace")
    if err:
        raise err[0]
    if pr.wait() != 0 or pv.wait() != 0:
        raise SystemExit("ffmpeg failed: %s" % verr[-300:])
    j = json.load(open(vj, encoding="utf-8"))
    if len(j["frames"]) != nfr or nfr != len(picked):
        raise SystemExit("vmaf scored %d frames of %d" % (len(j["frames"]), len(picked)))
    return j["frames"], picked, counts


def run_vmaf(run, w, rows, nfr, threads):
    # two NEG models: as published, and with the motion feature forced to 0 (the published
    # one saturates at 100 on a scrolling page, whatever the PSNR)
    lav = ("[0:v]null[d];[1:v]null[r];[d][r]libvmaf=n_threads=%d"
           ":model='version=vmaf_v0.6.1neg\\:name=neg|version=vmaf_v0.6.1neg\\:name=negnm\\:motion.motion_force_zero=true'"
           ":feature=name=psnr|name=float_ssim|name=float_ms_ssim:log_fmt=json:log_path=vmaf.json" % threads)
    args = [cm.FFMPEG, "-hide_banner", "-nostats", "-loglevel", "error",
            "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "%dx%d" % (w, rows), "-r", "60", "-i", "dist.yuv",
            "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "%dx%d" % (w, rows), "-r", "60", "-i", "ref.yuv",
            "-lavfi", lav, "-f", "null", "-"]
    for attempt in range(3):
        if os.path.exists(os.path.join(run, "vmaf.json")):
            os.remove(os.path.join(run, "vmaf.json"))
        r = subprocess.run(args, cwd=run, capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(os.path.join(run, "vmaf.json")):
            j = json.load(open(os.path.join(run, "vmaf.json"), encoding="utf-8"))
            if len(j["frames"]) == nfr:
                return j["frames"]
        sys.stderr.write("vmaf attempt %d failed: %s\n" % (attempt + 1, r.stderr[-300:]))
    raise SystemExit("libvmaf failed 3 times")


def ref_to_yuv(frames_rgb, w, rows, out):
    """RGB references -> yuv420p BT.709 limited, the conversion cmp_lab.vmaf_neg uses."""
    p = subprocess.Popen([cm.FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                          "-s", "%dx%d" % (w, rows), "-i", "-",
                          "-vf", "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
                          "-y", "-f", "rawvideo", out], stdin=subprocess.PIPE)
    for f in frames_rgb:
        p.stdin.write(np.ascontiguousarray(f).tobytes())
    p.stdin.close()
    if p.wait() != 0:
        raise SystemExit("ffmpeg ref conversion failed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--lpips-every", type=int, default=0)
    a = ap.parse_args()
    run = os.path.abspath(a.run)
    spec = json.load(open(os.path.join(run, "run.json"), encoding="utf-8"))
    w, h = spec["W"], spec["H"]
    rows = cm.crop_rows(h)
    scenes = {s["id"]: s for s in spec["scenes"]}
    srcs = {sid: cm.load_rgb(os.path.join(CONTENT, s["src"])) for sid, s in scenes.items()}

    stream, codec = cm.annexb(run)
    if a.lpips_every > 0:
        raise SystemExit("--lpips-every is not available: frames are no longer kept on disk")
    for attempt in range(3):
        try:
            vf, picked, counts = score_piped(run, stream, w, h, rows, scenes, srcs, a.threads)
            break
        except SystemExit as e:
            sys.stderr.write("piped scoring attempt %d failed: %s\n" % (attempt + 1, e))
    else:
        raise SystemExit("piped scoring failed 3 times")
    nall, unread, grey, dup = counts["decoded"], counts["unreadable"], counts["grey"], counts["duplicates"]
    lp = {}

    out = []
    for j, (i, sid, n) in enumerate(picked):
        m = vf[j]["metrics"]
        out.append({"dec": i, "scene": sid, "n": n, "vmaf_neg": round(m["neg"], 4), "vmaf_negnm": round(m["negnm"], 4),
                    "psnr_y": round(m["psnr_y"], 4), "ssim": round(m["float_ssim"], 6),
                    "ms_ssim": round(m["float_ms_ssim"], 6), "lpips": lp.get(j, "")})
    with open(os.path.join(run, "frames.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=list(out[0].keys()))
        wr.writeheader()
        wr.writerows(out)
    summ = {"decoded": nall, "scored": len(out), "unreadable": unread, "grey": grey, "duplicates": dup,
            "codec": codec, "scenes": {}}
    for sid, s in scenes.items():
        rows_s = [r for r in out if r["scene"] == sid]
        if not rows_s:
            continue
        vm = np.array([r["vmaf_neg"] for r in rows_s])
        ns = np.array([r["n"] for r in rows_s])
        nm = np.array([r["vmaf_negnm"] for r in rows_s])
        d = {"mode": s["mode"], "frames": len(rows_s), "first_n": int(ns.min()), "n_max": int(ns.max()),
             "lost_at_cut": int(ns.min()),
             "negnm_mean": float(nm.mean()), "negnm_p5": float(np.percentile(nm, 5)),
             "vmaf_mean": float(vm.mean()), "vmaf_p5": float(np.percentile(vm, 5)),
             "vmaf_min": float(vm.min()), "vmaf_jitter": float(np.mean(np.abs(np.diff(vm)))) if len(vm) > 1 else 0.0,
             "psnr_y_mean": float(np.mean([r["psnr_y"] for r in rows_s])),
             "ms_ssim_mean": float(np.mean([r["ms_ssim"] for r in rows_s]))}
        if s["mode"] == "still":
            d["vmaf_arrival"] = float(vm[0])
            d["negnm_arrival"] = float(nm[0])
            d["negnm_settled"] = float(nm[-30:].mean())
            tail = vm[-30:]
            d["vmaf_settled"] = float(tail.mean())
            thr = vm[0] + 0.9 * (d["vmaf_settled"] - vm[0])
            k = next((int(ns[t]) for t in range(len(vm)) if vm[t] >= thr), int(ns[-1]))
            d["frames_to_90pct"] = k - int(ns[0])
        lps = [r["lpips"] for r in rows_s if r["lpips"] != ""]
        if lps:
            d["lpips_mean"] = float(np.mean(lps))
        summ["scenes"][str(sid)] = d
    json.dump(summ, open(os.path.join(run, "summary.json"), "w", encoding="utf-8"), indent=1)
    print(json.dumps(summ, indent=1))


def lpips_subset(run, picked, scenes, srcs, w, h, rows, raw, every):
    """LPIPS (alex, v0.1) on every Nth scored frame, with Shader's CPU env and cmp_lab's script."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scaler-lab"))
    import cmp_lab  # noqa: E402
    py = os.environ.get("MW_LPIPS_PY")
    if not py:
        raise SystemExit("MW_LPIPS_PY not set")
    d = os.path.join(run, "lp")
    os.makedirs(d, exist_ok=True)
    res = {}
    by_ref = []
    for j, (i, sid, n) in enumerate(picked):
        if j % every:
            continue
        rp, op = os.path.join(d, "r%05d.png" % j), os.path.join(d, "o%05d.png" % j)
        Image.fromarray(cm.ref(scenes[sid], srcs[sid], w, h, n)[:rows]).save(rp)
        Image.fromarray(cm.nv12_to_rgb(cm.nv12_frame(raw, w, h, i), w, h)[:rows]).save(op)
        by_ref.append((j, rp, op))
    for j, rp, op in by_ref:
        r = subprocess.run([py, "-I", "-c", cmp_lab.LPIPS_SCRIPT, "8", rp, op], capture_output=True, text=True)
        line = [x for x in r.stdout.splitlines() if x.startswith("{")]
        if line:
            res[j] = round(list(json.loads(line[-1]).values())[0], 6)
        os.remove(rp)
        os.remove(op)
    return res


if __name__ == "__main__":
    main()
