"""Latency side of the Auto-bitrate study: content age (capture -> display) per bitrate.

    py -X utf8 latency.py --geo 1920x1080 --fps 60 --kbps 4000,8000,12000,20000,30000 \
        --passes 2 --secs 210 --out runs\\L

Same topology as driver.py (bras A): --dev NVENC on the bench desk, virtual display, client Chrome on
a second machine through the tunnel. The content Chrome shows the repository's
scroll.html?band=time (600 px/s text), calibrated on the host clock like
scripts/bench/content-age/age.py; the client's mwContentAge reads every frame.
Passes alternate the bitrates (pass 2 in reverse order), each one long enough to cover a
phase cycle between the free-running displays (photon-bench-display-beat).
No tap.js here: this process's own CDP session injects nothing.
"""
import argparse
import json
import os
import sys
import time
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import driver as dv  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content-age"))
import age  # noqa: E402

PAGE = "file:///" + os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content", "scroll.html")).replace(os.sep, "/") + "?band=time"


def one(cl, content, a, w, h, kbps, tag, out):
    path = os.path.join(out, tag + ".json")
    if os.path.exists(path):
        print(tag, "done already")
        return
    cl.navigate(a.host_url)
    cl.apply_settings(dv.settings_for(w, h, a.fps, kbps, a.codec))
    if "true" in str(cl.eval("JSON.stringify(!!window.__mwTap)")):
        raise dv.drive.PassFailed("tap.js is in the client page: latency would carry its copies")
    if not cl.wait_library(tries=15):
        raise dv.drive.PassFailed("no host card on the client page")
    card, app = cl.pick_tile("vdisplay")
    before = dv.monitors()
    cl.launch(card, app)
    cl.wait_picture(timeout=45)
    mon = dv.wait_new_monitor(before, w, h)
    watch = dv.Watch(mon)
    try:
        try:
            content.start(mon, PAGE, watch=watch)
            watch.check()
            age.calibrate(SimpleNamespace(port=dv.CONTENT_CDP, tries=40, page="scroll.html"))
            content.topmost()
            watch.nap(3)
            ov0 = cl.stats()
            print(cl.eval("mwContentAge ? mwContentAge.start({every: 1, inline: false}) : 'no mwContentAge'"), flush=True)
            watch.nap(a.secs)
            summ = cl.eval("JSON.stringify(mwContentAge.stop())")
            ov1 = cl.stats()
        finally:
            content.stop()
    finally:
        cl.stop()
        watch.stop()
    watch.check()
    if not summ or summ == "null":
        raise dv.drive.PassFailed("the probe returned nothing")
    d = json.loads(summ)
    d.update({"tag": tag, "W": w, "H": h, "fps": a.fps, "kbps": kbps, "codec": a.codec,
              "overlay_start": ov0, "overlay_end": ov1, "monitor": mon, "at": time.strftime("%H:%M:%S"),
              "watch_notes": watch.notes})
    json.dump(d, open(path, "w", encoding="utf-8"))
    print(tag, age.line(d), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--geo", required=True)
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--codec", default="hevc")
    ap.add_argument("--kbps", required=True)
    ap.add_argument("--passes", type=int, default=2)
    ap.add_argument("--secs", type=float, default=210)
    ap.add_argument("--host-url", default=os.environ.get("MW_BENCH_HOST_URL"))
    ap.add_argument("--client-cdp", type=int, default=9424)
    ap.add_argument("--content-luid", default="0,89259")
    ap.add_argument("--out", required=True)
    ap.add_argument("--allow-nvidia-overlay", action="store_true")
    ap.add_argument("--allow-handwriting-canvas", action="store_true")
    a = ap.parse_args()
    if a.allow_nvidia_overlay:
        dv.ALLOWED.add(dv.NVIDIA_OVERLAY)
    if a.allow_handwriting_canvas:
        dv.ALLOW_INK.append(True)
    w, h = map(int, a.geo.split("x"))
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    if os.path.exists(os.path.join(out, "STOPPED.json")):
        raise SystemExit("STOPPED.json in %s: the orchestrator decides first" % out)
    cl = dv.drive.Driver(a.client_cdp)
    content = dv.Content(a.content_luid)
    rates = [int(x) for x in a.kbps.split(",")]
    for p in range(a.passes):
        for kbps in (rates if p % 2 == 0 else rates[::-1]):
            if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), "PAUSE")):
                print("PAUSE flag: stopping", flush=True)
                return
            tag = "%dx%dp%d-%s-%05d-p%d" % (w, h, a.fps, a.codec, kbps, p + 1)
            fails = incidents = 0
            while True:
                try:
                    one(cl, content, a, w, h, kbps, tag, out)
                    break
                except dv.Foreign as e:
                    content.stop()
                    cl.stop()
                    incidents += 1
                    dv.note_incident(out, e, kbps, incidents, tag)
                    if incidents >= 3:
                        sys.exit(4)
                    time.sleep(60)
                except (dv.drive.PassFailed, SystemExit, RuntimeError) as e:
                    fails += 1
                    print("%s attempt %d failed: %s" % (tag, fails, e), flush=True)
                    content.stop()
                    cl.stop()
                    if fails >= 2:
                        break
                    time.sleep(5)

if __name__ == "__main__":
    main()
