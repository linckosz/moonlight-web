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
  '0ad': { kind: 'sdr', url: '/mwsr/img/0ad-1440.webp', spots: ZA },
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
  'hdr-game': { kind: 'hdr-game', url: '/mwsr/img/0ad-1440.webp', spots: ZA },
  file: { kind: 'file', spots: [] },
};
const NAMES = {
  native: 'Native',
  input: 'Input',
  mwsr: 'MWSR',
  mwsrz: 'MWSR zones',
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
// MWSR zones: MoonlightWeb's own, not in the MWSR anyone downloads (that one is the law, MWSR above)
const ZONES_ONLY =
  "MoonlightWeb only: its native host measures each zone of the native frame and lowers MWSR's sharpness there; the MWSR anyone downloads is the law, MWSR above";
const ZONES_NONE = 'the law here: zones from ×1.3 up, in SDR, from a native frame';
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

// ── the state, and its mirror in the address (#pic=…&scale=…) ──
const S = {
  pic: 'text',
  role: 'native',
  scale: 4 / 3,
  chroma: '444',
  c444: false,
  view: 'grid',
  a: 'mwsr',
  b: 'fsr1',
  flipped: false, // the flip view shows B
  zoom: matchMedia('(max-width: 560px)').matches ? 2 : 4, // a phone's panes are small
  center: [960, 220], // in the picture's pixels, whatever the scale draws it at
  split: 0.5,
  file: null,
};
// a link with a state (#pic=…), at the start or pasted later into this tab
function fromHash() {
  const h = new URLSearchParams(location.hash.slice(1));
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

// ── the worker ──
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
const call = (op, args, transfer = []) =>
  new Promise((resolve, reject) => {
    const id = ++seq;
    pending.set(id, { resolve, reject });
    worker.postMessage({ id, op, args }, transfer);
  });

// ── the engine ──
let engine = null;
let KERNELS = [];
let C = null; // the case on screen: sizes, hdr, and the native pixels for the overview
const R = {}; // scores per upscaler
let token = 0;

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
      onLost: (m) => status('The GPU device was lost (' + m + '). Reload the page to start again.', 'bad'),
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
      .map((id) => `<option value="${id}">${NAMES[id]}${id === 'mwsrz' ? ' (MoonlightWeb only)' : ''}</option>`)
      .join('');
  syncControls();
  await load();
}

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

// ── a case: the picture prepared, uploaded, drawn by every upscaler, scored ──
async function load() {
  if (!engine) return;
  const my = ++token;
  stopTiming();
  $('measure').disabled = true;
  const p = PICS[S.pic];
  const pic =
    p.kind === 'file' ? { kind: 'file', file: S.file } : { ...p, url: p.url && new URL(p.url, location.href).href };
  const big = p.kind === 'sdr' || p.kind === 'hdr-game' ? ' (6 MB the first time)' : '';
  status((S.pic === '0ad' || S.pic === 'hdr-game' ? 'Loading the picture' + big : 'Preparing the picture') + '…');
  for (const k of Object.keys(R)) delete R[k];
  let c;
  try {
    c = await call('prepare', {
      pic,
      scale: S.scale,
      chroma: S.chroma,
      role: S.pic === 'file' ? S.role : 'native',
      zoneUrls: { zones: new URL(ME.dataset.zones, location.href).href, tree: new URL(ME.dataset.zonetree, location.href).href },
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
  // the pixels are on the GPU and in the worker now: keep only whether there is a native frame
  C.native = !!C.native;
  C.input = null;
  C.zones = null;
  status('Drawing…');
  for (const k of KERNELS) await engine.draw(k.id, { c444: S.c444 });
  if (my !== token) return;
  buildPanes();
  renderTable();
  toHash();
  status('Scoring…');
  try {
    await score(my);
  } catch (e) {
    if (my === token) status('The scoring failed: ' + e.message, 'bad');
    return;
  }
  if (my !== token) return;
  // timed once the 16-bit gate has said which precisions may run
  $('measure').disabled = false;
  status('');
}

async function score(my, ids = KERNELS.map((k) => k.id)) {
  for (const id of ids) {
    const out = await engine.readOut(id);
    if (my !== token) return;
    const m = await call('measure', { out }, [out.buffer]);
    if (my !== token) return;
    R[id] = { ...R[id], ...m };
    renderTable();
    captions();
  }
  // the 16-bit gate: each 16-bit text held to its 32-bit picture on this GPU
  if (!engine.hasF16) return;
  for (const k of KERNELS.filter((x) => engine.halfOf(x.id) && ids.includes(x.id))) {
    await engine.draw(k.id, { half: true, c444: S.c444 });
    const a = await engine.readOut(k.id);
    const b = await engine.readScratch();
    if (my !== token) return;
    const g = await call('gate', { a, b }, [a.buffer, b.buffer]);
    if (my !== token) return;
    R[k.id] = { ...R[k.id], f16: g.share <= 0.001 ? 'ok' : 'refused', f16share: g.share };
    renderTable();
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
function line(id) {
  if (id === 'native' || id === 'input') return noteOf(id);
  const r = R[id];
  if (!r) return '…';
  const bits = id === 'mwsrz' && !engine.hasZones() ? [ZONES_NONE] : [];
  if (r.ssimY != null) bits.push(fid(r.ssimY));
  if (r.overshoot != null) bits.push(pct(r.overshoot) + ' over');
  const t = best(id);
  if (t) bits.push(ms3(t.ms) + ' ms');
  return bits.join(' · ');
}
let queued = false;
function redraw() {
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
const fid = (s) => (100 * s).toFixed(1);
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
  if (r.t16 == null) return { ms: r.t32, tag: '' };
  const ms = Math.min(r.t16, r.t32);
  const tag = Math.abs(r.t16 - r.t32) <= EVEN * Math.max(r.t16, r.t32) ? 'f16 ≈ f32' : r.t16 < r.t32 ? 'f16' : 'f32';
  return { ms, tag, both: `32-bit ${ms3(r.t32)} ms, 16-bit ${ms3(r.t16)} ms` };
}
const ms3 = (ms) => ms.toFixed(ms < 1 ? 3 : 2);
function renderTable() {
  const times = KERNELS.map((k) => best(k.id)).filter(Boolean);
  const max = Math.max(...times.map((t) => t.ms), 0);
  $('table').tBodies[0].innerHTML = KERNELS.map((k) => {
    const r = R[k.id] || {};
    const scored = C && C.native;
    const f = r.ssimY != null ? fid(r.ssimY) : scored ? '…' : '—';
    const col = r.ssimRgb != null ? fid(r.ssimRgb) : scored ? '…' : '—';
    const o = r.overshoot != null ? pct(r.overshoot) : '…';
    const t = best(k.id);
    let time = '—';
    if (t) {
      const w = max ? Math.max(2, Math.round((t.ms / max) * 90)) : 0;
      const tag = t.tag ? `<span class="tag"${t.both ? ` title="${t.both}"` : ''}>${t.tag}</span>` : '';
      time = `${ms3(t.ms)}${tag}<span class="bar" style="width:${w}px"></span>`;
    }
    let gate = '';
    if (r.f16 === 'refused')
      gate = `<span class="tag" title="Its 16-bit picture differs from its 32-bit one by more than 4/255 on ${pct(r.f16share)} of the pixels on this GPU: timed in 32-bit only">f16 refused</span>`;
    const star =
      k.id === 'nis'
        ? ' <small title="MoonlightWeb’s per-fragment port of NIS, not NVIDIA’s compute shader">port</small>'
        : k.id === 'mwsrz'
          ? ` <small title="${ZONES_ONLY}${engine.hasZones() ? '' : '. Here ' + ZONES_NONE}">MoonlightWeb</small>`
          : k.id === 'mwsr' && engine.mwsrMode() === 'down'
          ? ' <small title="MWSR’s reduction, as the client runs it: bilinear samples over each pixel’s footprint, averaged in linear light">reduction</small>'
          : '';
    return `<tr><th scope="row">${k.name}${star}</th><td class="num fid">${f}</td><td class="num">${col}</td><td class="num">${o}</td><td class="num">${time}${gate}</td></tr>`;
  }).join('');
}

// ── the timing ──
let timing = 0;
function stopTiming() {
  if (timing) timing = 0;
  $('measure').textContent = 'Measure the GPU time';
  $('tprogress').textContent = '';
}
$('measure').addEventListener('click', async () => {
  if (timing) {
    stopTiming();
    return;
  }
  if (!engine || !C) return;
  const my = (timing = token);
  $('measure').textContent = 'Stop';
  const full = $('full').checked;
  const rounds = full ? 3 : 1;
  const batches = full ? 6 : 3;
  const vars = [];
  for (const k of KERNELS) {
    vars.push({ id: k.id, half: false, t: [] });
    if (engine.halfOf(k.id) && R[k.id] && R[k.id].f16 === 'ok') vars.push({ id: k.id, half: true, t: [] });
  }
  const opts = { c444: S.c444 };
  const total = vars.length * rounds * batches;
  let done = 0;
  const alive = () => timing === my && token === my;
  try {
    // a short warm-up, which also sizes the command buffers (none above ~0.2 s)
    for (const v of vars) {
      const w = await engine.time(v.id, v.half, opts, 10);
      v.chunk = Math.max(1, Math.min(100, Math.floor(200 / Math.max(0.01, w.gpu != null ? w.gpu : w.wall))));
      if (!alive()) return;
    }
    for (let r = 0; r < rounds; r++) {
      for (const v of r % 2 ? [...vars].reverse() : vars) {
        for (let b = 0; b < batches; b++) {
          const t = await engine.time(v.id, v.half, opts, 100, v.chunk);
          if (!alive()) return;
          v.t.push(t.gpu != null ? t.gpu : t.wall);
          $('tprogress').textContent = `${++done} / ${total} batches`;
        }
      }
    }
  } catch (e) {
    status('The timing failed: ' + e.message, 'bad');
    stopTiming();
    return;
  }
  const median = (a) => {
    const s = [...a].sort((x, y) => x - y);
    const m = s.length >> 1;
    return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
  };
  for (const v of vars) {
    R[v.id] = R[v.id] || {};
    R[v.id][v.half ? 't16' : 't32'] = median(v.t);
  }
  timing = 0;
  $('measure').textContent = 'Measure again';
  $('tprogress').textContent = `${total} batches of 100 draws, median per draw` + (engine.hasTs ? '' : ' (wall time)');
  renderTable();
  captions();
});

addEventListener('resize', redraw);
boot();
