"""Main bench of the Auto-bitrate study: every combination, 11 bitrates each, scaled on
today's Auto for that combination (x0.075 .. x1.5). Host = the local --dev (NVENC), client =
the lab Chrome on RTX bus 3 (CDP 9363); quality is scored on the reference decoding, so the
client device does not enter the scores. No point starts after the deadline.

    py -E -X utf8 main_bench.py [--deadline 06:00]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nowin  # noqa: E402,F401

# usage order (most used first); (W, H, fps, codec)
COMBOS = [(1920, 1080, 60, "hevc"), (1280, 720, 60, "hevc"), (1366, 768, 60, "hevc"), (1920, 1080, 120, "hevc"),
          (1280, 720, 30, "hevc"), (2560, 1440, 60, "hevc"), (1920, 1200, 60, "hevc"), (1920, 1080, 30, "hevc"),
          (2560, 1440, 120, "hevc"), (3840, 2160, 60, "hevc"),
          (1920, 1080, 60, "h264"), (1280, 720, 60, "h264")]
STEPS = [0.075, 0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.6, 0.75, 1.0, 1.5]


def auto_mbps(w, h, fps):
    """computeAutoBitrate (frontend/js/util/AutoBitrate.js), SDR 4:2:0, not touch."""
    return max(1, min(150, round(20 * (w * h) / (1920 * 1080) * fps / 60)))


def nice(kbps):
    """Two significant digits, at least 300 kbps."""
    k = max(300, kbps)
    mag = 10 ** (len(str(int(k))) - 2)
    return int(round(k / mag) * mag)


def rates(w, h, fps):
    a = auto_mbps(w, h, fps) * 1000
    out = []
    for s in STEPS:
        k = nice(a * s)
        if k not in out:
            out.append(k)
    return out


def main():
    deadline = sys.argv[sys.argv.index("--deadline") + 1] if "--deadline" in sys.argv else "06:00"
    out = os.path.join(HERE, "runs", "M")
    os.makedirs(out, exist_ok=True)
    first = True
    for w, h, fps, codec in COMBOS:
        if os.path.exists(os.path.join(out, "STOPPED.json")) or os.path.exists(os.path.join(out, "DEADLINE")):
            break
        ks = rates(w, h, fps)
        print("== %dx%dp%d %s: auto %d Mbit/s, points %s (%s)" % (w, h, fps, codec, auto_mbps(w, h, fps), ks,
                                                                  time.strftime("%H:%M:%S")), flush=True)
        args = [sys.executable, "-E", "-X", "utf8", os.path.join(HERE, "driver.py"), "--geo", "%dx%d" % (w, h),
                "--fps", str(fps), "--codec", codec, "--kbps", ",".join(map(str, ks)),
                "--host-url", os.environ["MW_BENCH_HOST_URL"], "--client-cdp", "9363", "--out", out,
                "--scenes", "main", "--arm", "M", "--allow-nvidia-overlay", "--allow-handwriting-canvas",
                "--deadline", deadline, "--skip-vscode-point"]
        if first:
            args.append("--pair-dev")
            first = False
        r = subprocess.run(args)
        print("   exit %d (%s)" % (r.returncode, time.strftime("%H:%M:%S")), flush=True)
    print("ALL DONE %s" % time.strftime("%H:%M:%S"), flush=True)


if __name__ == "__main__":
    if "--list" in sys.argv:
        for w, h, fps, c in COMBOS:
            print("%dx%dp%d %s auto %d -> %s" % (w, h, fps, c, auto_mbps(w, h, fps), rates(w, h, fps)))
    else:
        main()
