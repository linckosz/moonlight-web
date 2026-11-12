"""Scores (score.py + verify.py) every run under the given folders that has run.json and no
summary.json yet; loops until no new run has appeared for --idle seconds. Meant to be started
through score_bg.py (Idle priority, no window). Stops at once on the night PAUSE flag.

    py -E -X utf8 score_all.py runs\\A [--idle 300]
"""
import glob
import os
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowin  # noqa: E402,F401  no child console window, ever
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PAUSE = os.path.join(HERE, "PAUSE")


def main():
    roots = [a for a in sys.argv[1:] if not a.startswith("--")]
    idle = int(sys.argv[sys.argv.index("--idle") + 1]) if "--idle" in sys.argv else 300
    last = time.time()
    log = open(os.path.join(HERE, "runs", "score_all.log"), "a", encoding="utf-8")
    while time.time() - last < idle:
        if os.path.exists(PAUSE):
            log.write("PAUSE flag: stop\n")
            return
        todo = [d for r in roots for d in sorted(glob.glob(os.path.join(r, "*")))
                if os.path.exists(os.path.join(d, "run.json")) and not os.path.exists(os.path.join(d, "summary.json"))
                and not os.path.exists(os.path.join(d, "score.failed"))]
        if not todo:
            time.sleep(10)
            continue
        for d in todo:
            until = sys.argv[sys.argv.index("--until") + 1] if "--until" in sys.argv else None
            if until and time.strftime("%H:%M") >= until:
                log.write("%s --until %s: stop before %s\n" % (time.strftime("%H:%M"), until, os.path.basename(d)))
                return
            t = time.time()
            extra = ["--cut-cols", sys.argv[sys.argv.index("--cut-cols") + 1]] if "--cut-cols" in sys.argv else []
            r = subprocess.run([sys.executable, "-E", "-X", "utf8", os.path.join(HERE, "score.py"), d, "--threads", "8"] + extra,
                               capture_output=True, text=True)
            v = subprocess.run([sys.executable, "-E", "-X", "utf8", os.path.join(HERE, "verify.py"), d],
                               capture_output=True, text=True)
            ok = r.returncode == 0
            if not ok:
                open(os.path.join(d, "score.failed"), "w").write(r.stderr[-3000:])
            exact = '"bit_exact": true' in v.stdout
            if ok:
                # raw pictures kept only as long as they are scored: the decoder's frames and the
                # Annex B copy go; bitstream.bin stays until the study's scoring is final
                for p in glob.glob(os.path.join(d, "frame-*.raw")) + glob.glob(os.path.join(d, "stream.*")):
                    os.remove(p)
            log.write("%s %s score %s verify %s %.0fs\n" % (time.strftime("%H:%M:%S"), os.path.basename(d),
                                                          "ok" if ok else "FAILED", "exact" if exact else "NOT EXACT/" + v.stderr[-200:].replace("\n", " "),
                                                          time.time() - t))
            log.flush()
            last = time.time()
            done_runs = globals().get("done_runs", 0) + 1
            globals()["done_runs"] = done_runs
            if "--max-runs" in sys.argv and done_runs >= int(sys.argv[sys.argv.index("--max-runs") + 1]):
                return


if __name__ == "__main__":
    main()
