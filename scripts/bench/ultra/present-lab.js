// UltraPlayer itself on a corpus stream, its frames shown as a stream's are
// (POC Ultra U3.7, B2.1): by VideoFrame on a desynchronized Canvas2D, the
// page's road today, or presented by the player on the page's canvas (bench
// key mw_ultra_present). One frame every `period` ms, as frames come off the
// network, with the player's own trace (mw_ultra_trace), which gpuwait.py
// reads. Driven by present-lab.html, through gpuwait_lab.py --page.
//
//   present  vf (the VideoFrame road), dom or offscreen
//   n        frames timed, after 60 untimed; the last one stays on the canvas
//   period   ms between two frames (1000 / 120)
//   clip, mbps, enc  the corpus stream (game10-1080p, 170, 0x10de)
//   early, nudge     UltraPlayer's bench keys mw_ultra_early, mw_ultra_nudge
import { UltraPlayer } from "/frontend/js/stream/ultra/UltraPlayer.js";
import { bytes, pwvFrames } from "./gpuwait-lab.js";

const WARM = 60;

const q = (xs, f) => {
  const s = xs
    .filter((x) => x !== null && !Number.isNaN(x))
    .sort((a, b) => a - b);
  return s.length
    ? Math.round(s[Math.min(s.length - 1, Math.floor(f * s.length))] * 1000) /
        1000
    : null;
};

// Until nothing is queued or on the GPU, then two animation frames: the last
// frame is on the screen.
async function settle(player) {
  for (
    let i = 0;
    i < 200 && (player._busy || player._waiting || player._inFlight);
    i++
  )
    await new Promise((r) => setTimeout(r, 5));
  await player.device.queue.onSubmittedWorkDone();
  for (let i = 0; i < 2; i++)
    await new Promise((r) => requestAnimationFrame(r));
}

export async function runPresentLab(p, log, view) {
  const present = p.present || "vf";
  const n = Number(p.n || 600);
  const period = Number(p.period || 1000 / 120);
  // The player reads its bench keys at its construction.
  const keys = {
    mw_ultra_trace: "1",
    mw_ultra_present: present === "vf" ? "" : present,
    mw_ultra_early: p.early === "1" ? "1" : "0",
    mw_ultra_nudge: p.nudge || "",
  };
  for (const [k, v] of Object.entries(keys)) localStorage.setItem(k, v);
  const clip = p.clip || "game10-1080p";
  const pv = pwvFrames(
    await bytes(`/corpus/${clip}-${p.mbps || "170"}-${p.enc || "0x10de"}.pwv`),
    60,
  );
  const ctx2d =
    present === "vf"
      ? view.getContext("2d", { alpha: false, desynchronized: true })
      : null;
  if (ctx2d) {
    view.width = pv.w;
    view.height = pv.h;
  }
  let shown = 0;
  const player = new UltraPlayer(
    pv.w,
    pv.h,
    (frame) => {
      if (ctx2d) ctx2d.drawImage(frame, 0, 0, view.width, view.height);
      frame.close();
      shown++;
    },
    { log, present: present === "vf" ? null : view },
  );
  if (!(await player.init())) throw new Error("no decoder");
  if (player.api !== "webgpu")
    throw new Error(`the decoder runs on ${player.api}`);
  if (present !== "vf" && !player.presenting)
    throw new Error(`not presenting (${present})`);
  const errors = [];
  player.device.onuncapturederror = (e) => errors.push(e.error.message);
  const info = player.device.adapterInfo || {};
  const adapter =
    `${info.vendor} ${info.architecture} ${info.description || ""}`.trim();
  log(`adapter: ${adapter}; ${clip} ${pv.w}x${pv.h}; present ${present}`);

  // count frames from the corpus' frame `from` on, one every period ms.
  const pushAll = (count, from) =>
    new Promise((resolve) => {
      const start = performance.now();
      let k = 0;
      const tick = () => {
        const fi = from + k;
        player.push(
          pv.frames[fi % pv.frames.length],
          fi + 1,
          performance.now(),
        );
        k++;
        if (k >= count) return resolve(performance.now() - start);
        setTimeout(tick, Math.max(0, start + k * period - performance.now()));
      };
      tick();
    });
  await pushAll(WARM, 0);
  await settle(player);
  player.trace.length = 0;
  const stats0 = { ...player.stats };
  const shown0 = shown;
  const elapsed = await pushAll(n, WARM);
  await settle(player);

  const frames = player.trace.filter((r) => !r.ref && r.t2 !== null);
  const sd = frames.map((r) => r.t2 - r.t1);
  const handed = frames.filter((r) => r.t3 !== null).map((r) => r.t3 - r.t1);
  const res = {
    adapter,
    params: {
      present,
      n,
      period,
      clip,
      early: keys.mw_ultra_early === "1",
      nudge: null,
    },
    isolated: !!globalThis.crossOriginIsolated,
    perSecond: Math.round((frames.length / elapsed) * 10000) / 10,
    submitToDone: {
      p10: q(sd, 0.1),
      p50: q(sd, 0.5),
      p90: q(sd, 0.9),
      p99: q(sd, 0.99),
    },
    submitToHanded: { p50: q(handed, 0.5), p90: q(handed, 0.9) },
    shown: shown - shown0,
    replaced: player.stats.replaced - stats0.replaced,
    lastFrame: (WARM + n - 1) % pv.frames.length,
    summary: player.summary(),
    errors,
    trace: player.trace,
  };
  // Not destroyed: the last frame stays on the canvas for the driver's
  // screenshot, and the next case's navigation lets the device go.
  return res;
}
