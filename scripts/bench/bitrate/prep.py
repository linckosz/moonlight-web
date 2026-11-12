"""Source pictures for bench.html, one folder per stream geometry: src/<W>x<H>/.

    py -E -X utf8 prep.py 1920x1080 1280x720 ...

Stills (junkshop, gears) are scaled to cover W x H and centre-cropped; the tall pages
(google, web text) are scaled to width W. Lanczos, plain sRGB, PNG without metadata.
The Google capture is lab-only: never published, never committed.
"""
import os
import sys

from PIL import Image

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


def main():
    for geo in sys.argv[1:]:
        w, h = map(int, geo.split("x"))
        d = os.path.join(HERE, "content", "src", geo)
        os.makedirs(d, exist_ok=True)
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
        for f in sorted(os.listdir(d)):
            im = Image.open(os.path.join(d, f))
            print(geo, f, im.size)


if __name__ == "__main__":
    main()
