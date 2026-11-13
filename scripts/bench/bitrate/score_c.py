"""Scores what cfactor.py needs from runs/C, rate by rate so a partial pass is still a curve:
the 4:4:4 run (score.py -> summary.json) then the 4:2:0 run at the same rate against the
full-chroma reference (score.py --as 444 -> summary-444.json). Already scored ones are skipped,
so a later start resumes. No new run is started at or after --until HH:MM.
Start it at Idle priority, on cores nothing else needs:
    py -E -X utf8 score_c.py --until 17:22 [--order 10000,4000,...]"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowin  # noqa: E402,F401  no child console window, ever

HERE = os.path.dirname(os.path.abspath(__file__))
# --runs E: the 13/11 fast444 slot (runs/E); runs/C by default
RUNS = os.path.join(HERE, "runs", sys.argv[sys.argv.index("--runs") + 1] if "--runs" in sys.argv else "C")
PAUSE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "PAUSE")
until = sys.argv[sys.argv.index("--until") + 1] if "--until" in sys.argv else None
# the next HH:MM from the start, so a stop after midnight works (23:12 is not "after" 05:50)
deadline = None
if until:
    import datetime
    now = datetime.datetime.now()
    deadline = now.replace(hour=int(until[:2]), minute=int(until[3:5]), second=0, microsecond=0)
    if deadline <= now:
        deadline += datetime.timedelta(days=1)
order = (sys.argv[sys.argv.index("--order") + 1].split(",") if "--order" in sys.argv else
         ["10000", "4000", "20000", "2000", "6000", "15000", "3000", "8000", "12000", "1500", "30000"])
log = open(os.path.join(HERE, "runs", "score_c.log"), "a", encoding="utf-8")


def say(s):
    log.write("%s %s\n" % (time.strftime("%H:%M:%S"), s))
    log.flush()


jobs = []
for k in order:
    jobs.append((os.path.join(RUNS, "1920x1080p60-hevc-444-%05d" % int(k)), [], "summary.json"))
    jobs.append((os.path.join(RUNS, "1920x1080p60-hevc-%05d" % int(k)), ["--as", "444"], "summary-444.json"))
say("start, until %s" % (deadline or "-"))
for d, extra, out in jobs:
    name = os.path.basename(d) + (" --as 444" if extra else "")
    if not os.path.exists(os.path.join(d, "run.json")) or os.path.exists(os.path.join(d, out)):
        continue
    if os.path.exists(PAUSE):
        say("PAUSE flag: stop before " + name)
        break
    if deadline and datetime.datetime.now() >= deadline:
        say("--until %s: stop before %s" % (until, name))
        break
    t = time.time()
    r = subprocess.run([sys.executable, "-E", "-X", "utf8", os.path.join(HERE, "score.py"), d, "--threads", "8"] + extra,
                       capture_output=True, text=True)
    if r.returncode != 0:
        open(os.path.join(d, "score%s.failed" % ("-444" if extra else "")), "w").write(r.stderr[-3000:])
    say("%s %s %.0fs" % (name, "ok" if r.returncode == 0 else "FAILED", time.time() - t))
say("end")
