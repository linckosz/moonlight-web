"""Starts score_all.py with no window, at Idle priority, on the cores of MW_BENCH_AFFINITY (default 0x3FC000);
its children inherit both. Replaces idle.cmd, whose `start` opened a console window.

    py -E -X utf8 score_bg.py runs\\A [more roots]
"""
import ctypes
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), "PAUSE")):
    raise SystemExit("PAUSE flag")
extra = [] if "--idle" in sys.argv else ["--idle", "400"]
p = subprocess.Popen([sys.executable, "-E", "-X", "utf8", os.path.join(HERE, "score_all.py")] + sys.argv[1:] + extra,
                     cwd=HERE, creationflags=0x08000000 | 0x00000040,  # CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS
                     stdout=subprocess.DEVNULL, stderr=open(os.path.join(HERE, "runs", "score_bg.err"), "a"))
k32 = ctypes.windll.kernel32
h = k32.OpenProcess(0x0200 | 0x0400, False, p.pid)  # SET_INFORMATION | QUERY_INFORMATION
ok = k32.SetProcessAffinityMask(h, ctypes.c_size_t(0x3FC000))
k32.CloseHandle(h)
print("score_all pid", p.pid, "affinity set", bool(ok), flush=True)
sys.exit(p.wait())
