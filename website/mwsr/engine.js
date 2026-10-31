// The comparison page's GPU side (website/mwsr/): one WebGPU device, the
// input and the native frame in textures, each upscaler drawn once into its
// own texture, the panes drawn from those textures pixel for pixel, and the
// timing: batches of draws wrapped by two GPU timestamps.
//
// The kernels are the product's (kernels.js, MwsrShader.js and NisTables.js,
// written by scripts/website/build-site.mjs, handed in by app.js under their
// cache-busting URLs), bound and fed as the lab binds and feeds them
// (scripts/bench/scaler-lab/scaler-lab.js): the figures of the page are the
// README's method, on the visitor's GPU.

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

// One bilinear sample: the floor every upscaler is read against.
const BILINEAR =
  VS +
  /* wgsl */ `
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var tex : texture_2d<f32>;
@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    return vec4f(textureSampleLevel(tex, samp, in.uv, 0.0).rgb, 1.0);
}
`;

// AMD's simple reversible tone map (ffx_fsr1.h, FsrSrtm and FsrSrtmInv), the
// way FSR1 is told to run on HDR; the page runs NIS and SGSR1 inside it too,
// as neither port reads values past 1.0.
const SRTM = (inv) =>
  VS +
  /* wgsl */ `
@group(0) @binding(0) var tex : texture_2d<f32>;
@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let c = max(textureLoad(tex, vec2i(in.pos.xy), 0).rgb, vec3f(0.0));
    let m = max(c.r, max(c.g, c.b));
    return vec4f(${inv ? 'c / max(1.0 / 32768.0, 1.0 - m)' : 'c / (m + 1.0)'}, 1.0);
}
`;

// The HDR feed (sRGB-encoded past 1.0) back to linear light, as the client's
// HDR pass 0 writes it for MWSR's reduction (LINEAR_LIGHT 3, journal J47): the
// sampler then mixes light, and the reduction encodes once, at its output.
const LIN =
  VS +
  /* wgsl */ `
@group(0) @binding(0) var tex : texture_2d<f32>;
@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let c = max(textureLoad(tex, vec2i(in.pos.xy), 0).rgb, vec3f(0.0));
    let hi = pow((c + 0.055) / 1.055, vec3f(2.4));
    return vec4f(select(hi, c / 12.92, c <= vec3f(0.04045)), 1.0);
}
`;

// A pane: the output pixels at `zoom` device pixels each, around `center`
// (output pixels), A left of `split` and B right of it (canvas pixels).
const VIEW =
  VS +
  /* wgsl */ `
struct View { size : vec2f, center : vec2f, texSize : vec2f, zoom : f32, split : f32, line : f32, _p : f32, _q : vec2f };
@group(0) @binding(0) var a : texture_2d<f32>;
@group(0) @binding(1) var b : texture_2d<f32>;
@group(0) @binding(2) var<uniform> v : View;
@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let p = in.pos.xy;
    if (v.split > 0.0 && abs(p.x - v.split) < v.line) { return vec4f(0.1, 0.83, 1.0, 1.0); }
    let s = floor(v.center + (p - v.size * 0.5) / v.zoom);
    if (any(s < vec2f(0.0)) || any(s >= v.texSize)) { return vec4f(0.02, 0.03, 0.05, 1.0); }
    var c = textureLoad(a, vec2i(s), 0).rgb;
    if (v.split > 0.0 && p.x > v.split) { c = textureLoad(b, vec2i(s), 0).rgb; }
    return vec4f(c, 1.0);
}
`;

/** The upscalers, in the order of the page. `wrap`: runs inside the reversible tone map on HDR. */
export const KERNELS = [
  { id: 'mwsr', name: 'MWSR', half: true },
  // MoonlightWeb's own: MWSR reading its native host's zones (zones.js); the law where there are none
  { id: 'mwsrz', name: 'MWSR zones', half: true },
  { id: 'fsr1', name: 'FSR1', half: true, wrap: true },
  { id: 'nis', name: 'NIS', half: false, wrap: true },
  { id: 'sgsr1', name: 'SGSR1', half: true, wrap: true },
  { id: 'bilinear', name: 'Bilinear', half: false },
];

export async function createEngine({ K, mwsr, nis, soft = false, onLost = () => {} }) {
  const { mwsrRegime, mwsrUniforms, mwsrWgsl } = mwsr;
  const { nisCoefTexture, nisConfig } = nis;
  if (!navigator.gpu) throw new Error('nogpu');
  const adapter = await navigator.gpu.requestAdapter(
    soft ? { forceFallbackAdapter: true } : { powerPreference: 'high-performance' },
  );
  if (!adapter) throw new Error('noadapter');
  const info = adapter.info || (adapter.requestAdapterInfo ? await adapter.requestAdapterInfo() : {});
  const label = [info.vendor, info.architecture, info.description].filter(Boolean).join(' ') || 'WebGPU adapter';
  // soft=1 (the tests): SwiftShader or nothing, before anything is compiled
  if (soft && !(info.isFallbackAdapter || /swiftshader/i.test(label)))
    throw new Error('soft=1 asks for SwiftShader, and the adapter is ' + label);
  const features = ['timestamp-query', 'shader-f16'].filter((f) => adapter.features.has(f));
  const device = await adapter.requestDevice({
    requiredFeatures: features,
    requiredLimits: { maxTextureDimension2D: Math.min(8192, adapter.limits.maxTextureDimension2D) },
  });
  device.lost.then((l) => onLost(l.message || l.reason || 'lost'));
  const hasTs = device.features.has('timestamp-query');
  const hasF16 = device.features.has('shader-f16');

  const T = GPUTextureUsage;
  const B = GPUBufferUsage;
  const FR = GPUShaderStage.FRAGMENT;
  const sampler = device.createSampler({
    magFilter: 'linear',
    minFilter: 'linear',
    addressModeU: 'clamp-to-edge',
    addressModeV: 'clamp-to-edge',
  });
  const layout = (entries) =>
    device.createPipelineLayout({
      bindGroupLayouts: [
        device.createBindGroupLayout({ entries: entries.map((e, binding) => ({ binding, visibility: FR, ...e })) }),
      ],
    });
  const plTex = layout([{ sampler: {} }, { texture: { sampleType: 'float' } }, { buffer: {} }]);
  const plSamp = layout([{ sampler: {} }, { texture: { sampleType: 'float' } }]);
  const plOne = layout([{ texture: { sampleType: 'unfilterable-float' } }]);
  const plNis = layout([
    { sampler: {} },
    { texture: { sampleType: 'float' } },
    { texture: { sampleType: 'unfilterable-float' } },
    { buffer: {} },
  ]);
  // MWSR's second text: the zones' r16float texture at binding 3, read bilinearly
  const plMap = layout([
    { sampler: {} },
    { texture: { sampleType: 'float' } },
    { buffer: {} },
    { texture: { sampleType: 'float' } },
  ]);
  const plView = layout([
    { texture: { sampleType: 'unfilterable-float' } },
    { texture: { sampleType: 'unfilterable-float' } },
    { buffer: {} },
  ]);

  const modules = new Map();
  const pipelines = new Map();
  function pipeline(key, code, format, pl = 'auto', entry = ['vs', 'fs'], constants = undefined) {
    const k = key + '|' + format;
    if (pipelines.has(k)) return pipelines.get(k);
    if (!modules.has(code)) modules.set(code, device.createShaderModule({ code }));
    const module = modules.get(code);
    const p = device.createRenderPipeline({
      layout: pl,
      vertex: { module, entryPoint: entry[0] },
      fragment: { module, entryPoint: entry[1], targets: [{ format }], constants },
      primitive: { topology: 'triangle-list' },
    });
    pipelines.set(k, p);
    return p;
  }
  const bind = (pl, resources) =>
    device.createBindGroup({
      layout: pl.getBindGroupLayout(0),
      entries: resources.map((resource, binding) => ({ binding, resource })),
    });
  const ubo = (arr) => {
    const b = device.createBuffer({
      size: Math.max(16, Math.ceil(arr.byteLength / 16) * 16),
      usage: B.UNIFORM | B.COPY_DST,
    });
    device.queue.writeBuffer(b, 0, arr);
    return b;
  };

  const nisCoef = device.createTexture({ size: [4, 64], format: 'rgba32float', usage: T.TEXTURE_BINDING | T.COPY_DST });
  device.queue.writeTexture(
    { texture: nisCoef },
    nisCoefTexture(),
    { bytesPerRow: 64, rowsPerImage: 64 },
    { width: 4, height: 64 },
  );
  const nisCfg = nisConfig(0.5);

  let C = null; // the case: sizes, format, textures, uniforms
  const owned = [];
  // SDR textures carry their `-srgb` view, which MWSR's reduction reads and
  // draws through, as the client's do (the hardware makes the linear light)
  const tex = (w, h, format, usage) => {
    const t = device.createTexture({
      size: [w, h],
      format,
      usage,
      viewFormats: format === 'rgba8unorm' ? ['rgba8unorm-srgb'] : [],
    });
    owned.push(t);
    return t;
  };

  /** Uploads a case: `native` may be null (a picture taken as the input); `zones`, the native host's
   *  codes (zones.js), only where its client would read them. */
  function setCase({ outW, outH, inW, inH, hdr, native, input, zones = null }) {
    for (const t of owned.splice(0)) t.destroy();
    // MWSR's mode as the client picks it, its reduction below 1:1; at 1:1,
    // where the client copies, its kernel all the same, as every other one
    // runs there: the page shows what each does to a picture it keeps the size of
    const reg =
      inW === outW && inH === outH ? { mode: 'up' } : mwsrRegime(inW, inH, outW, outH, { srgb: !hdr, linear: hdr });
    const format = hdr ? 'rgba16float' : 'rgba8unorm';
    const bpp = hdr ? 8 : 4;
    const RW = T.TEXTURE_BINDING | T.RENDER_ATTACHMENT | T.COPY_SRC;
    const up = (w, h, data) => {
      const t = tex(w, h, format, T.TEXTURE_BINDING | T.COPY_DST | T.COPY_SRC);
      device.queue.writeTexture({ texture: t }, data, { bytesPerRow: w * bpp }, { width: w, height: h });
      return t;
    };
    const inTex = up(inW, inH, input);
    C = {
      outW,
      outH,
      inW,
      inH,
      hdr,
      format,
      bpp,
      inTex,
      reg,
      native: native ? up(outW, outH, native) : null,
      out: Object.fromEntries(KERNELS.map((k) => [k.id, tex(outW, outH, format, RW)])),
      interm: tex(outW, outH, format, RW),
      scratch: tex(outW, outH, format, RW),
      tmIn: hdr ? tex(inW, inH, format, RW) : null,
      tmOut: hdr ? tex(outW, outH, format, RW) : null,
      linIn: hdr && reg.mode === 'down' ? tex(inW, inH, format, RW) : null,
      u: {
        sgsr: ubo(new Float32Array([1 / inW, 1 / inH, inW, inH])),
        easu: ubo(new Float32Array([inW, inH, outW, outH])),
        rcas: ubo(new Float32Array([outW, outH, 1 / outW, 1 / outH])),
        nis: ubo(
          new Float32Array([
            inW / outW,
            inH / outH,
            1 / inW,
            1 / inH,
            ...nisCfg.detect,
            ...nisCfg.sharpA,
            ...nisCfg.sharpB,
            outW,
            outH,
            0,
            0,
          ]),
        ),
        mwsr: ubo(mwsrUniforms(inW, inH, outW, outH, {})),
      },
      zoneView: null,
    };
    if (zones && reg.mode === 'up') {
      const z = tex(zones.cols, zones.rows, 'r16float', T.TEXTURE_BINDING | T.COPY_DST);
      device.queue.writeTexture(
        { texture: z },
        zones.texels,
        { bytesPerRow: zones.cols * 2 },
        { width: zones.cols, height: zones.rows },
      );
      C.zoneView = z.createView();
    }
    if (hdr) {
      // the input in the tone map, once for the three that run inside it
      const pl = pipeline('srtm', SRTM(false), format, plOne);
      run([{ pipeline: pl, group: bind(pl, [C.inTex.createView()]), view: C.tmIn.createView() }]);
    }
    if (C.linIn) {
      // and in linear light, once, for MWSR's reduction (untimed, as the client's pass 0)
      const pl = pipeline('lin', LIN, format, plOne);
      run([{ pipeline: pl, group: bind(pl, [C.inTex.createView()]), view: C.linIn.createView() }]);
    }
  }

  /** The passes of an upscaler reading `src`, the last one into `dst`. */
  function passes(id, half, { c444 = false } = {}, src, dst) {
    const f = C.format;
    const sv = src.createView();
    const dv = dst.createView();
    switch (id) {
      case 'mwsrz': {
        // without zones (a reduction, under x1.3, HDR, no native frame), the law: MWSR's own passes
        if (!C.zoneView) return passes('mwsr', half, { c444 }, src, dst);
        const constants = c444 ? { CHROMA444: 1 } : {};
        const pl = pipeline(
          'mwsrz' + (half ? 'h' : '') + (c444 ? '444' : ''),
          mwsrWgsl({ feed: 'tex', half, mode: 'up', map: true }),
          f,
          plMap,
          ['vs', 'fs'],
          constants,
        );
        return [{ pipeline: pl, group: bind(pl, [sampler, sv, { buffer: C.u.mwsr }, C.zoneView]), view: dv }];
      }
      case 'mwsr': {
        const r = C.reg;
        if (r.mode === 'down') {
          // SDR through the `-srgb` views, HDR from the linear light (setCase)
          const fv = C.hdr ? f : f + '-srgb';
          const pl = pipeline(
            `mwsrdown${r.taps}:${r.rDown}:${r.linearLight}`,
            mwsrWgsl({ feed: 'tex', mode: 'down', taps: r.taps }),
            fv,
            plTex,
            ['vs', 'fs'],
            { R_DOWN: r.rDown, LINEAR_LIGHT: r.linearLight },
          );
          const from = C.hdr ? C.linIn.createView() : src.createView({ format: fv });
          return [
            {
              pipeline: pl,
              group: bind(pl, [sampler, from, { buffer: C.u.mwsr }]),
              view: dst.createView({ format: fv }),
            },
          ];
        }
        const constants = c444 ? { CHROMA444: 1 } : {};
        const pl = pipeline(
          'mwsr' + (half ? 'h' : '') + (c444 ? '444' : ''),
          mwsrWgsl({ feed: 'tex', half, mode: 'up' }),
          f,
          plTex,
          ['vs', 'fs'],
          constants,
        );
        return [{ pipeline: pl, group: bind(pl, [sampler, sv, { buffer: C.u.mwsr }]), view: dv }];
      }
      case 'fsr1': {
        const e = pipeline('easu' + (half ? 'h' : ''), half ? K.EASU_H_WGSL : K.EASU_WGSL, f, 'auto', [
          'vs_main',
          'fs_main',
        ]);
        const r = pipeline('rcas' + (half ? 'h' : ''), half ? K.RCAS_H_WGSL : K.RCAS_WGSL, f, 'auto', [
          'vs_main',
          'fs_main',
        ]);
        const iv = C.interm.createView();
        return [
          { pipeline: e, group: bind(e, [sv, sampler, { buffer: C.u.easu }]), view: iv },
          { pipeline: r, group: bind(r, [iv, sampler, { buffer: C.u.rcas }]), view: dv },
        ];
      }
      case 'nis': {
        const pl = pipeline('nis', K.NIS_WGSL, f, plNis);
        return [{ pipeline: pl, group: bind(pl, [sampler, sv, nisCoef.createView(), { buffer: C.u.nis }]), view: dv }];
      }
      case 'sgsr1': {
        const pl = pipeline('sgsr' + (half ? 'h' : ''), half ? K.SGSR_H_WGSL : K.SGSR_WGSL, f);
        return [{ pipeline: pl, group: bind(pl, [sampler, sv, { buffer: C.u.sgsr }]), view: dv }];
      }
      case 'bilinear': {
        const pl = pipeline('bilinear', BILINEAR, f, plSamp);
        return [{ pipeline: pl, group: bind(pl, [sampler, sv]), view: dv }];
      }
    }
    throw new Error('kernel ' + id + '?');
  }

  /** The passes that draw upscaler `id` into `dst`, the tone map around it on HDR. */
  function chain(id, half, opts, dst) {
    const k = KERNELS.find((x) => x.id === id);
    if (!(C.hdr && k.wrap)) return passes(id, half, opts, C.inTex, dst);
    const inv = pipeline('srtminv', SRTM(true), C.format, plOne);
    return [
      ...passes(id, half, opts, C.tmIn, C.tmOut),
      { pipeline: inv, group: bind(inv, [C.tmOut.createView()]), view: dst.createView() },
    ];
  }

  function encode(enc, list, stamps = null) {
    list.forEach((p, i) => {
      const desc = {
        colorAttachments: [{ view: p.view, loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1] }],
      };
      if (stamps && stamps[i]) desc.timestampWrites = stamps[i];
      const pass = enc.beginRenderPass(desc);
      pass.setPipeline(p.pipeline);
      pass.setBindGroup(0, p.group);
      pass.draw(3);
      pass.end();
    });
  }
  function run(list) {
    const enc = device.createCommandEncoder();
    encode(enc, list);
    device.queue.submit([enc.finish()]);
  }

  /** Draws upscaler `id` into its output texture (32-bit, or 16-bit into `scratch` for the gate). */
  async function draw(id, { half = false, c444 = false } = {}) {
    run(chain(id, half, { c444 }, half ? C.scratch : C.out[id]));
    await device.queue.onSubmittedWorkDone();
  }

  /** A texture's pixels, tightly packed: Uint8Array (SDR) or Uint16Array of half floats (HDR). */
  async function read(t) {
    const w = t.width;
    const h = t.height;
    const row = w * C.bpp;
    const stride = Math.ceil(row / 256) * 256;
    const buf = device.createBuffer({ size: stride * h, usage: B.COPY_DST | B.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: t }, { buffer: buf, bytesPerRow: stride }, { width: w, height: h });
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buf.getMappedRange());
    const out = new Uint8Array(row * h);
    for (let y = 0; y < h; y++) out.set(src.subarray(y * stride, y * stride + row), y * row);
    buf.unmap();
    buf.destroy();
    return C.hdr ? new Uint16Array(out.buffer) : out;
  }
  const readOut = (id) => read(C.out[id]);
  const readScratch = () => read(C.scratch);

  // ── timing ──
  const qs = hasTs ? device.createQuerySet({ type: 'timestamp', count: 2 }) : null;
  const qResolve = hasTs ? device.createBuffer({ size: 256, usage: B.QUERY_RESOLVE | B.COPY_SRC }) : null;
  const qRead = hasTs ? device.createBuffer({ size: 256, usage: B.MAP_READ | B.COPY_DST }) : null;

  /**
   * `iters` draws of upscaler `id` (its own passes, not the tone map around it,
   * into a scratch texture), in command buffers of `chunk` draws submitted
   * together. One timestamp at the start of the first pass, one at the end of
   * the last: the browser rounds timestamps (to 0.1 ms in Chrome), a batch of
   * 100 draws makes that a small share. Without timestamp-query, the wall time
   * from the submission to the completion.
   */
  async function time(id, half, opts, iters = 100, chunk = iters) {
    const src = C.hdr && KERNELS.find((x) => x.id === id).wrap ? C.tmIn : C.inTex;
    const list = passes(id, half, opts, src, C.scratch);
    const n = list.length;
    const cbs = [];
    for (let i0 = 0; i0 < iters; i0 += chunk) {
      const enc = device.createCommandEncoder();
      for (let i = i0; i < Math.min(iters, i0 + chunk); i++) {
        let stamps = null;
        if (hasTs && (i === 0 || i === iters - 1)) {
          stamps = new Array(n).fill(null);
          const first = i === 0 ? { beginningOfPassWriteIndex: 0 } : {};
          const last = i === iters - 1 ? { endOfPassWriteIndex: 1 } : {};
          if (n === 1) stamps[0] = { querySet: qs, ...first, ...last };
          else {
            if (i === 0) stamps[0] = { querySet: qs, ...first };
            if (i === iters - 1) stamps[n - 1] = { querySet: qs, ...last };
          }
        }
        encode(enc, list, stamps);
      }
      if (hasTs && i0 + chunk >= iters) {
        enc.resolveQuerySet(qs, 0, 2, qResolve, 0);
        enc.copyBufferToBuffer(qResolve, 0, qRead, 0, 16);
      }
      cbs.push(enc.finish());
    }
    const t0 = performance.now();
    device.queue.submit(cbs);
    await device.queue.onSubmittedWorkDone();
    const wall = (performance.now() - t0) / iters;
    let gpu = null;
    if (hasTs) {
      await qRead.mapAsync(GPUMapMode.READ, 0, 16);
      const q = new BigInt64Array(qRead.getMappedRange(0, 16));
      const span = Number(q[1] - q[0]) / 1e6;
      qRead.unmap();
      if (span > 0) gpu = span / iters;
    }
    return { gpu, wall };
  }

  // ── panes ──
  const viewPl = new Map();
  const VIEW_FLOATS = 12;
  /** A canvas showing output textures; `hdr` asks for the float canvas with extended tone mapping. */
  function pane(canvas, hdr) {
    const ctx = canvas.getContext('webgpu');
    let format = navigator.gpu.getPreferredCanvasFormat();
    let shown = 'sdr';
    if (hdr) {
      try {
        ctx.configure({
          device,
          format: 'rgba16float',
          colorSpace: 'srgb',
          toneMapping: { mode: 'extended' },
          alphaMode: 'opaque',
        });
        format = 'rgba16float';
        const cfg = ctx.getConfiguration ? ctx.getConfiguration() : null;
        shown = cfg && cfg.toneMapping && cfg.toneMapping.mode === 'extended' ? 'extended' : 'clipped';
      } catch {
        shown = 'clipped';
      }
    }
    if (format !== 'rgba16float') ctx.configure({ device, format, alphaMode: 'opaque' });
    if (!viewPl.has(format)) viewPl.set(format, pipeline('view', VIEW, format, plView));
    const pl = viewPl.get(format);
    const u = device.createBuffer({ size: VIEW_FLOATS * 4, usage: B.UNIFORM | B.COPY_DST });
    let group = null;
    let bound = [null, null];
    return {
      shown,
      draw({ a, b = a, center, zoom, split = -1, line = 1 }) {
        if (!C || canvas.width < 2 || canvas.height < 2) return;
        const pick = (x) => (x === 'native' ? C.native : x === 'input' ? C.inTex : C.out[x]);
        const ta = pick(a);
        const tb = pick(b);
        if (!ta || !tb) return;
        if (ta !== bound[0] || tb !== bound[1]) {
          group = bind(pl, [ta.createView(), tb.createView(), { buffer: u }]);
          bound = [ta, tb];
        }
        // the input pane shows the input at its own scale: its texels are larger
        const s = a === 'input' ? C.inW / C.outW : 1;
        const sz = a === 'input' ? [C.inW, C.inH] : [C.outW, C.outH];
        device.queue.writeBuffer(
          u,
          0,
          new Float32Array([
            canvas.width,
            canvas.height,
            center[0] * s,
            center[1] * s,
            sz[0],
            sz[1],
            zoom / s,
            split,
            line,
            0,
            0,
            0,
          ]),
        );
        const enc = device.createCommandEncoder();
        encode(enc, [{ pipeline: pl, group, view: ctx.getCurrentTexture().createView() }]);
        device.queue.submit([enc.finish()]);
      },
      destroy() {
        u.destroy();
        try {
          ctx.unconfigure();
        } catch {
          /* already gone */
        }
      },
    };
  }

  return {
    label,
    hasTs,
    hasF16,
    /** Whether `id` has a 16-bit text for this case (MWSR's reduction has none). */
    halfOf: (id) =>
      hasF16 && KERNELS.find((k) => k.id === id).half && !(id.startsWith('mwsr') && C.reg.mode === 'down'),
    hasZones: () => !!(C && C.zoneView),
    /** MWSR's mode for this case: 'up' or 'down'. */
    mwsrMode: () => C && C.reg.mode,
    setCase,
    draw,
    readOut,
    readScratch,
    time,
    pane,
    get case() {
      return C;
    },
    done: () => device.queue.onSubmittedWorkDone(),
  };
}
