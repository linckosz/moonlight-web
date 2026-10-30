/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

// The HLSL of the Windows colour conversion, in one place: the D3D11 converter
// (ColorConvert) draws it, and the D3D12 one draws the very same text, compiled
// the same way, so a stream looks the same whichever pipeline carried it
// (design §32). Moved here unchanged from ColorConvert.cpp.

namespace mw::native::convert {

// ── The conversion, in HLSL ─────────────────────────────────────────────────
//
// BT.709, limited ("TV") range — the same colour space the existing pipeline
// already negotiates for SDR streams, so the browser's decoder needs no change
// and a native stream looks identical to a Sunshine one on the same screen.
//
// The vertex shader builds a full-screen triangle from SV_VertexID alone: no
// vertex buffer, no input layout, nothing to bind. A triangle rather than a
// quad because a quad's diagonal makes the GPU shade the seam twice.
inline constexpr char kShaderSource[] = R"HLSL(
Texture2D<float4> Source       : register(t0);
Texture2D<float4> CursorPixels : register(t1);
Texture2D<float>  CursorInvert : register(t2);
SamplerState      Linear       : register(s0);
SamplerState      Nearest      : register(s1);

// xy: the cursor's top-left in source UV. zw: its size in source UV.
// Enabled is 0 or 1 rather than a branch on the size, so a zero-size shape
// cannot divide by zero on its way to being invisible.
// SdrWhite is where the desktop's SDR white sits in scRGB — 1.0 for the 80-nit
// default, more when the "SDR content brightness" slider is up. Only the FP16
// paths read it; an 8-bit source has no such thing.
cbuffer Overlay : register(b0)
{
    float4 CursorRect;
    float  CursorEnabled;
    float  SdrWhite;
    float2 OverlayPad;
};

struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VsOut VsMain(uint id : SV_VertexID)
{
    VsOut o;
    // Two of these three vertices fall outside the viewport; the clip does the
    // rest. Cheaper than a quad and needs no buffer.
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// BT.709 luma weights.
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// Limited range: luma lives in 16..235, chroma in 16..240, both scaled to the
// 0..1 a UNORM target stores.
static const float kLumaScale  = 219.0 / 255.0;
static const float kLumaBias   =  16.0 / 255.0;
static const float kChromaScale = 224.0 / 255.0;
static const float kChromaBias = 128.0 / 255.0;

// ── HDR: scRGB FP16 → BT.2020 PQ, 10-bit limited range ──────────────────────
//
// What DXGI hands over for an HDR desktop is scRGB: linear light, BT.709
// primaries, and 1.0 meaning 80 nits. That is NOT where the desktop's own
// white is: the compositor paints SDR content (every window, the wallpaper, the
// pointer) at the "SDR content brightness" level, which is SdrWhite here —
// 1.0 at the slider's default, and typically 2 to 3 on a display someone has
// actually used. Values above that are the HDR highlights, and values below 0
// are the colours outside BT.709 that scRGB expresses as negatives.
//
// Four steps, in this order, and the order is not negotiable: primaries first
// (a matrix is only linear-light-valid), then the absolute scale, then the PQ
// curve, then the YCbCr matrix — which is defined on the PQ-encoded signal, not
// on light. Doing PQ after the YCbCr matrix is the classic way to get a picture
// that is nearly right and subtly wrong in every gradient.

// BT.709 → BT.2020 primaries, linear light.
static const float3x3 kBt709ToBt2020 = float3x3(
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8956050);

// PQ (SMPTE ST 2084) constants.
static const float kPqM1 = 0.1593017578125;   // 2610 / 16384
static const float kPqM2 = 78.84375;          // 2523 / 4096 * 128
static const float kPqC1 = 0.8359375;         // 3424 / 4096
static const float kPqC2 = 18.8515625;        // 2413 / 4096 * 32
static const float kPqC3 = 18.6875;           // 2392 / 4096 * 32

// scRGB 1.0 is 80 nits; PQ is defined against a 10000-nit peak.
static const float kScRgbToPqDomain = 80.0 / 10000.0;

// BT.2020 non-constant luminance.
static const float3 kLuma2020 = float3(0.2627, 0.6780, 0.0593);

// P010 keeps its 10 bits in the HIGH bits of each 16-bit word, so a UNORM
// render target wants the code shifted left by 6. Writing code/1023 instead
// would be a picture 64× too dark, which is the one mistake here that looks
// like a dead screen rather than a wrong colour.
static const float kP010 = 64.0 / 65535.0;

// One 10-bit code, placed where P010 wants it.
//
// The rounding is the point. Handing the target a fractional code lets the
// UNORM conversion land anywhere in the 16-bit range, which leaves rubbish in
// the six padding bits that the format defines as zero. Hardware reads the top
// ten bits and does not care, so this costs nothing visible either way — but a
// surface that is only accidentally valid is one nobody can check, and the
// round trip through a staging texture is exactly how the tests verify that
// the shift happened at all.
float P010Code(float value, float scale, float bias)
{
    return floor(value * scale + bias + 0.5) * kP010;
}

float3 PqFromLinear(float3 linearRgb)
{
    // Negative scRGB is real (it is how the format carries colours outside
    // BT.709) but PQ has no answer for it, and pow() of a negative is NaN —
    // which propagates through the whole frame. Clamp after the BT.2020
    // matrix, where the out-of-gamut values have already had their chance to
    // become positive.
    float3 y = max(linearRgb, 0.0) * kScRgbToPqDomain;
    float3 ym = pow(y, kPqM1);
    return pow((kPqC1 + kPqC2 * ym) / (1.0 + kPqC3 * ym), kPqM2);
}

// The HDR scene, in linear scRGB with the pointer composited in.
//
// The cursor's pixels are 8-bit sRGB, so they have to be linearised and placed
// at the desktop's SDR white — blending them as if they were already linear
// makes a pointer that is far too dark on an HDR desktop, and placing them at
// 1.0 makes one dimmer than the window it is over once the slider is up. The
// invert mask is bounded against SDR white too: "white - rgb" on a highlight
// far above it would be deeply negative, and a negative that then meets the
// clamp in PqFromLinear turns the I-beam into a black hole.
float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow(max(c + 0.055, 0.0) / 1.055, 2.4);
}

float3 SceneHdr(float2 uv)
{
    float3 rgb = Source.Sample(Linear, uv).rgb;
    if (CursorEnabled < 0.5) return rgb;

    float2 c = (uv - CursorRect.xy) / CursorRect.zw;
    if (c.x < 0.0 || c.y < 0.0 || c.x > 1.0 || c.y > 1.0) return rgb;

    if (CursorInvert.SampleLevel(Nearest, c, 0) > 0.5)
        return SdrWhite - clamp(rgb, 0.0, SdrWhite);

    float4 cursor = CursorPixels.SampleLevel(Linear, c, 0);
    return lerp(rgb, SrgbToLinear(cursor.rgb) * SdrWhite, cursor.a);
}

// ── HDR desktop → SDR stream: the tone map ──────────────────────────────────
//
// What an SDR session gets from an HDR desktop. DXGI can hand over 8-bit
// itself, but what it does on the way is a clip at 80 nits — and with the SDR
// slider up, every window is brighter than that, so the whole desktop arrives
// blown out (the beach wallpaper on DualRTX, 16/09/2026: sand and foam gone
// to flat white). So the desktop is taken as scRGB and brought down here.
//
// The curve is the client's own soft-clip (WebGpuRenderer, HDR_COMMON_WGSL),
// so an HDR desktop looks the same whichever side does the work: identity up
// to a knee at 90 % of SDR white — every window, all text, an SDR game come
// through exactly as an SDR stream would carry them — and a tanh shoulder
// above it that folds the HDR highlights into the headroom that is left.
// Scaled on luminance rather than per channel so a highlight keeps its hue
// on the way down. Not ACES: ACES moves the midtones too, and on a desktop
// the midtones are the picture.
float3 LinearToSrgb(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
}

static const float kKnee = 0.9;

float3 ToneMapToSdr(float3 scRgb)
{
    // SDR white to 1.0 first; the negatives (out-of-gamut) have nowhere to go
    // in BT.709 and are clipped.
    float3 rgb = max(scRgb / SdrWhite, 0.0);
    float  y   = dot(rgb, kLuma);
    if (y > kKnee) {
        float yc = kKnee + (1.0 - kKnee) * tanh((y - kKnee) / (1.0 - kKnee));
        rgb *= yc / y;
    }
    return LinearToSrgb(saturate(rgb));
}

// The desktop with the mouse pointer drawn on it, in 8-bit sRGB — what the
// BT.709 shaders below encode.
//
// Two sources, chosen when the shader is compiled (MW_SCRGB_SOURCE): the 8-bit
// desktop as it is, or the FP16 one tone-mapped down. Same entry points either
// way, so the session's draw code never knows.
//
// Windows composites the cursor at scan-out, so the duplicated frame never
// contains it and we have to put it back. Doing that HERE rather than in a
// second pass costs one texture fetch on the pixels the cursor covers and
// nothing at all anywhere else — a separate overlay pass would mean another
// full-frame render target and another round trip through VRAM.
//
// Colour is sampled linearly so a cursor scaled down with the picture keeps its
// antialiased edges; the invert mask is sampled nearest, because a half-inverted
// pixel is not a thing and interpolating the flag would fringe the I-beam.
#if MW_SCRGB_SOURCE
float3 Scene(float2 uv)
{
    return ToneMapToSdr(SceneHdr(uv));
}
#else
float3 Scene(float2 uv)
{
    float3 rgb = Source.Sample(Linear, uv).rgb;
    if (CursorEnabled < 0.5) return rgb;

    float2 c = (uv - CursorRect.xy) / CursorRect.zw;
    if (c.x < 0.0 || c.y < 0.0 || c.x > 1.0 || c.y > 1.0) return rgb;

    // Monochrome cursors carry no colour of their own: they are defined as
    // inverting the background, which is what keeps a text I-beam visible over
    // black text and over white paper alike.
    if (CursorInvert.SampleLevel(Nearest, c, 0) > 0.5) return 1.0 - rgb;

    float4 cursor = CursorPixels.SampleLevel(Linear, c, 0);
    return lerp(rgb, cursor.rgb, cursor.a);
}
#endif

float PsLuma(VsOut i) : SV_TARGET
{
    float3 rgb = Scene(i.uv);
    return dot(rgb, kLuma) * kLumaScale + kLumaBias;
}

// 4:4:4, written as packed AYUV in one draw.
//
// NVENC's AYUV is a 32-bit word with V in the lowest byte, then U, then Y, then
// A — so in memory the bytes are [V][U][Y][A], and an R8G8B8A8 render-target
// view lands them as R=V, G=U, B=Y, A=A. Getting that order wrong produces a
// picture with the colours swapped rather than an error, which is why it is
// spelled out here.
float4 PsPacked444(VsOut i) : SV_TARGET
{
    float3 rgb = Scene(i.uv);
    float  y   = dot(rgb, kLuma);

    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;

    return float4(cr * kChromaScale + kChromaBias,  // R <- V
                  cb * kChromaScale + kChromaBias,  // G <- U
                  y  * kLumaScale   + kLumaBias,    // B <- Y
                  1.0);
}

float2 PsChroma(VsOut i) : SV_TARGET
{
    // Sampling once at the chroma texel's centre lets the sampler average the
    // 2x2 luma neighbourhood for us, which is what 4:2:0 wants anyway.
    float3 rgb = Scene(i.uv);
    float  y   = dot(rgb, kLuma);

    // The BT.709 denominators: 2*(1-Kb) and 2*(1-Kr).
    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;
    return float2(cb, cr) * kChromaScale + kChromaBias;
}

float PsLumaHdr(VsOut i) : SV_TARGET
{
    float3 pq = PqFromLinear(mul(kBt709ToBt2020, SceneHdr(i.uv)));
    float  y  = dot(pq, kLuma2020);
    // Limited range, 10-bit: luma 64..940.
    return P010Code(y, 876.0, 64.0);
}

float2 PsChromaHdr(VsOut i) : SV_TARGET
{
    float3 pq = PqFromLinear(mul(kBt709ToBt2020, SceneHdr(i.uv)));
    float  y  = dot(pq, kLuma2020);

    // The BT.2020 denominators: 2*(1-Kb) and 2*(1-Kr).
    float cb = (pq.b - y) / 1.8814;
    float cr = (pq.r - y) / 1.4746;
    // Limited range, 10-bit: chroma 64..960 around a 512 centre.
    return float2(P010Code(cb, 896.0, 512.0), P010Code(cr, 896.0, 512.0));
}

// A plane at one value, the Overlay's first four floats: the D3D12 converter's
// black, drawn where a clear loses some drivers' device (ColorConvert12).
float4 PsFill(VsOut i) : SV_TARGET
{
    return CursorRect;
}
)HLSL";

// ── The resample pass: Lanczos-2 dilated to the ratio, separable ────────────
//
// What the bench (tools/scaler-bench, 17/09/2026) picked over every fixed-
// radius filter and every vendor upscaler: the only family that is a real
// low-pass at any ratio is a kernel STRETCHED to the ratio, and Lanczos-2 is
// the cheapest of it that stays within 1 dB of the reference. Two 1-D passes
// instead of the bench's one 2-D pass: 2 × 7 taps at 1440p → 1080p where the
// square kernel took 49, 2 × 13 at 4K → 720p where it took 169.
//
// In linear light: the 8-bit desktop is decoded per tap on the way in and the
// _SRGB render target re-encodes the result on the way out, so the average of
// black and white text pixels is the grey the eye expects rather than the
// darker one gamma-space averaging gives — measured +4.6 dB on that alone.
// The scRGB paths are linear already (MW_DECODE 0).
//
// Every size is a compile-time constant (this shader is built per session for
// one geometry), so the tap loop unrolls and there is no constant buffer. One
// entry point, compiled twice: MW_HORIZONTAL 1 reads the capture along X into
// an output-wide, source-high intermediate; 0 reads that intermediate along Y
// into the output-sized picture.
inline constexpr char kScaleShaderSource[] = R"HLSL(
Texture2D<float4> Source : register(t0);

struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow(max(c + 0.055, 0.0) / 1.055, 2.4);
}

// Lanczos with two lobes, x already divided by the dilation.
float Lanczos2(float x)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

float3 Fetch(int x, int y)
{
    float3 c = Source.Load(int3(x, y, 0)).rgb;
#if MW_DECODE
    c = SrgbToLinear(c);
#endif
    return c;
}

float4 PsScale(VsOut i) : SV_TARGET
{
    // The output pixel's centre in source pixels along the filtered axis, and
    // its row (or column) along the other, which the pass keeps as it is.
#if MW_HORIZONTAL
    float centre = i.uv.x * MW_LEN - 0.5;
    int   fixed  = int(i.uv.y * MW_FIXED);
#else
    float centre = i.uv.y * MW_LEN - 0.5;
    int   fixed  = int(i.uv.x * MW_FIXED);
#endif
    int first = int(ceil(centre - 2.0 * MW_DILATE));
    float3 acc = 0.0;
    float  sum = 0.0;
    [unroll]
    for (int t = 0; t < MW_TAPS; ++t) {
        int   p = first + t;
        float w = Lanczos2((float(p) - centre) / MW_DILATE);
        p = clamp(p, 0, int(MW_LEN) - 1);
#if MW_HORIZONTAL
        acc += Fetch(p, fixed) * w;
#else
        acc += Fetch(fixed, p) * w;
#endif
        sum += w;
    }
    // Lanczos undershoots on hard edges; light does not go negative, and an
    // _SRGB target would clamp anyway.
    return float4(max(acc / sum, 0.0), 1.0);
}
)HLSL";

// ── MWSR's statistics of the capture, for the sharpness by zone ─────────────
//
// Plan B §12 c (upscaler-mwsr-b.md): per pixel of the 8-bit capture, on its
// sRGB-encoded values, its neighbours 1 px away and the picture's edges
// clamped, the 8 statistics the bench's tree reads (MwsrZones.h), and their
// means per cell of a MW_CELLS_X × MW_CELLS_Y grid: cell k spans the pixels
// k * size / cells to (k + 1) * size / cells, as the bench cut them. A thread
// group per cell, 64 threads striding over it, then a tree in groupshared
// memory; the cell's 8 means go out as two float4.
//
// Every operation on a pixel is `precise`, so that FXC fuses no multiply and
// add and keeps the order written: the order a CPU reference can repeat bit
// for bit, and the one of the bench (numpy, in float32) but for the luma's
// three products and the sums. The capture's size is compiled in (MW_WIDTH,
// MW_HEIGHT), like the resample pass's.
inline constexpr char kMwsrStatsSource[] = R"HLSL(
Texture2D<float4>          Source : register(t0);
RWStructuredBuffer<float4> Cells  : register(u0);

static const int kW = MW_WIDTH;
static const int kH = MW_HEIGHT;

float Luma(int x, int y)
{
    precise float3 c = Source.Load(int3(x, y, 0)).rgb;
    precise float  l = c.r * 0.2126 + c.g * 0.7152 + c.b * 0.0722;
    return l;
}

groupshared float4 gHead[64];
groupshared float4 gTail[64];

[numthreads(8, 8, 1)]
void CsStats(uint3 cell : SV_GroupID, uint3 t : SV_GroupThreadID, uint i : SV_GroupIndex)
{
    const int x0 = int(cell.x) * kW / MW_CELLS_X;
    const int x1 = (int(cell.x) + 1) * kW / MW_CELLS_X;
    const int y0 = int(cell.y) * kH / MW_CELLS_Y;
    const int y1 = (int(cell.y) + 1) * kH / MW_CELLS_Y;

    // luma, luma², gradient, gradient > 0.05 | gradient < 0.01, |laplacian|,
    // saturation, luma < 0.15
    precise float4 head = 0.0;
    precise float4 tail = 0.0;
    for (int y = y0 + int(t.y); y < y1; y += 8) {
        for (int x = x0 + int(t.x); x < x1; x += 8) {
            precise float3 rgb = Source.Load(int3(x, y, 0)).rgb;
            precise float  c   = rgb.r * 0.2126 + rgb.g * 0.7152 + rgb.b * 0.0722;
            precise float  lx  = Luma(min(x + 1, kW - 1), y);
            precise float  lw  = Luma(max(x - 1, 0), y);
            precise float  ly  = Luma(x, min(y + 1, kH - 1));
            precise float  ln  = Luma(x, max(y - 1, 0));
            precise float  g   = abs(lx - c) + abs(ly - c);
            precise float  lap = abs(lx + ly + lw + ln - 4.0 * c);
            precise float  sat = max(rgb.r, max(rgb.g, rgb.b)) - min(rgb.r, min(rgb.g, rgb.b));
            head += float4(c, c * c, g, g > 0.05 ? 1.0 : 0.0);
            tail += float4(g < 0.01 ? 1.0 : 0.0, lap, sat, c < 0.15 ? 1.0 : 0.0);
        }
    }

    gHead[i] = head;
    gTail[i] = tail;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint s = 32; s > 0; s >>= 1) {
        if (i < s) {
            gHead[i] += gHead[i + s];
            gTail[i] += gTail[i + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (i == 0) {
        const float n = float((x1 - x0) * (y1 - y0));
        const uint  k = (cell.y * MW_CELLS_X + cell.x) * 2;
        Cells[k]     = gHead[0] / n;
        Cells[k + 1] = gTail[0] / n;
    }
}
)HLSL";

} // namespace mw::native::convert
