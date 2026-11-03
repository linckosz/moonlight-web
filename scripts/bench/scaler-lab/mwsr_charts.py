"""The scores chart of mwsr/README.md, in light and dark: LPIPS and VMAF NEG of the five upscalers on the game picture and
the text chart at x1.33 and x2, from lab-scores.json (cmp_lab.py). Each panel has its own scale, and in each
one the further right is the closer to the native frame.

    py -E -P mwsr_charts.py          writes mwsr/docs/scores-light.svg and scores-dark.svg
"""
import json
import os

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
SCORES = os.path.join(REPO, "scripts", "bench", "scaler-lab", "lab-scores.json")
OUT = os.path.join(REPO, "mwsr", "docs")
THEMES = {
    "light": dict(ink="#0b0b0b", soft="#52514e", mute="#8a8984", grid="#e4e3df", mwsr="#2a78d6", dot="#9b9a95", low="#c9c8c3"),
    "dark": dict(ink="#f0efec", soft="#c3c2b7", mute="#8f8e88", grid="#33322f", mwsr="#3987e5", dot="#8f8e88", low="#55544f"),
}
ROWS = [("MWSR", "mwsr"), ("FSR1", "fsr1"), ("NIS", "nis"), ("SGSR1", "sgsr1"), ("Bilinear", "bilinear")]
SCALES = [("1.33", "1080p → 1440p"), ("2", "720p → 1440p")]
PICS = [("0ad", "Game (0 A.D.)"), ("text", "Text chart")]
METRICS = [(0, "LPIPS", "%.4f", True), (1, "VMAF NEG", "%.1f", False)]
COLS = [(0, 78.0, 392.0), (428, 506.0, 820.0)]
ROW_H, PANEL_H, TOP, GROUP_GAP = 29, 233, 66, 34
WIDTH = 828


def panel(out, t, x0, x1, x2, y, title, scores, index, fmt, lower_is_better):
    out.append('<text x="%d" y="%d" font-size="13" font-weight="600" fill="%s">%s</text>' % (x0, y, t["ink"], title))
    vals = [scores[k][index] for _, k in ROWS]
    lo, hi = min(vals), max(vals)
    pad = (hi - lo) * 0.08 or 1.0

    def px(v):
        share = (v - (lo - pad)) / (hi - lo + 2 * pad)
        if lower_is_better:
            share = 1 - share
        return x1 + share * (x2 - 40 - x1)

    yy = y + 26
    for name, key in ROWS:
        v = scores[key][index]
        bold = ' font-weight="600"' if key == "mwsr" else ""
        ink = t["ink"] if key == "mwsr" else t["soft"]
        fill = t["mwsr"] if key == "mwsr" else t["low"] if key == "bilinear" else t["dot"]
        out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" stroke-width="1"/>' % (x1, yy, x2, yy, t["grid"]))
        out.append('<text x="%d" y="%d" font-size="13" fill="%s" dominant-baseline="middle"%s>%s</text>' % (x0, yy, ink, bold, name))
        out.append('<circle cx="%.1f" cy="%d" r="6" fill="%s"/>' % (px(v), yy, fill))
        out.append('<text x="%.1f" y="%d" font-size="12" fill="%s" dominant-baseline="middle">%s</text>' % (
            px(v) + 11, yy, t["soft"], fmt % v))
        yy += ROW_H
    yy -= 4
    out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" stroke-width="1.5" marker-end="url(#arrow)"/>' % (
        x1, yy, x2 - 2, yy, t["mute"]))
    out.append('<text x="%.1f" y="%d" font-size="11" fill="%s">further</text>' % (x1, yy + 16, t["mute"]))
    out.append('<text x="%.1f" y="%d" font-size="11" fill="%s" text-anchor="end">closer to the native frame</text>' % (
        x2, yy + 16, t["mute"]))


def chart(theme, scores):
    t = THEMES[theme]
    height = TOP + 2 * (2 * PANEL_H + 30) + GROUP_GAP + 12
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
           'font-family="-apple-system, BlinkMacSystemFont, \'Segoe UI\', Helvetica, Arial, sans-serif" role="img" '
           'aria-labelledby="t d">' % (WIDTH, height, WIDTH, height),
           '<title id="t">LPIPS and VMAF NEG of MWSR, FSR1, NIS, SGSR1 and bilinear against the native frame</title>',
           '<desc id="d">On a game picture and a text chart, at two scales. The values are in the tables below the '
           'figure.</desc>',
           '<text x="0" y="22" font-size="17" font-weight="600" fill="%s">Closeness to the native frame</text>' % t["ink"],
           '<text x="0" y="44" font-size="13" fill="%s">The further right, the closer. LPIPS: lower is closer. VMAF NEG: '
           'higher is closer. Each panel has its own scale.</text>' % t["soft"],
           '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
           'orient="auto"><path d="M0 0 L10 5 L0 10 z" fill="%s"/></marker></defs>' % t["mute"]]
    y = TOP
    for index, metric, fmt, lower in METRICS:
        out.append('<text x="0" y="%d" font-size="15" font-weight="600" fill="%s">%s</text>' % (y + 6, t["ink"], metric))
        y += 30
        for scale, label in SCALES:
            for (pic, name), (x0, x1, x2) in zip(PICS, COLS):
                panel(out, t, x0, x1, x2, y, "%s, %s" % (name, label), scores["%s|%s|444|0" % (pic, scale)], index, fmt, lower)
            y += PANEL_H
        y += GROUP_GAP
    out.append('<text x="0" y="%d" font-size="12" fill="%s">Native frame: 0 A.D. reduced from 4K, or the lab\'s text chart; '
               'input: its Lanczos-3 reduction in linear light.</text>' % (y - GROUP_GAP + 4, t["mute"]))
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    with open(SCORES, encoding="utf-8") as f:
        scores = json.load(f)["scores"]
    for theme in THEMES:
        p = os.path.join(OUT, "scores-%s.svg" % theme)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(chart(theme, scores))
        print("wrote", p)


if __name__ == "__main__":
    main()
