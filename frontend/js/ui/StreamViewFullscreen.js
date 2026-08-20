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

import { t } from '../i18n/i18n.js';
import { IS_MOBILE_OR_TABLET, IS_IOS, IS_STANDALONE } from '../util/BrowserDetect.js';

/** @typedef {import('./StreamView.js').StreamView} StreamViewInstance */

/**
 * When to re-read the device orientation after a rotation signal, in ms.
 * Rotation animations run ~300ms; the last probe is the safety net for slow
 * devices. See _scheduleOrientationSync().
 */
const ORIENTATION_SYNC_DELAYS = [60, 160, 320, 600];

/**
 * Fullscreen / wake-lock / immersive-exit subsystem for StreamView.
 *
 * Covers the standard Fullscreen API, the iOS CSS fallback, the Screen Wake
 * Lock, mobile orientation handling and the transient on-screen hints.
 *
 * Extracted from StreamView; plain prototype methods (`this` = the StreamView
 * instance), copied onto StreamView.prototype at the bottom of StreamView.js.
 */
export class StreamViewFullscreen {
    /** @this {StreamViewInstance} */
    _initMobileFullscreen() {
        // Mobile or tablet only (excludes touchscreen laptops via User-Agent detection)
        if (!IS_MOBILE_OR_TABLET) return;

        // Baseline for the rotation diffing in _syncOrientationState(): the
        // orientation the stream starts in. Every later sync compares against
        // it, so a read taken before the rotation settled is a no-op instead of
        // a wrong decision.
        this._lastLandscape = this._isDeviceLandscape();

        // Orientation change: three signals, all funnelled into one deferred
        // re-check (see _scheduleOrientationSync() for why it is deferred).
        this._onOrientationChange = () => this._scheduleOrientationSync();

        // 1. Media query — fires on every viewport reshape, rotation included.
        this._orientationMql = window.matchMedia('(orientation: landscape)');
        this._orientationMql.addEventListener('change', this._onOrientationChange);
        // 2. screen.orientation — the device signal, and the source
        //    _isDeviceLandscape() actually reads. Absent on iOS before 16.4.
        const screenOrientation = (window.screen || {}).orientation;
        if (screenOrientation && typeof screenOrientation.addEventListener === 'function') {
            try {
                screenOrientation.addEventListener('change', this._onOrientationChange);
                this._screenOrientation = screenOrientation;
            } catch (e) {
                /* older WebKit exposes the object without the event */
            }
        }
        // 3. window.orientationchange — legacy fallback for those iOS versions.
        window.addEventListener('orientationchange', this._onOrientationChange);

        // Fullscreen change listener (for button visibility)
        this._onFullscreenChange = () => {
            this._updateMobileFsButtonVisibility();
        };
        document.addEventListener('fullscreenchange', this._onFullscreenChange);
        if (this.videoEl) {
            this.videoEl.addEventListener('webkitbeginfullscreen', this._onFullscreenChange);
            this.videoEl.addEventListener('webkitendfullscreen', this._onFullscreenChange);
        }

        // No auto-fullscreen on launch: the user enters fullscreen only by
        // tapping the button (shown in landscape). Just set the initial state.
        this._updateMobileFsButtonVisibility();
    }

    /**
     * Re-check the device orientation after a rotation signal — deferred, and
     * more than once.
     *
     * The media query flips as soon as the viewport is reshaped, and the
     * viewport is reshaped BEFORE `screen.orientation.type` is updated. Reading
     * it straight from the handler therefore answers with the orientation the
     * device has just left, which is how the Fullscreen button ended up one
     * rotation behind: still hidden after turning to landscape, then shown
     * after turning back to portrait. Re-reading over the next few hundred
     * milliseconds catches the settled value whichever signal fired first, and
     * _syncOrientationState() discards the reads that still show the old one.
     * @this {StreamViewInstance}
     */
    _scheduleOrientationSync() {
        this._clearOrientationSync();

        const step = () => {
            // Applied: the remaining probes have nothing left to do.
            if (this._syncOrientationState()) this._clearOrientationSync();
        };

        this._orientationSyncTimers = ORIENTATION_SYNC_DELAYS.map((ms) => setTimeout(step, ms));
        if (typeof requestAnimationFrame === 'function') {
            this._orientationSyncRaf = requestAnimationFrame(step);
        }
    }

    /**
     * Drop any pending orientation re-check.
     * @this {StreamViewInstance}
     */
    _clearOrientationSync() {
        if (this._orientationSyncRaf) {
            cancelAnimationFrame(this._orientationSyncRaf);
            this._orientationSyncRaf = null;
        }
        for (const id of this._orientationSyncTimers || []) clearTimeout(id);
        this._orientationSyncTimers = null;
    }

    /**
     * Apply the device orientation — but only when it actually changed since
     * the last one we applied, so a probe that fired too early (still reading
     * the previous orientation) does nothing.
     *
     * Landscape: do NOT auto-enter fullscreen — just reveal the button so the
     * user keeps the header until they explicitly tap it. Portrait: always
     * leave fullscreen.
     *
     * @returns {boolean} true when the orientation changed and was applied.
     * @this {StreamViewInstance}
     */
    _syncOrientationState() {
        const landscape = this._isDeviceLandscape();
        if (landscape === this._lastLandscape) return false;
        this._lastLandscape = landscape;

        if (!landscape) this._exitMobileFullscreen();
        this._updateMobileFsButtonVisibility();
        return true;
    }

    /**
     * Drop the orientation listeners and any pending re-check. Called from both
     * teardown paths (quit and standby retire).
     * @this {StreamViewInstance}
     */
    _teardownOrientation() {
        this._clearOrientationSync();
        if (this._onOrientationChange) {
            if (this._orientationMql) {
                this._orientationMql.removeEventListener('change', this._onOrientationChange);
            }
            if (this._screenOrientation) {
                try {
                    this._screenOrientation.removeEventListener(
                        'change',
                        this._onOrientationChange,
                    );
                } catch (e) {
                    /* silently ignored */
                }
            }
            window.removeEventListener('orientationchange', this._onOrientationChange);
        }
        this._orientationMql = null;
        this._screenOrientation = null;
        this._onOrientationChange = null;
        this._lastLandscape = null;
    }

    /**
     * Mobile auto-fullscreen (orientation change / landscape launch).
     * Delegates to _requestFullscreen(): same chain, same CSS fallback.
     * @this {StreamViewInstance}
     */
    _requestMobileFullscreen() {
        this._requestFullscreen();
    }

    /**
     * Acquire a Screen Wake Lock so the device screen stays on while streaming
     * (prevents iPhone auto-lock and PC display sleep). The lock is auto-released
     * by the browser when the page is hidden, so we re-acquire it on visibility
     * change. No-op when the API is unavailable.
     * @this {StreamViewInstance}
     */
    async _acquireWakeLock() {
        if (!('wakeLock' in navigator)) return;
        // Cleared by _releaseWakeLock() so an intentional release doesn't loop.
        this._wakeReleased = false;

        try {
            this._wakeLock = await navigator.wakeLock.request('screen');
            // The lock can be dropped by the system without a visibility change
            // (notably iOS Safari auto-releases after a while). Clear our reference
            // and immediately re-request while still streaming and visible, so the
            // screen never sleeps mid-session.
            this._wakeLock.addEventListener('release', () => {
                this._wakeLock = null;
                if (!this._wakeReleased && document.visibilityState === 'visible') {
                    this._acquireWakeLock();
                }
            });
        } catch (err) {
            console.warn('[StreamView] Wake Lock request failed:', err.message);
        }

        // Re-acquire when the page becomes visible again (lock is released on hide).
        if (!this._onWakeLockVisibility) {
            this._onWakeLockVisibility = () => {
                if (document.visibilityState === 'visible' && !this._wakeLock) {
                    this._acquireWakeLock();
                }
            };
            document.addEventListener('visibilitychange', this._onWakeLockVisibility);
        }
    }

    /**
     * Release the wake lock and drop the visibility handler.
     *
     * @this {StreamViewInstance}
     */
    _releaseWakeLock() {
        // Mark intentional so the release handler doesn't re-acquire.
        this._wakeReleased = true;
        if (this._onWakeLockVisibility) {
            document.removeEventListener('visibilitychange', this._onWakeLockVisibility);
            this._onWakeLockVisibility = null;
        }
        if (this._wakeLock) {
            this._wakeLock.release().catch(() => {});
            this._wakeLock = null;
        }
    }

    /**
     * Request fullscreen for any transport mode, with CSS fallback on failure.
     *
     * IMPORTANT: never use webkitEnterFullscreen() (iOS native video player).
     * The native player swallows ALL touch/keyboard input — the user can see
     * the stream but cannot control the host. The CSS fallback keeps our DOM
     * (and therefore our input handlers) on screen, which is the only viable
     * fullscreen on iOS.
     *
     * Priority:
     *   1. document.documentElement.requestFullscreen() — standard Fullscreen API.
     *   2. _enterCssFallbackFullscreen() — fake fullscreen via CSS when the API
     *      is unavailable or rejected (iOS Safari, non-user-gesture calls).
     * @this {StreamViewInstance}
     */
    _requestFullscreen() {
        if (this._cssFullscreen) return;

        if (document.documentElement.requestFullscreen) {
            document.documentElement
                .requestFullscreen()
                .then(() => {
                    // Success — button visibility handled by fullscreenchange
                })
                .catch((err) => {
                    console.warn('[StreamView] requestFullscreen failed:', err.message);
                    this._enterCssFallbackFullscreen();
                });
            return;
        }

        // ── No fullscreen API available — CSS fallback ───────────────────────
        this._enterCssFallbackFullscreen();
    }

    /**
     * Enter CSS-based "fake" fullscreen when the Fullscreen API is unavailable
     * (iOS Safari canvas mode). Hides the header and stretches the canvas to
     * cover the viewport. No exit button: desktop uses the keyboard combo and
     * mobile auto-exits when rotating back to portrait.
     * @this {StreamViewInstance}
     */
    _enterCssFallbackFullscreen() {
        if (this._cssFullscreen || this._quitting) return;
        this._cssFullscreen = true;

        // Add CSS class to the stream-view container
        const streamView = this._rootEl || document.getElementById('stream-view');
        if (streamView) streamView.classList.add('stream-css-fs');

        // Hide the header fullscreen button
        if (this._mobileFsBtn) this._mobileFsBtn.style.display = 'none';

        // iOS blocks the Fullscreen API on canvas, so the browser chrome stays
        // visible. Hint the user that an installed PWA is the only true path.
        if (IS_IOS && !IS_STANDALONE) this._showInstallHint();
    }

    /**
     * Show a one-shot "Add to Home Screen" hint, then fade it out after 4s.
     * Sits above the CSS fullscreen canvas (z-index) and is shown once per view.
     * @this {StreamViewInstance}
     */
    _showInstallHint() {
        if (this._installHintShown) return;
        this._installHintShown = true;

        const hint = document.createElement('div');
        hint.className = 'install-hint';
        hint.textContent = t('stream.iosFullscreenHint');
        document.body.appendChild(hint);

        // Force reflow so the opacity transition runs on insert.
        void hint.offsetWidth;
        hint.classList.add('install-hint-visible');

        const dismiss = () => {
            hint.classList.remove('install-hint-visible');
            hint.addEventListener('transitionend', () => hint.remove(), { once: true });
        };

        // Dismiss immediately on tap/click.
        hint.addEventListener('click', dismiss, { once: true });

        // Otherwise fade out after 4s.
        setTimeout(dismiss, 4000);
    }

    /**
     * Sync the Escape Keyboard Lock with the current fullscreen state.
     * In fullscreen, lock Escape so it reaches the host (instead of exiting);
     * out of fullscreen, release it. Chrome/Edge only — no-op elsewhere.
     * @this {StreamViewInstance}
     */
    _syncKeyboardLock() {
        const kb = navigator.keyboard;
        if (!kb || typeof kb.lock !== 'function') return;
        if (this._gamingMode && this._mouseFocused) {
            // Immersive mode with the mouse captured: grab EVERY system key
            // (Meta/Windows, Alt+Tab, Escape…) so they reach the host. Only the
            // exit combo stays client-side. Effective in fullscreen (browser
            // limitation); a no-op call otherwise. lock() with no args = all keys.
            kb.lock().catch(() => {});
            this._keyboardLocked = true;
        } else if (document.fullscreenElement) {
            // Plain fullscreen (non-immersive): only keep Escape inside the host.
            kb.lock(['Escape']).catch(() => {});
            this._keyboardLocked = true;
        } else if (this._keyboardLocked) {
            try {
                kb.unlock();
            } catch (e) {}
            this._keyboardLocked = false;
        }
    }

    /**
     * Single "give me back control" action bound to the immersive exit combo.
     * Releases the mouse pointer lock, drops the full keyboard lock and leaves
     * fullscreen (standard or CSS fallback) — whichever are currently active.
     * @this {StreamViewInstance}
     */
    _exitImmersive() {
        if (document.pointerLockElement === this.inputEl) {
            document.exitPointerLock();
        }
        const kb = navigator.keyboard;
        if (this._keyboardLocked && kb && typeof kb.unlock === 'function') {
            try {
                kb.unlock();
            } catch (e) {}
            this._keyboardLocked = false;
        }
        if (document.fullscreenElement) {
            document.exitFullscreen().catch(() => {});
        } else if (this._cssFullscreen) {
            this._exitCssFallbackFullscreen();
        }
    }

    /**
     * Show a 2s transient tip explaining how to exit fullscreen, since Escape
     * is now forwarded to the host. Same transparent style as the install hint.
     * @this {StreamViewInstance}
     */
    _showFullscreenExitHint() {
        const isMac = /Mac/.test(navigator.platform);
        const combo = isMac ? 'Ctrl + Option + Cmd + X' : 'Shift + Ctrl + Alt + X';
        this._showTransientHint(t('stream.exitFullscreenHint', { combo }), 2000);
    }

    /**
     * Display a transparent on-screen hint for `ms`, reusing a single element.
     * Repeated calls reset the text and the auto-dismiss timer (no stacking).
     * @this {StreamViewInstance}
     */
    _showTransientHint(text, ms) {
        let hint = this._transientHintEl;
        if (!hint) {
            hint = document.createElement('div');
            hint.className = 'install-hint';
            // Click/tap to dismiss the hint (e.g. the fullscreen-exit reminder).
            hint.addEventListener('click', (e) => {
                e.stopPropagation();
                hint.classList.remove('install-hint-visible');
                if (this._transientHintTimer) clearTimeout(this._transientHintTimer);
            });
            document.body.appendChild(hint);
            this._transientHintEl = hint;
        }
        hint.textContent = text;
        void hint.offsetWidth;
        hint.classList.add('install-hint-visible');
        if (this._transientHintTimer) clearTimeout(this._transientHintTimer);
        this._transientHintTimer = setTimeout(() => {
            hint.classList.remove('install-hint-visible');
        }, ms);
    }

    /**
     * Exit CSS fallback fullscreen. Restores header and canvas area.
     * @this {StreamViewInstance}
     */
    _exitCssFallbackFullscreen() {
        if (!this._cssFullscreen) return;
        this._cssFullscreen = false;

        // Remove CSS class from the stream-view container
        const streamView = this._rootEl || document.getElementById('stream-view');
        if (streamView) streamView.classList.remove('stream-css-fs');

        // Restore header fullscreen button visibility (per current orientation)
        this._updateMobileFsButtonVisibility();
    }

    /**
     * Exit fullscreen on mobile.
     *
     * Tries both APIs:
     *   1. webkitExitFullscreen() — exits native video player fullscreen (iOS Safari).
     *   2. document.exitFullscreen() — standard Fullscreen API exit.
     *   3. _exitCssFallbackFullscreen() — CSS fallback exit.
     * @this {StreamViewInstance}
     */
    _exitMobileFullscreen() {
        // Also clean up CSS fallback fullscreen if active
        this._exitCssFallbackFullscreen();

        // iOS Safari: exit native video player fullscreen
        if (this.videoEl && typeof this.videoEl.webkitExitFullscreen === 'function') {
            try {
                this.videoEl.webkitExitFullscreen();
            } catch (e) {
                /* silently ignored */
            }
        }

        // Standard Fullscreen API exit
        if (document.fullscreenElement) {
            document.exitFullscreen().catch(() => {});
        }
    }

    /**
     * How the device is being held — not the shape of the viewport.
     *
     * `matchMedia('(orientation: landscape)')` compares viewport width to
     * height, and on iOS the soft keyboard shrinks the layout viewport until a
     * phone held upright measures wider than tall. It then answers "landscape"
     * in portrait, which is exactly how the Fullscreen button turned up on a
     * portrait iPhone with the on-screen keyboard open. `screen.orientation`
     * describes the device, so it is the one to ask; the older
     * `window.orientation` angle covers iOS before 16.4, and the media query
     * is the last resort.
     * @this {StreamViewInstance}
     */
    _isDeviceLandscape() {
        const type = ((window.screen && window.screen.orientation) || {}).type;
        if (typeof type === 'string' && type) return type.startsWith('landscape');
        if (typeof window.orientation === 'number') return Math.abs(window.orientation) === 90;
        return !!(window.matchMedia && window.matchMedia('(orientation: landscape)').matches);
    }

    /**
     * Show/hide the header fullscreen button (placed next to the Stop button).
     *
     * Desktop: always visible (unless already in fullscreen).
     * Mobile/tablet: visible only in landscape — in portrait the user should
     * rotate first (rotation auto-enters fullscreen anyway).
     * @this {StreamViewInstance}
     */
    _updateMobileFsButtonVisibility() {
        if (!this._mobileFsBtn) return;

        const inFullscreen =
            !!document.fullscreenElement ||
            (this.videoEl && this.videoEl.webkitDisplayingFullscreen) ||
            this._cssFullscreen;
        if (inFullscreen) {
            this._mobileFsBtn.style.display = 'none';
        } else if (!IS_MOBILE_OR_TABLET) {
            // Desktop: always available
            this._mobileFsBtn.style.display = '';
        } else {
            this._mobileFsBtn.style.display = this._isDeviceLandscape() ? '' : 'none';
        }

        // Reposition the reminder (header vs top-center).
        this._positionImmersiveOverlay();
    }
}
