"""The P4 flows of the laboratory driver (plan upscaler-mwsr.md §3.6, §8.1):
the conformity of the twins (C11).

The product's two renderers, driven by the chain cells on the same pictures
as the kernel cells, are held to the kernel in two ways:

- **pixels** — on the golden cases: the WebGPU chain must give the kernel
  cell's picture exactly (the same text, the same views); the WebGL2 chain
  the same picture to 2/255 per channel wherever no screen derivative is
  involved, and on the kernel the pixel statistics are
  reported: the vote and the edge frame come from the GPU's screen
  derivatives, whose quad (coarse) reference row is the implementation's
  choice and differs between the two APIs' window spaces (the first
  conformity run, 21/10/2026), so the twins cannot be identical on an edge;
  a reduction votes too since it sharpens (J58): held by a floor of PSNR, with
  the two APIs' bare bilinears (`tex-s1`, `chain-gl-off`) beside it, the
  samplers' own subtexel precision, which Linux's SwiftShader turns into
  36/255 on 1.4 % of the zone plate;
- **quality** — on the ground-truth pictures of the quality flow at ×2 (text,
  game, code): PSNR, SSIM, overshoot and FLIP of each twin against the ground
  truth must be the same to the noise (FLIP 0.002, PSNR 0.1 dB, overshoot
  0.1 %). That is the criterion the kernel is held to.

On the software adapters the two sides cannot share a Chrome (ANGLE forced on
SwiftShader switches WebGPU off), so the flow is run once per side: first
`--swiftshader --conformity` (the WebGPU cells, every input picture, posted
as PNG, the ground truths and the reduced inputs), then `--webgl-swiftshader
--conformity` (the WebGL2 chain on those same pixels), each run dumping its
cells under the same names and comparing
every pair whose two dumps are on disk. On a GPU (one Chrome for both) a single
run does it all. Called from scaler_lab.py with its `run_page`.

The gate itself (plan §9, P4: "which flavour in SDR, by device class") is the
--p4 set below: the product's chain in both renderers on the same pictures,
`off` (the plain copy), the shipped SGSR1 and FSR1 and `mwsr`, saturated and
at 60 Hz, at ×2 (text, game, interface), ×1.33 and ×0.67; per draw the
main-thread time (`submit`), the time to the GPU's completion (`done`) and
the GPU's own time where it can be read. --p4-table turns a run's files into
the gate's table and its figures.
"""
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import metrics  # noqa: E402
import p3_flows  # noqa: E402

# The channel difference a port may show where no derivative is involved
# (plan §3.6: beyond it, a bug, not a tolerance) — on all but a thousandth of
# the pixels, and never more than CONFORMITY_MAX: the two APIs' samplers do not
# weigh a bilinear tap to the same subtexel precision, which the zone plate
# turns into 3/255 on a hundred pixels (21/10/2026).
CONFORMITY_TOL = 2
CONFORMITY_SHARE = 0.001
CONFORMITY_MAX = 4
# The quality the two twins must share, against the ground truth.
QUALITY_TOL = {"flip": 0.002, "psnr": 0.1, "overshoot": 0.001}

# (what is compared, the cell it must match at scale, the cell it must match below 1:1)
PAIRS = [
    ("chain-mwsr", "mwsr", "mwsr:srgb=1"),
    ("chain-gl-mwsr", "mwsr", "mwsr"),
]
GL_CELLS = ["chain-gl-mwsr"]
# A reduction's yardstick: the two APIs' bare bilinears on the same pixels
# (the lab's `tex-s1` on WebGPU, the product's plain copy `chain-gl-off` on
# WebGL2) differ by the samplers' own subtexel precision — 1 to 3/255 on a
# few pixels with Windows' SwiftShader, 36/255 on 1.4 % of the zone plate
# with Linux's (J29).
BASE_PAIR = ("chain-gl-off", "tex-s1")
# Since the reduction sharpens (J58) it votes on its derivatives, and GLSL ES
# has no coarse one: the twin's vote falls on other quads. With the vote out of
# both texts the two pictures are the same to the bit on every case here
# (03/11/2026), so what this pair shows is that vote, up to 35/255 on 0.6 % of
# `code`: held by a floor of PSNR (51 dB at worst that day), lowered with the
# samplers' own difference on Linux's SwiftShader.
DOWN_GL_PSNR = 45
# The quality leg: the ground truth at this size, reduced to the input (×2).
QUALITY_IN, QUALITY_OUT = "1280x720", "2560x1440"
QUALITY_CONTENT = ["text", "game", "code"]
QUALITY_CELLS = ["tex-s1", "mwsr", "chain-mwsr"]


def cases():
    return sorted({(c, i, o) for c, i, o, _ in p3_flows.GOLDEN_CASES})


def dump_path(a, base, cell):
    return os.path.join(a.dir, base + "-" + re.sub(r"[^\w.+-]", "_", cell) + ".png")


def annotate(level, message):
    """A line on the run's summary page under GitHub Actions (::warning::,
    ::error::); nothing more elsewhere, the log says it already."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print("::%s title=Upscaler lab::%s" % (level, message.replace("\n", " ")))


def conformity(a, run_page):
    """Each golden case rendered by this Chrome's cells, the quality pictures
    too, then every pair whose two dumps exist. Exit 1 for a real difference
    (a pair out of tolerance, a twin of another quality) or a picture missing
    (a page or a cell that failed, a dump not on disk); exit 0 when the only
    problems are chains not applicable on this adapter, each one a warning
    annotation under GitHub Actions (Bruno, 22/10/2026). Their pairs are
    pending, like those whose other side has not run yet."""
    from scaler_lab import make_input  # the driver's own, lazily (it imports this module)

    gl_side = getattr(a, "webgl_swiftshader", False)
    bad = 0
    adapter = None
    # A WebGPU chain is not applicable where no frame reaches the product's
    # renderer on this adapter: no way takes a frame in, or the renderer gives
    # up (WebGpuRenderer.failed; the page would then move the picture to
    # WebGL2, which the other side judges). A platform limit, not a conformity
    # problem. The chains draw into the lab's textures (nopresent=1): a WebGPU
    # canvas cannot present on Linux with the software adapter, and Chrome
    # drops the page's whole WebGPU instance when one tries (CI 39737555924).
    na = {}

    def page(query, key):
        nonlocal bad, adapter
        res = run_page(query)
        if "error" in res:
            print(key, "ERROR", res["error"][:600])
            bad += 1
            return None
        adapter = (res.get("adapter") or {}).get("name", adapter)  # a gt=1 page names none
        # what the renderers said: the chain's kind and GPU, every console warning
        log = str(res.get("pageLog", ""))
        why = "no way took a frame in"
        for line in log.splitlines():
            if line.startswith("chain ") or line.startswith("console."):
                print("   ", key, "|", line[:200])
            if "[WebGpuRenderer] the readback failed too (" in line:
                refused = line.split("failed too (", 1)[1].rsplit("): ", 1)[0]
                why = "import, ImageBitmap and readback refused: " + refused[:160]
            if "[WebGpuRenderer] giving up: " in line:
                why = "the renderer gave up: " + line.split("giving up: ", 1)[1][:160]
        errors = list(res.get("errors") or [])
        gave_up = [e for e in errors if (e.startswith("[chain-") or e.startswith("[dump chain-"))
                   and "no frame reached the renderer's canvas" in e]
        for e in gave_up:
            cell = e.split("]")[0].replace("[dump ", "[").strip("[]")
            if cell not in na:
                na[cell] = why
                annotate("warning", "%s not applicable on %s: %s" % (cell, adapter or "this adapter", why))
        if gave_up:
            print(key, "WebGPU chain not applicable on this adapter:", why)
        errors = [e for e in errors if e not in gave_up]
        if errors:
            print(key, "errors:", errors[:5])
            bad += 1
        return res

    # ── pixels: the golden cases ──
    for content, size_in, size_out in cases():
        down = int(size_out.split("x")[0]) < int(size_in.split("x")[0])
        base = "conf-%s-%s-%s" % (content, size_in, size_out)
        key = "%s %s->%s" % (content, size_in, size_out)
        # The twins on the same pixels: the WebGPU side draws each case's
        # picture once and posts it, and both sides read that PNG back as their
        # input. A mire each Chrome drew itself (Canvas2D text) was not the same
        # picture in the runner's two Chromes, a glyph edge a few /255 apart
        # before any renderer (J32): the reduction of `code` fell out of the
        # bilinears' yardstick on it (CI 39912544431, 40233795656).
        src = base + "-src"
        if gl_side:
            if not os.path.exists(os.path.join(a.dir, src + "-gt.png")):
                print(key, "pending: the WebGPU side draws the input (--swiftshader --conformity first)")
                continue
            cells = list(GL_CELLS) + ([BASE_PAIR[0]] if down else [])
        else:
            if page("gt=1&content=%s&out=%s&tag=%s" % (content, size_in, src), key + " input") is None:
                continue
            cells = sorted({c for p in PAIRS for c in (p[0], p[2] if down else p[1])})
            if down:
                # the two bare bilinears, the reduction's yardstick
                cells += [BASE_PAIR[1], BASE_PAIR[0]]
            if a.swiftshader:
                cells = [c for c in cells if not c.startswith("chain-gl-")]
        res = page("in=%s&out=%s&content=img:/__dir/%s-gt.png&codec=none&cells=%s&dump=all%s&nopresent=1&tag=%s" % (
            size_in, size_out, src, ",".join(cells), p3_flows.QUICK, base), key)
        if res is None:
            continue
        missing = [c for c in cells if not os.path.exists(dump_path(a, base, c)) and c not in na]
        if missing:
            print(key, "not dumped:", missing)
            bad += 1
        sys.stdout.flush()

    # ── quality: the ground-truth pictures at ×2 ──
    iw, ih = (int(x) for x in QUALITY_IN.split("x"))
    for content in QUALITY_CONTENT:
        base = "conf-q-%s" % content
        gt_path = os.path.join(a.dir, base + "-gt.png")
        in_path = os.path.join(a.dir, base + "-in.png")
        if gl_side:
            if not os.path.exists(in_path):
                print(base, "pending: the WebGPU side makes the ground truth and the input (--swiftshader --conformity first)")
                continue
            cells = list(GL_CELLS)
        else:
            res = page("gt=1&content=%s&out=%s&tag=%s" % (content, QUALITY_OUT, base), base + " ground truth")
            if res is None:
                continue
            make_input(gt_path, in_path, (iw, ih))
            cells = list(QUALITY_CELLS) + ([] if a.swiftshader else GL_CELLS)
        res = page("in=%s&out=%s&content=img:/__dir/%s-in.png&codec=none&cells=%s&dump=all%s&nopresent=1&tag=%s" % (
            QUALITY_IN, QUALITY_OUT, base, ",".join(cells), p3_flows.QUICK, base), base)
        if res is None:
            continue
        missing = [c for c in cells if not os.path.exists(dump_path(a, base, c)) and c not in na]
        if missing:
            print(base, "not dumped:", missing)
            bad += 1
        sys.stdout.flush()
    print("side %s (%s) rendered; comparing the dumps on disk" % ("WebGL2" if gl_side else "WebGPU", adapter))
    for cell, why in sorted(na.items()):
        print("not applicable on this adapter:", cell, "-", why)

    # ── the pixel table ──
    rows = []
    pending = 0
    for content, size_in, size_out in cases():
        down = int(size_out.split("x")[0]) < int(size_in.split("x")[0])
        key = "%s %s->%s" % (content, size_in, size_out)
        base = "conf-%s-%s-%s" % (content, size_in, size_out)

        def diff(what, ref):
            """(worst, differ, over, psnr) of two dumps; None when one is not
            there; ("shapes", x, y) when they are not the same size."""
            px, py = dump_path(a, base, what), dump_path(a, base, ref)
            if not (os.path.exists(px) and os.path.exists(py)):
                return None
            x, y = metrics.load(px), metrics.load(py)
            if x.shape != y.shape:
                return ("shapes", x.shape, y.shape)
            d = np.abs(np.rint(x * 255.0) - np.rint(y * 255.0))
            worst = int(d.max())
            return (worst, float((d > 0).any(axis=-1).mean()), float((d > CONFORMITY_TOL).any(axis=-1).mean()),
                    metrics.psnr(y, x) if worst else float("inf"))

        # the yardstick of a reduction: the two APIs' bare bilinears
        bil = diff(*BASE_PAIR) if down else None
        if bil is not None and bil[0] == "shapes":
            print("  %-28s %-14s vs %-12s shapes differ %s %s" % (key, BASE_PAIR[0], BASE_PAIR[1], bil[1], bil[2]))
            bad += 1
            bil = None
        if bil is not None:
            rows.append((key, BASE_PAIR[0], BASE_PAIR[1]) + bil + (True, None))
            print("  %-28s %-14s vs %-12s max %3d/255  differ %6.2f %%  over %6.3f %%  psnr %s  %s" % (
                key, BASE_PAIR[0], BASE_PAIR[1], bil[0], 100 * bil[1], 100 * bil[2],
                "exact" if bil[0] == 0 else metrics.fmt(bil[3], 1), "the samplers' own difference"))
        for what, up_ref, down_ref in PAIRS:
            ref = down_ref if down else up_ref
            r = diff(what, ref)
            if r is None:
                pending += 1
                continue
            if r[0] == "shapes":
                print("  %-28s %-14s vs %-12s shapes differ %s %s" % (key, what, ref, r[1], r[2]))
                bad += 1
                continue
            worst, differ, over, psnr = r
            # the WebGL2 kernel is held by its quality (below), not by its pixels
            held = not (what == "chain-gl-mwsr" and not down)
            yard = ""
            if held and what == "chain-gl-mwsr":
                # its reduction by how far the vote takes it
                floor = DOWN_GL_PSNR if bil is None else min(DOWN_GL_PSNR, bil[3] - 3)
                ok = worst == 0 or psnr >= floor
                yard = " (the vote's derivatives, floor %.1f dB)" % floor
            else:
                ok = (over <= CONFORMITY_SHARE and worst <= CONFORMITY_MAX) if held else True
            if not ok:
                bad += 1
            rows.append((key, what, ref, worst, differ, over, psnr, ok, held))
            print("  %-28s %-14s vs %-12s max %3d/255  differ %6.2f %%  over %6.3f %%  psnr %s  %s" % (
                key, what, ref, worst, 100 * differ, 100 * over,
                "exact" if worst == 0 else metrics.fmt(psnr, 1),
                (("OK" if ok else "OUT OF TOLERANCE") + yard) if held else "reported (derivatives)"))
    if pending:
        print("  %d pixel pair(s) pending: the other side has not run yet, or not applicable here" % pending)

    # ── the quality table ──
    qrows = []
    qpending = 0
    for content in QUALITY_CONTENT:
        base = "conf-q-%s" % content
        gt_path = os.path.join(a.dir, base + "-gt.png")
        in_path = os.path.join(a.dir, base + "-in.png")
        if not (os.path.exists(gt_path) and os.path.exists(in_path)):
            qpending += 1
            continue
        gt, inp = metrics.load(gt_path), metrics.load(in_path)
        got = {}
        for cell in QUALITY_CELLS + GL_CELLS:
            p = dump_path(a, base, cell)
            if os.path.exists(p):
                got[cell] = metrics.measure(gt, inp, metrics.load(p))
        for cell, m in got.items():
            print("  %-6s %-14s psnr %6.2f  ssim %.4f  overshoot %5.2f %%  flip %s" % (
                content, cell, m["psnr"], m["ssim"], 100 * m["overshoot"], metrics.fmt(m["flip"])))
        ref, gl = got.get("mwsr"), got.get("chain-gl-mwsr")
        if ref is None or gl is None:
            qpending += 1
            continue
        d_flip = None if ref["flip"] is None or gl["flip"] is None else abs(gl["flip"] - ref["flip"])
        d_psnr = abs(gl["psnr"] - ref["psnr"])
        d_over = abs(gl["overshoot"] - ref["overshoot"])
        ok = (d_flip is None or d_flip <= QUALITY_TOL["flip"]) and d_psnr <= QUALITY_TOL["psnr"] \
            and d_over <= QUALITY_TOL["overshoot"]
        if not ok:
            bad += 1
        qrows.append((content, ref, gl, d_flip, d_psnr, d_over, ok))
        print("  %-6s WebGL2 twin vs kernel: flip %s  psnr %.3f dB  overshoot %.3f %%  %s" % (
            content, metrics.fmt(d_flip) if d_flip is not None else "-", d_psnr, 100 * d_over,
            "OK" if ok else "NOT THE SAME QUALITY"))
    if qpending:
        print("  %d quality case(s) pending: the other side has not run yet" % qpending)
    print("conformity: %s%s" % ("OK" if not bad else "%d problem(s)" % bad,
                                 " (not applicable on this adapter: %s)" % ", ".join(sorted(na)) if na else ""))
    if bad:
        annotate("error", "conformity of the %s side: %d problem(s), a real difference or a picture missing"
                 " (the step's log has them)" % ("WebGL2" if gl_side else "WebGPU", bad))

    if a.md and (rows or qrows):
        path = os.path.join(a.dir, a.tag + "-conformity.md")
        with open(path, "w", encoding="utf-8") as f:
            f.write("# Conformity of the twins\n\n")
            if rows:
                f.write("## Pixels (golden cases)\n\n")
                f.write("| case | chain | reference | max /255 | pixels differing | pixels over %d/255 | PSNR dB | |\n"
                        % CONFORMITY_TOL)
                f.write("|---|---|---|---|---|---|---|---|\n")
                for key, what, ref, worst, differ, over, psnr, ok, held in rows:
                    f.write("| %s | `%s` | `%s` | %d | %.2f %% | %.3f %% | %s | %s |\n" % (
                        key, what, ref, worst, 100 * differ, 100 * over,
                        "exact" if worst == 0 else metrics.fmt(psnr, 1),
                        ("OK" if ok else "**out**") if held else
                        ("the samplers' own difference" if held is None else "reported")))
            if qrows:
                f.write("\n## Quality against the ground truth (×2)\n\n")
                f.write("| content | twin | PSNR dB | SSIM | overshoot % | FLIP | ΔFLIP | ΔPSNR | |\n")
                f.write("|---|---|---|---|---|---|---|---|---|\n")
                for content, ref, gl, d_flip, d_psnr, d_over, ok in qrows:
                    for name, m in (("WGSL `mwsr`", ref), ("GLSL `gl-mwsr`", gl)):
                        f.write("| %s | %s | %.2f | %.4f | %.2f | %s | | | |\n" % (
                            content, name, m["psnr"], m["ssim"], 100 * m["overshoot"], metrics.fmt(m["flip"])))
                    f.write("| %s | difference | | | | | %s | %.3f | %s |\n" % (
                        content, metrics.fmt(d_flip) if d_flip is not None else "-", d_psnr,
                        "OK" if ok else "**not the same**"))
        print("wrote", path)
    return 1 if bad else 0


# ── the gate (--p4) ──────────────────────────────────────────────────────

GATE_GPU = "chain-off,chain-sgsr,chain-fsr1,chain-mwsr"
GATE_GL = "chain-gl-off,chain-gl-sgsr,chain-gl-fsr1,chain-gl-mwsr"
# (name, input, output, content): the SDR production cases of a desktop screen
GATE_SIZES = [
    ("x2-text", "1280x720", "2560x1440", "text"),
    ("x2-game", "1280x720", "2560x1440", "game"),
    ("x2-ui", "1280x720", "2560x1440", "ui"),
    ("x133-text", "1920x1080", "2560x1440", "text"),
    ("x067-text", "3840x2160", "2560x1440", "text"),
]
# 2 rounds (ABBA) of 24 draws per chain, one warm-up: ~10 s a page on a GPU
GATE_RUN = "&rounds=2&batches=6&batch=16&warm=1"
# The verdict's cases (upscale: the SDR production of a desktop) and its margins:
# a flavour wins when its `done` p50 is under 0.9 × the other's on every one of
# them and its main-thread p50 is not worse by more than a fifth or a quarter
# of a millisecond (the p99 of 48 draws is their maximum: a stall of one
# frame, reported, not judged); otherwise the two are equal and the production
# flavour (WebGL2 in SDR) stays.
GATE_VERDICT_CASES = ["x2-text", "x2-game", "x133-text"]
GATE_WIN, GATE_MAIN, GATE_MAIN_MS = 0.9, 1.2, 0.25


def gate_cells(a):
    if getattr(a, "swiftshader", False):
        return GATE_GPU
    if getattr(a, "webgl_swiftshader", False) or getattr(a, "gl", False):
        return GATE_GL
    return GATE_GPU + "," + GATE_GL


def gate_sizes(a):
    sizes = list(GATE_SIZES)
    screen = getattr(a, "screen", "")
    if screen:
        w, h = (int(x) for x in screen.lower().split("x"))

        def at(f):
            return "%dx%d" % (int(w * f) // 8 * 8, int(h * f) // 8 * 8)
        sizes += [
            ("screen-x2-text", at(0.5), screen, "text"),
            ("screen-x2-game", at(0.5), screen, "game"),
            ("screen-x133-text", at(0.75), screen, "text"),
            ("screen-x067-text", at(1.5), screen, "text"),
        ]
    return sizes


def gate_cases(a):
    cells = gate_cells(a)
    out = []
    for name, i, o, content in gate_sizes(a):
        q = "in=%s&out=%s&content=%s&cells=%s%s" % (i, o, content, cells, GATE_RUN)
        out.append((name, q))
        out.append((name + "-tick", q + "&pace=tick"))
    return out


def _best_posted(files, want_cells=()):
    """The run to read among a case's posted files: one with every cell the
    case asks for and no error first (an older run of the same tag and case
    name, from another set of cells, does not count); then one the page ran in
    the foreground (`hiddenMs` 0); then the quickest (a page from before that
    field: a second, throttled run of the same page takes thirty seconds, the
    one watched three to seven)."""
    import json

    def rank(path):
        try:
            with open(path, encoding="utf-8") as f:
                r = json.load(f)
        except Exception:  # noqa: BLE001
            return (3, 1, 0, path)
        names = {c.get("name") for c in r.get("cells") or []}
        complete = 0 if names and not r.get("errors") and set(want_cells) <= names else 1
        hidden = r.get("hiddenMs")
        foreground = 0 if hidden is None or hidden == 0 else 1
        return (complete, foreground, r.get("elapsedMs") or 0, path)
    best = min(files, key=rank)
    if len(files) > 1:
        print("  %d posted for %s, reading %s" % (
            len(files), os.path.basename(best).rsplit("-", 1)[0], os.path.basename(best)))
    return best


def _posted_shape(path):
    """A posted run's sizes and regime, as its page read them from the URL."""
    import json
    try:
        with open(path, encoding="utf-8") as f:
            p = json.load(f).get("params") or {}
    except Exception:  # noqa: BLE001
        return None
    return {"in": p.get("in"), "out": p.get("out"), "pace": p.get("pace") or "back"}


def _gate_files(a, tag):
    """case name -> the run's result (a desktop run's <tag>-<case>.json, or the
    latest phone-<tag>-<case>-<time>.json a phone posted)."""
    import glob
    import json
    found = {}
    for name, query in gate_cases(a):
        q = dict(kv.split("=", 1) for kv in query.split("&"))
        want = [c for c in (q.get("cells") or "").split(",") if c]
        # a run posted under the same tag and case name at another size is not
        # this case: another phone on the same links, or --screen changed
        shape = {"in": q.get("in"), "out": q.get("out"), "pace": q.get("pace") or "back"}
        p = os.path.join(a.dir, "%s-%s.json" % (tag, name))
        # phone-<tag>-x2-text-*.json also matches the -tick case's files: keep
        # the ones whose suffix is the posting time alone
        head = "phone-%s-%s-" % (tag, name)
        posted = sorted(f for f in glob.glob(os.path.join(a.dir, head + "*.json"))
                        if os.path.basename(f)[len(head):-len(".json")].isdigit())
        same = [f for f in posted if _posted_shape(f) == shape]
        if len(same) < len(posted):
            print("  %d posted for %s at another size or regime, left out (the case is %s->%s %s)" % (
                len(posted) - len(same), head[:-1], shape["in"], shape["out"], shape["pace"]))
        posted = same
        if posted:
            p = _best_posted(posted, want)
        if os.path.exists(p):
            with open(p, encoding="utf-8") as f:
                found[name] = json.load(f)
    return found


def _st(c, key, q):
    v = (c.get(key) or {}).get(q)
    return v if isinstance(v, (int, float)) else None


def gate_table(a, tag):
    """The gate's table of one run: per case and regime, each chain's submit,
    done and GPU figures; the WebGPU/WebGL2 ratios of each pass; the verdict
    of GATE_VERDICT_CASES. Printed, and written to <dir>/<tag>-gate.md."""
    runs = _gate_files(a, tag)
    if not runs:
        print(tag, ": no result file in", a.dir)
        return
    first = next(iter(runs.values()))
    adapter = (first.get("adapter") or {}).get("name", "?")
    ua = first.get("userAgent", "")
    lines = ["# Porte P4 — %s" % tag, "", "Adapter: %s" % adapter, "", "UA: `%s`" % ua, ""]
    lines += ["`submit` = the draw's main-thread time (WebGPU: encode and submit; WebGL2: the GL calls); "
              "`done` = from the draw's start to the GPU's completion (WebGPU: `draw()` itself, which waits "
              "for `onSubmittedWorkDone`; WebGL2: `draw()` then a fence polled to its signal, Chrome's `finish()` being a flush); `gpu` = the GPU's own time "
              "(WebGPU: the stamped passes, pass 0 and the kernel; WebGL2: `EXT_disjoint_timer_query_webgl2` "
              "around the draw, upload included). Milliseconds, p50 and p99 over the draws.", ""]
    lines += ["| case | regime | chain | submit p50 | submit p99 | done p50 | done p99 | gpu p50 | 1:1 copies | errors |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    print("%-18s %-5s %-14s %9s %9s %9s %9s %9s" % ("case", "pace", "chain", "submit50", "submit99", "done50", "done99", "gpu50"))
    ratios = {}  # (case, regime, pass) -> (submit, done, gpu) ratios WebGPU / WebGL2
    for name, res in runs.items():
        if "error" in res:
            lines.append("| %s | | ERROR %s | | | | | | | |" % (name, res["error"][:120]))
            print(name, "ERROR", res["error"][:200])
            continue
        case, regime = (name[:-5], "tick") if name.endswith("-tick") else (name, "back")
        cells = {c["name"]: c for c in res.get("cells", [])}
        errs = len(res.get("errors") or [])
        if not cells:
            first_err = (res.get("errors") or ["no cell"])[0][:160]
            lines.append("| %s | %s | no result | | | | | | | %s |" % (case, regime, first_err))
            print("%-18s %-5s no result: %s" % (case, regime, first_err))
        for cname, c in cells.items():
            seq = c.get("passSeq") or {}
            row = (name, regime, cname, _st(c, "submit", "p50"), _st(c, "submit", "p99"),
                   _st(c, "done", "p50"), _st(c, "done", "p99"), _st(c, "gpu", "p50"))
            lines.append("| %s | %s | `%s` | %s | %s | %s | %s | %s | %s/%s | %s |" % (
                case, regime, cname, metrics.fmt(row[3]), metrics.fmt(row[4]), metrics.fmt(row[5]),
                metrics.fmt(row[6]), metrics.fmt(row[7]), c.get("copies"), sum(seq.values()) if seq else "-",
                errs or ""))
            print("%-18s %-5s %-14s %9s %9s %9s %9s %9s" % (
                case, regime, cname, metrics.fmt(row[3]), metrics.fmt(row[4]), metrics.fmt(row[5]),
                metrics.fmt(row[6]), metrics.fmt(row[7])))
        for p in ("off", "sgsr", "fsr1", "mwsr"):
            g, w = cells.get("chain-" + p), cells.get("chain-gl-" + p)
            if g is None or w is None:
                continue

            def ratio(key, q):
                x, y = _st(g, key, q), _st(w, key, q)
                return x / y if x is not None and y else None
            sg, sw = _st(g, "submit", "p50"), _st(w, "submit", "p50")
            ratios[(case, regime, p)] = (ratio("submit", "p50"), ratio("done", "p50"), ratio("gpu", "p50"),
                                         None if sg is None or sw is None else sg - sw)
    if ratios:
        lines += ["", "## WebGPU / WebGL2, per pass", "",
                  "| case | regime | pass | submit p50 | done p50 | gpu p50 |", "|---|---|---|---|---|---|"]
        print("%-12s %-5s %-5s %10s %10s %10s" % ("case", "pace", "pass", "submit50", "done50", "gpu50"))
        for (case, regime, p), (rs, rd, rg, _) in sorted(ratios.items()):
            f = lambda v: "-" if v is None else "%.2f" % v  # noqa: E731
            lines.append("| %s | %s | `%s` | %s | %s | %s |" % (case, regime, p, f(rs), f(rd), f(rg)))
            print("%-12s %-5s %-5s %10s %10s %10s" % (case, regime, p, f(rs), f(rd), f(rg)))
    # the verdict: mwsr, at 60 Hz, on the upscale cases
    verdict = []
    for regime in ("tick", "back"):
        rows = [ratios.get((c, regime, "mwsr")) for c in GATE_VERDICT_CASES]
        if any(r is None or r[1] is None for r in rows):
            verdict.append("%s: incomplete (%d of %d cases)" % (regime, sum(r is not None for r in rows), len(rows)))
            continue
        dones = [r[1] for r in rows]
        mains = [r[0] for r in rows if r[0] is not None]
        diffs = [r[3] for r in rows if r[3] is not None]  # WebGPU's main thread minus WebGL2's, ms
        gpu_main_ok = all(m <= GATE_MAIN or d <= GATE_MAIN_MS for m, d in zip(mains, diffs))
        gl_main_ok = all(m >= 1 / GATE_MAIN or -d <= GATE_MAIN_MS for m, d in zip(mains, diffs))
        if all(d <= GATE_WIN for d in dones) and gpu_main_ok:
            v = "WebGPU (done %.2f-%.2f × WebGL2, main thread p50 %s)" % (
                min(dones), max(dones), "/".join("%.2f" % m for m in mains))
        elif all(d >= 1 / GATE_WIN for d in dones) and gl_main_ok:
            v = "WebGL2 (WebGPU's done %.2f-%.2f × WebGL2's, main thread p50 %s)" % (
                min(dones), max(dones), "/".join("%.2f" % m for m in mains))
        else:
            v = "equal within the margins (done %.2f-%.2f ×, main thread p50 %s): the production flavour stays" % (
                min(dones), max(dones), "/".join("%.2f" % m for m in mains))
        verdict.append("%s: %s" % (regime, v))
    red = ratios.get(("x067-text", "tick", "mwsr"))
    if red and red[1] is not None:
        verdict.append("reduction ×0.67 at 60 Hz: WebGPU's done %.2f × WebGL2's (sRGB views against the 2×2 grid)" % red[1])
    lines += ["", "## Verdict (`mwsr`, cases %s; a flavour wins under %.2f × on `done` with its main thread p50 within %.1f × or %.2f ms)" % (
        ", ".join(GATE_VERDICT_CASES), GATE_WIN, GATE_MAIN, GATE_MAIN_MS), ""] + ["- " + v for v in verdict] + [""]
    for v in verdict:
        print("verdict:", v)
    path = os.path.join(a.dir, tag + "-gate.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote", path)
