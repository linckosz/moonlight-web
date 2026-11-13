"""Source pictures for bench.html, one folder per stream geometry: src/<W>x<H>/.

    py -E -X utf8 prep.py 1920x1080 1280x720 ...
    py -E -X utf8 prep.py 1920x1080 --code-only     # the coloured code page alone (color444 plan)

Stills (junkshop, gears) are scaled to cover W x H and centre-cropped; the tall pages
(google, web text) are scaled to width W. Lanczos, plain sRGB, PNG without metadata.
The Google capture is lab-only: never published, never committed.
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
PAGES = os.environ.get("MW_BENCH_PAGES", os.path.join(HERE, "content", "pages"))
STILLS = {
    "junkshop": os.path.join(REPO, r"website\mwsr\img\junkshop-1440.webp"),
    "gears": os.path.join(REPO, r"website\mwsr\img\gears-1440.webp"),
    # native 4K still: witness for 2160p, where the 1440 sources above are upscaled
    "cp4k": os.environ.get("MW_BENCH_CP4K", os.path.join(HERE, "content", "cp4k.png")),
}


def text_page(h):
    best = {720: "web-720.png", 1080: "web-1080.png", 1440: "web-1440.png"}
    for k in sorted(best):
        if h <= k:
            return os.path.join(PAGES, best[k])
    return os.path.join(PAGES, "web-1440.png")


def cover(im, w, h):
    s = max(w / im.width, h / im.height)
    nw, nh = max(w, round(im.width * s)), max(h, round(im.height * s))
    im = im.resize((nw, nh), Image.LANCZOS) if (nw, nh) != im.size else im
    x, y = (nw - w) // 2, (nh - h) // 2
    return im.crop((x, y, x + w, y + h))


def width(im, w):
    if im.width == w:
        return im
    return im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)


# ── coloured code (the "color444" plan) ─────────────────────────────────────
# Where full chroma matters: thin coloured text on a dark and on a light editor theme, as an
# IDE shows it. The text is this repository's own source; the font is Windows' Consolas,
# greyscale antialiasing (no ClearType: the same pixels on every machine).
CODE_SRC = os.path.join(REPO, r"backend\src\backend\VirtualDisplayRules.cpp")
FONT = os.environ.get("MW_BENCH_CODE_FONT", "consola.ttf")
THEMES = {
    # background, gutter, plain, keyword, control, type, function, string, number, comment, preproc
    "dark": ("#1e1e1e", "#858585", "#9cdcfe", "#569cd6", "#c586c0", "#4ec9b0", "#dcdcaa",
             "#ce9178", "#b5cea8", "#6a9955", "#c586c0"),
    "light": ("#ffffff", "#237893", "#001080", "#0000ff", "#af00db", "#267f99", "#795e26",
              "#a31515", "#098658", "#008000", "#af00db"),
}
KEYWORDS = set("auto bool char class const constexpr double enum explicit false float int long namespace "
               "nullptr operator private protected public static struct template true typename using "
               "virtual void unsigned signed sizeof".split())
CONTROL = set("if else for while do switch case default break continue return goto try catch throw".split())
TOKEN = re.compile(r'(//.*)|("(?:\\.|[^"\\])*")|(\'(?:\\.|[^\'\\])*\')|(\b\d[\w.]*)|(#\s*\w+)|([A-Za-z_]\w*)|(\s+)|(.)')


def tokens(line):
    """(text, role) for one line of C++: role indexes the theme tuple."""
    for m in TOKEN.finditer(line):
        t = m.group(0)
        if m.group(1):
            yield t, 9
        elif m.group(2) or m.group(3):
            yield t, 7
        elif m.group(4):
            yield t, 8
        elif m.group(5):
            yield t, 10
        elif m.group(6):
            rest = line[m.end():].lstrip()
            if t in CONTROL:
                yield t, 4
            elif t in KEYWORDS:
                yield t, 3
            elif rest.startswith("("):
                yield t, 6
            elif t[0].isupper():
                yield t, 5
            else:
                yield t, 2
        else:
            yield t, 2


def code_page(w, h):
    """W x 3H: the dark theme over the top half, the light one below; a pan crosses both."""
    size = max(8, round(15 * h / 1080))
    font = ImageFont.truetype(FONT, size)
    adv = font.getlength("M")
    lh = round(size * 1.35)
    lines = open(CODE_SRC, encoding="utf-8").read().expandtabs(4).splitlines()
    # from the first line after the licence header (no name or address in the picture)
    end = next(i for i, l in enumerate(lines) if l.strip() == "*/")
    dash = {"—": "-", "–": "-", "’": "'", "‘": "'", "“": '"', "”": '"', "…": "..."}
    lines = ["".join(dash.get(c, c) for c in l).encode("ascii", "replace").decode()
             for l in lines[end + 1:]]  # Consolas, plain ASCII (dashes and quotes made ASCII first)
    while lines and not lines[0].strip():
        lines.pop(0)
    page = Image.new("RGB", (w, 3 * h))
    half = 3 * h // 2
    gut = round(6 * adv)
    x0 = gut + round(2 * adv)
    k = 0
    for top, bottom, theme in ((0, half, "dark"), (half, 3 * h, "light")):
        th = THEMES[theme]
        d = ImageDraw.Draw(page)
        d.rectangle((0, top, w - 1, bottom - 1), fill=th[0])
        y = top + lh // 2
        while y + lh <= bottom:
            line = lines[k % len(lines)]
            d.text((gut - font.getlength(str(k % len(lines) + 1)) - adv, y), str(k % len(lines) + 1),
                   font=font, fill=th[1])
            col = 0
            for t, role in tokens(line):
                if not t.isspace():
                    d.text((x0 + col * adv, y), t, font=font, fill=th[role])
                col += len(t)
            k += 1
            y += lh
    return page


def main():
    code_only = "--code-only" in sys.argv  # the coloured code alone, the other pictures left as they are
    for geo in [g for g in sys.argv[1:] if not g.startswith("--")]:
        w, h = map(int, geo.split("x"))
        d = os.path.join(HERE, "content", "src", geo)
        os.makedirs(d, exist_ok=True)
        if code_only:
            code_page(w, h).save(os.path.join(d, "code.png"), optimize=False)
            print(geo, "code.png", Image.open(os.path.join(d, "code.png")).size)
            continue
        for name, p in STILLS.items():
            cover(Image.open(p).convert("RGB"), w, h).save(os.path.join(d, name + ".png"), optimize=False)
        width(Image.open(os.path.join(PAGES, "google-1440.png")).convert("RGB"), w).save(os.path.join(d, "google.png"))
        width(Image.open(text_page(h)).convert("RGB"), w).save(os.path.join(d, "web.png"))
        # The HDR highlights' scroll ("hdrpeak" plan): the two renders, one above the other,
        # W x 2H, so a pan crosses both pictures and their highlights.
        tall = Image.new("RGB", (w, 2 * h))
        tall.paste(Image.open(os.path.join(d, "junkshop.png")), (0, 0))
        tall.paste(Image.open(os.path.join(d, "gears.png")), (0, h))
        tall.save(os.path.join(d, "jsgears.png"), optimize=False)
        code_page(w, h).save(os.path.join(d, "code.png"), optimize=False)
        for f in sorted(os.listdir(d)):
            im = Image.open(os.path.join(d, f))
            print(geo, f, im.size)


if __name__ == "__main__":
    main()
