// Bitrate bench tap, injected by the driver over CDP (Page.addScriptToEvaluateOnNewDocument)
// into the LAB client page only. Nothing in the repository changes.
//
// It wraps window.VideoDecoder so that, while armed:
//   - every EncodedVideoChunk handed to decode() is copied (bitstream, a few MB):
//     the whole stream is decoded again offline by ffmpeg, which is bit-exact,
//     so every frame of the run can be scored;
//   - the decoded VideoFrames at a few output indices are copied too (copyTo,
//     the decoder's own planes), to prove offline that the hardware decoder and
//     ffmpeg give the same picture, bit for bit.
// Nothing is sent while the stream runs: mwTap.flush(sink) uploads after it stops.
(() => {
  if (window.__mwTap || typeof window.VideoDecoder !== 'function') return;
  const Orig = window.VideoDecoder;
  // The app's own 'input' data channel: the bench asks for a keyframe on it with the
  // app's own message, so recording can start on a picture the bench page is in.
  const origCreate = RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel = function (label, init) {
    const ch = origCreate.call(this, label, init);
    if (label === 'input') T.input = ch;
    return ch;
  };
  // The page's barcode (content/bench.html), read from an NV12/I420 luma plane:
  // same cells and check bits as common.read_bar. null when unreadable.
  // 10-bit frames carry two bytes a sample, little-endian: P010 in the high bits, I420P10 /
  // I444P10 in the low ones; both are brought to the 8-bit scale.
  function readBar(b, layout, w, h, format) {
    const off = layout[0].offset, stride = layout[0].stride;
    const c = 2 * Math.round(h / 120), q = Math.max(1, Math.floor(c / 4));
    const wide = /P010|P10|P12/.test(format || ''), shift = format === 'P010' ? 8 : 2;
    // RGB frames (a 4:4:4 stream comes out of Chrome's decoder as BGRA): the barcode is black
    // and white, so the green byte reads it.
    const rgb = /^(RGBA|RGBX|BGRA|BGRX)$/.test(format || '');
    const at = (y, x) => {
      if (rgb) return b[off + y * stride + 4 * x + 1];
      if (!wide) return b[off + y * stride + x];
      const i = off + y * stride + 2 * x;
      return (b[i] | (b[i + 1] << 8)) >> shift;
    };
    const mean = (y0, x0) => {
      let s = 0, n = 0;
      for (let y = y0 - q; y < y0 + q; y++) for (let x = x0 - q; x < x0 + q; x++) { s += at(y, x); n++; }
      return s / n;
    };
    const bits = [];
    for (let k = 0; k < 24; k++) {
      const cx = k * c + Math.floor(c / 2);
      const top = mean(h - 2 * c + Math.floor(c / 2), cx), bot = mean(h - c + Math.floor(c / 2), cx);
      if (Math.abs(top - bot) < 40) return null;
      bits.push(top > bot ? 1 : 0);
    }
    let v = 0, ch = 0;
    for (let i = 0; i < 16; i++) v |= bits[i] << i;
    for (let i = 0; i < 8; i++) ch |= bits[16 + i] << i;
    return ch === ((((v & 255) + ((v >> 8) & 255)) ^ 0xa5) & 255) ? v : null;
  }
  const T = (window.__mwTap = {
    armed: false,
    waiting: false,
    startTs: null,
    lastConfig: null,
    bars: [],
    bad: null,
    input: null,
    configs: [],
    chunks: [],
    frames: [],
    outCount: 0,
    pending: 0,
    errors: [],
    frameIdx: new Set(),
    arm(frameIdx) {
      this.configs = [];
      this.chunks = [];
      this.frames = [];
      this.outCount = 0;
      this.errors = [];
      this.frameIdx = new Set(frameIdx || []);
      this.armed = true;
      return 'armed';
    },
    // Nothing is kept until the next keyframe: the driver calls this once the bench
    // page is on the screen, then idr(). Output frame 0 (that keyframe) is always
    // grabbed and must carry the page's barcode, or everything is dropped.
    armOnKey(frameIdx) {
      this.armed = false;
      this.configs = [];
      this.chunks = [];
      this.frames = [];
      this.outCount = 0;
      this.errors = [];
      this.bars = [];
      this.bad = null;
      this.startTs = null;
      this.frameIdx = new Set([0, ...(frameIdx || [])]);
      this.waiting = true;
      return 'waiting';
    },
    idr() {
      if (!this.input || this.input.readyState !== 'open') return 'no input channel';
      this.input.send(JSON.stringify({ type: 'requestidr', reason: 'bench' }));
      return 'asked';
    },
    wipe(why) {
      this.armed = false;
      this.waiting = false;
      this.bad = this.bad || why || 'wiped';
      this.chunks = [];
      this.frames = [];
      return this.bad;
    },
    disarm() {
      this.armed = false;
      this.waiting = false;
      return this.stats();
    },
    stats() {
      let bytes = 0, keys = 0;
      for (const c of this.chunks) { bytes += c.b.byteLength; if (c.type === 'key') keys++; }
      return { armed: this.armed, waiting: this.waiting, configs: this.configs.length, chunks: this.chunks.length, keys, bytes,
               frames: this.frames.length, outCount: this.outCount, pending: this.pending,
               bars: this.bars.slice(), bad: this.bad, errors: this.errors.slice(0, 5) };
    },
    async flush(sink, tag) {
      const q = (o) => Object.entries(o).map(([k, v]) => k + '=' + encodeURIComponent(v)).join('&');
      const meta = {
        tag, ua: navigator.userAgent,
        configs: this.configs,
        chunks: this.chunks.map((c) => ({ type: c.type, ts: c.ts, n: c.b.byteLength, at: c.at })),
        frames: this.frames.map((f) => ({ idx: f.idx, ts: f.ts, format: f.format, codedW: f.codedW, codedH: f.codedH,
                                          vis: f.vis, layout: f.layout, colorSpace: f.colorSpace, n: f.b.byteLength })),
      };
      await fetch(sink + '/meta?' + q({ tag }), { method: 'POST', body: JSON.stringify(meta) });
      const total = new Uint8Array(this.chunks.reduce((s, c) => s + c.b.byteLength, 0));
      let o = 0;
      for (const c of this.chunks) { total.set(c.b, o); o += c.b.byteLength; }
      await fetch(sink + '/bitstream?' + q({ tag }), { method: 'POST', body: total });
      for (const f of this.frames) await fetch(sink + '/frame?' + q({ tag, idx: f.idx }), { method: 'POST', body: f.b });
      return { chunks: this.chunks.length, bytes: total.byteLength, frames: this.frames.length };
    },
  });

  function grab(frame, idx) {
    const f = frame.clone();
    T.pending++;
    // An HDR frame from the GPU decoder cannot be copied as it is (allocationSize refuses it):
    // it is copied converted to RGBA then, which keeps the black-and-white barcode.
    let opts, b;
    try { b = new Uint8Array(f.allocationSize()); } catch (e) { opts = { format: 'RGBA' }; }
    try { if (opts) b = new Uint8Array(f.allocationSize(opts)); } catch (e) { T.errors.push('grab ' + idx + ': ' + e); T.pending--; f.close(); return; }
    const fmt = opts ? 'RGBA' : f.format;
    f.copyTo(b, opts)
      .then((layout) => {
        const v = f.visibleRect;
        let bar = null;
        try { bar = readBar(b, layout, v.width, v.height, fmt); } catch (e) { T.errors.push('bar ' + idx + ': ' + e); }
        T.bars.push([idx, bar]);
        if (bar === null) { T.wipe('no barcode on output frame ' + idx + ' (' + f.format + ', ' + layout.length + ' planes, stride ' + layout[0].stride + ', ' + v.width + 'x' + v.height + ')'); return; }
        if (!T.armed) return;
        T.frames.push({ idx, ts: f.timestamp, format: fmt, source_format: f.format, codedW: f.codedWidth, codedH: f.codedHeight,
                        vis: v ? { x: v.x, y: v.y, w: v.width, h: v.height } : null,
                        layout, colorSpace: f.colorSpace ? f.colorSpace.toJSON() : null, b });
      })
      .catch((e) => T.errors.push('copyTo ' + idx + ': ' + e))
      .finally(() => { f.close(); T.pending--; });
  }

  class TappedDecoder extends Orig {
    constructor(init) {
      const out = init.output;
      super({
        ...init,
        output: (frame) => {
          if (T.armed && T.startTs !== null && frame.timestamp >= T.startTs) {
            const idx = T.outCount++;
            if (T.frameIdx.has(idx)) {
              try { grab(frame, idx); } catch (e) { T.errors.push('grab ' + idx + ': ' + e); }
            }
          }
          out(frame);
        },
      });
    }
    configure(cfg) {
      let desc = null;
      if (cfg && cfg.description) {
        const u = cfg.description instanceof ArrayBuffer ? new Uint8Array(cfg.description)
          : new Uint8Array(cfg.description.buffer, cfg.description.byteOffset, cfg.description.byteLength);
        desc = btoa(String.fromCharCode(...u));
      }
      T.lastConfig = { codec: cfg.codec, codedWidth: cfg.codedWidth, codedHeight: cfg.codedHeight,
                       hardwareAcceleration: cfg.hardwareAcceleration, description: desc };
      if (T.armed) T.configs.push({ ...T.lastConfig, atChunk: T.chunks.length, atOut: T.outCount });
      return super.configure(cfg);
    }
    decode(chunk) {
      if (T.waiting && chunk.type === 'key') {
        T.waiting = false;
        T.armed = true;
        T.startTs = chunk.timestamp;
        if (T.lastConfig) T.configs.push({ ...T.lastConfig, atChunk: 0, atOut: 0 });
      }
      if (T.armed) {
        try {
          const b = new Uint8Array(chunk.byteLength);
          chunk.copyTo(b);
          T.chunks.push({ type: chunk.type, ts: chunk.timestamp, b, at: performance.now() });
        } catch (e) { T.errors.push('chunk: ' + e); }
      }
      return super.decode(chunk);
    }
  }
  window.VideoDecoder = TappedDecoder;
})();
