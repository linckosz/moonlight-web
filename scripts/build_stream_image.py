"""Build the marketing 'stream' illustration from blf.ai source.
 - remove white/checkerboard background (border flood-fill on whiteness mask)
 - despeckle laptop (morphological closing of alpha, restore highlights from original)
 - replace laptop screen game with a cityscape (perspective warp), keep the www. bar
 - lengthen the Internet arrow x2, redraw it + crisp 'Internet' label in cyan
Outputs transparent PNG + lossless WebP.
"""
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SRC = r"C:\Users\Minis\Downloads\stream.webp"
CITY = r"C:\Users\Minis\Downloads\laptop_background.png"
OUT_PNG = r"C:\Users\Minis\Downloads\stream.png"
OUT_WEBP = r"C:\Users\Minis\Downloads\stream_transparent.webp"
FONT = r"C:\Windows\Fonts\arialbd.ttf"
CY = (25, 211, 255, 255)

# ---- content quad of the laptop screen (below the www. bar), original coords ----
TL = (745, 317); TR = (917, 397); BR = (905, 530); BL = (735, 452)
T_TITLE = 0.30            # fraction of screen height kept for the www. title bar
EXTRA = 258              # arrow lengthening (== original arrow length -> x2)

def lerp(a, b, t): return (a[0] + (b[0]-a[0])*t, a[1] + (b[1]-a[1])*t)

def find_coeffs(pa, pb):
    """coeffs mapping OUTPUT coords (pa) -> INPUT coords (pb) for Image.PERSPECTIVE."""
    m = []
    for (x, y), (u, v) in zip(pa, pb):
        m.append([x, y, 1, 0, 0, 0, -u*x, -u*y, u])
        m.append([0, 0, 0, x, y, 1, -v*x, -v*y, v])
    # Gaussian elimination on 8x9 augmented matrix
    n = 8
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(m[r][col]))
        m[col], m[piv] = m[piv], m[col]
        pv = m[col][col]
        for r in range(n):
            if r != col and m[r][col]:
                f = m[r][col] / pv
                m[r] = [m[r][k] - f*m[col][k] for k in range(9)]
    return [m[i][8] / m[i][i] for i in range(n)]

# ---------- 1) background removal ----------
src = Image.open(SRC).convert("RGBA")
W, H = src.size
data = list(src.getdata()); N = W*H; TH = 244
white = bytearray(1 if (p[0] >= TH and p[1] >= TH and p[2] >= TH) else 0 for p in data)
vis = bytearray(N); st = []
for x in range(W):
    for y in (0, H-1):
        i = y*W+x
        if white[i] and not vis[i]: vis[i] = 1; st.append(i)
for y in range(H):
    for x in (0, W-1):
        i = y*W+x
        if white[i] and not vis[i]: vis[i] = 1; st.append(i)
while st:
    i = st.pop()
    for j in ((i-1) if i % W else -1, (i+1) if i % W < W-1 else -1, i-W, i+W):
        if 0 <= j < N and white[j] and not vis[j]: vis[j] = 1; st.append(j)
cdata = [(0, 0, 0, 0) if vis[i] else data[i] for i in range(N)]
clean = Image.new("RGBA", (W, H)); clean.putdata(cdata)

# ---------- 2) despeckle: morphological closing of alpha, restore from original ----------
A = clean.getchannel("A")
Ac = A.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.MinFilter(9))
ac = list(Ac.getdata()); a0 = list(A.getdata())
for i in range(N):
    if ac[i] > 0 and a0[i] == 0:
        r, g, b, _ = data[i]
        cdata[i] = (r, g, b, 255)
clean.putdata(cdata)

# ---------- 3) replace screen content with cityscape (keep www. bar) ----------
city = Image.open(CITY).convert("RGBA")
cw, ch = city.size
cTL = lerp(TL, BL, T_TITLE); cTR = lerp(TR, BR, T_TITLE)
quad = [cTL, cTR, BR, BL]
coeffs = find_coeffs(quad, [(0, 0), (cw, 0), (cw, ch), (0, ch)])
warp = city.transform((W, H), Image.PERSPECTIVE, coeffs, Image.BICUBIC)
mask = Image.new("L", (W, H), 0)
ImageDraw.Draw(mask).polygon([(int(round(x)), int(round(y))) for x, y in quad], fill=255)
clean.paste(warp, (0, 0), mask)

# ---------- 4) compose wider canvas, lengthen arrow, redraw label ----------
out = Image.new("RGBA", (W + EXTRA + 40, H), (0, 0, 0, 0))
out.alpha_composite(clean.crop((0, 0, 336, H)), (0, 0))
out.alpha_composite(clean.crop((600, 290, 945, 662)), (600 + EXTRA, 290))

d = ImageDraw.Draw(out)
arrow_start = 339; tip_x = arrow_start + 516; yc = 479; th = 9; dash = 27; gap = 15
base_x = tip_x - 40
x = arrow_start
while x + dash < base_x - 6:
    d.rounded_rectangle([x, yc-th//2, x+dash, yc+th//2], radius=4, fill=CY); x += dash + gap
d.polygon([(tip_x, yc), (base_x, yc-31), (base_x, yc+31)], fill=CY)

font = ImageFont.truetype(FONT, 46)
label = "Internet"
tb = d.textbbox((0, 0), label, font=font)
tw, thh = tb[2]-tb[0], tb[3]-tb[1]
cx = (arrow_start + tip_x)//2
d.text((cx - tw//2 - tb[0], 405 - tb[1]), label, font=font, fill=CY)

# ---------- 5) crop tight and save ----------
out = out.crop(out.getbbox())
out.save(OUT_PNG)
out.save(OUT_WEBP, lossless=True)
# dark-bg preview copy
prev = Image.new("RGBA", out.size, (8, 12, 17, 255))
prev = Image.alpha_composite(prev, out)
prev.convert("RGB").save(r"D:\Code\moonlight-web\website\assets\_screen.png")
print("done", out.size)
