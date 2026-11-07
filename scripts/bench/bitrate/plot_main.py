"""Plot every fully scored main-bench combination (FR to plots/, EN to wiki/)."""
import glob, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nowin  # noqa
import main_bench as mb
PY = os.path.join(HERE, "..", "plotenv", "Scripts", "python.exe")
only = sys.argv[1:]
for w, h, fps, codec in mb.COMBOS:
    tag = "%dx%dp%d-%s" % (w, h, fps, codec)
    if only and tag not in only:
        continue
    ks = mb.rates(w, h, fps)
    runs = [os.path.join(HERE, "runs", "M", "%s-%05d" % (tag, k)) for k in ks]
    done = [r for r in runs if os.path.exists(os.path.join(r, "summary.json"))]
    if len(done) < len(ks):
        print("skip %s: %d/%d scored" % (tag, len(done), len(ks)))
        continue
    title = "%s%dp%d %s" % ("" if (w, h) in ((1920, 1080), (1280, 720), (2560, 1440), (3840, 2160)) else "%dx" % w, h, fps, codec.upper().replace("HEVC", "HEVC").replace("H264", "H.264"))
    for lang, out in (("fr", os.path.join(HERE, "plots", "m-%s.png" % tag)), ("en", os.path.join(HERE, "wiki", "m-%s.png" % tag))):
        if os.path.exists(out) and not os.environ.get("FORCE"):
            continue
        subprocess.run([PY, os.path.join(HERE, "plot.py"), "--plan", "main", "--lang", lang, "--title", title + " - NVENC (RTX 5060 Ti)" if lang == "en" else title + " - NVENC (RTX 5060 Ti)",
                        "--out", out, "--auto", str(mb.auto_mbps(w, h, fps)),
                        "NVENC=" + os.path.join(HERE, "runs", "M", tag + "-*")], check=True)
