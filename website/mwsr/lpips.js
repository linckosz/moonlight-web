// LPIPS (AlexNet, v0.1) in WebGPU, on the visitor's GPU: the network, the weights and the arithmetic of
// `lpips.LPIPS(net="alex")`: lpips 0.1.4's ScalingLayer, AlexNet's five ReLU taps, normalize_tensor, the
// squared difference, its 1x1 linear layers, the spatial mean, summed in layer order. In 32-bit floats.
// The picture's weight: lpips-alex.bin, AlexNet's convolutions and LPIPS' linear layers as raw f32.
//
//     const L = await createLpips(device, weights);   // weights: Float32Array of lpips-alex.bin
//     await L.reference(rgba, w, h);                  // the native frame's features, kept
//     const v = await L.score(rgba);                  // the distance of one picture to it
//     L.destroy();

const LAYERS = [
  { in: 3, out: 64, k: 11, stride: 4, pad: 2, pool: false },
  { in: 64, out: 192, k: 5, stride: 1, pad: 2, pool: true },
  { in: 192, out: 384, k: 3, stride: 1, pad: 1, pool: true },
  { in: 384, out: 256, k: 3, stride: 1, pad: 1, pool: false },
  { in: 256, out: 256, k: 3, stride: 1, pad: 1, pool: false },
];
const vecs = (c) => Math.ceil(c / 4);
const SHIFT = [-0.03, -0.088, -0.188].map(Math.fround);
const SCALE = [0.458, 0.448, 0.45].map(Math.fround);

// One output row at a time: x from the invocation, y from the push of `row`. Four output channels at once:
// each input vec4 against a mat4 of their weights, which is 16 multiplications for 5 reads.
const CONV = /* wgsl */ `
override K : i32;
override STRIDE : i32;
override PAD : i32;
override IN_V : i32;
override OUT_V : i32;
struct P { inW : i32, inH : i32, outW : i32, row : i32 };
@group(0) @binding(0) var<storage, read> src : array<vec4f>;
@group(0) @binding(1) var<storage, read> wts : array<mat4x4f>;
@group(0) @binding(2) var<storage, read> bias : array<vec4f>;
@group(0) @binding(3) var<storage, read_write> dst : array<vec4f>;
@group(0) @binding(4) var<uniform> p : P;
@compute @workgroup_size(32)
fn main(@builtin(global_invocation_id) g : vec3u) {
    let x = i32(g.x);
    let y = p.row + i32(g.y);
    if (x >= p.outW) { return; }
    let x0 = x * STRIDE - PAD;
    let y0 = y * STRIDE - PAD;
    let kx0 = max(0, -x0);
    let kx1 = min(K, p.inW - x0);
    let ky0 = max(0, -y0);
    let ky1 = min(K, p.inH - y0);
    let at = (y * p.outW + x) * OUT_V;
    for (var c = 0; c < OUT_V; c++) {
        var acc = bias[c];
        for (var ky = ky0; ky < ky1; ky++) {
            let sr = (y0 + ky) * p.inW + x0;
            let wr = ((c * K + ky) * K) * IN_V;
            for (var kx = kx0; kx < kx1; kx++) {
                let s = (sr + kx) * IN_V;
                let w = wr + kx * IN_V;
                for (var v = 0; v < IN_V; v++) {
                    acc += src[s + v] * wts[w + v];
                }
            }
        }
        dst[at + c] = max(acc, vec4f(0.0));
    }
}
`;

// torch's MaxPool2d(3, stride 2), no padding.
const POOL = /* wgsl */ `
override V : i32;
struct P { inW : i32, inH : i32, outW : i32, row : i32 };
@group(0) @binding(0) var<storage, read> src : array<vec4f>;
@group(0) @binding(3) var<storage, read_write> dst : array<vec4f>;
@group(0) @binding(4) var<uniform> p : P;
@compute @workgroup_size(32)
fn main(@builtin(global_invocation_id) g : vec3u) {
    let x = i32(g.x);
    let y = p.row + i32(g.y);
    if (x >= p.outW) { return; }
    for (var c = 0; c < V; c++) {
        var m = vec4f(-1e30);
        for (var j = 0; j < 3; j++) {
            for (var i = 0; i < 3; i++) {
                m = max(m, src[((y * 2 + j) * p.inW + x * 2 + i) * V + c]);
            }
        }
        dst[(y * p.outW + x) * V + c] = m;
    }
}
`;

// normalize_tensor: f / (sqrt(sum f^2) + 1e-10), kept for the reference.
const NORM = /* wgsl */ `
override V : i32;
struct P { inW : i32, inH : i32, outW : i32, row : i32 };
@group(0) @binding(0) var<storage, read> src : array<vec4f>;
@group(0) @binding(3) var<storage, read_write> dst : array<vec4f>;
@group(0) @binding(4) var<uniform> p : P;
@compute @workgroup_size(32)
fn main(@builtin(global_invocation_id) g : vec3u) {
    let x = i32(g.x);
    let y = p.row + i32(g.y);
    if (x >= p.outW) { return; }
    let at = (y * p.outW + x) * V;
    var sum = 0.0;
    for (var c = 0; c < V; c++) {
        let f = src[at + c];
        sum += dot(f, f);
    }
    let n = 1.0 / (sqrt(sum) + 1e-10);
    for (var c = 0; c < V; c++) {
        dst[at + c] = src[at + c] * n;
    }
}
`;

// One row's sum of lin . (f0 - f1)^2, the picture's features normalized as they are read.
const DIFF = /* wgsl */ `
override V : i32;
struct P { inW : i32, inH : i32, outW : i32, row : i32 };
@group(0) @binding(0) var<storage, read> src : array<vec4f>;
@group(0) @binding(1) var<storage, read> ref0 : array<vec4f>;
@group(0) @binding(2) var<storage, read> lin : array<vec4f>;
@group(0) @binding(3) var<storage, read_write> sums : array<f32>;
@group(0) @binding(4) var<uniform> p : P;
@compute @workgroup_size(16)
fn main(@builtin(global_invocation_id) g : vec3u) {
    let y = i32(g.x);
    if (y >= p.inH) { return; }
    var row = 0.0;
    for (var x = 0; x < p.inW; x++) {
        let at = (y * p.inW + x) * V;
        var sum = 0.0;
        for (var c = 0; c < V; c++) {
            let f = src[at + c];
            sum += dot(f, f);
        }
        let n = 1.0 / (sqrt(sum) + 1e-10);
        var d = 0.0;
        for (var c = 0; c < V; c++) {
            let e = ref0[at + c] - src[at + c] * n;
            d += dot(lin[c], e * e);
        }
        row += d;
    }
    sums[y] = row;
}
`;

export const LPIPS_WEIGHT_FLOATS = 2470848;

export async function createLpips(device, weights, { onProgress = null, budgetMs = 120 } = {}) {
  if (weights.length !== LPIPS_WEIGHT_FLOATS) throw new Error('lpips: weights ' + weights.length + '?');
  const B = GPUBufferUsage;
  const buffers = [];
  const storage = (size, usage = B.STORAGE) => {
    const b = device.createBuffer({ size, usage: usage | B.STORAGE });
    buffers.push(b);
    return b;
  };
  const filled = (data) => {
    const b = device.createBuffer({ size: data.byteLength, usage: B.STORAGE | B.COPY_DST, mappedAtCreation: true });
    new Float32Array(b.getMappedRange()).set(data);
    b.unmap();
    buffers.push(b);
    return b;
  };
  // The file: for each layer the weights (out, in, kh, kw), then the bias; then the linear layers.
  let o = 0;
  for (const L of LAYERS) {
    const iv = vecs(L.in);
    const ov = vecs(L.out);
    // (out vec4, ky, kx, in vec4): a mat4x4f whose columns are the four output channels
    const w = new Float32Array(ov * L.k * L.k * iv * 16);
    for (let c = 0; c < L.out; c++)
      for (let i = 0; i < L.in; i++)
        for (let ky = 0; ky < L.k; ky++)
          for (let kx = 0; kx < L.k; kx++) {
            const m = ((c >> 2) * L.k + ky) * L.k * iv + kx * iv + (i >> 2);
            w[m * 16 + (c & 3) * 4 + (i & 3)] = weights[o + (((c * L.in + i) * L.k + ky) * L.k + kx)];
          }
    o += L.out * L.in * L.k * L.k;
    const bias = new Float32Array(ov * 4);
    bias.set(weights.subarray(o, o + L.out));
    o += L.out;
    L.gpuW = filled(w);
    L.gpuB = filled(bias);
  }
  for (const L of LAYERS) {
    const lin = new Float32Array(vecs(L.out) * 4);
    lin.set(weights.subarray(o, o + L.out));
    o += L.out;
    L.gpuLin = filled(lin);
  }
  const module = (code) => device.createShaderModule({ code });
  const conv = module(CONV);
  const pool = module(POOL);
  const norm = module(NORM);
  const diff = module(DIFF);
  const pipe = (m, constants) => device.createComputePipelineAsync({ layout: 'auto', compute: { module: m, entryPoint: 'main', constants } });
  const pipes = await Promise.all(
    LAYERS.flatMap((L) => [
      pipe(conv, { K: L.k, STRIDE: L.stride, PAD: L.pad, IN_V: vecs(L.in), OUT_V: vecs(L.out) }),
      pipe(pool, { V: vecs(L.in) }),
      pipe(norm, { V: vecs(L.out) }),
      pipe(diff, { V: vecs(L.out) }),
    ]),
  );
  LAYERS.forEach((L, i) => {
    [L.pConv, L.pPool, L.pNorm, L.pDiff] = pipes.slice(i * 4, i * 4 + 4);
  });

  let shape = null;
  const uniform = device.createBuffer({ size: 16, usage: B.UNIFORM | B.COPY_DST });
  buffers.push(uniform);

  function allocate(w, h) {
    // Each layer's sizes; torch's floors.
    let W = w;
    let H = h;
    const layers = [];
    for (const L of LAYERS) {
      let iw = W;
      let ih = H;
      let poolW = 0;
      let poolH = 0;
      if (L.pool) {
        poolW = Math.floor((W - 3) / 2) + 1;
        poolH = Math.floor((H - 3) / 2) + 1;
        iw = poolW;
        ih = poolH;
      }
      const ow = Math.floor((iw + 2 * L.pad - L.k) / L.stride) + 1;
      const oh = Math.floor((ih + 2 * L.pad - L.k) / L.stride) + 1;
      layers.push({ L, inRawW: W, inRawH: H, poolW, poolH, iw, ih, ow, oh });
      W = ow;
      H = oh;
    }
    const act = layers.map((l) => storage(l.ow * l.oh * vecs(l.L.out) * 16));
    const pooled = layers.map((l) => (l.L.pool ? storage(l.poolW * l.poolH * vecs(l.L.in) * 16) : null));
    const ref = layers.map((l) => storage(l.ow * l.oh * vecs(l.L.out) * 16));
    const input = device.createBuffer({ size: w * h * 16, usage: B.STORAGE | B.COPY_DST });
    buffers.push(input);
    const maxRows = Math.max(...layers.map((l) => l.oh));
    const sums = storage(maxRows * 4, B.STORAGE | B.COPY_SRC);
    const read = device.createBuffer({ size: maxRows * 4, usage: B.COPY_DST | B.MAP_READ });
    buffers.push(read);
    shape = { w, h, layers, act, pooled, ref, input, sums, read };
  }

  // The rows of a pass, in submissions of about budgetMs each: no command buffer long enough for the system to
  // reset the GPU, and the page keeps its frames.
  let rowsPerMs = 0.02;
  // The page's say between two submissions: resolves once it may go on, false to drop the picture.
  let hold = null;
  const DROPPED = new Error('dropped');
  async function rows(pipeline, group, widthGroups, total, setRow) {
    for (let row = 0; row < total; ) {
      if (hold && !(await hold())) throw DROPPED;
      const n = Math.min(total - row, Math.max(1, Math.round(rowsPerMs * budgetMs)));
      await setRow(row);
      const enc = device.createCommandEncoder();
      const pass = enc.beginComputePass();
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, group);
      pass.dispatchWorkgroups(widthGroups, n);
      pass.end();
      const t0 = performance.now();
      device.queue.submit([enc.finish()]);
      await device.queue.onSubmittedWorkDone();
      const dt = Math.max(1, performance.now() - t0);
      rowsPerMs = 0.7 * rowsPerMs + 0.3 * (n / dt);
      row += n;
    }
  }
  // Each pass has its uniform written just before its submission, on the queue's order.
  const pGroup = (pipeline, s, wb, bb, d) =>
    device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: s } },
        ...(wb ? [{ binding: 1, resource: { buffer: wb } }] : []),
        ...(bb ? [{ binding: 2, resource: { buffer: bb } }] : []),
        { binding: 3, resource: { buffer: d } },
        { binding: 4, resource: { buffer: uniform } },
      ],
    });
  const setP = (inW, inH, outW, row) => device.queue.writeBuffer(uniform, 0, new Int32Array([inW, inH, outW, row]));

  // The network up to each activation, the picture in shape.input.
  async function forward(each) {
    const S = shape;
    let prev = S.input;
    for (let i = 0; i < S.layers.length; i++) {
      const l = S.layers[i];
      if (l.L.pool) {
        const g = pGroup(l.L.pPool, prev, null, null, S.pooled[i]);
        await rows(l.L.pPool, g, Math.ceil(l.poolW / 32), l.poolH, (row) => setP(l.inRawW, l.inRawH, l.poolW, row));
        prev = S.pooled[i];
      }
      const g = pGroup(l.L.pConv, prev, l.L.gpuW, l.L.gpuB, S.act[i]);
      await rows(l.L.pConv, g, Math.ceil(l.ow / 32), l.oh, (row) => setP(l.iw, l.ih, l.ow, row));
      prev = S.act[i];
      await each(i, l);
      if (onProgress) onProgress(i + 1, S.layers.length);
    }
  }

  function upload(rgba) {
    const { w, h } = shape;
    const f = new Float32Array(w * h * 4);
    const fr = Math.fround;
    for (let i = 0, p = 0, q = 0; i < w * h; i++, p += 4, q += 4) {
      for (let c = 0; c < 3; c++) {
        // lpips.im2tensor's arithmetic, in floats: v / 127.5 - 1, then the ScalingLayer
        f[q + c] = fr(fr(fr(fr(rgba[p + c] / 127.5) - 1) - SHIFT[c]) / SCALE[c]);
      }
    }
    device.queue.writeBuffer(shape.input, 0, f);
  }

  return {
    get sizes() {
      return shape;
    },
    /** @returns {Promise<boolean>} false when `pause` dropped it */
    async reference(rgba, w, h, pause = null) {
      hold = pause;
      if (!shape || shape.w !== w || shape.h !== h) {
        if (shape) for (const b of [...shape.act, ...shape.pooled, ...shape.ref, shape.input, shape.sums, shape.read]) b && b.destroy();
        allocate(w, h);
      }
      upload(rgba);
      try {
        await forward(async (i, l) => {
          const g = pGroup(l.L.pNorm, shape.act[i], null, null, shape.ref[i]);
          await rows(l.L.pNorm, g, Math.ceil(l.ow / 32), l.oh, (row) => setP(l.ow, l.oh, l.ow, row));
        });
      } catch (e) {
        if (e === DROPPED) return false;
        throw e;
      }
      return true;
    },
    /** @returns {Promise<?{lpips: number, layers: number[]}>} null when `pause` dropped it */
    async score(rgba, pause = null) {
      hold = pause;
      const per = [];
      upload(rgba);
      const each = async (i, l) => {
        const g = pGroup(l.L.pDiff, shape.act[i], shape.ref[i], l.L.gpuLin, shape.sums);
        setP(l.ow, l.oh, l.ow, 0);
        const enc = device.createCommandEncoder();
        const pass = enc.beginComputePass();
        pass.setPipeline(l.L.pDiff);
        pass.setBindGroup(0, g);
        pass.dispatchWorkgroups(Math.ceil(l.oh / 16));
        pass.end();
        enc.copyBufferToBuffer(shape.sums, 0, shape.read, 0, l.oh * 4);
        device.queue.submit([enc.finish()]);
        await shape.read.mapAsync(GPUMapMode.READ, 0, l.oh * 4);
        const s = new Float32Array(shape.read.getMappedRange(0, l.oh * 4));
        let sum = 0;
        for (let y = 0; y < l.oh; y++) sum += s[y];
        shape.read.unmap();
        per.push(sum / (l.ow * l.oh));
      };
      try {
        await forward(each);
      } catch (e) {
        if (e === DROPPED) return null;
        throw e;
      }
      let v = 0;
      for (const p of per) v += p;
      return { lpips: v, layers: per };
    },
    destroy() {
      for (const b of buffers) b.destroy();
    },
  };
}

/*
LPIPS' linear layers, in lpips-alex.bin:

Copyright (c) 2018, Richard Zhang, Phillip Isola, Alexei A. Efros, Eli Shechtman, Oliver Wang
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


AlexNet's weights, in lpips-alex.bin, are torchvision's alexnet-owt-7be5be79, under its licence and whatever terms the dataset they were trained on sets:

BSD 3-Clause License

Copyright (c) Soumith Chintala 2016, 
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
