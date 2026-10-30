// The upscaler laboratory of the v0.4.1 plan (docs/design/upscaler-mwsr.md,
// §6.1, §6.4 and §7): the same pictures through N cells, each cell a chain of
// full-screen passes, timed by the GPU's own timestamps (timestamp-query),
// batched so that the fence round trip never enters the figure.
//
// Driven by scaler-lab.html, whose query carries the parameters:
//   in=1280x720 out=2560x1440   input and output sizes (16:9 both, as a stream)
//   content=text|ui|game|zone|code|codeiso|img:<url>|f16:<url>
//                                the picture: drawn by the page, fetched as an
//                                image, or (hdr=1) fetched as RGBA half floats
//   hdr=1                        the HDR feed: rgba16float textures and targets
//                                (the renderer's `linear` mode), dumps as .f16
//   set=feed,base,chain          which cells (see CELL_SETS), or cells=a,b,c
//   codec=avc|hevc|vp9|none      VideoFrames from a WebCodecs encode → decode
//                                loop (the external-texture feed), or none
//   enc=sw                       that encode in software (plan B §6: never the
//                                encoder on the GPU that decodes and is timed);
//                                vp9 where a phone has no software H.264
//   pace=back|tick period=16.67  back to back, or one iteration per period
//   batch=16 batches=8 warm=2 rounds=3   the measurement (ABBA across rounds)
//   power=high-performance|low-power fallback=1   which adapter
//   f16=1                        ask for shader-f16 when the adapter has it
//   shot=1                       a crop of each cell's output, as PNG data URLs
//   post=1 tag=<name>            POST the result to the serving script
//   importfail=1                 the product's renderer gets every video import
//                                refused (its ImageBitmap fallback, as on a
//                                machine that cannot share frames with WebGPU)
//   importfail=2                 …and its ImageBitmap copy too (the readback:
//                                VideoFrame.copyTo, then writeTexture)
//   importfail=3                 …and the readback too: no way in, and the
//                                chain cells fail saying so
//   nopresent=1                  the product's WebGPU chains draw into textures
//                                of the lab's, never into their canvas' swap
//                                chain (see HOOK.noPresent)
//
// Three feeds (§7.1, rule 1): `ext` reads the decoded VideoFrame through a
// texture_external (no pass 0); `tex` reads an rgba8unorm texture already
// materialised (the kernel alone); `blit+…` is pass 0 then the kernel, the
// chain the shipped WebGPU enhancers pay. `chain-*` drives the product's own
// WebGpuRenderer, unchanged, on an OffscreenCanvas, its passes timestamped
// from the outside (prototype hooks, nothing in the product).
//
// The reference shaders are read from the product's module text, so the lab
// always measures what ships (SGSR_WGSL, EASU_WGSL, RCAS_WGSL, NIS_WGSL,
// BLIT_WGSL). The "sgsr-src" cell is the same port with Qualcomm's published
// constants (threshold 8/255, sharpness 2.0, delta clamp ±23/255), the witness
// of §7.1 rule 6.
import {
  nisConfig,
  nisCoefTexture,
} from "/frontend/js/stream/renderers/NisTables.js";

const RENDERER_URL = "/frontend/js/stream/renderers/WebGpuRenderer.js";
const WEBGL_RENDERER_URL = "/frontend/js/stream/renderers/WebGlRenderer.js";
const MWSR_URL = "/frontend/js/stream/renderers/MwsrShader.js";

const CELL_SETS = {
  feed: [
    "blit",
    "ext-s1",
    "ext-s5",
    "ext-s9",
    "ext-l1",
    "ext-l5",
    "ext-s1-fw",
    "tex-s1",
    "tex-s2",
    "tex-s5",
    "tex-s9",
    "tex-s1-fw",
    "blit+tex-s1",
    "blit+tex-s5",
    "blit+tex-s9",
  ],
  base: [
    "sgsr-repo",
    "sgsr-src",
    "nis",
    "easu",
    "rcas",
    "fsr1",
    "blit+sgsr-repo",
    "blit+fsr1",
  ],
  chain: ["chain-off", "chain-sgsr", "chain-nis", "chain-fsr1", "chain-mwsr"],
  // The product's WebGL2 renderer, the same way (`chain-gl-<pass>`, `chain-gl-off`
  // its plain copy): the draw's main-thread time, the time to the GPU's
  // completion (gl.finish(), as the WebGPU draw() waits for its work), the
  // GPU's own time by EXT_disjoint_timer_query_webgl2 where the browser has
  // it, and the picture.
  glchain: ["chain-gl-off", "chain-gl-sgsr", "chain-gl-nis", "chain-gl-fsr1", "chain-gl-mwsr"],
  // The mwsr kernel (plan §3.2, §4.3): `mwsr[:key=val;key=val…]` sets its
  // overrides (show, walong, wacross, ralong, racross, sfull, wmin, dering,
  // c444, rdown, ll, flat, fc, gz, fz: MWSR_KEYS), the uniforms (tau,
  // sharp), half=1 (the f16
  // module; the page needs f16=1), mode=up|down (default: by the scale),
  // taps=1|4|9|16 (the reduction's grid) and srgb=1 (SDR only: the input
  // read and the output written through `-srgb` views, the sampler decoding
  // and the target encoding; the regime of the views: one sample down to x0.5,
  // LINEAR_LIGHT 0, unless taps or ll say otherwise), fine=1 (dpdxFine and
  // dpdyFine in the vote: the quad's other row, J18); `ext-mwsr` reads the
  // external texture (no pass 0), `blit+mwsr` pays the pass 0 first;
  // `tex-s1-srgb` is the bilinear floor through the same views.
  mwsr: ["mwsr", "mwsr:dering=0", "mwsr:half=1", "blit+mwsr"],
  // P3: the reduction grids and lights (the mode follows the scale), 4:4:4.
  p3: ["mwsr", "mwsr:srgb=1", "mwsr:taps=9", "mwsr:ll=2", "mwsr:c444=1", "smooth2d"],
  // Plan B (upscaler-mwsr-b.md §3): MWSR on the planes of a YUV frame.
  // `yuv:v=N[;c=420|444][;bits=8|10][;mwsr options]` reads planes made from
  // the input picture once (the kernel alone; v=0 is the RGB kernel on those
  // planes turned back into RGB, as a browser's pass 0 hands them over);
  // `split+yuv:…` makes them from the decoder's frame at every iteration, the
  // browser's pass 0 split in two (Y, UV), to set against `blit+mwsr`.
  yuv: [
    "blit+mwsr",
    "split+yuv:v=1",
    "split+yuv:v=2",
    "split+yuv:v=3",
    "split+yuv:v=4",
    "mwsr",
    "yuv:v=1",
    "yuv:v=2",
    "yuv:v=3",
    "yuv:v=4",
  ],
};

const VS = /* wgsl */ `
struct VSOut {
    @builtin(position) pos : vec4f,
    @location(0) uv : vec2f,
};
@vertex
fn vs(@builtin(vertex_index) vid : u32) -> VSOut {
    var out : VSOut;
    let uv = vec2f(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.uv = uv;
    out.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    return out;
}
`;

// N bilinear samples (a cross at one texel, then the diagonals) averaged, on
// an external texture or an ordinary one, by sample or by load; `fw` adds the
// screen derivative of the luma (the vote of §3.2) to one sample. The average
// keeps every sample alive in the compiled code.
function tapsShader(feed, n, mode, fw) {
  const ext = feed === "ext";
  const decl = ext
    ? "var tex : texture_external;"
    : "var tex : texture_2d<f32>;";
  const sample =
    mode === "l"
      ? ext
        ? `fn S(uv : vec2f, off : vec2f) -> vec3f {
    let d = vec2i(textureDimensions(tex));
    let c = clamp(vec2i(uv * vec2f(d)) + vec2i(off), vec2i(0), d - vec2i(1));
    return textureLoad(tex, c).rgb;
}`
        : `fn S(uv : vec2f, off : vec2f) -> vec3f {
    let d = vec2i(textureDimensions(tex));
    let c = clamp(vec2i(uv * vec2f(d)) + vec2i(off), vec2i(0), d - vec2i(1));
    return textureLoad(tex, c, 0).rgb;
}`
      : ext
        ? `fn S(uv : vec2f, off : vec2f) -> vec3f {
    return textureSampleBaseClampToEdge(tex, samp, uv + off * u.texel).rgb;
}`
        : `fn S(uv : vec2f, off : vec2f) -> vec3f {
    return textureSampleLevel(tex, samp, uv + off * u.texel, 0.0).rgb;
}`;
  const offs = [
    [0, 0],
    [0, -1],
    [-1, 0],
    [1, 0],
    [0, 1],
    [-1, -1],
    [1, -1],
    [-1, 1],
    [1, 1],
  ].slice(0, n);
  const sum = offs
    .map(([x, y]) => `S(in.uv, vec2f(${x}.0, ${y}.0))`)
    .join(" + ");
  const body = fw
    ? `let c = ${sum};
    let l = dot(c, vec3f(0.25, 0.5, 0.25));
    let w = fwidth(l);
    return vec4f(c / ${n}.0 + vec3f(w), 1.0);`
    : `let c = ${sum};
    return vec4f(c / ${n}.0, 1.0);`;
  return (
    VS +
    `
struct U { texel : vec2f, pad : vec2f };
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) ${decl}
@group(0) @binding(2) var<uniform> u : U;
${sample}
@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    ${body}
}
`
  );
}

// A WGSL template literal of the product's renderer module, by the name of
// its const; the ones built on FULLSCREEN_VS get it prepended as the module does.
function extractWgsl(src, name) {
  const at = src.indexOf("const " + name);
  if (at < 0) throw new Error(name + " not found in " + RENDERER_URL);
  const b0 = src.indexOf("`", at);
  const b1 = src.indexOf("`", b0 + 1);
  const head = src.slice(at, b0);
  const body = src.slice(b0 + 1, b1);
  if (body.includes("${"))
    throw new Error(
      name + " has an interpolation; the extractor cannot read it",
    );
  return head.includes("FULLSCREEN_VS +")
    ? extractWgsl(src, "FULLSCREEN_VS") + body
    : body;
}

function sgsrSource(repo) {
  const edits = [
    ["6.0 / 255.0", "8.0 / 255.0"],
    ["EDGE_SHARPNESS : f32 = 1.5", "EDGE_SHARPNESS : f32 = 2.0"],
    [
      "let deltaY = finalY - pixW;",
      "let deltaY = clamp(finalY - pixW, -23.0 / 255.0, 23.0 / 255.0);",
    ],
  ];
  let s = repo;
  for (const [a, b] of edits) {
    if (!s.includes(a))
      throw new Error('sgsr-src: "' + a + '" not found in the port');
    s = s.replace(a, b);
  }
  return s;
}

// ── the pictures ───────────────────────────────────────────────────────────

function mulberry(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function drawText(ctx, w, h) {
  ctx.fillStyle = "#fff";
  ctx.fillRect(0, 0, w, h);
  const px = Math.max(9, Math.round(h / 60));
  ctx.font = px + "px Georgia, serif";
  ctx.fillStyle = "#111";
  const words = (
    "the quick brown fox jumps over the lazy dog while distant thunder rolls across " +
    "an empty plain and the lantern gutters in a wind that smells of rain and iron "
  ).split(" ");
  const cols = 3;
  const gap = Math.round(w * 0.03);
  const colW = (w - gap * (cols + 1)) / cols;
  let wi = 0;
  for (let c = 0; c < cols; c++) {
    let y = gap + px;
    while (y < h - gap) {
      let line = "";
      while (true) {
        const next = line + words[wi % words.length] + " ";
        if (ctx.measureText(next).width > colW) break;
        line = next;
        wi++;
      }
      ctx.fillStyle = c === 1 && y % 7 < 2 ? "#1a4fd6" : "#111";
      ctx.fillText(line, gap + c * (colW + gap), y);
      y += Math.round(px * 1.5);
    }
  }
}

function drawUi(ctx, w, h) {
  const rnd = mulberry(7);
  const g = ctx.createLinearGradient(0, 0, 0, h);
  g.addColorStop(0, "#2b2f3a");
  g.addColorStop(1, "#14161c");
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, w, h);
  const px = Math.max(9, Math.round(h / 54));
  ctx.font = px + "px system-ui, sans-serif";
  // a side bar, cards, a progress bar, thin lines, icons
  ctx.fillStyle = "#1d212b";
  ctx.fillRect(0, 0, Math.round(w * 0.18), h);
  for (let i = 0; i < 14; i++) {
    const y = Math.round(h * 0.06 + i * px * 2.2);
    ctx.fillStyle = i === 3 ? "#3b82f6" : "#8a93a6";
    ctx.fillRect(Math.round(w * 0.02), y - px, px, px);
    ctx.fillStyle = i === 3 ? "#fff" : "#c7cdd8";
    ctx.fillText(
      "Item " + (i + 1) + " — settings",
      Math.round(w * 0.02) + px * 1.6,
      y,
    );
  }
  for (let r = 0; r < 3; r++)
    for (let c = 0; c < 4; c++) {
      const x = Math.round(w * 0.21 + c * w * 0.19);
      const y = Math.round(h * 0.08 + r * h * 0.29);
      const cw = Math.round(w * 0.17);
      const ch = Math.round(h * 0.24);
      ctx.fillStyle = "#272c38";
      ctx.fillRect(x, y, cw, ch);
      ctx.strokeStyle = "#3a4152";
      ctx.lineWidth = 1;
      ctx.strokeRect(x + 0.5, y + 0.5, cw - 1, ch - 1);
      ctx.fillStyle = `hsl(${Math.floor(rnd() * 360)}, 60%, 55%)`;
      ctx.beginPath();
      ctx.arc(x + px * 1.5, y + px * 1.5, px * 0.8, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = "#e6e9ef";
      ctx.fillText("Card " + (r * 4 + c + 1), x + px * 3, y + px * 1.9);
      ctx.fillStyle = "#9aa3b5";
      ctx.font = Math.round(px * 0.8) + "px system-ui, sans-serif";
      ctx.fillText("1 920 × 1 080 · 60 fps · 20 Mbit/s", x + px, y + px * 3.4);
      ctx.font = px + "px system-ui, sans-serif";
      ctx.fillStyle = "#1a1e27";
      ctx.fillRect(x + px, y + ch - px * 2, cw - px * 2, px * 0.6);
      ctx.fillStyle = "#22c55e";
      ctx.fillRect(x + px, y + ch - px * 2, (cw - px * 2) * rnd(), px * 0.6);
      for (let k = 0; k < 6; k++) {
        ctx.fillStyle = k % 2 ? "#3a4152" : "#5a6478";
        ctx.fillRect(x + px + k * px * 1.2, y + px * 4.5, px * 0.9, 1);
      }
    }
}

// A landscape with soft gradients, sharp silhouettes and a HUD: the mix of a
// game picture (flat regions, a few hard edges, text on top).
function drawGame(ctx, w, h) {
  const rnd = mulberry(11);
  const sky = ctx.createLinearGradient(0, 0, 0, h * 0.6);
  sky.addColorStop(0, "#1b2a6b");
  sky.addColorStop(0.6, "#f0906a");
  sky.addColorStop(1, "#ffd9a0");
  ctx.fillStyle = sky;
  ctx.fillRect(0, 0, w, h);
  const sun = ctx.createRadialGradient(
    w * 0.7,
    h * 0.5,
    0,
    w * 0.7,
    h * 0.5,
    h * 0.12,
  );
  sun.addColorStop(0, "#fff6d6");
  sun.addColorStop(1, "rgba(255,220,160,0)");
  ctx.fillStyle = sun;
  ctx.fillRect(0, 0, w, h);
  for (let layer = 0; layer < 4; layer++) {
    const base = h * (0.5 + layer * 0.11);
    const amp = h * (0.08 - layer * 0.012);
    ctx.fillStyle = `hsl(${120 + layer * 10}, ${30 + layer * 8}%, ${12 + layer * 9}%)`;
    ctx.beginPath();
    ctx.moveTo(0, h);
    for (let x = 0; x <= w; x += 4) {
      const t = x / w;
      const y =
        base -
        amp *
          (Math.sin(t * 7 + layer) * 0.5 +
            Math.sin(t * 19 + layer * 3) * 0.3 +
            Math.sin(t * 53) * 0.2);
      ctx.lineTo(x, y);
    }
    ctx.lineTo(w, h);
    ctx.fill();
    // trees: thin triangles, the hard edges
    ctx.fillStyle = `hsl(${110 + layer * 10}, 35%, ${8 + layer * 6}%)`;
    for (let k = 0; k < 40 - layer * 6; k++) {
      const x = rnd() * w;
      const t = x / w;
      const y =
        base -
        amp *
          (Math.sin(t * 7 + layer) * 0.5 +
            Math.sin(t * 19 + layer * 3) * 0.3 +
            Math.sin(t * 53) * 0.2);
      const s = h * (0.05 - layer * 0.008) * (0.6 + rnd() * 0.8);
      ctx.beginPath();
      ctx.moveTo(x, y - s);
      ctx.lineTo(x - s * 0.3, y + s * 0.1);
      ctx.lineTo(x + s * 0.3, y + s * 0.1);
      ctx.fill();
    }
  }
  // film grain, the noise a codec fights
  const img = ctx.getImageData(
    0,
    Math.round(h * 0.55),
    w,
    Math.round(h * 0.45),
  );
  const d = img.data;
  for (let i = 0; i < d.length; i += 4) {
    const n = (rnd() - 0.5) * 18;
    d[i] += n;
    d[i + 1] += n;
    d[i + 2] += n;
  }
  ctx.putImageData(img, 0, Math.round(h * 0.55));
  // HUD
  const px = Math.max(10, Math.round(h / 40));
  ctx.font = "bold " + px + "px system-ui, sans-serif";
  ctx.fillStyle = "rgba(0,0,0,0.45)";
  ctx.fillRect(px, px, px * 14, px * 2.2);
  ctx.fillStyle = "#fff";
  ctx.fillText("HP 87   AMMO 24/120", px * 1.5, px * 2.5);
  ctx.strokeStyle = "#fff";
  ctx.lineWidth = Math.max(1, px / 10);
  ctx.beginPath();
  ctx.moveTo(w / 2 - px, h / 2);
  ctx.lineTo(w / 2 + px, h / 2);
  ctx.moveTo(w / 2, h / 2 - px);
  ctx.lineTo(w / 2, h / 2 + px);
  ctx.stroke();
}

// The reduction test pattern (§7.1, C10): a zone plate on the left (every
// spatial frequency up to Nyquist, where aliasing shows as moiré), gratings of
// 1 to 6 px period on the right, and thin diagonal lines.
function drawZone(ctx, w, h) {
  const img = ctx.createImageData(w, h);
  const d = img.data;
  const half = Math.floor(w / 2);
  const cx = half / 2;
  const cy = h / 2;
  const kmax = Math.PI / Math.max(cx, cy); // Nyquist at the edge
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      let v;
      if (x < half) {
        const dx = x - cx;
        const dy = y - cy;
        v = 0.5 + 0.5 * Math.cos((kmax * (dx * dx + dy * dy)) / 2);
      } else {
        const band = Math.floor(((y / h) * 6) % 6); // period 1..6 px
        const period = band + 1;
        v = Math.floor((x - half) / period) % 2 ? 0.9 : 0.1;
        if (band === 5) v = Math.floor(y / period) % 2 ? 0.9 : 0.1;
      }
      const i = (y * w + x) * 4;
      const g = Math.round(v * 255);
      d[i] = d[i + 1] = d[i + 2] = g;
      d[i + 3] = 255;
    }
  ctx.putImageData(img, 0, 0);
  ctx.strokeStyle = "#000";
  ctx.lineWidth = 1;
  for (let k = 0; k < 8; k++) {
    ctx.beginPath();
    ctx.moveTo(half + 10 + k * 30, h - 10);
    ctx.lineTo(w - 10, h * 0.1 + k * 12);
    ctx.stroke();
  }
}

// Coloured syntax on a dark background: text whose edges live in the chroma
// as much as in the luma (§4.2, C9). `iso`: the same text in colours of one
// luma (Rec. 709 within 5/255 of the background's), so that every edge is a
// chroma edge and nothing else — the case the 4:4:4 vote exists for.
function drawCode(ctx, w, h, iso = false) {
  ctx.fillStyle = iso ? "#786e28" : "#1e1e1e";
  ctx.fillRect(0, 0, w, h);
  const px = Math.max(9, Math.round(h / 56));
  ctx.font = px + "px Consolas, monospace";
  const colors = iso
    ? {
        kw: "#287d5a",
        str: "#b45a46",
        cmt: "#5a5fe6",
        id: "#c846b4",
        num: "#3c7882",
        fn: "#96643c",
        pl: "#467378",
      }
    : {
        kw: "#569cd6",
        str: "#ce9178",
        cmt: "#6a9955",
        id: "#9cdcfe",
        num: "#b5cea8",
        fn: "#dcdcaa",
        pl: "#d4d4d4",
      };
  const lines = [
    [
      ["kw", "const "],
      ["id", "frame "],
      ["pl", "= "],
      ["kw", "await "],
      ["fn", "decode"],
      ["pl", "(chunk, "],
      ["num", "16667"],
      ["pl", ");"],
    ],
    [["cmt", "// the external texture expires with the frame"]],
    [
      ["kw", "for "],
      ["pl", "("],
      ["kw", "let "],
      ["id", "i "],
      ["pl", "= "],
      ["num", "0"],
      ["pl", "; i < "],
      ["id", "n"],
      ["pl", "; i++) {"],
    ],
    [
      ["pl", "    "],
      ["id", "pass"],
      ["pl", "."],
      ["fn", "setBindGroup"],
      ["pl", "("],
      ["num", "0"],
      ["pl", ", "],
      ["fn", "bind"],
      ["pl", "(ext));"],
    ],
    [
      ["pl", "    "],
      ["id", "log"],
      ["pl", "("],
      ["str", "'gpu p50 ' "],
      ["pl", "+ "],
      ["id", "t"],
      ["pl", "."],
      ["fn", "toFixed"],
      ["pl", "("],
      ["num", "3"],
      ["pl", "));"],
    ],
    [["pl", "}"]],
  ];
  const lh = Math.round(px * 1.45);
  let y = px * 2;
  let n = 0;
  // Two columns: the lines are short, and with one column the central third
  // of the picture (where the metrics also look) held no text at all (P0 §6).
  while (y < h - px) {
    const line = lines[n % lines.length];
    for (const x0 of [px, Math.round(w * 0.5)]) {
      let x = x0;
      ctx.fillStyle = iso ? "#64696e" : "#858585";
      ctx.fillText(String(n + 1).padStart(3), x, y);
      x += px * 3;
      for (const [kind, text] of line) {
        ctx.fillStyle = colors[kind];
        ctx.fillText(text, x, y);
        x += ctx.measureText(text).width;
      }
    }
    y += lh;
    n++;
  }
}

async function makePicture(kind, w, h, k) {
  const cv = new OffscreenCanvas(w, h);
  const ctx = cv.getContext("2d", {
    alpha: false,
    willReadFrequently: kind === "game",
  });
  if (kind.startsWith("img:")) {
    const url = kind.slice(4);
    const r = await fetch(url);
    if (!r.ok) throw new Error(url + ": " + r.status);
    const bmp = await createImageBitmap(await r.blob());
    const s = Math.max(w / bmp.width, h / bmp.height);
    const dw = bmp.width * s;
    const dh = bmp.height * s;
    ctx.drawImage(bmp, (w - dw) / 2, (h - dh) / 2, dw, dh);
    bmp.close();
  } else if (kind === "ui") drawUi(ctx, w, h);
  else if (kind === "game") drawGame(ctx, w, h);
  else if (kind === "zone") drawZone(ctx, w, h);
  else if (kind === "code" || kind === "codeiso") drawCode(ctx, w, h, kind === "codeiso");
  else drawText(ctx, w, h);
  // frame k: a small moving square, so that an encoded clip has P-frames
  // with motion and the artefacts of a real stream (still.html's anim).
  if (k > 0) {
    const s = Math.round(h / 24);
    ctx.fillStyle = "#c22";
    ctx.fillRect(w - s * 3 - k * 7, h - s * 3, s, s);
  }
  return cv;
}

// ── the video frames ───────────────────────────────────────────────────────

async function makeFrames(pictures, codec, log, encSw = false) {
  const w = pictures[0].width;
  const h = pictures[0].height;
  // H.264 High at the level the picture needs: 5.0 holds 1440p60 (22080
  // macroblocks), 4K60 needs 5.1 (36864); the encoder refuses a level too
  // small for the frame ("no encoder"), as the 4K reduction case did. VP9
  // profile 0 at 5.0 or 5.1 likewise, 8 bits.
  const mbs = Math.ceil(w / 16) * Math.ceil(h / 16);
  const big = mbs > 22080;
  const codecString =
    codec === "hevc"
      ? "hvc1.1.6.L120.B0"
      : codec === "vp9"
        ? big
          ? "vp09.00.51.08"
          : "vp09.00.50.08"
        : big
          ? "avc1.640033"
          : "avc1.640032";
  const chunks = [];
  const encConfig = {
    codec: codecString,
    width: w,
    height: h,
    bitrate: 20_000_000,
    framerate: 60,
    latencyMode: "realtime",
    hardwareAcceleration: encSw ? "prefer-software" : "prefer-hardware",
  };
  if (codec === "hevc") encConfig.hevc = { format: "annexb" };
  else if (codec !== "vp9") encConfig.avc = { format: "annexb" };
  let sup = await VideoEncoder.isConfigSupported(encConfig);
  if (!sup.supported && !encSw) {
    encConfig.hardwareAcceleration = "no-preference";
    sup = await VideoEncoder.isConfigSupported(encConfig);
  }
  if (!sup.supported) throw new Error("no encoder for " + codecString + (encSw ? " in software" : ""));
  const encHw = encConfig.hardwareAcceleration;
  let encErr = null;
  const encoder = new VideoEncoder({
    output: (chunk) => chunks.push(chunk),
    error: (e) => {
      encErr = e;
    },
  });
  encoder.configure(encConfig);
  for (let k = 0; k < pictures.length; k++) {
    const vf = new VideoFrame(pictures[k], {
      timestamp: k * 16667,
      duration: 16667,
    });
    encoder.encode(vf, { keyFrame: k === 0 });
    vf.close();
  }
  await encoder.flush();
  encoder.close();
  if (encErr) throw new Error("encoder: " + encErr.message);
  const decConfig = {
    codec: codecString,
    hardwareAcceleration: "prefer-hardware",
    optimizeForLatency: true,
  };
  let dsup = await VideoDecoder.isConfigSupported(decConfig);
  if (!dsup.supported) {
    decConfig.hardwareAcceleration = "no-preference";
    dsup = await VideoDecoder.isConfigSupported(decConfig);
    if (!dsup.supported) throw new Error("no decoder for " + codecString);
  }
  const frames = [];
  let decErr = null;
  const decoder = new VideoDecoder({
    output: (f) => {
      if (frames.length < 6) frames.push(f);
      else f.close();
    },
    error: (e) => {
      decErr = e;
    },
  });
  decoder.configure(decConfig);
  for (const c of chunks) decoder.decode(c);
  await decoder.flush();
  decoder.close();
  if (decErr) throw new Error("decoder: " + decErr.message);
  if (!frames.length) throw new Error("no decoded frame");
  const f0 = frames[0];
  const bytes = chunks.reduce((a, c) => a + c.byteLength, 0);
  const info = {
    codec: codecString,
    encoderAcceleration: encHw,
    decoderAcceleration: decConfig.hardwareAcceleration,
    format: f0.format,
    coded: f0.codedWidth + "x" + f0.codedHeight,
    display: f0.displayWidth + "x" + f0.displayHeight,
    colorSpace: f0.colorSpace
      ? {
          primaries: f0.colorSpace.primaries,
          transfer: f0.colorSpace.transfer,
          matrix: f0.colorSpace.matrix,
          fullRange: f0.colorSpace.fullRange,
        }
      : null,
    frames: frames.length,
    kbitPerFrame: Math.round((bytes * 8) / chunks.length / 1000),
  };
  log("frames: " + JSON.stringify(info));
  return { frames, info };
}


// ── the WebGL2 side ────────────────────────────────────────────────────────

// RGBA frames straight from the pictures (codec=none): the exact texels of the
// lab's input texture, so that a chain renders the same picture as the cells.
function makeRgbaFrames(pictures) {
  const frames = pictures.map(
    (pic, k) => new VideoFrame(pic, { timestamp: k * 16667, duration: 16667 }),
  );
  const info = {
    codec: "none",
    format: frames[0].format,
    coded: frames[0].codedWidth + "x" + frames[0].codedHeight,
    display: frames[0].displayWidth + "x" + frames[0].displayHeight,
    frames: frames.length,
  };
  return { frames, info };
}

// The WebGL2 renderer's GPU timer, when the browser exposes it (Chrome on a
// desktop or Android GPU; not WebKit, not every software renderer): one
// TIME_ELAPSED query around each draw, read after the batch.
function glTimer(r) {
  if (!r.gl) return null;
  if (r.__tq === undefined)
    r.__tq = r.gl.getExtension("EXT_disjoint_timer_query_webgl2") || null;
  return r.__tq;
}

// A task boundary without setTimeout's clamping: Chrome refreshes a WebGL
// sync's or query's cached status once per task.
function nextTask() {
  return new Promise((res) => {
    const ch = new MessageChannel();
    ch.port1.onmessage = () => res();
    ch.port2.postMessage(0);
  });
}

// The GPU's completion of what was drawn. Chrome's gl.finish() is a flush on
// purpose ("Intentionally a flush, not a finish"), so the lab sets a fence and
// polls its status task by task — what the product's renderer reads to know
// whether the GPU is behind.
async function glDone(gl) {
  const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!sync) return;
  gl.flush();
  const t = performance.now();
  for (;;) {
    await nextTask();
    if (gl.getSyncParameter(sync, gl.SYNC_STATUS) === gl.SIGNALED) break;
    if (performance.now() - t > 5000) break;
  }
  gl.deleteSync(sync);
}

async function readGlQueries(gl, tq, queries) {
  const qs = queries.filter(Boolean);
  const t = performance.now();
  // results arrive a task later at the earliest
  while (performance.now() - t < 3000) {
    await nextTask();
    if (qs.every((q) => gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE))) break;
  }
  const disjoint = gl.getParameter(tq.GPU_DISJOINT_EXT);
  const out = queries.map((q) =>
    q && !disjoint && gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE)
      ? Number(gl.getQueryParameter(q, gl.QUERY_RESULT)) / 1e6
      : null,
  );
  for (const q of qs) gl.deleteQuery(q);
  return out;
}

// gl=1: a Chrome without WebGPU (ANGLE on SwiftShader, the driver's
// --webgl-swiftshader; forcing it switches WebGPU off, so the two twins cannot
// share a Chrome on the software adapters). Only the chain-gl-* cells: the
// product's WebGL2 renderer on the same pictures, each draw timed on the wall,
// the picture dumped through a 2D canvas under the name the WebGPU side would
// give it, so that the conformity flow compares the two sides from two runs.
async function runGlLab(p, log, ctx) {
  const { inW, inH, outW, outH, content, codec, cellNames, dumpList } = ctx;
  const { batch, batches, warm, rounds, pace, period } = ctx;
  const errors = [];
  const bad = cellNames.filter((c) => !c.startsWith("chain-gl-"));
  if (bad.length) throw new Error("gl=1 runs chain-gl-* cells only, not " + bad.join(","));
  const pictures = [];
  for (let k = 0; k < 12; k++) pictures.push(await makePicture(content, inW, inH, k));
  const made =
    codec !== "none" ? await makeFrames(pictures, codec, log, p.enc === "sw") : makeRgbaFrames(pictures);
  const frames = made.frames;
  log("frames: " + JSON.stringify(made.info));
  const { WebGlRenderer } = await import(WEBGL_RENDERER_URL);
  let gpu = "?";
  const renderers = new Map();
  async function renderer(algo) {
    if (renderers.has(algo)) return renderers.get(algo);
    const canvas = new OffscreenCanvas(outW, outH);
    const r = await WebGlRenderer.create(canvas, {
      algo: algo === "gl-off" ? "off" : algo,
      videoCodec: "h264",
    });
    r.setOutputSize(outW, outH);
    renderers.set(algo, r);
    const dbg = r.gl.getExtension("WEBGL_debug_renderer_info");
    gpu = String(dbg ? r.gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : r.gl.getParameter(r.gl.RENDERER));
    if (p.soft === "1" && !/swiftshader/i.test(gpu)) {
      r.dispose();
      throw new Error("soft=1 but WebGL2 runs on " + gpu);
    }
    log("chain " + algo + ": " + r.kind + " renderer on " + gpu);
    return r;
  }
  let cursor = 0;
  const nextFrame = () => frames[cursor++ % frames.length];
  async function draws(r, n, timed) {
    const walls = [];
    const dones = [];
    const queries = [];
    const tq = timed ? glTimer(r) : null;
    let copies = 0;
    const start = performance.now();
    for (let i = 0; i < n; i++) {
      if (pace === "tick") {
        const due = start + i * period;
        const now = performance.now();
        if (due > now) await new Promise((res) => setTimeout(res, due - now));
      }
      const f = nextFrame().clone();
      let q = null;
      if (tq) {
        q = r.gl.createQuery();
        r.gl.beginQuery(tq.TIME_ELAPSED_EXT, q);
      }
      const t0 = performance.now();
      await r.draw(f);
      const t1 = performance.now();
      if (tq) r.gl.endQuery(tq.TIME_ELAPSED_EXT);
      // draw() only queues: the GPU's completion, as the WebGPU draw() waits
      await glDone(r.gl);
      walls.push(t1 - t0);
      dones.push(performance.now() - t0);
      queries.push(q);
      if (r.passthrough === true) copies++;
    }
    const gpu = tq ? (await readGlQueries(r.gl, tq, queries)).filter((v) => v !== null) : [];
    return { walls, dones, gpu, copies };
  }
  const t0 = performance.now();
  const out = [];
  for (const name of cellNames) {
    const algo = name.slice(6);
    try {
      const r = await renderer(algo);
      if (warm > 0) await draws(r, 4 * warm, false);
      const walls = [];
      const dones = [];
      const gpu = [];
      let copies = 0;
      for (let b = 0; b < batches * rounds; b++) {
        const d = await draws(r, batch, true);
        walls.push(...d.walls);
        dones.push(...d.dones);
        gpu.push(...d.gpu);
        copies += d.copies;
      }
      const w = stats(walls);
      const dn = stats(dones);
      const g = stats(gpu);
      log(
        `${name.padEnd(16)} draw p50 ${w.p50.toFixed(3)} p99 ${w.p99.toFixed(3)} | done p50 ${dn.p50.toFixed(3)} p99 ${dn.p99.toFixed(3)} | gpu ${g ? "p50 " + g.p50.toFixed(3) : "no timer"} ms`,
      );
      out.push({
        name,
        feed: "chain",
        algo,
        passes: g ? 1 : 0,
        gpu: g,
        perPass: g ? [g.p50] : [],
        disp: null,
        wall: w,
        wallPerIter: w.mean,
        iters: walls.length,
        draws: walls,
        submit: w,
        done: dn,
        copies,
        passSeq: {},
      });
    } catch (e) {
      errors.push("[" + name + "] " + (e.message || e));
      log(name + " failed: " + (e.message || e));
    }
  }
  const dumped = [];
  for (const name of cellNames) {
    if (!dumpList.includes(name)) continue;
    try {
      const r = await renderer(name.slice(6));
      cursor = 0; // the clip's first frame: the kernel cells' picture
      await draws(r, 1, false);
      const bmp = r.canvas.transferToImageBitmap();
      const cv = new OffscreenCanvas(bmp.width, bmp.height);
      cv.getContext("2d").drawImage(bmp, 0, 0);
      bmp.close();
      const blob = await cv.convertToBlob({ type: "image/png" });
      const dname = (p.tag || "lab") + "-" + name.replace(/[^\w.+-]/g, "_");
      await postBlob("/__png?name=" + encodeURIComponent(dname), blob);
      dumped.push(dname);
    } catch (e) {
      errors.push("[dump " + name + "] " + (e.message || e));
    }
  }
  if (dumped.length) log("dumped: " + dumped.join(" "));
  for (const f of frames) f.close();
  for (const r of renderers.values()) r.dispose();
  return {
    adapter: { name: "webgl2: " + gpu, vendor: null, architecture: null, device: null, description: gpu },
    features: [],
    timestamps: false,
    format: "canvas",
    userAgent: navigator.userAgent,
    isolated: !!globalThis.crossOriginIsolated,
    params: {
      in: `${inW}x${inH}`,
      out: `${outW}x${outH}`,
      content,
      codec,
      pace,
      period,
      batch,
      batches,
      warm,
      rounds,
      sets: [],
      power: null,
      fallback: false,
      flip: false,
      gl: true,
    },
    frames: made.info,
    elapsedMs: performance.now() - t0,
    cells: out,
    shots: null,
    dumped,
    errors,
  };
}

// ── statistics ─────────────────────────────────────────────────────────────

function quant(xs, f) {
  const s = xs
    .filter((x) => x !== null && !Number.isNaN(x))
    .sort((a, b) => a - b);
  if (!s.length) return null;
  return s[Math.min(s.length - 1, Math.floor(f * s.length))];
}
function stats(xs) {
  if (!xs.length) return null;
  const mean = xs.reduce((a, b) => a + b, 0) / xs.length;
  return {
    n: xs.length,
    p50: quant(xs, 0.5),
    p90: quant(xs, 0.9),
    p99: quant(xs, 0.99),
    mean,
  };
}

// ── the lab ────────────────────────────────────────────────────────────────

export async function runLab(p, log = () => {}) {
  // The renderers speak through the console; the page log keeps what they
  // say (every warning and error, and their own lines), so that a run on a
  // machine nobody watches — the CI — tells which pipeline was refused.
  const taps = [];
  for (const level of ["log", "warn", "error"]) {
    const orig = console[level];
    taps.push([level, orig]);
    console[level] = (...a) => {
      const s = a.map((x) => (x && x.message) || String(x)).join(" ");
      if (level !== "log" || /^\[Web(Gpu|Gl)Renderer\]/.test(s))
        log("console." + level + ": " + s.slice(0, 400));
      orig.apply(console, a);
    };
  }
  try {
    return await runLabInner(p, log);
  } finally {
    for (const [level, orig] of taps) console[level] = orig;
  }
}

async function runLabInner(p, log) {
  const [inW, inH] = (p.in || "1280x720").split("x").map(Number);
  const [outW, outH] = (p.out || "2560x1440").split("x").map(Number);
  const content = p.content || "text";
  const codec = p.codec || "avc";
  const pace = p.pace || "back";
  const period = Number(p.period || 1000 / 60);
  const flip = p.flip === "1";
  const batch = Number(p.batch || 16);
  const batches = Number(p.batches || 8);
  const warm = Number(p.warm || 2);
  const rounds = Number(p.rounds || 3);
  const wantShots = p.shot === "1";
  const sets = (p.set || "feed,base").split(",").filter(Boolean);
  const cellNames = p.cells
    ? p.cells.split(",")
    : sets.flatMap((s) => CELL_SETS[s] || []);
  const errors = [];

  // gt=1: the ground truth, the same picture at the output size, posted as PNG
  // to the serving script, which reduces it (Lanczos-3, linear light) into the
  // input of the quality runs (§7.1, rule 2). Nothing else is done.
  if (p.gt === "1") {
    const cv = await makePicture(content, outW, outH, 0);
    const blob = await cv.convertToBlob({ type: "image/png" });
    const name = (p.tag || "lab") + "-gt";
    await postBlob("/__png?name=" + encodeURIComponent(name), blob);
    log("ground truth posted: " + name);
    return { gt: name, params: { out: `${outW}x${outH}`, content } };
  }
  // dump=base|all|a,b,c: after the measurement, each of these cells rendered
  // once more and its whole output posted as PNG (for metrics.py).
  const dumpList =
    p.dump === "all"
      ? cellNames.slice() // chain cells too (SDR), see the dump loop
      : p.dump === "base"
        ? ["tex-s1", "sgsr-repo", "sgsr-src", "nis", "easu", "fsr1", "mwsr"].filter(
            (c) => cellNames.includes(c),
          )
        : (p.dump || "").split(",").filter(Boolean);

  // gl=1: the WebGL2 side alone, in a Chrome without WebGPU (see runGlLab).
  if (p.gl === "1")
    return runGlLab(p, log, {
      inW, inH, outW, outH, content, codec, cellNames, dumpList,
      batch, batches, warm, rounds, pace, period,
    });

  if (!navigator.gpu) throw new Error("no WebGPU (navigator.gpu)");
  // The product's renderer asks for the default adapter and a bare device:
  // the hooks give it the same adapter as the lab and the timestamp feature,
  // and stamp its render passes from the outside (chain-* cells).
  const adapterOpts = {};
  if (p.power) adapterOpts.powerPreference = p.power;
  if (p.fallback === "1") adapterOpts.forceFallbackAdapter = true;
  installHooks(adapterOpts);
  HOOK.importFail = +p.importfail || 0;
  HOOK.noPresent = p.nopresent === "1";
  const adapter = await navigator.gpu.requestAdapter(adapterOpts);
  if (!adapter) throw new Error("requestAdapter returned null");
  const info = adapter.info || {};
  const features = [...adapter.features].sort();
  const hasTs = adapter.features.has("timestamp-query");
  const hasF16 = adapter.features.has("shader-f16");
  const required = [];
  if (hasTs) required.push("timestamp-query");
  if (hasF16 && p.f16 === "1") required.push("shader-f16");
  const device = await adapter.requestDevice({ requiredFeatures: required });
  device.onuncapturederror = (e) => errors.push(e.error.message);
  HOOK.labDevice = device;
  HOOK.productErrors = [];
  const adapterName =
    `${info.vendor || "?"} ${info.architecture || ""} ${info.device || ""} ${info.description || ""}`
      .replace(/\s+/g, " ")
      .trim();
  // soft=1 (the driver's --swiftshader): this Chrome may touch no GPU of the
  // bench machine — the adapter must be SwiftShader's, and the WebGL2 chain
  // cells, which would open a context on the machine's default GPU, belong to
  // the --webgl-swiftshader Chrome (gl=1).
  if (p.soft === "1") {
    if (!/swiftshader/i.test(adapterName))
      throw new Error("soft=1 but the WebGPU adapter is " + adapterName);
    const gl = cellNames.filter((c) => c.startsWith("chain-gl-"));
    if (gl.length)
      throw new Error("soft=1: " + gl.join(",") + " run in the --webgl-swiftshader Chrome (gl=1)");
  }
  log(
    `adapter: ${adapterName} | timestamps ${hasTs} f16 ${hasF16} | features ${features.join(" ")}`,
  );
  const format = navigator.gpu.getPreferredCanvasFormat();
  // hdr=1: the renderer's `linear` HDR feed — rgba16float textures and targets,
  // the picture fetched as RGBA half floats (f16:<url>), the dumps written back
  // as half floats.
  const hdr = p.hdr === "1";
  const texFormat = hdr ? "rgba16float" : "rgba8unorm";
  const outFormat = hdr ? "rgba16float" : format;

  // Pictures: F frames of the content at the input size (the clip), and the
  // first one as the materialised texture; or the half floats of an HDR picture.
  const F = 12;
  const pictures = [];
  let halfData = null;
  if (content.startsWith("f16:")) {
    const r = await fetch(content.slice(4));
    if (!r.ok) throw new Error(content + ": " + r.status);
    halfData = new Uint16Array(await r.arrayBuffer());
    if (halfData.length !== inW * inH * 4)
      throw new Error(`${content}: ${halfData.length} halves, not ${inW}x${inH}x4`);
    if (!hdr) throw new Error("an f16 picture needs hdr=1");
  } else {
    for (let k = 0; k < F; k++)
      pictures.push(await makePicture(content, inW, inH, k));
  }
  let frames = [];
  let frameInfo = null;
  const needExt = cellNames.some(
    (c) => c.startsWith("ext") || c.startsWith("blit") || c.startsWith("chain") || c.startsWith("split"),
  );
  if (codec !== "none" && needExt && pictures.length) {
    try {
      const r = await makeFrames(pictures, codec, log, p.enc === "sw");
      frames = r.frames;
      frameInfo = r.info;
    } catch (e) {
      errors.push("frames: " + e.message);
      log("frames failed: " + e.message);
    }
  } else if (needExt && pictures.length) {
    // codec=none: RGBA frames straight from the pictures, the exact texels
    // of the lab's input texture — the product's chain renders the same
    // picture as the kernel cells, so the two can be fingerprinted together
    // (the golden set).
    const made = makeRgbaFrames(pictures);
    frames = made.frames;
    frameInfo = made.info;
    log("frames: " + JSON.stringify(frameInfo));
  }

  const mkTex = (w, h, fmt, usage, viewFormats = []) =>
    device.createTexture({
      size: { width: w, height: h },
      format: fmt,
      usage,
      viewFormats,
    });
  // SDR: the input and the output also as `-srgb` views (srgb cells): the
  // sampler then decodes sRGB before it filters, the target encodes on write.
  const srgbViews = !hdr;
  const T = GPUTextureUsage;
  const inTex = mkTex(
    inW,
    inH,
    texFormat,
    T.TEXTURE_BINDING | T.COPY_DST | T.RENDER_ATTACHMENT,
    srgbViews ? [texFormat + "-srgb"] : [],
  );
  if (halfData)
    device.queue.writeTexture(
      { texture: inTex },
      halfData,
      { bytesPerRow: inW * 8, rowsPerImage: inH },
      { width: inW, height: inH },
    );
  else
    device.queue.copyExternalImageToTexture(
      { source: pictures[0] },
      { texture: inTex },
      { width: inW, height: inH },
    );
  const blitTex = mkTex(
    inW,
    inH,
    texFormat,
    T.TEXTURE_BINDING | T.RENDER_ATTACHMENT,
  );
  const midTex = mkTex(
    outW,
    outH,
    texFormat,
    T.TEXTURE_BINDING | T.RENDER_ATTACHMENT,
  );
  const intermTex = mkTex(
    outW,
    outH,
    texFormat,
    T.TEXTURE_BINDING | T.RENDER_ATTACHMENT,
  );
  const outTex = mkTex(
    outW,
    outH,
    outFormat,
    T.RENDER_ATTACHMENT | T.COPY_SRC | T.COPY_DST,
    srgbViews ? [outFormat + "-srgb"] : [],
  );
  const views = {
    blit: blitTex.createView(),
    interm: intermTex.createView(),
    mid: midTex.createView(),
    out: outTex.createView(),
    "out-srgb": srgbViews
      ? outTex.createView({ format: outFormat + "-srgb" })
      : null,
  };
  const sampler = device.createSampler({
    magFilter: "linear",
    minFilter: "linear",
    addressModeU: "clamp-to-edge",
    addressModeV: "clamp-to-edge",
  });
  const uniform = (arr) => {
    const b = device.createBuffer({
      size: Math.max(16, arr.byteLength),
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(b, 0, arr);
    return b;
  };
  const texelIn = uniform(new Float32Array([1 / inW, 1 / inH, 0, 0]));
  const texelOut = uniform(new Float32Array([1 / outW, 1 / outH, 0, 0]));

  const src = await (await fetch(RENDERER_URL)).text();
  const SH = {
    blit: extractWgsl(src, "BLIT_WGSL"),
    sgsr: extractWgsl(src, "SGSR_WGSL"),
    nis: extractWgsl(src, "NIS_WGSL"),
    easu: extractWgsl(src, "EASU_WGSL"),
    rcas: extractWgsl(src, "RCAS_WGSL"),
  };
  SH.sgsrSrc = sgsrSource(SH.sgsr);
  // FSR1's 16-bit path (FSR_EASU_H, FSR_RCAS_H), the lab's own port: cells
  // easuh and fsr1h, for a comparison at each kernel's best precision.
  const { EASU_H_WGSL, RCAS_H_WGSL, asF32 } = await import("./fsr1-half.js");
  SH.easuH = EASU_H_WGSL;
  SH.rcasH = RCAS_H_WGSL;
  // easuh32, fsr1h32: the same port lowered to f32 (its logic, on SwiftShader)
  SH.easuH32 = asF32(EASU_H_WGSL);
  SH.rcasH32 = asF32(RCAS_H_WGSL);
  // SGSR1's half precision (sgsr1.h computes in half): cells sgsrh, sgsrh32
  const { sgsrHalf } = await import("./sgsr1-half.js");
  SH.sgsrH = sgsrHalf(SH.sgsr);
  SH.sgsrH32 = asF32(SH.sgsrH);
  const needF16 = (name) => {
    if (!device.features.has("shader-f16"))
      throw new Error(name + ": the device has no shader-f16 (f16=1 asks for it)");
  };
  const { mwsrWgsl, mwsrUniforms, mwsrRegime } = await import(MWSR_URL);

  const pipelines = new Map();
  // The formats of the targets that are neither the output nor texFormat
  // (plan B's planes), by their name in `views`.
  const targetFormats = {};
  const pipeline = (
    key,
    code,
    target,
    entry = ["vs", "fs"],
    layout = "auto",
    constants = undefined,
  ) => {
    if (pipelines.has(key)) return pipelines.get(key);
    const module = device.createShaderModule({ code });
    const pl = device.createRenderPipeline({
      layout,
      vertex: { module, entryPoint: entry[0] },
      fragment: {
        module,
        entryPoint: entry[1],
        targets: [
          {
            format:
              targetFormats[target] ??
              (target === "out"
                ? outFormat
                : target === "out-srgb"
                  ? outFormat + "-srgb"
                  : texFormat),
          },
        ],
        constants,
      },
      primitive: { topology: "triangle-list" },
    });
    pipelines.set(key, pl);
    return pl;
  };
  const bind = (pl, entries) =>
    device.createBindGroup({
      layout: pl.getBindGroupLayout(0),
      entries: entries.map((resource, binding) => ({ binding, resource })),
    });
  // Explicit layouts for the lab's own shaders: an `auto` layout drops a
  // binding the code does not use (the sampler of a load, the texel size of
  // a load), and the bind group would then be refused.
  const FR = GPUShaderStage.FRAGMENT;
  const L = (entries) =>
    device.createPipelineLayout({
      bindGroupLayouts: [device.createBindGroupLayout({ entries })],
    });
  const plExt = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, externalTexture: {} },
    { binding: 2, visibility: FR, buffer: {} },
  ]);
  const plTex = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, texture: { sampleType: "float" } },
    { binding: 2, visibility: FR, buffer: {} },
  ]);
  const plBlit = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, externalTexture: {} },
  ]);

  // Reference kernels, on a texture already materialised (`tex`: inTex; after
  // a blit: blitTex), each a pass to `out`.
  const sgsrU = uniform(new Float32Array([1 / inW, 1 / inH, inW, inH]));
  const easuU = uniform(new Float32Array([inW, inH, outW, outH]));
  const rcasU = uniform(new Float32Array([outW, outH, 1 / outW, 1 / outH]));
  const cfg = nisConfig(0.5);
  const nisU = uniform(
    new Float32Array([
      inW / outW,
      inH / outH,
      1 / inW,
      1 / inH,
      ...cfg.detect,
      ...cfg.sharpA,
      ...cfg.sharpB,
      outW,
      outH,
      0,
      0,
    ]),
  );
  const nisCoef = mkTex(4, 64, "rgba32float", T.TEXTURE_BINDING | T.COPY_DST);
  device.queue.writeTexture(
    { texture: nisCoef },
    nisCoefTexture(),
    { bytesPerRow: 64, rowsPerImage: 64 },
    { width: 4, height: 64 },
  );
  const nisLayout = device.createBindGroupLayout({
    entries: [
      { binding: 0, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
      {
        binding: 1,
        visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: "float" },
      },
      {
        binding: 2,
        visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: "unfilterable-float" },
      },
      { binding: 3, visibility: GPUShaderStage.FRAGMENT, buffer: {} },
    ],
  });
  const nisPl = device.createPipelineLayout({ bindGroupLayouts: [nisLayout] });

  const inView = inTex.createView();
  const inViewSrgb = srgbViews
    ? inTex.createView({ format: texFormat + "-srgb" })
    : null;
  const kernelPass = (name, srcView) => {
    switch (name) {
      case "sgsr-repo": {
        const pl = pipeline("sgsr-repo", SH.sgsr, "out");
        return {
          pipeline: pl,
          target: "out",
          group: bind(pl, [sampler, srcView, { buffer: sgsrU }]),
        };
      }
      case "sgsrh":
      case "sgsrh32": {
        if (name === "sgsrh") needF16(name);
        const pl = pipeline(name, name === "sgsrh" ? SH.sgsrH : SH.sgsrH32, "out");
        return {
          pipeline: pl,
          target: "out",
          group: bind(pl, [sampler, srcView, { buffer: sgsrU }]),
        };
      }
      case "sgsr-src": {
        const pl = pipeline("sgsr-src", SH.sgsrSrc, "out");
        return {
          pipeline: pl,
          target: "out",
          group: bind(pl, [sampler, srcView, { buffer: sgsrU }]),
        };
      }
      case "nis": {
        const pl = pipeline("nis", SH.nis, "out", ["vs", "fs"], nisPl);
        return {
          pipeline: pl,
          target: "out",
          group: device.createBindGroup({
            layout: nisLayout,
            entries: [
              { binding: 0, resource: sampler },
              { binding: 1, resource: srcView },
              { binding: 2, resource: nisCoef.createView() },
              { binding: 3, resource: { buffer: nisU } },
            ],
          }),
        };
      }
      case "easu": {
        const pl = pipeline("easu-out", SH.easu, "out", ["vs_main", "fs_main"]);
        return {
          pipeline: pl,
          target: "out",
          group: bind(pl, [srcView, sampler, { buffer: easuU }]),
        };
      }
      case "easuh":
      case "easuh32": {
        if (name === "easuh") needF16(name);
        const code = name === "easuh" ? SH.easuH : SH.easuH32;
        const pl = pipeline(name + "-out", code, "out", ["vs_main", "fs_main"]);
        return {
          pipeline: pl,
          target: "out",
          group: bind(pl, [srcView, sampler, { buffer: easuU }]),
        };
      }
      default:
        throw new Error("kernel " + name + "?");
    }
  };
  // the FSR1 variant: "" the product's fp32 port, "h" the 16-bit one, "h32"
  // the 16-bit one lowered to f32
  const FSR1_SH = {
    "": [SH.easu, SH.rcas],
    h: [SH.easuH, SH.rcasH],
    h32: [SH.easuH32, SH.rcasH32],
  };
  const easuToInterm = (srcView, v = "") => {
    if (v === "h") needF16("fsr1h");
    const pl = pipeline("easu" + v + "-interm", FSR1_SH[v][0], "interm", [
      "vs_main",
      "fs_main",
    ]);
    return {
      pipeline: pl,
      target: "interm",
      group: bind(pl, [srcView, sampler, { buffer: easuU }]),
    };
  };
  const rcasFrom = (srcView, v = "") => {
    const pl = pipeline("rcas" + v, FSR1_SH[v][1], "out", ["vs_main", "fs_main"]);
    return {
      pipeline: pl,
      target: "out",
      group: bind(pl, [srcView, sampler, { buffer: rcasU }]),
    };
  };
  const blitPass = () => {
    const pl = pipeline("blit", SH.blit, "blit", ["vs", "fs"], plBlit);
    return {
      pipeline: pl,
      target: "blit",
      ext: (ext) => bind(pl, [sampler, ext]),
    };
  };

  // Plan B: the planes of a YUV frame (mwsr-yuv.js). One set per chroma and
  // depth, and per source: `p` made once from the input picture, `s` made
  // from the decoder's frame by the split cells at every iteration.
  const YUV = await import("./mwsr-yuv.js");
  const plYuv = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, texture: { sampleType: "float" } },
    { binding: 2, visibility: FR, buffer: {} },
    { binding: 3, visibility: FR, texture: { sampleType: "float" } },
  ]);
  const plPlanesTex = L([
    { binding: 0, visibility: FR, texture: { sampleType: "float" } },
  ]);
  const plRgbPlanes = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, texture: { sampleType: "float" } },
    { binding: 2, visibility: FR, texture: { sampleType: "float" } },
  ]);
  const planeSets = new Map();
  const planeSet = (c, bits, src) => {
    const key = `${src}${c}-${bits}`;
    if (planeSets.has(key)) return planeSets.get(key);
    const d = c === "444" ? 1 : 2;
    const [fy, fuv] = bits === 10 ? ["r16float", "rg16float"] : ["r8unorm", "rg8unorm"];
    const use = T.TEXTURE_BINDING | T.RENDER_ATTACHMENT;
    const set = {
      y: "y-" + key,
      uv: "uv-" + key,
      yOut: "yout-" + key,
      rgb: "rgb-" + key,
      d,
      bits,
    };
    const add = (name, w, h, fmt) => {
      views[name] = mkTex(w, h, fmt, use).createView();
      targetFormats[name] = fmt;
    };
    add(set.y, inW, inH, fy);
    add(set.uv, inW / d, inH / d, fuv);
    add(set.yOut, outW, outH, fy);
    add(set.rgb, inW, inH, texFormat);
    const consts = bits === 10 ? { BITS: 10 } : {};
    if (src === "p") {
      // made once, from the input picture, outside any measure
      const fromRgb = (which, target) => {
        const pl = pipeline(
          `planes-${which}-${key}`,
          YUV.planesFromRgbWgsl(which),
          target,
          ["vs", "fs"],
          plPlanesTex,
          which === "uv" ? { ...consts, CHROMA_DIV: d } : consts,
        );
        return { pipeline: pl, target, group: bind(pl, [inView]) };
      };
      const toRgb = rgbPass(set, set.rgb, false);
      const enc = device.createCommandEncoder();
      for (const ps of [fromRgb("y", set.y), fromRgb("uv", set.uv), toRgb]) {
        const pass = enc.beginRenderPass({
          colorAttachments: [{ view: views[ps.target], loadOp: "clear", storeOp: "store" }],
        });
        pass.setPipeline(ps.pipeline);
        pass.setBindGroup(0, ps.group);
        pass.draw(3);
        pass.end();
      }
      device.queue.submit([enc.finish()]);
    }
    planeSets.set(key, set);
    return set;
  };
  // The planes back to RGB at the target's size: the RGB a browser's pass 0
  // gives from them (yFull false), or V1's composition of the luma MWSR drew
  // at the output size with the UV plane.
  const rgbPass = (set, target, yFull) => {
    const consts = { ...(set.bits === 10 ? { BITS: 10 } : {}), Y_FULL: yFull ? 1 : 0 };
    const pl = pipeline(
      `rgb-planes-${target}-${yFull ? 1 : 0}`,
      YUV.rgbFromPlanesWgsl(),
      target,
      ["vs", "fs"],
      plRgbPlanes,
      consts,
    );
    return {
      pipeline: pl,
      target,
      group: bind(pl, [sampler, views[yFull ? set.yOut : set.y], views[set.uv]]),
    };
  };
  // The browser's pass 0 split in two: the planes from the decoder's frame.
  const splitPasses = (set) =>
    ["y", "uv"].map((which) => {
      const target = which === "y" ? set.y : set.uv;
      const pl = pipeline(
        `split-${which}-${target}`,
        YUV.planesFromExtWgsl(which),
        target,
        ["vs", "fs"],
        plBlit,
        set.bits === 10 ? { BITS: 10 } : {},
      );
      return { pipeline: pl, target, ext: (ext) => bind(pl, [sampler, ext]) };
    });
  const tapsPass = (feed, n, mode, fw, srcView, srgb = false) => {
    const key = `${feed}-${mode}${n}${fw ? "-fw" : ""}${srgb ? "-srgb" : ""}`;
    const target = srgb ? "out-srgb" : "out";
    const pl = pipeline(
      key,
      tapsShader(feed, n, mode, fw),
      target,
      ["vs", "fs"],
      feed === "ext" ? plExt : plTex,
    );
    if (feed === "ext")
      return {
        pipeline: pl,
        target,
        ext: (ext) => bind(pl, [sampler, ext, { buffer: texelIn }]),
      };
    return {
      pipeline: pl,
      target,
      group: bind(pl, [sampler, srcView, { buffer: texelIn }]),
    };
  };

  // The mwsr kernel, one pass to `out`; the cell's options become overrides
  // (pipeline constants) and uniforms. The override defaults are the text's.
  const MWSR_KEYS = {
    show: "SHOW",
    walong: "W_ALONG",
    wacross: "W_ACROSS",
    ralong: "R_ALONG",
    racross: "R_ACROSS",
    sfull: "S_FULL",
    wmin: "W_MIN",
    dering: "DERING",
    c444: "CHROMA444",
    rdown: "R_DOWN",
    ll: "LINEAR_LIGHT",
    flat: "FLAT_TAPS",
    fc: "FLAT_C",
    gz: "GAZE",
    fz: "FLAT_ZONE",
  };
  // The gaze zone's uniforms (MwsrShader's MWSR_ZONE): a cell naming one of
  // them, or gaze=, takes the GAZE text unless gz= says otherwise.
  const ZONE_KEYS = {
    zcx: "cx",
    zcy: "cy",
    zrx: "rx",
    zry: "ry",
    zring: "ring",
    ztau: "tau",
    zsharp: "sharp",
  };
  const regimes = {
    soft: mwsrRegime(inW, inH, outW, outH),
    srgb: mwsrRegime(inW, inH, outW, outH, { srgb: true }),
  };
  const regime = regimes.soft;

  // The adaptive sharpness's cost (mwsr-map.js, lab only): `mwsr:map=WxH`
  // reads a grid of factors (a fixed pattern), `mwsr:map=WxH;ana=1` writes it
  // first from the picture — statistics per cell of a `cells=CWxCH` grid at
  // the stride `q` (`nb=1`: the neighbours 1 texel away, not q; `rot=1`: a
  // quarter of those texels a frame, in turn), then per tile, then the tree
  // — four passes in all.
  const MAP = await import("./mwsr-map.js");
  const plTexMap = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, texture: { sampleType: "float" } },
    { binding: 2, visibility: FR, buffer: {} },
    { binding: 3, visibility: FR, texture: { sampleType: "float" } },
  ]);
  const plExtMap = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, externalTexture: {} },
    { binding: 2, visibility: FR, buffer: {} },
    { binding: 3, visibility: FR, texture: { sampleType: "float" } },
  ]);
  // via=vtx: the map read by the vertices (mapKernelVtx)
  const VF = GPUShaderStage.VERTEX | FR;
  const plTexMapV = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, texture: { sampleType: "float" } },
    { binding: 2, visibility: FR, buffer: {} },
    { binding: 3, visibility: VF, texture: { sampleType: "float" } },
  ]);
  const plExtMapV = L([
    { binding: 0, visibility: FR, sampler: {} },
    { binding: 1, visibility: FR, externalTexture: {} },
    { binding: 2, visibility: FR, buffer: {} },
    { binding: 3, visibility: VF, texture: { sampleType: "float" } },
  ]);
  const plStatsTex = L([{ binding: 0, visibility: FR, texture: { sampleType: "float" } }]);
  const plStatsExt = L([{ binding: 0, visibility: FR, externalTexture: {} }]);
  const plTwo = L([
    { binding: 0, visibility: FR, texture: { sampleType: "unfilterable-float" } },
    { binding: 1, visibility: FR, texture: { sampleType: "unfilterable-float" } },
  ]);
  const dimsOf = (s) => {
    const m = /^(\d+)x(\d+)$/.exec(s);
    if (!m) throw new Error("map/cells: WxH, not " + s);
    return [Number(m[1]), Number(m[2])];
  };
  const mapTargets = new Map();
  // mapv=V: every factor V instead of the pattern (mapv=1 must draw as mwsr)
  const sharpMapView = (spec, analysed, value) => {
    const key = "map-" + spec + (analysed ? "-a" : "-p" + (value ?? ""));
    if (!mapTargets.has(key)) {
      const [w, h] = dimsOf(spec);
      const tex = mkTex(w, h, "r16float", T.TEXTURE_BINDING | T.RENDER_ATTACHMENT | T.COPY_DST);
      if (!analysed)
        device.queue.writeTexture({ texture: tex }, MAP.mapPattern(w, h, value), { bytesPerRow: w * 2 }, [w, h]);
      views[key] = tex.createView();
      targetFormats[key] = "r16float";
      mapTargets.set(key, key);
    }
    return views[mapTargets.get(key)];
  };
  const mrtPipeline = (key, code, layout, constants) => {
    if (pipelines.has(key)) return pipelines.get(key);
    const module = device.createShaderModule({ code });
    const pl = device.createRenderPipeline({
      layout,
      vertex: { module, entryPoint: "vs" },
      fragment: {
        module,
        entryPoint: "fs",
        targets: [{ format: "rgba16float" }, { format: "rgba16float" }],
        constants,
      },
      primitive: { topology: "triangle-list" },
    });
    pipelines.set(key, pl);
    return pl;
  };
  const analysisPasses = (name, feed, opts, srcView) => {
    const [mw, mh] = dimsOf(opts.map);
    const [cw, ch] = dimsOf(opts.cells || "64x36");
    const q = Number(opts.q || 4);
    const nb = Number(opts.nb || 0);
    const rot = opts.rot === "1";
    const T2 = T.TEXTURE_BINDING | T.RENDER_ATTACHMENT;
    const tag = `${cw}x${ch}-${mw}x${mh}`;
    for (const [k, w, h] of [["sa-" + tag, cw, ch], ["sb-" + tag, cw, ch], ["ta-" + tag, mw, mh], ["tb-" + tag, mw, mh]])
      if (!views[k]) views[k] = mkTex(w, h, "rgba16float", T2).createView();
    // rot=1: one pipeline per phase, the batch's iterations take them in turn
    const phases = (rot ? [0, 1, 2, 3] : [0]).map((p) =>
      mrtPipeline(
        `stats-${feed}-${tag}-q${q}-d${nb}` + (rot ? `-r${p}` : ""),
        MAP.statsWgsl(feed),
        feed === "ext" ? plStatsExt : plStatsTex,
        { CW: cw, CH: ch, Q: q, D: nb, R: rot ? 1 : 0, P: p },
      ),
    );
    const stats = phases[0];
    const tiles = mrtPipeline(`tiles-${tag}`, MAP.TILE_WGSL, plTwo, { MW: mw, MH: mh });
    sharpMapView(opts.map, true); // the map's target (its format) before its pipeline
    // tree=lum: the factor is the tile's mean luma (the chain's check against the bench's numpy);
    // tree=fKxS: the tile's statistic K (0-7) times S, the same check for the others
    const fk = /^f([0-7])x([\d.]+)$/.exec(opts.tree || "");
    const tree = opts.tree === "lum" ? "    return f[0];" : fk ? `    return f[${fk[1]}] * ${Number(fk[2]).toFixed(3)};` : undefined;
    const decide = pipeline(`decide-${tag}-${opts.tree || "std"}`, MAP.decideWgsl(tree), "map-" + opts.map + "-a", ["vs", "fs"], plTwo);
    const p1 =
      feed === "ext"
        ? { pipeline: stats, targets: ["sa-" + tag, "sb-" + tag], ext: (ext) => bind(stats, [ext]) }
        : { pipeline: stats, targets: ["sa-" + tag, "sb-" + tag], group: bind(stats, [srcView]) };
    if (rot) p1.pick = (i) => phases[i % 4];
    return [
      p1,
      { pipeline: tiles, targets: ["ta-" + tag, "tb-" + tag], group: bind(tiles, [views["sa-" + tag], views["sb-" + tag]]) },
      { pipeline: decide, target: "map-" + opts.map + "-a", group: bind(decide, [views["ta-" + tag], views["tb-" + tag]]) },
    ];
  };

  const mwsrPass = (name, feed, opts, srcView) => {
    const over = {};
    const uni = {};
    let half = false;
    let fine = false;
    const srgb = opts.srgb === "1";
    const reg = srgb ? regimes.srgb : regimes.soft;
    let mode = reg.mode === "down" ? "down" : "up";
    let taps = reg.mode === "down" ? reg.taps : 4;
    if (reg.mode === "down") {
      over.R_DOWN = reg.rDown;
      over.LINEAR_LIGHT = reg.linearLight;
    }
    for (const [k, v] of Object.entries(opts)) {
      if (k === "tau" || k === "sharp") uni[k] = Number(v);
      else if (["map", "mapv", "ana", "cells", "q", "nb", "rot", "tree", "via"].includes(k)) continue; // mwsr-map.js
      else if (k === "gaze") {
        uni.gaze = Number(v);
        if (!("GAZE" in over)) over.GAZE = 1;
      } else if (k in ZONE_KEYS) {
        if (!uni.zone) uni.zone = {};
        uni.zone[ZONE_KEYS[k]] = Number(v);
        if (!("GAZE" in over)) over.GAZE = 1;
      } else if (k === "half") half = v === "1";
      else if (k === "fine") fine = v === "1";
      else if (k === "srgb") continue;
      else if (k === "mode") mode = v;
      else if (k === "taps") taps = Number(v);
      else if (k in MWSR_KEYS) over[MWSR_KEYS[k]] = Number(v);
      else throw new Error(name + ": option " + k + "?");
    }
    if (half && !device.features.has("shader-f16"))
      throw new Error(name + ": the device has no shader-f16 (f16=1 asks for it)");
    if (srgb) {
      if (!srgbViews || feed === "ext" || srcView !== inView)
        throw new Error(name + ": srgb=1 reads the SDR input texture only");
      srcView = inViewSrgb;
    }
    const target = srgb ? "out-srgb" : "out";
    const ub = uniform(mwsrUniforms(inW, inH, outW, outH, uni));
    if (opts.map) {
      // mwsr-map.js: the kernel reading a grid of sharpness factors (map=WxH),
      // per pixel from the texture, or (via=vtx) by the vertices of a mesh
      const mapView = sharpMapView(opts.map, !!opts.ana, opts.mapv === undefined ? undefined : Number(opts.mapv));
      const vtx = opts.via === "vtx";
      if (opts.via && !vtx) throw new Error(name + ": via=vtx, or no via");
      const code = (vtx ? MAP.mapKernelVtx : MAP.mapKernel)(mwsrWgsl({ feed, half, mode, taps, fine }));
      const layout = feed === "ext" ? (vtx ? plExtMapV : plExtMap) : vtx ? plTexMapV : plTexMap;
      const pl = pipeline(name, code, target, ["vs", "fs"], layout, over);
      const count = vtx ? MAP.vtxCount(...dimsOf(opts.map)) : 3;
      if (feed === "ext")
        return {
          pipeline: pl,
          target,
          count,
          ext: (ext) => bind(pl, [sampler, ext, { buffer: ub }, mapView]),
        };
      return { pipeline: pl, target, count, group: bind(pl, [sampler, srcView, { buffer: ub }, mapView]) };
    }
    const pl = pipeline(
      name,
      mwsrWgsl({ feed, half, mode, taps, fine }),
      target,
      ["vs", "fs"],
      feed === "ext" ? plExt : plTex,
      over,
    );
    if (feed === "ext")
      return {
        pipeline: pl,
        target,
        ext: (ext) => bind(pl, [sampler, ext, { buffer: ub }]),
      };
    return {
      pipeline: pl,
      target,
      group: bind(pl, [sampler, srcView, { buffer: ub }]),
    };
  };

  // midTex: the picture at the output size (a bilinear blit, once), the input
  // of the `rcas` cell alone, as RCAS reads EASU's output in the product.
  {
    const pl = pipeline(
      "mid",
      tapsShader("tex", 1, "s", false),
      "mid",
      ["vs", "fs"],
      plTex,
    );
    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({
      colorAttachments: [
        { view: views.mid, loadOp: "clear", storeOp: "store" },
      ],
    });
    pass.setPipeline(pl);
    pass.setBindGroup(0, bind(pl, [sampler, inView, { buffer: texelIn }]));
    pass.draw(3);
    pass.end();
    device.queue.submit([enc.finish()]);
  }

  // A cell: its feed and its passes, in order.
  function makeCell(name) {
    const m = /^(blit\+)?(ext|tex)-(s|l)(\d)(-fw)?(-srgb)?$/.exec(name);
    if (m) {
      const pre = !!m[1];
      const feed = m[2];
      const srgb = !!m[6];
      if (feed === "ext" && pre)
        throw new Error(name + ": blit then external makes no sense");
      if (srgb && (feed === "ext" || pre || !srgbViews))
        throw new Error(name + ": -srgb reads the SDR input texture only");
      const passes = [];
      if (pre) passes.push(blitPass());
      passes.push(
        tapsPass(
          feed,
          Number(m[4]),
          m[3],
          !!m[5],
          srgb ? inViewSrgb : pre ? views.blit : inView,
          srgb,
        ),
      );
      return { name, feed: pre || feed === "ext" ? "ext" : "tex", passes };
    }
    if (name === "blit") return { name, feed: "ext", passes: [blitPass()] };
    // Canvas2D's own resampling (imageSmoothingQuality high: the Smooth2D of
    // criterion C10), a CPU cell for the quality flows only: no GPU time.
    if (name === "smooth2d")
      return {
        name,
        feed: "cpu",
        passes: [],
        run: async () => {
          if (!pictures.length) throw new Error("smooth2d: no canvas picture (hdr)");
          const cv = new OffscreenCanvas(outW, outH);
          const ctx = cv.getContext("2d", { alpha: false });
          ctx.imageSmoothingEnabled = true;
          ctx.imageSmoothingQuality = "high";
          ctx.drawImage(pictures[0], 0, 0, outW, outH);
          device.queue.copyExternalImageToTexture(
            { source: cv },
            { texture: outTex },
            { width: outW, height: outH },
          );
          await device.queue.onSubmittedWorkDone();
        },
      };
    const w = /^(blit\+)?(ext-)?mwsr(?::(.*))?$/.exec(name);
    if (w) {
      const pre = !!w[1];
      const feed = w[2] ? "ext" : "tex";
      if (feed === "ext" && pre)
        throw new Error(name + ": blit then external makes no sense");
      const opts = Object.fromEntries(
        (w[3] || "")
          .split(";")
          .filter(Boolean)
          .map((kv) => kv.split("=")),
      );
      const passes = pre ? [blitPass()] : [];
      // mwsr-map.js: ana=1 writes the map from the picture first (three passes)
      if (opts.map && opts.ana) passes.push(...analysisPasses(name, feed, opts, pre ? views.blit : inView));
      passes.push(mwsrPass(name, feed, opts, pre ? views.blit : inView));
      return { name, feed: pre || feed === "ext" ? "ext" : "tex", passes };
    }
    const y = /^(split\+)?yuv(?::(.*))?$/.exec(name);
    if (y) {
      if (hdr) throw new Error(name + ": the planes are SDR (8 or 10 bits)");
      const split = !!y[1];
      const opts = Object.fromEntries(
        (y[2] || "")
          .split(";")
          .filter(Boolean)
          .map((kv) => kv.split("=")),
      );
      const v = Number(opts.v ?? 2);
      const c = opts.c ?? "420";
      const bits = Number(opts.bits ?? 8);
      if (![0, 1, 2, 3, 4].includes(v) || !["420", "444"].includes(c) || ![8, 10].includes(bits))
        throw new Error(name + ": v=0..4, c=420|444, bits=8|10");
      if (split && v === 0) throw new Error(name + ": the split's v=0 is blit+mwsr");
      const set = planeSet(c, bits, split ? "s" : "p");
      const over = {};
      const uni = {};
      let half = false;
      for (const [k, val] of Object.entries(opts)) {
        if (k === "v" || k === "c" || k === "bits") continue;
        if (k === "tau" || k === "sharp") uni[k] = Number(val);
        else if (k === "half") half = val === "1";
        else if (k in MWSR_KEYS) over[MWSR_KEYS[k]] = Number(val);
        else throw new Error(name + ": option " + k + "?");
      }
      if (half && !device.features.has("shader-f16"))
        throw new Error(name + ": the device has no shader-f16 (f16=1 asks for it)");
      const passes = split ? splitPasses(set) : [];
      const ub = uniform(mwsrUniforms(inW, inH, outW, outH, uni));
      const up = mwsrWgsl({ feed: "tex", half });
      if (v === 0) {
        // the RGB kernel on the planes made RGB again (the fair V0 at 4:2:0)
        const pl = pipeline(name, up, "out", ["vs", "fs"], plTex, over);
        passes.push({
          pipeline: pl,
          target: "out",
          group: bind(pl, [sampler, views[set.rgb], { buffer: ub }]),
        });
      } else {
        const code =
          v === 1
            ? YUV.yKernel(up, "plane")
            : v === 2
              ? YUV.yKernel(up, "rgb")
              : v === 3
                ? YUV.rawKernel(up)
                : YUV.rgbKernel(up);
        const target = v === 1 ? set.yOut : "out";
        const consts = { ...over, ...(bits === 10 ? { BITS: 10 } : {}) };
        const pl = pipeline(name, code, target, ["vs", "fs"], plYuv, consts);
        passes.push({
          pipeline: pl,
          target,
          group: bind(pl, [sampler, views[set.y], { buffer: ub }, views[set.uv]]),
        });
        if (v === 1) passes.push(rgbPass(set, "out", true));
      }
      return { name, feed: split ? "ext" : "tex", passes };
    }
    const k = /^(blit\+)?(sgsr-repo|sgsr-src|sgsrh|sgsrh32|nis|easu|easuh|easuh32|rcas|fsr1|fsr1h|fsr1h32)$/.exec(name);
    if (k) {
      const pre = !!k[1];
      const srcView = pre ? views.blit : inView;
      const passes = pre ? [blitPass()] : [];
      if (k[2] === "rcas") passes.push(rcasFrom(views.mid));
      else if (k[2].startsWith("fsr1")) {
        const v = k[2].slice(4); // "", "h", "h32"
        passes.push(easuToInterm(srcView, v), rcasFrom(views.interm, v));
      } else passes.push(kernelPass(k[2], srcView));
      return { name, feed: pre ? "ext" : "tex", passes };
    }
    if (name.startsWith("chain-"))
      return { name, feed: "chain", algo: name.slice(6) };
    throw new Error("cell " + name + "?");
  }

  // Timestamps: a query set wide enough for a batch of the longest chain,
  // resolved once per batch into a mappable buffer.
  const maxPasses = 6; // mwsr-map.js's analysed cells: three passes before the kernel
  const qCount = 2 * maxPasses * batch;
  let querySet = null;
  let resolveBuf = null;
  let readBuf = null;
  if (hasTs) {
    querySet = device.createQuerySet({ type: "timestamp", count: qCount });
    const bytes = Math.ceil((qCount * 8) / 256) * 256;
    resolveBuf = device.createBuffer({
      size: bytes,
      usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC,
    });
    readBuf = device.createBuffer({
      size: bytes,
      usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
    });
  }
  let frameCursor = 0;
  const nextFrame = () => frames[frameCursor++ % frames.length];

  // One batch of `iters` iterations of a cell in one command buffer; the
  // GPU time of each pass of each iteration, and the wall time submit → done.
  async function runBatch(cell, iters, stamp) {
    const enc = device.createCommandEncoder();
    let q = 0;
    for (let i = 0; i < iters; i++) {
      let ext = null;
      if (cell.feed === "ext") {
        if (!frames.length)
          throw new Error(
            cell.name + ": no VideoFrame (codec=none, or the loop failed)",
          );
        ext = device.importExternalTexture({
          source: nextFrame(),
          colorSpace: "srgb",
        });
      }
      for (const ps of cell.passes) {
        const desc = {
          // several targets: the analysis passes of mwsr-map.js
          colorAttachments: (ps.targets || [ps.target]).map((t) => ({
            view: views[t],
            loadOp: "clear",
            storeOp: "store",
            clearValue: { r: 0, g: 0, b: 0, a: 1 },
          })),
        };
        if (stamp) {
          desc.timestampWrites = {
            querySet,
            beginningOfPassWriteIndex: q,
            endOfPassWriteIndex: q + 1,
          };
          q += 2;
        }
        const pass = enc.beginRenderPass(desc);
        pass.setPipeline(ps.pick ? ps.pick(i) : ps.pipeline); // rot=1: the phase of iteration i
        pass.setBindGroup(0, ps.ext ? ps.ext(ext) : ps.group);
        pass.draw(ps.count || 3); // a mesh's vertices for via=vtx (mwsr-map.js)
        pass.end();
      }
    }
    if (stamp && q) {
      enc.resolveQuerySet(querySet, 0, q, resolveBuf, 0);
      enc.copyBufferToBuffer(resolveBuf, 0, readBuf, 0, q * 8);
    }
    const t0 = performance.now();
    device.queue.submit([enc.finish()]);
    await device.queue.onSubmittedWorkDone();
    const wall = performance.now() - t0;
    let times = null;
    if (stamp && q) {
      await readBuf.mapAsync(GPUMapMode.READ, 0, q * 8);
      const a = new BigInt64Array(readBuf.getMappedRange(0, q * 8));
      times = [];
      for (let k = 0; k < q; k += 2) times.push(Number(a[k + 1] - a[k]) / 1e6);
      readBuf.unmap();
    }
    return { wall, times, passes: cell.passes.length, iters };
  }

  // The product's renderer, on an OffscreenCanvas, one draw per iteration;
  // its passes stamped by the hooks, its draw() awaited as the page does.
  // pace=tick spaces the draws by `period`, like the product's frames; flip=1
  // alternates the output size between the page's and the input's (1:1) from
  // one draw to the next: the renderer's 1:1 copy (P1) and the passage both
  // ways, with the pass count and the draw time of every iteration.
  const renderers = new Map();
  async function chainRenderer(algo) {
    if (renderers.has(algo)) return renderers.get(algo);
    const canvas = new OffscreenCanvas(outW, outH);
    let r;
    if (algo.startsWith("gl-")) {
      const { WebGlRenderer } = await import(WEBGL_RENDERER_URL);
      r = await WebGlRenderer.create(canvas, {
        algo: algo === "gl-off" ? "off" : algo,
        videoCodec: "h264",
      });
    } else {
      const { WebGpuRenderer } = await import(RENDERER_URL);
      r = await WebGpuRenderer.create(canvas, { algo, videoCodec: "h264" });
    }
    r.setOutputSize(outW, outH);
    renderers.set(algo, r);
    let gpu = "";
    if (r.gl) {
      const dbg = r.gl.getExtension("WEBGL_debug_renderer_info");
      gpu = " on " + (dbg ? r.gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : r.gl.getParameter(r.gl.RENDERER));
      if (p.soft === "1" && !/swiftshader/i.test(gpu))
        throw new Error("soft=1 but WebGL2 runs" + gpu);
    }
    log("chain " + algo + ": " + r.kind + " renderer" + gpu);
    return r;
  }
  async function runChain(cell, iters, stamp) {
    if (!frames.length) throw new Error(cell.name + ": no VideoFrame");
    const r = await chainRenderer(cell.algo);
    const times = [];
    const walls = [];
    const submits = [];
    const dones = [];
    const passSeq = [];
    const queries = [];
    const tq = stamp ? glTimer(r) : null;
    let copies = 0;
    const start = performance.now();
    for (let i = 0; i < iters; i++) {
      if (flip) {
        if (i % 2) r.setOutputSize(inW, inH);
        else r.setOutputSize(outW, outH);
      }
      if (pace === "tick") {
        const due = start + i * period;
        const now = performance.now();
        if (due > now) await new Promise((res) => setTimeout(res, due - now));
      }
      HOOK.active = stamp;
      HOOK.q = 0;
      const f = nextFrame().clone();
      let q = null;
      if (tq) {
        q = r.gl.createQuery();
        r.gl.beginQuery(tq.TIME_ELAPSED_EXT, q);
      }
      const t0 = performance.now();
      await r.draw(f);
      const t1 = performance.now();
      if (tq) r.gl.endQuery(tq.TIME_ELAPSED_EXT);
      // WebGL2's draw() only queues; the GPU's completion is waited for
      // here, as the WebGPU renderer's draw() waits for onSubmittedWorkDone:
      // `done` is the same quantity on both sides, `submit` the main thread's.
      if (r.gl) await glDone(r.gl);
      walls.push(t1 - t0);
      submits.push(r.gl ? t1 - t0 : r.lastDraw.submitMs);
      dones.push(performance.now() - t0);
      queries.push(q);
      HOOK.active = false;
      if (r.passthrough === true) copies++;
      let np = 0;
      if (stamp && HOOK.q && HOOK.dev) {
        const q = HOOK.q;
        await HOOK.read.mapAsync(GPUMapMode.READ, 0, q * 8);
        const a = new BigInt64Array(HOOK.read.getMappedRange(0, q * 8));
        const ts = [];
        for (let k = 0; k < q; k += 2) ts.push(Number(a[k + 1] - a[k]) / 1e6);
        HOOK.read.unmap();
        np = q / 2;
        times.push(ts);
      }
      passSeq.push(np);
    }
    if (flip) r.setOutputSize(outW, outH);
    // No frame reached its canvas: every way in refused the last one (its
    // path ends in `+lost`), or the renderer gave up (WebGpuRenderer.failed;
    // the page would move the picture to another renderer). Nothing to time
    // or dump.
    const lost = /\+lost$/.test((r.lastDraw && r.lastDraw.path) || "");
    if (r.failed || lost)
      throw new Error(
        cell.name + ": no frame reached the renderer's canvas (" + (r.failed ? "it gave up" : "no way in") + ")",
      );
    if (tq) {
      // the WebGL2 renderer's GPU time per draw, as one pass each
      const ms = await readGlQueries(r.gl, tq, queries);
      ms.forEach((v, i) => {
        if (v === null) return;
        times.push([v]);
        passSeq[i] = 1;
      });
    }
    return {
      wall: walls.reduce((a, b) => a + b, 0),
      iterTimes: stamp && times.length ? times : null,
      passSeq,
      copies,
      iters,
      draws: walls,
      submits,
      dones,
    };
  }

  // The picture the product's renderer presented, into outTex, so that a
  // chain cell has a shot and a dump like the others (SDR: the canvas is
  // 8-bit): the OffscreenCanvas' bitmap, copied in without conversion.
  async function chainToOut(cell) {
    const r = renderers.get(cell.algo);
    if (!r) throw new Error(cell.name + ": no renderer");
    frameCursor = 0; // the clip's first frame: the kernel cells' picture
    if (r.gl) {
      // WebGL2: one draw, then the drawing buffer as a bitmap.
      await runChain(cell, 1, false);
      const bmp = r.canvas.transferToImageBitmap();
      try {
        if (bmp.width !== outW || bmp.height !== outH)
          throw new Error(cell.name + ": canvas " + bmp.width + "x" + bmp.height);
        device.queue.copyExternalImageToTexture(
          { source: bmp },
          { texture: outTex },
          { width: outW, height: outH },
        );
      } finally {
        bmp.close();
      }
      return;
    }
    // WebGPU: during this one draw the renderer gets a texture of ours in
    // place of the swap chain's (the getCurrentTexture hook) and we read it
    // back on its own device — the presented bitmap of a WebGPU
    // OffscreenCanvas came back black on a Linux runner's software adapter
    // (CI 39627353471) while the same read was exact on Windows.
    HOOK.capture = {};
    let cap;
    try {
      await runChain(cell, 1, false);
    } finally {
      cap = HOOK.capture;
      HOOK.capture = null;
    }
    if (!cap.texture) throw new Error(cell.name + ": the renderer drew no canvas texture");
    try {
      if (cap.w !== outW || cap.h !== outH)
        throw new Error(cell.name + ": canvas " + cap.w + "x" + cap.h);
      const bpr = Math.ceil((cap.w * 4) / 256) * 256;
      const buf = cap.device.createBuffer({
        size: bpr * cap.h,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
      });
      const enc = cap.device.createCommandEncoder();
      enc.copyTextureToBuffer(
        { texture: cap.texture },
        { buffer: buf, bytesPerRow: bpr },
        { width: cap.w, height: cap.h },
      );
      cap.device.queue.submit([enc.finish()]);
      await buf.mapAsync(GPUMapMode.READ);
      const bytes = new Uint8Array(buf.getMappedRange()).slice();
      buf.unmap();
      buf.destroy();
      // Both canvases take the preferred format; should they differ, swap R and B.
      const swap = (cap.format.startsWith("bgra") !== format.startsWith("bgra"));
      if (swap)
        for (let y = 0; y < cap.h; y++)
          for (let o = y * bpr, e = o + cap.w * 4; o < e; o += 4) {
            const t = bytes[o];
            bytes[o] = bytes[o + 2];
            bytes[o + 2] = t;
          }
      device.queue.writeTexture(
        { texture: outTex },
        bytes,
        { bytesPerRow: bpr, rowsPerImage: cap.h },
        { width: cap.w, height: cap.h },
      );
    } finally {
      cap.texture.destroy();
    }
  }

  async function measure(cell, iters, stamp) {
    if (cell.feed === "chain") return runChain(cell, iters, stamp);
    if (cell.feed === "cpu") {
      await cell.run();
      return { wall: 0, times: null, passes: 0, iters };
    }
    if (pace === "tick") {
      // one iteration per period, each its own submission
      const times = [];
      let wall = 0;
      const start = performance.now();
      for (let i = 0; i < iters; i++) {
        const due = start + i * period;
        const now = performance.now();
        if (due > now) await new Promise((res) => setTimeout(res, due - now));
        const b = await runBatch(cell, 1, stamp);
        wall += b.wall;
        if (b.times) times.push(...b.times);
      }
      return {
        wall,
        times: stamp && times.length ? times : null,
        passes: cell.passes.length,
        iters,
      };
    }
    return runBatch(cell, iters, stamp);
  }

  const cells = cellNames.map((n) => {
    try {
      return makeCell(n);
    } catch (e) {
      errors.push(e.message);
      log("cell " + n + " skipped: " + e.message);
      return null;
    }
  });
  const results = new Map();
  const order = [];
  for (let r = 0; r < rounds; r++) {
    const seq = r % 2 ? [...cells].reverse() : cells;
    for (const c of seq) if (c) order.push([c, r]);
  }
  // A page Safari preloads behind the address bar (iPhone, 22/10) runs hidden,
  // throttled to half rate, and posts a second, slower result: measure only
  // once the page is seen, and tell how long it was hidden meanwhile.
  if (document.hidden) {
    log("page hidden: waiting to be shown before measuring");
    await new Promise((resolve) => {
      const onShown = () => {
        if (document.hidden) return;
        document.removeEventListener("visibilitychange", onShown);
        resolve();
      };
      document.addEventListener("visibilitychange", onShown);
    });
  }
  let hiddenMs = 0;
  let hiddenSince = 0;
  const onVisibility = () => {
    if (document.hidden) hiddenSince = performance.now();
    else if (hiddenSince) {
      hiddenMs += performance.now() - hiddenSince;
      hiddenSince = 0;
    }
  };
  document.addEventListener("visibilitychange", onVisibility);
  const t0 = performance.now();
  for (const [cell, r] of order) {
    const e0 = errors.length;
    try {
      const iters =
        cell.feed === "chain"
          ? Math.max(8, Math.ceil((batch * batches) / 4))
          : batch;
      for (let w = 0; w < warm; w++)
        await measure(cell, cell.feed === "chain" ? 4 : iters, false);
      const totals = [];
      const perPass = [];
      let wallMs = 0;
      let n = 0;
      const nb = cell.feed === "chain" ? 1 : batches;
      for (let b = 0; b < nb; b++) {
        const m = await measure(cell, iters, hasTs || cell.feed === "chain");
        wallMs += m.wall;
        n += m.iters;
        if (m.iterTimes) {
          // the chain: every iteration its own passes, whose count may change
          // from one draw to the next (flip=1); totals also kept by that count
          for (const ts of m.iterTimes) {
            let s = 0;
            ts.forEach((t, k) => {
              s += t;
              (perPass[k] ||= []).push(t);
            });
            totals.push(s);
            ((cell.byPasses ||= {})[ts.length] ||= []).push(s);
          }
        } else if (m.times) {
          const np = m.passes;
          for (let i = 0; i + np <= m.times.length; i += np) {
            let s = 0;
            for (let k = 0; k < np; k++) {
              s += m.times[i + k];
              (perPass[k] ||= []).push(m.times[i + k]);
            }
            totals.push(s);
          }
        }
        if (m.draws) (cell.draws ||= []).push(...m.draws);
        if (m.submits) (cell.submits ||= []).push(...m.submits);
        if (m.dones) (cell.dones ||= []).push(...m.dones);
        if (m.passSeq) (cell.passSeq ||= []).push(...m.passSeq);
        if (m.copies !== undefined) cell.copies = (cell.copies || 0) + m.copies;
      }
      if (!results.has(cell.name)) results.set(cell.name, { cell, rounds: [] });
      results
        .get(cell.name)
        .rounds.push({ round: r, totals, perPass, wallPerIter: wallMs / n });
      const tot = stats(totals);
      log(
        `r${r} ${cell.name.padEnd(16)} ${tot ? "gpu p50 " + tot.p50.toFixed(3) + " p99 " + tot.p99.toFixed(3) : "no ts"}  wall/iter ${(wallMs / n).toFixed(3)} ms`,
      );
    } catch (e) {
      errors.push(String(e.message || e));
      log(cell.name + " failed: " + (e.message || e));
    }
    // a WebGPU validation error that surfaced during this cell: say which
    for (let i = e0; i < errors.length; i++)
      errors[i] = "[" + cell.name + "] " + errors[i];
  }
  const elapsed = performance.now() - t0;
  document.removeEventListener("visibilitychange", onVisibility);
  if (hiddenSince) hiddenMs += performance.now() - hiddenSince;
  if (hiddenMs) log(`page hidden ${hiddenMs.toFixed(0)} ms during the run`);

  const out = [];
  for (const [name, r] of results) {
    const all = r.rounds.flatMap((x) => x.totals);
    const roundMed = r.rounds
      .map((x) => quant(x.totals, 0.5))
      .filter((x) => x !== null);
    const gpu = stats(all);
    const np = Math.max(...r.rounds.map((x) => x.perPass.length));
    const perPass = [];
    for (let k = 0; k < np; k++)
      perPass.push(
        quant(
          r.rounds.flatMap((x) => x.perPass[k] || []),
          0.5,
        ),
      );
    const wallPerIter = quant(
      r.rounds.map((x) => x.wallPerIter),
      0.5,
    );
    out.push({
      name,
      feed: r.cell.feed,
      passes: r.cell.feed === "chain" ? np : r.cell.passes.length,
      gpu,
      perPass,
      roundMedians: roundMed,
      disp:
        gpu && roundMed.length > 1
          ? ((Math.max(...roundMed) - Math.min(...roundMed)) / gpu.p50) * 100
          : null,
      wallPerIter,
      draw: r.cell.draws ? stats(r.cell.draws) : null,
      // the chain: the draw's main-thread time and the time to the GPU's completion
      submit: r.cell.submits ? stats(r.cell.submits) : null,
      done: r.cell.dones ? stats(r.cell.dones) : null,
      // the chain: how many draws had which pass count, how many were the
      // renderer's 1:1 copy, and the GPU total by pass count (flip=1)
      passSeq: r.cell.passSeq
        ? r.cell.passSeq.reduce((h, k) => ((h[k] = (h[k] || 0) + 1), h), {})
        : null,
      copies: r.cell.copies === undefined ? null : r.cell.copies,
      byPasses: r.cell.byPasses
        ? Object.fromEntries(
            Object.entries(r.cell.byPasses).map(([k, v]) => [k, stats(v)]),
          )
        : null,
    });
  }

  let shots = null;
  if (wantShots && !hdr) {
    shots = {};
    for (const c of cells) {
      if (!c) continue;
      try {
        if (c.feed === "chain") await chainToOut(c);
        else await measure(c, 1, false);
        shots[c.name] = await crop(device, outTex, format, outW, outH);
      } catch (e) {
        errors.push("[shot " + c.name + "] " + e.message);
      }
    }
  }
  const dumped = [];
  for (const c of cells) {
    if (!c || !dumpList.includes(c.name) || (c.feed === "chain" && hdr)) continue;
    try {
      if (c.feed === "chain") await chainToOut(c);
      else await measure(c, 1, false);
      const name = (p.tag || "lab") + "-" + c.name.replace(/[^\w.+-]/g, "_");
      if (hdr) {
        const bytes = await readHalf(device, outTex, outW, outH);
        await postBlob("/__bin?name=" + encodeURIComponent(name), new Blob([bytes]));
      } else {
        const blob = await readPng(device, outTex, format, 0, 0, outW, outH);
        await postBlob("/__png?name=" + encodeURIComponent(name), blob);
      }
      dumped.push(name);
    } catch (e) {
      errors.push("[dump " + c.name + "] " + e.message);
    }
  }
  if (dumped.length) log("dumped: " + dumped.join(" "));

  for (const f of frames) f.close();
  for (const r of renderers.values()) r.dispose();
  device.destroy();
  for (const m of HOOK.productErrors) errors.push("[product device] " + m);
  HOOK.productErrors = [];
  return {
    adapter: {
      name: adapterName,
      vendor: info.vendor,
      architecture: info.architecture,
      device: info.device,
      description: info.description,
    },
    features,
    timestamps: hasTs,
    format: outFormat,
    userAgent: navigator.userAgent,
    isolated: !!globalThis.crossOriginIsolated,
    params: {
      in: `${inW}x${inH}`,
      out: `${outW}x${outH}`,
      content,
      codec,
      pace,
      period,
      batch,
      batches,
      warm,
      rounds,
      sets,
      power: p.power || null,
      fallback: p.fallback === "1",
      flip,
    },
    frames: frameInfo,
    elapsedMs: elapsed,
    hiddenMs: Math.round(hiddenMs),
    cells: out,
    shots,
    dumped,
    errors,
  };
}

async function postBlob(url, blob) {
  const r = await fetch(url, { method: "POST", body: blob });
  if (!r.ok) throw new Error("POST " + url + ": " + r.status);
}

// A centre crop of the output texture (512×288), as a PNG data URL.
async function crop(device, tex, format, w, h) {
  const cw = Math.min(512, w);
  const ch = Math.min(288, h);
  const x = Math.floor((w - cw) / 2);
  const y = Math.floor((h - ch) / 2);
  const blob = await readPng(device, tex, format, x, y, cw, ch);
  return await new Promise((res) => {
    const fr = new FileReader();
    fr.onload = () => res(fr.result);
    fr.readAsDataURL(blob);
  });
}

// A region of a texture read back, as a PNG blob.
// The whole of an rgba16float texture, as tightly packed half floats (RGBA).
async function readHalf(device, tex, cw, ch) {
  const bpr = Math.ceil((cw * 8) / 256) * 256;
  const buf = device.createBuffer({
    size: bpr * ch,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
  });
  const enc = device.createCommandEncoder();
  enc.copyTextureToBuffer(
    { texture: tex },
    { buffer: buf, bytesPerRow: bpr },
    { width: cw, height: ch },
  );
  device.queue.submit([enc.finish()]);
  await buf.mapAsync(GPUMapMode.READ);
  const a = new Uint8Array(buf.getMappedRange());
  const out = new Uint8Array(cw * ch * 8);
  for (let r = 0; r < ch; r++) out.set(a.subarray(r * bpr, r * bpr + cw * 8), r * cw * 8);
  buf.unmap();
  buf.destroy();
  return out;
}

async function readPng(device, tex, format, x, y, cw, ch) {
  const bpr = Math.ceil((cw * 4) / 256) * 256;
  const buf = device.createBuffer({
    size: bpr * ch,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
  });
  const enc = device.createCommandEncoder();
  enc.copyTextureToBuffer(
    { texture: tex, origin: { x, y } },
    { buffer: buf, bytesPerRow: bpr },
    { width: cw, height: ch },
  );
  device.queue.submit([enc.finish()]);
  await buf.mapAsync(GPUMapMode.READ);
  const a = new Uint8Array(buf.getMappedRange());
  const img = new ImageData(cw, ch);
  const bgra = format === "bgra8unorm";
  for (let r = 0; r < ch; r++)
    for (let c = 0; c < cw; c++) {
      const s = r * bpr + c * 4;
      const d = (r * cw + c) * 4;
      img.data[d] = bgra ? a[s + 2] : a[s];
      img.data[d + 1] = a[s + 1];
      img.data[d + 2] = bgra ? a[s] : a[s + 2];
      img.data[d + 3] = 255;
    }
  buf.unmap();
  buf.destroy();
  const cv = new OffscreenCanvas(cw, ch);
  cv.getContext("2d").putImageData(img, 0, 0);
  return await cv.convertToBlob({ type: "image/png" });
}

// ── hooks for the product's renderer ───────────────────────────────────────
// Nothing in the product changes: its adapter request takes the lab's options,
// its device gets timestamp-query when the adapter has it, and while a chain
// cell draws, every render pass it begins is stamped and every command buffer
// it finishes resolves the stamps into a buffer the lab maps afterwards.
const HOOK = {
  active: false,
  q: 0,
  dev: null,
  qs: null,
  resolve: null,
  read: null,
  count: 8,
  /** The lab's own device (its errors are its own). */
  labDevice: null,
  /** Uncaptured errors of the product's devices, joined to the run's. */
  productErrors: [],
  /** While set, getCurrentTexture hands the renderer a texture of ours (chainToOut). */
  capture: null,
  /**
   * importfail=N: the product's devices get every video import refused (1),
   * and their ImageBitmap copies (2), and the readback of the frames (3).
   */
  importFail: 0,
  /**
   * nopresent=1: every draw of a product's WebGPU canvas gets a texture of
   * ours from getCurrentTexture (the `sink`), as a capture does. On Linux with
   * the software adapter a WebGPU canvas cannot present: its swap chain finds
   * no shared image to allocate, and Chrome destroys the device and drops the
   * page's whole WebGPU instance, the lab's device with it (22/10/2026, CI
   * 39737555924). The renderer's draw is the same either way.
   */
  noPresent: false,
  sink: null,
};
let hooked = false;
function installHooks(adapterOpts) {
  if (hooked) return;
  hooked = true;
  const origReq = GPU.prototype.requestAdapter;
  GPU.prototype.requestAdapter = function (opts) {
    return origReq.call(this, { ...(opts || {}), ...adapterOpts });
  };
  // importfail=1: the product's devices (never the lab's) get every import
  // refused, as Chrome does on a machine whose decoded frames cannot be
  // shared with WebGPU (the Linux runner of CI 39661997273).
  const origImport = GPUDevice.prototype.importExternalTexture;
  GPUDevice.prototype.importExternalTexture = function (d) {
    if (HOOK.importFail >= 1 && this !== HOOK.labDevice)
      throw new DOMException("Failed to import texture from video (lab importfail=1)", "OperationError");
    return origImport.call(this, d);
  };
  // importfail=2: their copies of an ImageBitmap too (the lab's own queue
  // copies pictures in, and keeps doing so).
  const origCopyExt = GPUQueue.prototype.copyExternalImageToTexture;
  GPUQueue.prototype.copyExternalImageToTexture = function (src, dst, size) {
    if (HOOK.importFail >= 2 && (!HOOK.labDevice || this !== HOOK.labDevice.queue))
      throw new DOMException("Failed to copy content from external image (lab importfail=2)", "OperationError");
    return origCopyExt.call(this, src, dst, size);
  };
  // importfail=3: and a VideoFrame read back in another format (the lab never
  // reads one back itself).
  const origCopyTo = VideoFrame.prototype.copyTo;
  VideoFrame.prototype.copyTo = function (dst, opts) {
    if (HOOK.importFail >= 3 && opts && opts.format)
      return Promise.reject(new DOMException("copyTo refused (lab importfail=3)", "NotSupportedError"));
    return origCopyTo.call(this, dst, opts);
  };
  const origDev = GPUAdapter.prototype.requestDevice;
  GPUAdapter.prototype.requestDevice = function (desc) {
    const d = { ...(desc || {}) };
    const f = new Set(d.requiredFeatures || []);
    if (this.features.has("timestamp-query")) f.add("timestamp-query");
    d.requiredFeatures = [...f];
    return origDev.call(this, d).then((dev) => {
      dev.addEventListener("uncapturederror", (e) => {
        if (dev !== HOOK.labDevice) HOOK.productErrors.push(e.error.message);
      });
      return dev;
    });
  };
  // The canvas: its configuration remembered, and while a capture is on the
  // current texture is one of ours — same device, format, size and view
  // formats as the swap chain's, readable (COPY_SRC) once the draw is in.
  const origCfg = GPUCanvasContext.prototype.configure;
  GPUCanvasContext.prototype.configure = function (cfg) {
    this.__cfg = cfg;
    return origCfg.call(this, cfg);
  };
  const origCur = GPUCanvasContext.prototype.getCurrentTexture;
  GPUCanvasContext.prototype.getCurrentTexture = function () {
    const cap = HOOK.capture || (HOOK.noPresent ? (HOOK.sink ||= {}) : null);
    if (!cap || !this.__cfg) return origCur.call(this);
    const w = this.canvas.width;
    const h = this.canvas.height;
    if (!cap.texture || cap.ctx !== this || cap.device !== this.__cfg.device || cap.w !== w || cap.h !== h) {
      if (cap.texture) cap.texture.destroy();
      cap.ctx = this;
      cap.w = w;
      cap.h = h;
      cap.device = this.__cfg.device;
      cap.format = this.__cfg.format;
      cap.texture = cap.device.createTexture({
        size: { width: w, height: h },
        format: cap.format,
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
        viewFormats: this.__cfg.viewFormats || [],
      });
    }
    return cap.texture;
  };
  const origEnc = GPUDevice.prototype.createCommandEncoder;
  GPUDevice.prototype.createCommandEncoder = function (d) {
    const e = origEnc.call(this, d);
    e.__dev = this;
    return e;
  };
  const ensure = (dev) => {
    if (HOOK.dev === dev) return true;
    if (!dev.features.has("timestamp-query")) return false;
    HOOK.dev = dev;
    HOOK.qs = dev.createQuerySet({ type: "timestamp", count: HOOK.count });
    HOOK.resolve = dev.createBuffer({
      size: 256,
      usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC,
    });
    HOOK.read = dev.createBuffer({
      size: 256,
      usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
    });
    return true;
  };
  const origPass = GPUCommandEncoder.prototype.beginRenderPass;
  GPUCommandEncoder.prototype.beginRenderPass = function (desc) {
    if (
      HOOK.active &&
      !desc.timestampWrites &&
      this.__dev &&
      ensure(this.__dev) &&
      HOOK.q + 2 <= HOOK.count
    ) {
      desc = {
        ...desc,
        timestampWrites: {
          querySet: HOOK.qs,
          beginningOfPassWriteIndex: HOOK.q,
          endOfPassWriteIndex: HOOK.q + 1,
        },
      };
      HOOK.q += 2;
      this.__stamped = true;
    }
    return origPass.call(this, desc);
  };
  const origFinish = GPUCommandEncoder.prototype.finish;
  GPUCommandEncoder.prototype.finish = function (d) {
    if (this.__stamped && HOOK.q) {
      this.resolveQuerySet(HOOK.qs, 0, HOOK.q, HOOK.resolve, 0);
      this.copyBufferToBuffer(HOOK.resolve, 0, HOOK.read, 0, HOOK.q * 8);
      this.__stamped = false;
    }
    return origFinish.call(this, d);
  };
}
