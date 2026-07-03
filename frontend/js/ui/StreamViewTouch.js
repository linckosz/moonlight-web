/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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
 * Touch input (mobile trackpad-like) subsystem for StreamView.
 *
 * Extracted from StreamView; plain prototype methods (`this` = the StreamView
 * instance), copied onto StreamView.prototype at the bottom of StreamView.js.
 */
export class StreamViewTouch {
    // =========================================================================
    // Touch input (mobile) — trackpad-like behaviour
    // =========================================================================

    /**
     * Handle touch start anywhere on the streaming overlay.
     *
     * Laptop-trackpad model:
     *   1 finger → start tracking for relative cursor movement / tap.
     *   2 fingers (simultaneous) → right click (mousedown+mouseup button=3).
     */
    handleTouchStart(e) {
        // A touch anywhere on screen dismisses the gesture hint, not just a tap
        // on the slide itself (it takes up real estate on mobile).
        if (
            this._shortcutsSlide &&
            this._shortcutsSlide.style.display !== 'none' &&
            !this._shortcutsSlide.classList.contains('fading-out')
        ) {
            this._hideShortcutsSlide();
        }
        // Let UI buttons (Stop, Fullscreen, Keyboard) receive their native taps.
        // Touches on the draggable stats card are handled by its own pointer
        // listeners — never feed them into the game's trackpad tracking, or the
        // cursor moves while dragging the card and jumps on the next real touch.
        if (e.target.closest('button') || e.target.closest('#stream-stats-overlay')) return;
        e.preventDefault();
        const newCount = e.touches.length;

        // Any new touch interrupts an ongoing inertial scroll glide.
        this._stopScrollMomentum();

        // New sequence: first finger of the gesture goes down.
        if (!this._touchActive) {
            this._touchActive = true;
            this._touchMaxFingers = 0;
            this._touchMoved = false;
            this._touchDragging = false;
            this._touchHadTwoFingers = false;
            this._scrollAccum = 0;
            this._scrollSamples.length = 0;
            const t = e.touches[0];
            this._touchStartX = t.clientX;
            this._touchStartY = t.clientY;
            this._touchLastX = t.clientX;
            this._touchLastY = t.clientY;
            this._touchStartTime = performance.now();
            this._lastMoveFingerCount = newCount;
        }

        this._touchFingerCount = newCount;
        this._touchMaxFingers = Math.max(this._touchMaxFingers, newCount);
        if (newCount >= 2) this._touchHadTwoFingers = true;

        if (newCount === 1) {
            // Arm long-press → drag: hold still to grab the left button
            // (lets you move windows / select text without a physical button).
            this._clearLongPress();
            this._touchLongPressTimer = setTimeout(() => {
                if (
                    this._touchActive &&
                    !this._touchMoved &&
                    this._touchFingerCount === 1 &&
                    this._touchMaxFingers === 1 &&
                    !this._touchDragging
                ) {
                    this._touchDragging = true;
                    // Touch-screen mode: grab the button right under the finger.
                    if (this._touchScreen) this._sendAbsTouch(this._touchStartX, this._touchStartY);
                    this.webrtc.send({ type: 'mousedown', button: 1 });
                }
            }, this._touchLongPressMs);
        } else {
            // Multi-finger gesture — never a drag candidate.
            this._clearLongPress();
            if (newCount >= 2) {
                // Seed spacing + centroid for zoom/scroll (2 fingers) or pan (3).
                this._seedMultiTouch(e.touches);
            }
        }
    }

    /** Seed multi-finger trackers from the current touch list: finger spacing
     *  (first two fingers, for pinch) and the centroid of all touches (for
     *  2-finger scroll and 3-finger pan). */
    _seedMultiTouch(touches) {
        const n = touches.length;
        if (n < 2) return;
        const t0 = touches[0],
            t1 = touches[1];
        this._pinchPrevDist = Math.hypot(t0.clientX - t1.clientX, t0.clientY - t1.clientY);
        let cx = 0,
            cy = 0;
        for (let i = 0; i < n; i++) {
            cx += touches[i].clientX;
            cy += touches[i].clientY;
        }
        this._pinchPrevCx = cx / n;
        this._pinchPrevCy = cy / n;
        // Anchor for "did this multi-finger gesture actually move?" — small
        // jitter while holding 2/3 fingers must NOT disqualify a tap.
        this._multiOriginCx = cx / n;
        this._multiOriginCy = cy / n;
        this._multiOriginDist = this._pinchPrevDist;
        // New 2-finger sequence: gesture mode is decided on the first clear frame.
        this._twoFingerMode = null;
    }

    /** True once a multi-finger gesture has clearly moved (centroid travel or,
     *  for 2 fingers, a real pinch) beyond the given tap-jitter tolerance (px).
     *  Pass dist=null for 3-finger gestures (spacing irrelevant). */
    _multiMovedBeyondTol(cx, cy, dist, tol) {
        const movedC = Math.hypot(
            cx - (this._multiOriginCx ?? cx),
            cy - (this._multiOriginCy ?? cy),
        );
        const movedD =
            dist != null && this._multiOriginDist != null
                ? Math.abs(dist - this._multiOriginDist)
                : 0;
        return movedC > tol || movedD > tol;
    }

    /** Touch-screen mode: map a finger's client position to the host's absolute
     *  cursor position (over the real picture, letterbox-aware) and send it. */
    _sendAbsTouch(clientX, clientY) {
        const rect = this._mediaRect();
        if (!rect || !rect.width || !rect.height) return;
        const x = Math.round(Math.max(0, Math.min(clientX - rect.left, rect.width)));
        const y = Math.round(Math.max(0, Math.min(clientY - rect.top, rect.height)));
        this.webrtc.send({
            type: 'mousemove',
            x,
            y,
            referenceWidth: Math.round(rect.width),
            referenceHeight: Math.round(rect.height),
        });
    }

    /**
     * Apply the current zoom/pan to the streamed display (canvas + video).
     * Pan is clamped so the scaled image edges never reveal the black area.
     * Origin is the center; the input layer stays unscaled.
     */
    _applyZoomTransform() {
        if (this.canvasArea) {
            const rect = this.canvasArea.getBoundingClientRect();
            // Displayed image size at zoom 1 (object-fit: contain letterboxes it
            // inside the area). Clamp the pan so the *scaled image* always covers
            // the area — its edges can never reveal the black bars.
            const el = this._displayEl();
            let iw = 0,
                ih = 0;
            if (this._videoIsDisplay()) {
                iw = el.videoWidth;
                ih = el.videoHeight;
            } else if (this.canvas) {
                iw = this.canvas.width;
                ih = this.canvas.height;
            }
            let imgW = rect.width,
                imgH = rect.height;
            if (iw > 0 && ih > 0) {
                const fit = Math.min(rect.width / iw, rect.height / ih);
                imgW = iw * fit;
                imgH = ih * fit;
            }
            const maxX = Math.max(0, (imgW * this._zoom - rect.width) / 2);
            const maxY = Math.max(0, (imgH * this._zoom - rect.height) / 2);
            // Allow pushing the image up beyond the standard clamp so its bottom
            // edge can travel up to the middle of the visible area (black bar up
            // to half the height). Other edges keep the strict no-black clamp.
            const extraDown = rect.height / 2;
            this._panX = Math.max(-maxX, Math.min(maxX, this._panX));
            this._panY = Math.max(-(maxY + extraDown), Math.min(maxY, this._panY));
        }
        const transform =
            this._zoom > 1.001
                ? `translate(${this._panX}px, ${this._panY}px) scale(${this._zoom})`
                : '';
        if (this.canvas) this.canvas.style.transform = transform;
        if (this.videoEl) this.videoEl.style.transform = transform;
    }

    /** Cancel a pending long-press → drag timer. */
    _clearLongPress() {
        if (this._touchLongPressTimer) {
            clearTimeout(this._touchLongPressTimer);
            this._touchLongPressTimer = null;
        }
    }

    /**
     * Handle touch move (drag).
     *
     * 1 finger  → RELATIVE cursor movement (trackpad), like a laptop touchpad:
     *             the finger moves the host cursor by deltas, wherever it is.
     * 2 fingers → pinch = zoom the local display (focal recenter), otherwise
     *             vertical scroll wheel to the host (works zoomed in too).
     * 3 fingers → pan the zoomed display (no scroll/zoom side effects).
     */
    handleTouchMove(e) {
        if (e.target.closest('button') || e.target.closest('#stream-stats-overlay')) return;
        e.preventDefault();
        if (!this._touchActive) return;

        const count = e.touches.length;
        this._touchFingerCount = count;

        // Finger count changed mid-gesture (lifted/added a finger): reseed the
        // multi-finger trackers and skip this frame's delta to avoid a jump.
        if (count !== this._lastMoveFingerCount) {
            this._lastMoveFingerCount = count;
            if (count === 1) {
                const t = e.touches[0];
                this._touchLastX = t.clientX;
                this._touchLastY = t.clientY;
            } else {
                this._seedMultiTouch(e.touches);
            }
            return;
        }

        if (count === 1) {
            // Single finger: relative trackpad movement (also drags when the
            // long-press grab is active — the left button stays held down).
            const touch = e.touches[0];

            // Past the tap threshold → it's a move, cancel long-press arming.
            const movedDist = Math.hypot(
                touch.clientX - this._touchStartX,
                touch.clientY - this._touchStartY,
            );
            if (movedDist > this._touchTapThreshold) {
                this._touchMoved = true;
                if (!this._touchDragging) this._clearLongPress();
            }

            if (this._touchScreen) {
                // Absolute: the cursor follows the finger 1:1 over the picture,
                // but ONLY for a genuine single-finger gesture — a 2/3-finger
                // gesture (or its leftover finger) must never move the cursor.
                if (this._touchMaxFingers === 1) {
                    this._sendAbsTouch(touch.clientX, touch.clientY);
                }
            } else {
                // Slow the cursor proportionally to zoom (linear: -0.1 per step,
                // x2→0.9, x3→0.8, x4→0.7…) for finer aim when zoomed in.
                const zoomSlow = Math.max(0.1, 1 - 0.1 * (this._zoom - 1));
                const sens = this._touchSensitivity * zoomSlow;
                const dx = (touch.clientX - this._touchLastX) * sens;
                const dy = (touch.clientY - this._touchLastY) * sens;
                if (dx !== 0 || dy !== 0) {
                    this.webrtc.send({
                        type: 'mousemove',
                        dx: Math.round(dx),
                        dy: Math.round(dy),
                    });
                }
            }

            this._touchLastX = touch.clientX;
            this._touchLastY = touch.clientY;
        } else if (count === 2) {
            // Two fingers: pinch → zoom (focal recenter); otherwise → scroll
            // wheel to the host. Pan is reserved for 3 fingers, so scrolling
            // works whether or not the display is zoomed in.
            this._clearLongPress();
            const t1 = e.touches[0];
            const t2 = e.touches[1];
            const cx = (t1.clientX + t2.clientX) / 2;
            const cy = (t1.clientY + t2.clientY) / 2;
            const dist = Math.hypot(t1.clientX - t2.clientX, t1.clientY - t2.clientY);
            // Only count as "moved" past a tolerance, so a slightly imperfect
            // 2-finger tap still registers. Tighter than 3-finger (12px) — a
            // compromise between the old zero-tolerance and the looser pass.
            if (this._multiMovedBeyondTol(cx, cy, dist, 12)) this._touchMoved = true;

            const dDist = this._pinchPrevDist > 0 ? dist - this._pinchPrevDist : 0;
            const dCy = this._pinchPrevCy != null ? cy - this._pinchPrevCy : 0;

            // Lock the gesture to zoom OR scroll for the whole 2-finger sequence,
            // so a scroll never zooms (and vice versa). The mode is decided on the
            // first frame with clear intent: a change in finger spacing → zoom,
            // a parallel drag → scroll. Ambiguous tiny frames wait until it's clear.
            // Bias toward scroll: zoom only locks when the finger-spacing change
            // clearly dominates the vertical drag (1.6x), so a slightly uneven
            // two-finger swipe scrolls instead of zooming by accident.
            if (this._twoFingerMode == null) {
                if (Math.abs(dDist) > 3 && Math.abs(dDist) >= Math.abs(dCy) * 1.6) {
                    this._twoFingerMode = 'zoom';
                } else if (Math.abs(dCy) > 1.5) {
                    this._twoFingerMode = 'scroll';
                }
            }

            if (this._twoFingerMode === 'zoom') {
                const newZoom = Math.min(8, Math.max(1, this._zoom * (dist / this._pinchPrevDist)));
                const f = newZoom / this._zoom;
                if (f !== 1 && this.canvasArea) {
                    const rect = this.canvasArea.getBoundingClientRect();
                    const focalX = cx - (rect.left + rect.width / 2);
                    const focalY = cy - (rect.top + rect.height / 2);
                    this._panX = focalX * (1 - f) + f * this._panX;
                    this._panY = focalY * (1 - f) + f * this._panY;
                }
                this._zoom = newZoom;
                if (this._zoom <= 1.001) {
                    this._panX = 0;
                    this._panY = 0;
                }
                this._applyZoomTransform();
                // Re-render the enhancer backing at the new zoom step (crisp pinch-zoom).
                if (this._outputZoomScale() !== this._lastOutputZoomScale) this._applyOutputSize();
                this._scrollSamples.length = 0; // pinching cancels pending inertia
            } else if (this._twoFingerMode === 'scroll' && Math.abs(dCy) > 0.1) {
                // Parallel two-finger drag → vertical scroll wheel, amplified and
                // with fractional carry so slow drags still register. Record
                // centroid samples to derive the flick velocity at release.
                const scaled = dCy * this._scrollScale;
                this._scrollAccum += scaled;
                const whole = Math.trunc(this._scrollAccum);
                if (whole !== 0) {
                    this.webrtc.send({ type: 'mousewheel', delta: whole });
                    this._scrollAccum -= whole;
                }
                const now = performance.now();
                this._scrollSamples.push({ t: now, y: cy });
                // Keep only the last ~120 ms of samples.
                while (this._scrollSamples.length > 2 && now - this._scrollSamples[0].t > 120) {
                    this._scrollSamples.shift();
                }
            }

            this._pinchPrevDist = dist;
            this._pinchPrevCx = cx;
            this._pinchPrevCy = cy;
        } else if (count >= 3) {
            // Three fingers: pan the zoomed display (no effect at base zoom).
            this._clearLongPress();
            let cx = 0,
                cy = 0;
            for (let i = 0; i < count; i++) {
                cx += e.touches[i].clientX;
                cy += e.touches[i].clientY;
            }
            cx /= count;
            cy /= count;
            // Tolerate jitter so a slightly imperfect 3-finger tap still fires.
            if (this._multiMovedBeyondTol(cx, cy, null, 24)) this._touchMoved = true;
            const dCx = this._pinchPrevCx != null ? cx - this._pinchPrevCx : 0;
            const dCy = this._pinchPrevCy != null ? cy - this._pinchPrevCy : 0;
            if (this._zoom > 1.01) {
                this._panX += dCx;
                this._panY += dCy;
                this._applyZoomTransform();
            }
            this._pinchPrevCx = cx;
            this._pinchPrevCy = cy;
        }
    }

    /**
     * Handle touch end.
     *
     * 1→0 finger transition with minimal movement → left click (tap).
     */
    handleTouchEnd(e) {
        if (e.target.closest('button') || e.target.closest('#stream-stats-overlay')) return;
        e.preventDefault();
        this._touchFingerCount = e.touches.length;

        // Resolve the gesture only once ALL fingers are lifted.
        if (e.touches.length > 0) return;

        this._clearLongPress();

        const elapsed = performance.now() - this._touchStartTime;
        const tch = e.changedTouches[0];
        const dist = tch
            ? Math.hypot(tch.clientX - this._touchStartX, tch.clientY - this._touchStartY)
            : 0;
        // Multi-finger taps (right-click / keyboard) are harder to land cleanly:
        // fingers touch down staggered and the first one drifts. 3-finger stays
        // the most forgiving; 2-finger sits midway between that and a precise
        // 1-finger tap.
        const distTol =
            this._touchMaxFingers >= 3
                ? 45
                : this._touchMaxFingers === 2
                  ? 28
                  : this._touchTapThreshold;
        const timeTol =
            this._touchMaxFingers >= 3
                ? 600
                : this._touchMaxFingers === 2
                  ? 450
                  : this._touchTapTimeThreshold;
        const isTap =
            !this._touchMoved && dist < distTol && elapsed < timeTol && this._touchStartTime > 0;

        if (this._touchDragging) {
            // End the long-press drag — release the held left button.
            this.webrtc.send({ type: 'mouseup', button: 1 });
        } else if (isTap) {
            if (this._touchMaxFingers >= 3) {
                this._toggleVirtualKeyboard(); // 3-finger tap → keyboard
            } else if (this._touchMaxFingers === 2) {
                this.webrtc.send({ type: 'mousedown', button: 3 });
                this.webrtc.send({ type: 'mouseup', button: 3 }); // 2-finger tap → right click
            } else {
                // 1-finger tap → left click. Touch-screen mode positions the
                // cursor first; a fast double-tap at the same spot lands a second
                // click at the SAME coordinate, so the host registers a true
                // double-click and selects the word under it.
                const now = performance.now();
                const near =
                    Math.hypot(
                        this._touchStartX - (this._lastTapX || 0),
                        this._touchStartY - (this._lastTapY || 0),
                    ) < 28;
                const isDouble =
                    this._touchScreen &&
                    this._lastTapTime &&
                    now - this._lastTapTime <= 300 &&
                    near;
                if (this._touchScreen) {
                    const px = isDouble ? this._lastTapX : this._touchStartX;
                    const py = isDouble ? this._lastTapY : this._touchStartY;
                    this._sendAbsTouch(px, py);
                }
                this.webrtc.send({ type: 'mousedown', button: 1 });
                this.webrtc.send({ type: 'mouseup', button: 1 });
                if (isDouble) {
                    this._lastTapTime = 0; // consumed — avoid chaining into a triple
                } else {
                    this._lastTapTime = now;
                    this._lastTapX = this._touchStartX;
                    this._lastTapY = this._touchStartY;
                }
            }
        }

        // Inertial scroll: if the gesture ended on a flicking two-finger drag,
        // keep gliding with a decaying velocity (phone-like momentum). Velocity
        // is derived from the recent centroid samples (px/frame → wheel units).
        if (!isTap && !this._touchDragging && this._touchMaxFingers === 2) {
            this._startScrollMomentum();
        }

        // Reset sequence state.
        this._touchActive = false;
        this._touchDragging = false;
        this._touchMoved = false;
        this._touchHadTwoFingers = false;
        this._touchFingerCount = 0;
        this._touchMaxFingers = 0;
        this._scrollAccum = 0;
        this._scrollSamples.length = 0;
        this._pinchPrevDist = 0;
        this._pinchPrevCx = null;
        this._pinchPrevCy = null;
        this._twoFingerMode = null;
        this._lastMoveFingerCount = 0;
    }

    /** Start the inertial scroll glide. Computes the release velocity from the
     *  recent centroid samples, then sends decaying wheel deltas each frame
     *  until the velocity dies out (or a new touch cancels it). */
    _startScrollMomentum() {
        this._stopScrollMomentum();

        // Flick velocity from the oldest→newest recent sample (px per frame).
        const s = this._scrollSamples;
        if (s.length < 2) return;
        const a = s[0],
            b = s[s.length - 1];
        const dt = b.t - a.t;
        // Only glide on a fresh flick (last sample very recent, real movement).
        if (dt <= 0 || performance.now() - b.t > 80) return;
        let v = ((b.y - a.y) / dt) * 16.67 * this._scrollScale; // wheel units/frame
        v = Math.max(-400, Math.min(400, v)); // clamp wild flicks
        if (Math.abs(v) < 1.5) return; // too slow → no glide

        let acc = this._scrollAccum; // carry leftover fractional delta
        const friction = 0.95; // per-frame decay (higher = longer glide)
        const step = () => {
            v *= friction;
            if (Math.abs(v) < 0.4) {
                this._scrollMomentumRaf = null;
                return;
            }
            acc += v;
            const whole = Math.trunc(acc);
            if (whole !== 0) {
                this.webrtc.send({ type: 'mousewheel', delta: whole });
                acc -= whole;
            }
            this._scrollMomentumRaf = requestAnimationFrame(step);
        };
        this._scrollMomentumRaf = requestAnimationFrame(step);
    }

    /** Cancel any running inertial scroll glide. */
    _stopScrollMomentum() {
        if (this._scrollMomentumRaf != null) {
            cancelAnimationFrame(this._scrollMomentumRaf);
            this._scrollMomentumRaf = null;
        }
    }
}
