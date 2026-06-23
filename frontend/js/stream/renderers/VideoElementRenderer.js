/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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

/**
 * VideoElementRenderer — routes decoded VideoFrames to a <video> element via a
 * MediaStreamTrackGenerator, instead of drawing them to a canvas.
 *
 * Why: the WebGPU/Canvas paths sample frames through importExternalTexture /
 * drawImage, which only output SDR-referred color spaces (srgb/display-p3) and
 * tone-map away the HDR (PQ → SDR) — HDR content looks washed out. A <video>
 * element, by contrast, presents decoded VideoFrames with their native color
 * space (BT.2020 + PQ), so the browser does the HDR compositing correctly
 * (proven on Windows Chrome). This reuses the existing WebCodecs decode
 * pipeline (DataChannel transport) and only swaps the render sink.
 *
 * The decoded frame already carries the right VideoColorSpace (set by the
 * decoder config: bt2020 / pq), so no per-frame color handling is needed here —
 * we just write the frame to the generator, which presents it on the <video>.
 *
 * Chrome-only (MediaStreamTrackGenerator). createVideoRenderer falls back to the
 * WebGPU/Canvas2D path when it is unavailable.
 *
 * Trade-off: no WebGPU enhancers (FSR1/SGSR) on this path — the frames bypass
 * the shader pipeline. HDR fidelity is the priority here.
 */
import { VideoRenderer } from './VideoRenderer.js';

export class VideoElementRenderer extends VideoRenderer {
    static async create(canvas, opts) {
        if (typeof MediaStreamTrackGenerator === 'undefined') {
            throw new Error('MediaStreamTrackGenerator unavailable');
        }
        const videoEl = opts && opts.videoEl;
        if (!videoEl) throw new Error('VideoElementRenderer requires opts.videoEl');

        const r = new VideoElementRenderer();
        r.videoEl = videoEl;
        r._disposed = false;
        r._wrote = 0;

        // MediaStreamTrackGenerator is a writable sink of VideoFrames that also
        // behaves as a MediaStreamTrack — drive a <video> from it.
        r._generator = new MediaStreamTrackGenerator({ kind: 'video' });
        if (!r._generator.writable) throw new Error('generator.writable is undefined');
        r._writer = r._generator.writable.getWriter();
        r._stream = new MediaStream([r._generator]);
        videoEl.srcObject = r._stream;
        // Make the <video> visible BEFORE play() — a display:none element can keep
        // play() pending. Caller's _applyRendererSink also sets this, but do it here
        // too to avoid any ordering gap.
        videoEl.style.display = 'block';
        // Minimize playout delay for real-time streaming when supported.
        if ('playoutDelayHint' in videoEl) videoEl.playoutDelayHint = 0;
        // play() MUST NOT be awaited: it stays pending until the first frame is
        // written, but frames are only written after create() returns (the render
        // loop needs this._renderer). Awaiting it here deadlocks. Fire-and-forget;
        // re-armed on the first successful write.
        r._playRequested = false;
        r._tryPlay();
        console.log(
            '[VideoElementRenderer] created; track.readyState=' + r._generator.readyState,
        );
        return r;
    }

    get kind() {
        return 'video-element';
    }

    // Fire-and-forget play(): never awaited (would deadlock against the first
    // frame write). Logs the outcome once for diagnostics.
    _tryPlay() {
        if (!this.videoEl) return;
        this.videoEl
            .play()
            .then(() => {
                if (!this._playRequested) {
                    this._playRequested = true;
                    console.log('[VideoElementRenderer] videoEl.play() OK');
                }
            })
            .catch((e) => console.warn('[VideoElementRenderer] play() rejected: ' + e.message));
    }

    /** <video> presents the decoded HDR frames natively. */
    get hdrActive() {
        return true;
    }

    setOutputSize() {
        // The <video> element scales itself via CSS; nothing to do here.
    }

    async draw(frame) {
        if (this._disposed || !this._writer) {
            try {
                frame.close();
            } catch (e) {}
            return;
        }
        try {
            // Honor backpressure so we never queue ahead of the compositor.
            if (this._writer.desiredSize !== null && this._writer.desiredSize <= 0) {
                await this._writer.ready;
            }
            // The generator's writable takes ownership of the frame and closes it.
            await this._writer.write(frame);
            if (this._wrote < 3) {
                this._wrote++;
                // Re-arm play() now that a frame is available (some browsers keep
                // play() pending until the MediaStream produces its first frame).
                if (this.videoEl && this.videoEl.paused) this._tryPlay();
                console.log(
                    '[VideoElementRenderer] wrote frame #' +
                        this._wrote +
                        ' ' +
                        (this.videoEl
                            ? this.videoEl.videoWidth + 'x' + this.videoEl.videoHeight
                            : '?') +
                        ' track=' +
                        (this._generator ? this._generator.readyState : '?'),
                );
            }
        } catch (e) {
            console.warn('[VideoElementRenderer] write failed: ' + e.message);
            try {
                frame.close();
            } catch (e2) {}
        }
    }

    dispose() {
        this._disposed = true;
        try {
            if (this._writer) this._writer.close();
        } catch (e) {}
        try {
            if (this.videoEl) this.videoEl.srcObject = null;
        } catch (e) {}
        this._writer = null;
        this._generator = null;
        this._stream = null;
        this.videoEl = null;
    }
}
