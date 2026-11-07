// The MWSR comparison (website/mwsr/index.html): the settings, the panes, the
// scores and the timing. engine.js draws on the GPU, worker.js prepares the
// pictures and computes the scores; the kernels are the product's, written
// next to this file by scripts/website/build-site.mjs.
//
//   ?soft=1   SwiftShader or nothing (the tests: refuses any other adapter)

const ME = document.getElementById('mwsr-app');
const SOFT = new URLSearchParams(location.search).get('soft') === '1';
const $ = (id) => document.getElementById(id);

const ZA = [
  ['Interface text', 1656, 42],
  ['Columns', 1576, 152],
  ['Fence', 306, 602],
];
const PICS = {
  text: {
    kind: 'sdr',
    url: '/mwsr/img/text-1440.webp',
    spots: [
      ['Serif text', 960, 220],
      ['Centre', 1280, 720],
    ],
  },
  '0ad': { kind: 'sdr', url: '/mwsr/img/0ad-1440.webp', mb: 6, spots: ZA },
  gears: {
    kind: 'sdr',
    url: '/mwsr/img/gears-1440.webp',
    mb: 4,
    spots: [
      ['Power chain', 1584, 752],
      ['Saw', 1888, 330],
      ['Scoreboard', 832, 170],
    ],
  },
  junkshop: {
    kind: 'sdr',
    url: '/mwsr/img/junkshop-1440.webp',
    mb: 3,
    spots: [
      ['Retail sign', 1808, 300],
      ['Colander', 1600, 500],
      ['Robot', 784, 1104],
    ],
  },
  iso: {
    kind: 'iso',
    w: 2560,
    h: 1440,
    spots: [
      ['Code', 420, 110],
      ['Right column', 1700, 700],
    ],
  },
  'hdr-text': {
    kind: 'hdr-text',
    w: 2560,
    h: 1440,
    spots: [
      ['On 1000 nits', 330, 80],
      ['1000-nit text', 330, 560],
      ['Red on white', 330, 1040],
      ['Small text', 1900, 560],
    ],
  },
  'hdr-ramp': {
    kind: 'hdr-ramp',
    w: 2560,
    h: 1440,
    spots: [
      ['Steps', 1180, 720],
      ['Stripes', 640, 1010],
      ['Ramps', 1280, 290],
    ],
  },
  'hdr-game': { kind: 'hdr-game', url: '/mwsr/img/0ad-1440.webp', mb: 6, spots: ZA },
  file: { kind: 'file', spots: [] },
};
const NAMES = {
  native: 'Native',
  input: 'Input',
  mwsr: 'MWSR',
  fsr1: 'FSR1',
  nis: 'NIS',
  sgsr1: 'SGSR1',
  bilinear: 'Bilinear',
};
const NOTES = {
  native: 'the frame each upscaler tries to rebuild',
  input: 'what the upscalers read, each texel enlarged',
  nis: "MoonlightWeb's per-fragment port",
};
// A reduction scores against the picture reduced by Lanczos-3: no native frame, a reference
const reducing = () => C && C.outW < C.inW;
const nameOf = (id) => (id === 'native' && reducing() ? 'Reference' : NAMES[id]);
const noteOf = (id) =>
  id === 'native' && reducing()
    ? 'the picture reduced by Lanczos-3 in linear light'
    : id === 'input' && reducing()
      ? 'what the scalers read, at its own size'
      : NOTES[id];
// the scales and their keys in the link
const SCALES = [
  [0.5, '0.5'],
  [0.75, '0.75'],
  [1, '1'],
  [4 / 3, '1.33'],
  [2, '2'],
];

// The scores, in the order they come: the cheapest first, each shown as soon as it is known.
// [key in R, its name, its format, whether higher is closer]
const SCORES = [
  ['psnr', 'PSNR', (x) => x.toFixed(2), true],
  ['msssim', 'MS-SSIM', (x) => x.toFixed(4), true],
  ['ssim', 'SSIM', (x) => x.toFixed(4), true],
  ['vmaf', 'VMAF NEG', (x) => x.toFixed(1), true],
  ['flip', 'FLIP', (x) => x.toFixed(4), false],
  ['lpips', 'LPIPS', (x) => x.toFixed(4), false],
];
const SCORE_COLUMNS = ['lpips', 'vmaf', 'psnr', 'ssim', 'msssim', 'flip'];
const SCORE_OF = Object.fromEntries(SCORES.map((m) => [m[0], m]));
// ?soft=1&lab=1: the lab reads the native frame and each upscaler's picture (cmp_lab.py)
const LABMODE = SOFT && new URLSearchParams(location.search).get('lab') === '1';
let labRef = null;
// LPIPS' network on its own device, made when the first case wants it: null where it cannot run
let lpipsNet = null;
function lpipsReady() {
  lpipsNet ||= (async () => {
    const adapter = await navigator.gpu.requestAdapter(
      SOFT ? { forceFallbackAdapter: true } : { powerPreference: 'high-performance' },
    );
    if (!adapter) return null;
    const { maxStorageBufferBindingSize, maxBufferSize } = adapter.limits;
    const device = await adapter.requestDevice({ requiredLimits: { maxStorageBufferBindingSize, maxBufferSize } });
    const [{ createLpips }, weights] = await Promise.all([
      import(ME.dataset.lpips),
      fetch(ME.dataset.lpipsWeights).then((r) => r.arrayBuffer()),
    ]);
    return createLpips(device, new Float32Array(weights));
  })().catch((e) => {
    console.warn('LPIPS:', e);
    return null;
  });
  return lpipsNet;
}

// ── the state, and its mirror in the address (#pic=…&scale=…) ──
// what the page opens on, when its link holds no state
const HOME = {
  pic: 'junkshop',
  scale: 4 / 3,
  chroma: '420',
  c444: false,
  view: 'ab',
  a: 'native',
  b: 'mwsr',
  zoom: 1,
  center: [1610, 629], // in the picture's pixels, whatever the scale draws it at
};
const S = {
  ...HOME,
  role: 'native',
  flipped: false, // the flip view shows B
  split: 0.5,
  file: null,
};
// a link with a state (#pic=…), at the start or pasted later into this tab: one written earlier keeps its meaning
function fromHash() {
  const h = new URLSearchParams(location.hash.slice(1));
  if (![...h].length) {
    Object.assign(S, HOME);
    return;
  }
  const pic = h.get('pic');
  if (PICS[pic] && pic !== 'file') S.pic = pic;
  S.scale = (SCALES.find(([, k]) => k === h.get('scale')) || [4 / 3])[0];
  S.chroma = h.get('chroma') === '420' ? '420' : '444';
  S.c444 = h.get('c444') === '1' && S.chroma === '444';
  S.view = ['grid', 'ab', 'flip', 'wipe'].includes(h.get('view')) ? h.get('view') : 'grid';
  for (const k of ['a', 'b']) if (NAMES[h.get(k)] && h.get(k) !== 'input') S[k] = h.get(k);
  if (['1', '2', '4', '8'].includes(h.get('zoom'))) S.zoom = Number(h.get('zoom'));
  const x = Number(h.get('x'));
  const y = Number(h.get('y'));
  S.center = h.has('x') && Number.isFinite(x) && Number.isFinite(y) ? [x, y] : PICS[S.pic].spots[0].slice(1);
}
fromHash();
addEventListener('hashchange', () => {
  fromHash();
  syncControls();
  load();
});
function toHash() {
  const h = new URLSearchParams();
  if (S.pic !== 'file') h.set('pic', S.pic);
  h.set('scale', SCALES.find(([v]) => v === S.scale)[1]);
  if (S.chroma === '420') h.set('chroma', '420');
  if (S.c444) h.set('c444', '1');
  h.set('view', S.view);
  if (S.view !== 'grid') {
    h.set('a', S.a);
    h.set('b', S.b);
  }
  h.set('zoom', String(S.zoom));
  h.set('x', String(Math.round(S.center[0])));
  h.set('y', String(Math.round(S.center[1])));
  history.replaceState(null, '', '#' + h.toString());
}

function status(text, kind = '') {
  const s = $('status');
  s.textContent = text;
  s.className = 'msg' + (kind ? ' ' + kind : '');
}

// ── the workers ──
function startWorker() {
  const worker = new Worker(ME.dataset.worker, { type: 'module' });
  const pending = new Map();
  let seq = 0;
  worker.onmessage = (e) => {
    const p = pending.get(e.data.id);
    pending.delete(e.data.id);
    if (e.data.ok) p.resolve(e.data.result);
    else p.reject(new Error(e.data.error));
  };
  worker.onerror = (e) => status('The page’s worker failed: ' + (e.message || 'unknown error'), 'bad');
  return {
    call: (op, args, transfer = []) =>
      new Promise((resolve, reject) => {
        const id = ++seq;
        pending.set(id, { resolve, reject });
        worker.postMessage({ id, op, args }, transfer);
      }),
    end() {
      worker.terminate();
      for (const p of pending.values()) p.reject(new Error('ended'));
      pending.clear();
    },
  };
}
const { call } = startWorker();
// The scores of a case: their own worker, ended with the case. A score takes seconds on a phone, and
// in the worker that prepares the pictures the next picture would wait for it.
let scorer = null;
function endScorer() {
  if (scorer) scorer.end();
  scorer = null;
}

// ── the engine ──
let engine = null;
let KERNELS = [];
let C = null; // the case on screen: sizes, hdr, and the native pixels for the overview
const R = {}; // scores per upscaler
let token = 0;
let lostWhy = '';

async function boot() {
  status('Starting WebGPU…');
  if (!navigator.gpu) {
    status(
      'This page needs WebGPU, and this browser does not have it. It runs in Chrome or Edge, in Safari 26, and in Firefox 141 or later on Windows.',
      'bad',
    );
    return;
  }
  try {
    const [E, K, mwsr, nis] = await Promise.all(
      [ME.dataset.engine, ME.dataset.kernels, ME.dataset.mwsr, ME.dataset.nis].map((u) => import(u)),
    );
    KERNELS = E.KERNELS;
    engine = await E.createEngine({
      K,
      mwsr,
      nis,
      soft: SOFT,
      onLost: (m) => {
        lostWhy = m;
        status('The GPU device was lost (' + m + '). Reload the page to start again.', 'bad');
      },
    });
  } catch (e) {
    const m = String(e.message || e);
    status(
      m === 'noadapter'
        ? 'This browser has WebGPU but gave this page no GPU adapter (WebGPU may be turned off, or this GPU blocked).'
        : 'WebGPU could not start: ' + m,
      'bad',
    );
    return;
  }
  $('device').textContent =
    'Measured on: ' +
    engine.label +
    (engine.hasF16 ? ' · shader-f16: yes' : ' · shader-f16: no (32-bit only)') +
    (engine.hasTs ? ' · GPU timestamps: yes' : ' · GPU timestamps: no (wall time, which includes the submission)');
  for (const sel of [$('a'), $('b')])
    sel.innerHTML = ['native', ...KERNELS.map((k) => k.id)]
      .map((id) => `<option value="${id}">${NAMES[id]}</option>`)
      .join('');
  syncControls();
  if (LABMODE) labHook();
  await load();
}

// The lab's hook (?soft=1&lab=1 only): cmp_lab.py sets each case and reads its pictures, RGBA8.
function labHook() {
  const failed = () => $('status').classList.contains('bad');
  window.mwsrLab = {
    kernels: () => KERNELS.map((k) => k.id),
    // one case: picture, scale key, chroma, c444; resolves once it is drawn and scored, rejects on the
    // status line's first failure (a dead worker leaves load() waiting, its message there)
    async run(pic, scale, chroma, c444) {
      S.pic = pic;
      S.scale = SCALES.find(([, k]) => k === scale)[0];
      S.chroma = chroma;
      S.c444 = c444 && chroma === '444';
      S.center = PICS[pic].spots[0].slice(1);
      syncControls();
      let poll;
      const bad = new Promise((_, reject) => {
        poll = setInterval(() => failed() && reject(new Error($('status').textContent)), 500);
      });
      try {
        await Promise.race([load(), bad]);
      } finally {
        clearInterval(poll);
      }
      if (failed()) throw new Error($('status').textContent);
      return { outW: C.outW, outH: C.outH, inW: C.inW, inH: C.inH, hdr: C.hdr, native: !!labRef };
    },
    ref: () => labRef,
    // the 32-bit picture (the 16-bit gate draws into the scratch texture, never here)
    read: (id) => engine.readOut(id),
    // the overshoot this page measured on it (the three-GPU tables' column)
    overshoot: (id) => (R[id] && R[id].overshoot != null ? R[id].overshoot : null),
  };
}

// ── who has the GPU ──
// What the eye waits for comes first: the page's load, a picture after a change of menu, a pan. The table is built
// in what is left, one step at a time, and only on a GPU this page is not using: its draws would be in the
// timing's figures. The page sees its own use alone: what another tab or program asks of the GPU stays in them.
const QUIET_MS = 500;
let eyeCount = 0;
let eyeAt = 0;
function eyeFirst() {
  eyeCount++;
  eyeAt = performance.now();
}
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
/** Waits until the table may use the GPU. False once the work it is asked for no longer stands. */
async function gpuFree(alive) {
  if (LABMODE) return alive();
  while (alive()) {
    const page = document.readyState === 'complete' && document.visibilityState === 'visible';
    if (page && performance.now() - eyeAt >= QUIET_MS) return true;
    await sleep(100);
  }
  return false;
}
// scrolling is the compositor's work on the same GPU; the capture catches the table's own scroll too
addEventListener('scroll', eyeFirst, { passive: true, capture: true });
document.addEventListener('visibilitychange', eyeFirst);

// ── the settings ──
function radios(name) {
  return [...document.querySelectorAll(`input[name="${name}"]`)];
}
function syncControls() {
  for (const r of radios('pic')) r.checked = r.value === S.pic;
  for (const r of radios('role')) r.checked = r.value === S.role;
  for (const r of radios('scale')) r.checked = Number(r.value) === S.scale;
  for (const r of radios('chroma')) r.checked = r.value === S.chroma;
  for (const r of radios('view')) r.checked = r.value === S.view;
  for (const r of radios('zoom')) r.checked = Number(r.value) === S.zoom;
  $('c444').checked = S.c444 && S.chroma === '444';
  $('c444').disabled = S.chroma !== '444';
  $('role-set').hidden = S.pic !== 'file';
  // the two choices keep their place in the grid, unused: shown and hidden, they moved the panes
  for (const id of ['a', 'swap', 'b']) $(id).disabled = S.view === 'grid';
  $('wipe-slider').hidden = S.view !== 'wipe';
  $('a').value = S.a;
  $('b').value = S.b;
  $('split').value = String(S.split);
  $('spots').innerHTML = PICS[S.pic].spots
    .map(([n, x, y]) => `<button type="button" data-x="${x}" data-y="${y}">${n}</button>`)
    .join('');
  $('spots').parentElement.hidden = !PICS[S.pic].spots.length;
}
function onRadio(name, fn) {
  for (const r of radios(name)) r.addEventListener('change', () => r.checked && fn(r.value));
}
onRadio('pic', (v) => {
  S.pic = v;
  if (PICS[v].spots.length) S.center = PICS[v].spots[0].slice(1);
  syncControls();
  load();
});
onRadio('role', (v) => {
  S.role = v;
  load();
});
onRadio('scale', (v) => {
  S.scale = Number(v);
  load();
});
onRadio('chroma', (v) => {
  S.chroma = v;
  if (v !== '444') S.c444 = false;
  syncControls();
  load();
});
onRadio('view', (v) => {
  S.view = v;
  syncControls();
  buildPanes();
});
onRadio('zoom', (v) => {
  S.zoom = Number(v);
  redraw();
  toHash();
});
// the tip opens on focus: a tap gives it, Safari on an iPhone included
$('c444-tip').addEventListener('click', (e) => e.currentTarget.focus());
$('c444').addEventListener('change', () => {
  S.c444 = $('c444').checked;
  load();
});
for (const k of ['a', 'b'])
  $(k).addEventListener('change', () => {
    S[k] = $(k).value;
    buildPanes();
  });
$('swap').addEventListener('click', () => {
  [S.a, S.b] = [S.b, S.a];
  syncControls();
  buildPanes();
});
$('split').addEventListener('input', () => {
  S.split = Number($('split').value);
  redraw();
});
$('spots').addEventListener('click', (e) => {
  const b = e.target.closest('button');
  if (!b) return;
  S.center = [Number(b.dataset.x), Number(b.dataset.y)];
  clampCenter();
  redraw();
  toHash();
});

// your own picture: the picker, or a drop on the panes
// (a second click on it, once it is shown, picks another one)
document.querySelector('label.file').addEventListener('click', (e) => {
  if (!S.file || S.pic === 'file') {
    e.preventDefault();
    $('file').click();
  }
});
$('file').addEventListener('change', () => $('file').files[0] && useFile($('file').files[0]));
function useFile(file) {
  if (!/^image\//.test(file.type)) {
    status(file.name + ' is not a picture this browser can read.', 'bad');
    return;
  }
  S.file = file;
  S.pic = 'file';
  S.center = null; // its middle, once its size is known
  const r = radios('pic').find((x) => x.value === 'file');
  r.disabled = false;
  // the chip keeps its width (mwsr.css): a long name ends in an ellipsis, and shows whole on hover
  $('file-name').textContent = file.name;
  $('file-name').title = file.name;
  syncControls();
  load();
}
const viewer = $('viewer');
viewer.addEventListener('dragover', (e) => {
  if (![...e.dataTransfer.items].some((i) => i.kind === 'file')) return;
  e.preventDefault();
  $('drop').hidden = false;
});
viewer.addEventListener('dragleave', (e) => {
  if (!viewer.contains(e.relatedTarget)) $('drop').hidden = true;
});
viewer.addEventListener('drop', (e) => {
  e.preventDefault();
  $('drop').hidden = true;
  const f = e.dataTransfer.files[0];
  if (f) useFile(f);
});

// ── a case: the picture prepared, uploaded, drawn by every upscaler. Its table waits for the button ──
let caseNative = null; // the native frame's pixels while the case is on screen: what a measure scores against
async function load() {
  if (!engine) return;
  const my = ++token;
  eyeFirst();
  measured = false;
  stopMeasure();
  $('measure').disabled = true;
  const p = PICS[S.pic];
  const pic =
    p.kind === 'file' ? { kind: 'file', file: S.file } : { ...p, url: p.url && new URL(p.url, location.href).href };
  status((p.mb ? `Loading the picture (${p.mb} MB the first time)` : 'Preparing the picture') + '…');
  for (const k of Object.keys(R)) delete R[k];
  timedTexts.clear();
  caseNative = null;
  // no figure of the last case next to the next one
  if (C) {
    renderTable();
    captions();
  }
  let c;
  try {
    c = await call('prepare', {
      pic,
      scale: S.scale,
      chroma: S.chroma,
      role: S.pic === 'file' ? S.role : 'native',
    });
  } catch (e) {
    if (my === token) status('The picture could not be prepared: ' + e.message, 'bad');
    return;
  }
  if (my !== token) return;
  try {
    engine.setCase(c);
  } catch (e) {
    status('The GPU refused the picture: ' + e.message, 'bad');
    return;
  }
  C = c;
  // a picture of one's own: its sizes, under the panes
  const fn = $('file-note');
  fn.hidden = S.pic !== 'file';
  fn.textContent =
    S.pic !== 'file'
      ? ''
      : `Drawn from ${c.inW}×${c.inH} to ${c.outW}×${c.outH}.` +
        (c.reduced ? ` Your picture (${c.reduced[0]}×${c.reduced[1]}) was first reduced to fit 3840×2160.` : '');
  clampCenter();
  await makePreview();
  if (my !== token) return;
  // the pixels are on the GPU and in the worker now: keep only whether there is a native frame
  if (LABMODE) labRef = C.native;
  caseNative = C.native;
  C.native = !!C.native;
  C.input = null;
  status('Drawing…');
  for (const k of KERNELS) await engine.draw(k.id, { c444: S.c444 });
  if (my !== token) return;
  buildPanes();
  renderTable();
  toHash();
  // the lab reads each case's overshoots with its pictures (cmp_lab.py)
  if (LABMODE) {
    status('Scoring…');
    try {
      await score(
        () => my === token,
        () => {},
      );
    } catch (e) {
      if (my === token) status('The scoring failed: ' + e.message, 'bad');
      return;
    }
    if (my !== token) return;
  }
  $('measure').disabled = false;
  status('');
}

const known = (id, key) => R[id] != null && R[id][key] != null;

// The scores of SDR pictures with a native frame, one at a time, each on the page as soon as it is known.
// A measure takes only those the table lacks: a measure stopped halfway goes on from there.
async function liveScores(alive, tick) {
  const urls = {
    scores: new URL(ME.dataset.scores, location.href).href,
    vmaf: new URL(ME.dataset.vmaf, location.href).href,
    model: new URL(ME.dataset.vmafModel, location.href).href,
  };
  const plain = SCORES.filter(([metric]) => metric !== 'lpips');
  const lacking = KERNELS.filter((k) => plain.some(([metric]) => !known(k.id, metric)));
  if (lacking.length) {
    if (!(await gpuFree(alive))) return;
    scorer = startWorker();
    await scorer.call('scoreCase', { w: C.outW, h: C.outH, native: caseNative });
    for (const k of lacking) {
      if (!(await gpuFree(alive))) return;
      const out = await engine.readOut(k.id);
      if (!alive()) return;
      await scorer.call('scoreOut', { id: k.id, out }, [out.buffer]);
    }
    for (const [metric, name] of plain) {
      for (const k of KERNELS) {
        if (known(k.id, metric)) continue;
        if (!(await gpuFree(alive))) return;
        status(`Scoring: ${name}, ${k.name}…`);
        const { value } = await scorer.call('scoreOne', { metric, id: k.id, urls });
        if (!alive()) return;
        R[k.id] = { ...R[k.id], [metric]: value };
        renderTable();
        tick();
      }
    }
    endScorer();
  }
  if (lpipsOff || KERNELS.every((k) => known(k.id, 'lpips'))) return;
  if (!(await gpuFree(alive))) return;
  status('Scoring: LPIPS, loading its network the first time…');
  const net = await lpipsReady();
  if (!alive()) return;
  if (!net) {
    lpipsOff = true;
    renderTable();
    return;
  }
  // the network waits between two submissions, in place: what it reckons has no clock in it
  const hold = () => gpuFree(alive);
  status('Scoring: LPIPS, the native frame…');
  if (!(await net.reference(caseNative, C.outW, C.outH, hold))) return;
  for (const k of KERNELS) {
    if (known(k.id, 'lpips')) continue;
    if (!(await gpuFree(alive))) return;
    status(`Scoring: LPIPS, ${k.name}…`);
    const out = await engine.readOut(k.id);
    if (!alive()) return;
    const scored = await net.score(out, hold);
    if (!scored || !alive()) return;
    const { lpips } = scored;
    R[k.id] = { ...R[k.id], lpips };
    renderTable();
    captions();
    tick();
  }
}
let lpipsOff = false;

// The overshoot, and the 16-bit gate, which says which precisions the GPU time may run.
async function score(alive, tick) {
  for (const { id } of KERNELS) {
    if (known(id, 'overshoot')) continue;
    if (!(await gpuFree(alive))) return;
    const out = await engine.readOut(id);
    if (!alive()) return;
    const m = await call('measure', { id, out }, [out.buffer]);
    if (!alive()) return;
    R[id] = { ...R[id], ...m };
    renderTable();
    captions();
    tick();
  }
  // the 16-bit gate: each 16-bit text held to its 32-bit picture on this GPU
  if (!engine.hasF16) return;
  for (const k of KERNELS.filter((x) => engine.halfOf(x.id) && !known(x.id, 'f16'))) {
    if (!(await gpuFree(alive))) return;
    await engine.draw(k.id, { half: true, c444: S.c444 });
    const a = await engine.readOut(k.id);
    const b = await engine.readScratch();
    if (!alive()) return;
    const g = await call('gate', { a, b }, [a.buffer, b.buffer]);
    if (!alive()) return;
    R[k.id] = { ...R[k.id], f16: g.share <= 0.001 ? 'ok' : 'refused', f16share: g.share };
    renderTable();
    tick();
  }
}

// ── the panes ──
let panes = [];
const ro = new ResizeObserver((entries) => {
  for (const e of entries) {
    const dp = e.devicePixelContentBoxSize && e.devicePixelContentBoxSize[0];
    const dpr = devicePixelRatio || 1;
    e.target.width = Math.max(2, dp ? dp.inlineSize : Math.round(e.contentRect.width * dpr));
    e.target.height = Math.max(2, dp ? dp.blockSize : Math.round(e.contentRect.height * dpr));
  }
  redraw();
});
function buildPanes() {
  if (!C) return;
  eyeFirst();
  for (const p of panes) {
    ro.unobserve(p.canvas);
    p.gpu.destroy();
  }
  const box = $('panes');
  box.innerHTML = '';
  for (const sel of [$('a'), $('b')]) sel.options[0].textContent = nameOf('native');
  const first = C.native ? 'native' : 'input';
  const pick = (x) => (x === 'native' && !C.native ? 'input' : x);
  let defs;
  if (S.view === 'grid') defs = [first, ...KERNELS.map((k) => k.id)].map((a) => ({ a }));
  else if (S.view === 'ab') defs = [{ a: pick(S.a) }, { a: pick(S.b) }];
  else if (S.view === 'flip') defs = [{ a: pick(S.a), b: pick(S.b), flip: true }];
  else defs = [{ a: pick(S.a), b: pick(S.b), wipe: true }];
  box.className = 'panes' + (S.view === 'ab' ? ' two' : S.view === 'grid' ? '' : ' one');
  panes = defs.map((d) => {
    const fig = document.createElement('figure');
    fig.className = 'pane';
    const canvas = document.createElement('canvas');
    const cap = document.createElement('figcaption');
    fig.append(canvas, cap);
    let side = null;
    if (d.wipe) {
      fig.insertAdjacentHTML(
        'beforeend',
        `<span class="side l">${nameOf(d.a)}</span><span class="side r">${nameOf(d.b)}</span>`,
      );
      canvas.setAttribute('aria-label', nameOf(d.a) + ' on the left, ' + nameOf(d.b) + ' on the right');
    } else if (d.flip) {
      side = document.createElement('span');
      side.className = 'side l';
      fig.append(side);
      canvas.tabIndex = 0;
      canvas.addEventListener('keydown', (e) => {
        if (e.key !== ' ' && e.key !== 'Enter') return;
        e.preventDefault();
        flip();
      });
    } else canvas.setAttribute('aria-label', nameOf(d.a));
    box.append(fig);
    const p = { ...d, canvas, cap, side, gpu: engine.pane(canvas, C.hdr) };
    ro.observe(canvas);
    pan(canvas, d.flip ? flip : null);
    return p;
  });
  captions();
  hdrNote();
  toHash();
}
// the flip view: the one its pane shows, and the other
const flipped = (p) => (S.flipped ? [p.b, p.a] : [p.a, p.b]);
const TAP = matchMedia('(pointer: coarse)').matches ? 'tap' : 'click';
function flip() {
  S.flipped = !S.flipped;
  captions();
  redraw();
}
function captions() {
  for (const p of panes) {
    if (p.flip) {
      const [on, off] = flipped(p);
      p.cap.innerHTML = `<b>${nameOf(on)}</b><span class="m">${line(on)}</span><span class="m next">${TAP}: ${nameOf(off)}</span>`;
      p.side.textContent = nameOf(on);
      p.canvas.setAttribute('aria-label', `${nameOf(on)}: ${TAP}, or press Space, for ${nameOf(off)}`);
      continue;
    }
    if (p.wipe) {
      p.cap.innerHTML = `<b>${nameOf(p.a)}</b><span class="m">${line(p.a)}</span><b>${nameOf(p.b)}</b><span class="m">${line(p.b)}</span>`;
      continue;
    }
    p.cap.innerHTML = `<b>${nameOf(p.a)}</b><span class="m">${line(p.a)}</span>`;
  }
}
// The figures come during a measure: their place is kept from the start (mwsr.css), or they would push the table
// and its button down, under a visitor's finger, by up to two lines per row of panes on a phone.
const WIDEST_FIGURES = 'LPIPS 0.0000 · < 0.00 % over · 0.000 ms';
function line(id) {
  if (id === 'native' || id === 'input') return noteOf(id);
  const r = R[id] || {};
  const bits = [];
  if (r.lpips != null) bits.push('LPIPS ' + r.lpips.toFixed(4));
  if (r.overshoot != null) bits.push(pct(r.overshoot) + ' over');
  const t = best(id);
  if (t) bits.push(ms3(t.ms) + ' ms');
  return `<span class="figs"><span class="widest">${WIDEST_FIGURES}</span><span>${bits.join(' · ')}</span></span>`;
}
let queued = false;
function redraw() {
  eyeFirst();
  if (queued) return;
  queued = true;
  requestAnimationFrame(() => {
    queued = false;
    const dpr = devicePixelRatio || 1;
    const z = Math.max(1, Math.round(S.zoom * dpr));
    for (const p of panes) {
      const a = p.flip ? flipped(p)[0] : p.a;
      p.gpu.draw({
        a,
        b: p.wipe ? p.b : a,
        center: outCenter(),
        zoom: z,
        split: p.wipe ? Math.round(S.split * p.canvas.width) + 0.5 : -1,
        line: Math.max(1, Math.round(dpr)) + 0.5,
      });
    }
    drawOverview();
  });
}
// The centre is kept in the picture's pixels (the places and the link stay
// right at every scale); the panes and the small picture take the output's.
const kx = () => C.outW / C.picW;
const ky = () => C.outH / C.picH;
const outCenter = () => [S.center[0] * kx(), S.center[1] * ky()];
function clampCenter() {
  if (!C) return;
  if (!S.center) S.center = [C.picW / 2, C.picH / 2];
  S.center = [Math.min(Math.max(S.center[0], 0), C.picW), Math.min(Math.max(S.center[1], 0), C.picH)];
}
// A drag moves the view; with `onTap`, a click that hardly moves (a few CSS
// pixels: a finger shakes) calls it instead.
function pan(canvas, onTap = null) {
  let last = null;
  let moved = 0;
  canvas.addEventListener('pointerdown', (e) => {
    canvas.setPointerCapture(e.pointerId);
    last = [e.clientX, e.clientY];
    moved = 0;
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!last) return;
    const dpr = devicePixelRatio || 1;
    const z = Math.max(1, Math.round(S.zoom * dpr));
    moved += Math.abs(e.clientX - last[0]) + Math.abs(e.clientY - last[1]);
    S.center = [
      S.center[0] - ((e.clientX - last[0]) * dpr) / z / kx(),
      S.center[1] - ((e.clientY - last[1]) * dpr) / z / ky(),
    ];
    last = [e.clientX, e.clientY];
    clampCenter();
    redraw();
  });
  const end = (e) => {
    if (last) {
      toHash();
      if (onTap && e.type === 'pointerup' && moved < 6) onTap();
    }
    last = null;
  };
  canvas.addEventListener('pointerup', end);
  canvas.addEventListener('pointercancel', end);
}
function hdrNote() {
  const n = $('hdr-note');
  if (!C || !C.hdr) {
    n.hidden = true;
    return;
  }
  const shown = panes[0] && panes[0].gpu.shown;
  const high = matchMedia('(dynamic-range: high)').matches;
  n.hidden = false;
  n.className = 'msg' + (shown === 'extended' && high ? '' : ' warn');
  n.textContent =
    shown === 'extended' && high
      ? 'HDR on screen: what lies above SDR white is shown up to your screen’s headroom.'
      : shown === 'extended'
        ? 'This browser shows HDR, but this screen is SDR (or its HDR is off): what lies above SDR white is clipped on screen. The scores are computed on the floats all the same.'
        : 'This browser shows no HDR on a WebGPU canvas (toneMapping “extended”): what lies above SDR white is clipped on screen. The scores are computed on the floats all the same.';
}

// ── the overview: the whole picture, and where the panes look ──
let preview = null;
const HALF = (() => {
  const t = new Float32Array(65536);
  for (let h = 0; h < 65536; h++) {
    const e = (h >> 10) & 0x1f;
    const m = h & 0x3ff;
    const v = e === 0 ? m * 2 ** -24 : e === 31 ? 65504 : (1 + m / 1024) * 2 ** (e - 15);
    t[h] = h & 0x8000 ? -v : v;
  }
  return t;
})();
async function makePreview() {
  const src = C.native || C.input;
  const w = C.native ? C.outW : C.inW;
  const h = C.native ? C.outH : C.inH;
  const pw = 480;
  const ph = Math.max(1, Math.round((pw * h) / w));
  const px = new Uint8ClampedArray(pw * ph * 4);
  for (let y = 0; y < ph; y++)
    for (let x = 0; x < pw; x++) {
      const i = (Math.floor(((y + 0.5) * h) / ph) * w + Math.floor(((x + 0.5) * w) / pw)) * 4;
      const o = (y * pw + x) * 4;
      for (let c = 0; c < 3; c++) px[o + c] = C.hdr ? Math.min(255, Math.max(0, HALF[src[i + c]] * 255)) : src[i + c];
      px[o + 3] = 255;
    }
  preview = await createImageBitmap(new ImageData(px, pw, ph));
}
function drawOverview() {
  const cv = $('overview');
  const dpr = devicePixelRatio || 1;
  const W = Math.round(cv.clientWidth * dpr);
  const H = Math.round(cv.clientHeight * dpr);
  if (cv.width !== W || cv.height !== H) {
    cv.width = W;
    cv.height = H;
  }
  const g = cv.getContext('2d');
  g.fillStyle = '#04070b';
  g.fillRect(0, 0, W, H);
  if (!preview || !C) return;
  const s = Math.min(W / C.outW, H / C.outH);
  const ox = (W - C.outW * s) / 2;
  const oy = (H - C.outH * s) / 2;
  g.imageSmoothingQuality = 'high';
  g.drawImage(preview, ox, oy, C.outW * s, C.outH * s);
  const p = panes[0];
  if (!p) return;
  const z = Math.max(1, Math.round(S.zoom * dpr));
  const vw = p.canvas.width / z;
  const vh = p.canvas.height / z;
  g.strokeStyle = '#19d3ff';
  g.lineWidth = Math.max(1, dpr);
  const [cx, cy] = outCenter();
  g.strokeRect(ox + (cx - vw / 2) * s, oy + (cy - vh / 2) * s, Math.max(2, vw * s), Math.max(2, vh * s));
}
{
  const cv = $('overview');
  let down = false;
  const jump = (e) => {
    if (!C) return;
    const r = cv.getBoundingClientRect();
    const s = Math.min(r.width / C.outW, r.height / C.outH);
    const ox = (r.width - C.outW * s) / 2;
    const oy = (r.height - C.outH * s) / 2;
    S.center = [(e.clientX - r.left - ox) / s / kx(), (e.clientY - r.top - oy) / s / ky()];
    clampCenter();
    redraw();
  };
  cv.addEventListener('pointerdown', (e) => {
    down = true;
    cv.setPointerCapture(e.pointerId);
    jump(e);
  });
  cv.addEventListener('pointermove', (e) => down && jump(e));
  cv.addEventListener('pointerup', () => {
    down = false;
    toHash();
  });
}

// ── the table ──
function pct(x) {
  if (x === 0) return '0 %';
  const p = 100 * x;
  if (p < 0.005) return '< 0.01 %';
  return (p < 0.1 ? p.toFixed(2) : p.toFixed(1)) + ' %';
}
// Within 2 % the two precisions are even: such a gap changes sides from one
// run to the next (0.435 against 0.439 ms on a Mac), so no winner is named.
const EVEN = 0.02;
function best(id) {
  const r = R[id];
  if (!r || r.t32 == null) return null;
  // MWSR shows its 32-bit text, the one the client runs: its 16-bit time goes with the report only
  if (id === 'mwsr' || r.t16 == null) return { ms: r.t32, tag: '' };
  const ms = Math.min(r.t16, r.t32);
  // what the row says under its name; nothing when the full precision is the faster
  const tag =
    Math.abs(r.t16 - r.t32) <= EVEN * Math.max(r.t16, r.t32) ? 'Half ≈ full' : r.t16 < r.t32 ? 'Half precision' : '';
  return { ms, tag, both: `32-bit ${ms3(r.t32)} ms, 16-bit ${ms3(r.t16)} ms` };
}
const ms3 = (ms) => ms.toFixed(ms < 1 ? 3 : 2);
function renderTable() {
  const times = KERNELS.map((k) => best(k.id)).filter(Boolean);
  const max = Math.max(...times.map((t) => t.ms), 0);
  // each score as soon as it is known, the best of its column in bold once all are in
  const scored = C && C.native && !C.hdr;
  const top = {};
  for (const key of SCORE_COLUMNS) {
    const v = KERNELS.map((k) => R[k.id] && R[k.id][key]).filter((x) => x != null);
    if (v.length === KERNELS.length) top[key] = SCORE_OF[key][3] ? Math.max(...v) : Math.min(...v);
  }
  scoreNote();
  // until the table is built, what builds it: over the table while it is empty, under the button once it shows a figure
  const waiting = C && !measuring && !measured;
  const shows = KERNELS.some((k) => best(k.id) || ['overshoot', ...SCORE_COLUMNS].some((key) => known(k.id, key)));
  $('press').classList.toggle('off', !waiting || shows);
  $('unfinished').hidden = !waiting || !shows;
  // a table begun goes on, a finished one starts again
  if (!measuring) $('measure').textContent = measured ? 'Measure again' : waiting && shows ? 'Continue' : 'Measure';
  // one line under every name where any row may need it: rows of one height, and nothing moves when a measure ends
  const lines = engine.hasF16 || engine.mwsrMode() === 'down';
  $('table').tBodies[0].innerHTML = KERNELS.map((k) => {
    const r = R[k.id] || {};
    const cells = SCORE_COLUMNS.map((key) => {
      const v = r[key];
      const none = !scored || (key === 'lpips' && lpipsOff);
      const text = v != null ? SCORE_OF[key][2](v) : none || !measuring ? '—' : '…';
      // the best as it reads: two figures that read the same are both in bold
      const isTop = v != null && top[key] != null && text === SCORE_OF[key][2](top[key]);
      return `<td class="num${key === 'lpips' ? ' fid' : ''}">${isTop ? `<strong>${text}</strong>` : text}</td>`;
    }).join('');
    const o = r.overshoot != null ? pct(r.overshoot) : measuring ? '…' : '—';
    const t = best(k.id);
    let time = measuring ? '…' : '—';
    if (t) {
      // a share of the longest bar, whose length fitTables() sets; during a measure each row's time comes as soon
      // as it is known, and the bars all come at its end, once the longest is known
      const w = max ? `max(2px, calc(var(--bar) * ${(t.ms / max).toFixed(4)}))` : '0px';
      time = `<span class="ms">${ms3(t.ms)}</span>` + (measuring ? '' : `<span class="bar" style="width:${w}"></span>`);
    }
    // under the name, what its time or its text is
    const reduces = k.id === 'mwsr' && engine.mwsrMode() === 'down';
    const refused = k.id !== 'mwsr' && r.f16 === 'refused';
    const said = reduces ? 'reduction' : refused ? 'Half refused' : t ? t.tag : '';
    const why = reduces
      ? 'MWSR’s reduction, as the client runs it: bilinear samples over each pixel’s footprint, averaged in linear light, lightly sharpened from ×0.5 to 1:1'
      : refused
        ? `Its 16-bit picture differs from its 32-bit one by more than 4/255 on ${pct(r.f16share)} of the pixels on this GPU: timed in 32-bit only`
        : t && t.both;
    const under = lines
      ? `<small class="subline"><span class="widest">Half precision</span><span${said && why ? ` title="${why}"` : ''}>${said}</span></small>`
      : '';
    return `<tr><th scope="row">${k.name}${under}</th>${cells}<td class="num">${o}</td><td class="num">${time}</td></tr>`;
  }).join('');
  placePress();
  fitTables();
}
// A table that scrolls sideways under the names, on a phone, ends with no column half under them: the room the last
// column needs (--tail) for the end to put a column's start right after the names. The scores' table ends on the
// overshoot and the time, both whole: its longest bar (--bar) shortens for them to fit, down to 30 px.
function fitTables() {
  for (const wrap of document.querySelectorAll('.table-wrap')) {
    const table = wrap.querySelector('table');
    const heads = table && table.tHead ? [...table.tHead.rows[0].cells] : [];
    if (heads.length < 3) continue;
    const tail = () => parseFloat(wrap.style.getPropertyValue('--tail')) || 0;
    const stick = heads[0].getBoundingClientRect().width;
    if (table.id === 'table') {
      const bar = parseFloat(table.style.getPropertyValue('--bar')) || 90;
      const lastTwo = heads.slice(-2).reduce((sum, th) => sum + th.getBoundingClientRect().width, 0) - tail();
      const fit = Math.min(90, Math.max(30, Math.floor(bar + wrap.clientWidth - stick - lastTwo)));
      if (fit !== bar) table.style.setProperty('--bar', `${fit}px`);
    }
    const box = table.getBoundingClientRect();
    const room = box.width - tail() - wrap.clientWidth;
    const at = room > 0.5 ? heads.slice(1).map((th) => th.getBoundingClientRect().left - box.left - stick).find((x) => x >= room - 0.5) : null;
    wrap.style.setProperty('--tail', `${at != null ? Math.max(0, at - room).toFixed(1) : 0}px`);
  }
}
{
  const resized = new ResizeObserver(() => fitTables());
  document.querySelectorAll('.table-wrap').forEach((w) => resized.observe(w));
  document.fonts.ready.then(fitTables);
}
// The badge lies over the table's figures, not over its titles: where they are depends on the page's width.
function placePress() {
  const body = $('table').tBodies[0];
  if (!body.rows.length) return;
  const box = $('press').parentElement.getBoundingClientRect();
  const rows = body.getBoundingClientRect();
  const { style } = $('press');
  style.left = `${body.rows[0].cells[0].getBoundingClientRect().right - box.left}px`;
  style.top = `${rows.top - box.top}px`;
  style.height = `${rows.height}px`;
}
new ResizeObserver(placePress).observe($('table'));
// what the score columns hold for this picture, or why they are empty
function scoreNote() {
  const n = $('lab-note');
  if (!n) return;
  const kind = PICS[S.pic].kind;
  n.textContent = kind.startsWith('hdr')
    ? 'No score but the overshoot in HDR: LPIPS, VMAF NEG, PSNR, SSIM, MS-SSIM and this FLIP are made for SDR pictures.'
    : C && !C.native
      ? 'No score against a native frame: your picture is the input here. The overshoot and the GPU time are measured.'
      : 'Every score is computed in this tab, on this device, against the native frame (below ×1, against the reference). They leave it only at the end of a measure.' +
        (lpipsOff ? ' LPIPS cannot run here: this device refused its network.' : '');
}

// ── the report of a measure ──
// At the end of a measure, one POST of technical values: the server keeps the fields it knows and nothing else.
// An unknown value is left out. No retry, no word to the visitor if it fails, and the count is in memory only.
const PAGE_VERSION = new URL(import.meta.url).searchParams.get('v');
const REPORTS = 3;
const REPORTS_MS = 5 * 60 * 1000;
const reported = [];
const num = (x) => (Number.isFinite(x) ? Number(x.toPrecision(6)) : undefined);
const text = (x, n = 300) => (x ? String(x).slice(0, n) : undefined);
/** The browser's family and the two major versions, worked out here: the user agent itself never leaves. */
async function clientFacts() {
  const ua = navigator.userAgent;
  const data = navigator.userAgentData;
  const major = (re) => {
    const m = ua.match(re);
    return m ? Number(m[1]) : undefined;
  };
  let browser = 'other';
  let browserMajor;
  if (/\bEdg(?:e|A|iOS)?\//.test(ua)) {
    browser = 'edge';
    browserMajor = major(/\bEdg(?:e|A|iOS)?\/(\d+)/);
  } else if (/\b(?:OPR|OPiOS|SamsungBrowser|Vivaldi|YaBrowser|DuckDuckGo)\//.test(ua)) browser = 'other';
  else if (/\b(?:CriOS|Chrome)\//.test(ua)) {
    browser = 'chrome';
    browserMajor = major(/\b(?:CriOS|Chrome)\/(\d+)/);
  } else if (/\b(?:FxiOS|Firefox)\//.test(ua)) {
    browser = 'firefox';
    browserMajor = major(/\b(?:FxiOS|Firefox)\/(\d+)/);
  } else if (/\bVersion\/\d+.*\bSafari\//.test(ua)) {
    browser = 'safari';
    browserMajor = major(/\bVersion\/(\d+)/);
  }
  const iPad = /Macintosh/.test(ua) && navigator.maxTouchPoints > 1;
  let os = 'other';
  let osMajor;
  if (/iPhone|iPad|iPod/.test(ua) || iPad) {
    os = 'ios';
    osMajor = major(/\bOS (\d+)_/);
  } else if (/Android/.test(ua)) {
    os = 'android';
    if (!data) osMajor = major(/Android (\d+)/); // Chrome's user agent freezes it, and states it below
  } else if (/Windows/.test(ua)) os = 'windows';
  else if (/Macintosh/.test(ua)) os = 'macos';
  else if (/Linux|X11/.test(ua) && !/CrOS/.test(ua)) os = 'linux';
  // Windows and macOS freeze theirs in the user agent: only what the browser states, reduced to its major
  if (data && data.getHighEntropyValues && ['windows', 'macos', 'android'].includes(os)) {
    const v = await data.getHighEntropyValues(['platformVersion']).catch(() => null);
    const m = v ? parseInt(v.platformVersion, 10) : NaN;
    if (Number.isFinite(m)) osMajor = os === 'windows' ? (m >= 13 ? 11 : 10) : m;
  }
  const mobile = data ? !!data.mobile : /Mobi|Android|iPhone|iPod/.test(ua) || iPad;
  return { browser, browserMajor, os, osMajor, mobile };
}
async function report(vars, fail, batches) {
  const now = performance.now();
  while (reported.length && now - reported[0] > REPORTS_MS) reported.shift();
  if (reported.length >= REPORTS) return;
  reported.push(now);
  try {
    const results = vars.map((v) => {
      const r = R[v.id] || {};
      const ms = median(v.t);
      return {
        k: v.id,
        half: v.half,
        // the scores are the 32-bit picture's
        ...(v.half
          ? {}
          : {
              lpips: num(r.lpips),
              vmafNeg: num(r.vmaf),
              psnr: num(r.psnr),
              ssim: num(r.ssim),
              msssim: num(r.msssim),
              flip: num(r.flip),
              overshoot: num(r.overshoot),
            }),
        gpuMs: num(ms),
        gpuSpread: v.t.length && ms ? num((Math.max(...v.t) - Math.min(...v.t)) / ms) : undefined,
        dropped: v.dropped,
      };
    });
    const body = {
      v: 1,
      page: PAGE_VERSION || undefined,
      shader: engine.mwsrKey(),
      ...(await clientFacts()),
      gpuVendor: text(engine.vendor, 64),
      gpuArch: text(engine.architecture, 64),
      f16: engine.hasF16,
      timestamps: engine.hasTs,
      // the batches each text was timed in
      batches,
      // the size of a visitor's own picture stays here
      ...(S.pic === 'file' ? {} : { canvasW: C.outW, canvasH: C.outH, inputW: C.inW, inputH: C.inH }),
      dpr: num(devicePixelRatio),
      gamut: ['rec2020', 'p3', 'srgb'].find((g) => matchMedia(`(color-gamut: ${g})`).matches),
      hdr: !!C.hdr,
      picture: S.pic === 'file' ? 'own' : S.pic,
      scale: (SCALES.find(([v]) => v === S.scale) || [])[1],
      chroma: S.chroma,
      c444: !!S.c444,
      path: 'webgpu',
      results,
      fail,
    };
    fetch('/v1/mwsr', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
      // No keepalive: the page is still open when a measure ends.
      credentials: 'omit',
      mode: 'same-origin',
      // Not 'no-referrer': a POST may then say Origin: null (WebKit), which the server refuses. No Referer either way.
      referrerPolicy: 'same-origin',
      referrer: '',
      cache: 'no-store',
    }).catch(() => {});
  } catch {
    /* a report never troubles the page */
  }
}

// ── the measure ──
// The whole table, on the button: what it lacks, its scores then its rows' GPU time. A batch the page's draws
// touched is dropped and taken again, warm.
let measuring = 0; // the run under way
let measured = false; // the table of this case is built
// the texts of the rows a stopped measure timed, with their batches: its Continue times only the others
const timedTexts = new Map();
let runs = 0;
let lastRun = Promise.resolve(); // two runs would share the timestamps' buffer
const timed = { dropped: 0 };
if (SOFT) window.mwsrTimed = timed;
// The GPU time's plan, the same at every measure: 3 batches of each text, so that the result comes soon. A batch
// is 100 draws, or about 0.1 s of GPU when that is fewer: a slow upscaler on a phone gives its time from 4 draws as
// well as from 100.
// How far it is, beside the button: the dots come and go in a place of their own and the percentage has its
// own width, so nothing moves. A change of text only: no draw of the page, and it holds no batch back.
let dotsTimer = 0;
function showMeasuring() {
  $('tprogress').innerHTML =
    '<span class="measuring">Measuring<span class="dots" aria-hidden="true"><i>.</i><i>.</i><i>.</i></span> <span class="pct">0 %</span></span>';
  const dots = [...$('tprogress').querySelectorAll('.dots i')];
  clearInterval(dotsTimer);
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
  let lit = 0;
  const step = () => {
    dots.forEach((d, i) => d.classList.toggle('off', i > lit));
    lit = (lit + 1) % 3;
  };
  step();
  dotsTimer = setInterval(step, 500);
}
function showPercent(p) {
  const el = $('tprogress').querySelector('.pct');
  const t = p + ' %';
  if (el && el.textContent !== t) el.textContent = t;
}
// what the button says once the run is over, renderTable() writes
function stopMeasure() {
  measuring = 0;
  endScorer();
  clearInterval(dotsTimer);
  $('tprogress').textContent = '';
}
function startMeasure() {
  const run = (measuring = ++runs);
  const my = token;
  // a finished table starts again from nothing: the overshoot, the 16-bit gate, the scores and the GPU time all
  // again. A table begun goes on (Continue) with what it lacks.
  if (measured) {
    for (const k of Object.keys(R)) delete R[k];
    timedTexts.clear();
  }
  measured = false;
  $('measure').textContent = 'Stop measuring';
  showMeasuring();
  renderTable();
  captions();
  lastRun = lastRun.then(() => measureCase(run, my)).catch((e) => console.warn('measure:', e));
}
$('measure').addEventListener('click', () => {
  if (measuring) {
    stopMeasure();
    if (!$('status').classList.contains('bad')) status('');
    renderTable();
    captions();
  } else if (engine && C) startMeasure();
});
function median(a) {
  if (!a.length) return undefined;
  const s = [...a].sort((x, y) => x - y);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}
async function measureCase(run, my) {
  const alive = () => measuring === run && token === my;
  if (!alive()) return;
  const rounds = 1;
  const batches = 3;
  // How far it is: every step counts one, and what is done never goes back. What the gate refuses leaves the plan.
  let done = 0;
  const scoring = caseNative && !C.hdr;
  let steps = 0;
  for (const k of KERNELS) {
    const half = engine.hasF16 && engine.halfOf(k.id);
    if (!known(k.id, 'overshoot')) steps++;
    if (half && !known(k.id, 'f16')) steps++;
    if (scoring) steps += SCORES.filter(([m]) => !known(k.id, m) && !(m === 'lpips' && lpipsOff)).length;
    if (!known(k.id, 't32'))
      steps += (1 + rounds * batches) * (half && (!known(k.id, 'f16') || R[k.id].f16 === 'ok') ? 2 : 1);
  }
  const progress = () => showPercent(Math.floor((100 * done) / steps));
  const tick = () => {
    done++;
    progress();
  };
  // what the GPU refused while this case was drawn, then during the scores and each batch
  let fail;
  const refusal = (step, id) => {
    const [msg] = engine.takeRefused();
    if (msg && !fail) fail = { step, shader: id, msg: text(msg), lost: text(lostWhy, 120), fallback: false };
  };
  refusal('draw');
  let scoresFailed = false;
  try {
    progress();
    await score(alive, tick);
    if (!alive()) return;
    if (scoring) await liveScores(alive, tick);
    if (!alive()) return;
    status('');
  } catch (e) {
    if (!alive()) return;
    // the GPU time does not depend on them: it still runs, and the report says what failed
    scoresFailed = true;
    endScorer();
    status('The scoring failed: ' + e.message, 'bad');
    fail ||= { step: 'score', msg: text(e.message), lost: text(lostWhy, 120), fallback: false };
  }
  refusal('score');
  // the rows a stopped measure timed keep their batches: only the others are timed
  const take = (id, half) => timedTexts.get(`${id}/${half}`) || { id, half, t: [], chunk: 10, iters: 100, dropped: 0 };
  const vars = [];
  for (const k of KERNELS) {
    vars.push(take(k.id, false));
    if (engine.halfOf(k.id) && R[k.id] && R[k.id].f16 === 'ok') vars.push(take(k.id, true));
  }
  const todo = vars.filter((v) => !timedTexts.has(`${v.id}/${v.half}`));
  const opts = { c444: S.c444 };
  const total = vars.length * rounds * batches;
  // the warm-ups count, one each; a batch dropped was not yet counted
  steps = done + todo.length * (1 + rounds * batches);
  let at = 'warmup';
  let current = null;
  // one batch, on a GPU this page is not using; null when the run is over
  const batch = async (v, iters) => {
    for (;;) {
      if (!(await gpuFree(alive))) return null;
      await engine.done();
      const seen = eyeCount;
      const touched = () => eyeCount !== seen || !alive();
      current = v.id;
      const t = await engine.time(v.id, v.half, opts, iters, Math.min(iters, v.chunk), touched);
      if (!alive()) return null;
      refusal(at, v.id);
      if (t && !touched()) return t;
      v.dropped++;
      timed.dropped++;
      if (!(await gpuFree(alive))) return null;
      await engine.time(v.id, v.half, opts, 10, Math.min(10, v.chunk));
      if (!alive()) return null;
    }
  };
  try {
    progress();
    // a short warm-up, which also sizes the pieces of a batch (about 100 ms of GPU each: the most the page waits)
    // and the batch, then about 0.1 s more of each upscaler, thrown away, for a GPU that starts cold
    for (const v of todo) {
      const w = await batch(v, 10);
      if (!w) return;
      const per = Math.max(0.01, w.gpu != null ? w.gpu : w.wall);
      v.chunk = Math.max(1, Math.min(100, Math.floor(100 / per)));
      v.iters = Math.max(4, Math.min(100, Math.round(100 / per)));
      if (!(await batch(v, Math.max(4, Math.round(100 / per))))) return;
      tick();
    }
    at = 'batch';
    for (let r = 0; r < rounds; r++) {
      for (const v of r % 2 ? [...todo].reverse() : todo) {
        for (let b = 0; b < batches; b++) {
          const t = await batch(v, v.iters);
          if (!t) return;
          v.t.push(t.gpu != null ? t.gpu : t.wall);
          tick();
        }
        // a row's time as soon as its upscaler's texts have their batches
        const mine = todo.filter((x) => x.id === v.id);
        if (mine.every((x) => x.t.length >= (r + 1) * batches)) {
          R[v.id] = R[v.id] || {};
          for (const x of mine) {
            R[x.id][x.half ? 't16' : 't32'] = median(x.t);
            timedTexts.set(`${x.id}/${x.half}`, x);
          }
          renderTable();
        }
      }
    }
  } catch (e) {
    if (!alive()) return;
    status('The timing failed: ' + e.message, 'bad');
    report(
      vars,
      fail || { step: at, shader: current, msg: text(e.message), lost: text(lostWhy, 120), fallback: false },
      rounds * batches,
    );
    stopMeasure();
    renderTable();
    captions();
    return;
  }
  measured = !scoresFailed;
  stopMeasure();
  $('tprogress').textContent =
    `${total} batches of up to 100 draws, median per draw` + (engine.hasTs ? '' : ' (wall time)');
  renderTable();
  captions();
  report(vars, fail, rounds * batches);
}

addEventListener('resize', redraw);
boot();
