#!/usr/bin/env python3
"""Recolour the production app icons into the DEV edition's blue.

A DEV identity (the MoonlightWebDev build, or a --dev instance) wears these in
the tray, in the browser tab, on its shortcuts and on MoonlightWebDev.exe, so it
can never be taken for the production install sitting beside it. Only the ICONS
change: the logos (logo.png, logo-512.png) and the website keep their colour.

The source glyph is a single flat colour on transparency, so recolouring is
exact: every pixel takes the new colour and keeps its own alpha, anti-aliased
edges included. Each frame of favicon.ico is recoloured on its own rather than
regenerated from the 512 px PNG, so the small sizes keep whatever hinting they
were drawn with.

    python scripts/make-dev-icons.py

Writes frontend/assets/dev/ (committed; re-run after changing a source icon).
"""
from pathlib import Path

from PIL import Image

# Cobalt: reads as blue at a glance and stays visible on a dark taskbar, where a
# true midnight blue disappears.
DEV_BLUE = (0x3D, 0x6B, 0xFF)

ASSETS = Path(__file__).resolve().parent.parent / "frontend" / "assets"
OUT = ASSETS / "dev"
PNGS = ("icon-180.png", "icon-192.png", "icon-512.png")


def recolour(image: Image.Image) -> Image.Image:
    rgba = image.convert("RGBA")
    out = Image.new("RGBA", rgba.size, DEV_BLUE + (255,))
    out.putalpha(rgba.getchannel("A"))
    return out


def main() -> None:
    OUT.mkdir(exist_ok=True)

    for name in PNGS:
        recolour(Image.open(ASSETS / name)).save(OUT / name, optimize=True)
        print(f"wrote {OUT / name}")

    source = Image.open(ASSETS / "favicon.ico")
    sizes = sorted(source.info["sizes"])
    frames = []
    for size in sizes:
        source.size = size
        frames.append(recolour(source.copy()))
    largest = frames[-1]
    largest.save(OUT / "favicon.ico", format="ICO", sizes=sizes, append_images=frames[:-1])
    print(f"wrote {OUT / 'favicon.ico'} {sizes}")


if __name__ == "__main__":
    main()
