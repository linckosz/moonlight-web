/**
 * Moonlight-Web — Streaming Settings view
 *
 * Streaming-related user preferences (video codec, resolution, bitrate, etc.).
 * Settings are stored in localStorage (per-browser) with default values
 * fetched from the server on first visit.
 *
 * Layout:
 *   - Video section: codec, resolution, FPS, bitrate (grouped)
 *   - Advanced section: gaming mode, performance stats
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';

/** True when the browser supports touch events (mobile/tablet, or touchscreen laptop). */
const IS_TOUCH_DEVICE = 'ontouchstart' in window ||
    (typeof navigator.maxTouchPoints !== 'undefined' && navigator.maxTouchPoints > 0);

const STORAGE_KEY = 'mw-streaming-settings';

export class SettingsView {
    constructor(container, onClose) {
        this.container = container;
        this.onClose = onClose || (() => {});

        this._videoCodec = 'hevc';
        this._gamingMode = true;
        this._showPerformanceStats = false;
        this._streamBitrateMbps = 20;
        this._streamHeight = 1080;
        this._streamFps = 60;
        this._hdrEnabled = false;
        this._touchSensitivity = 2.0;
        this._vsync = true;
        // Worker decode mode: 'auto' (heuristic, default), 'on' or 'off' (explicit).
        this._videoWorker = 'auto';
        this._mediaTrackOnlyH264 = false;

        // Per-codec browser support map: { h264:bool, hevc:bool, av1:bool } or null
        this._codecSupport = null;

        // Debounce timer to avoid rapid repeated saves
        this._saveTimer = null;
    }

    async start() {
        await this._loadState();
        this._codecSupport = await this._checkCodecSupport();
        this.render();
        this.bindEvents();
    }

    async _loadState() {
        // 1. Try localStorage first (per-browser preferences)
        const stored = localStorage.getItem(STORAGE_KEY);
        if (stored) {
            try {
                const data = JSON.parse(stored);
                this._applySettings(data);
            } catch (err) {
                console.warn('[Settings] Failed to parse localStorage, falling back to server');
            }
        }

        // 2. Fetch server settings.
        //    media_track_only_h264 is a server-enforced constraint, not a user preference.
        //    Always use the server's value, overriding localStorage.
        //    On first visit (no localStorage), also load all defaults from the server.
        try {
            const data = await BackendClient.getStreamingSettings();
            this._mediaTrackOnlyH264 = data.media_track_only_h264 === true;

            if (!stored) {
                this._applySettings(data);
                await this._saveToStorage();
            }
        } catch (err) {
            if (!stored) {
                console.warn('[Settings] Failed to load streaming settings:', err);
            } else {
                console.warn('[Settings] Failed to fetch media_track_only_h264 from server:', err);
            }
        }
    }

    /** Apply a settings object to internal state (normalises field names). */
    _applySettings(data) {
        this._videoCodec = data.video_codec || 'hevc';
        this._mediaTrackOnlyH264 = data.media_track_only_h264 === true;
        this._gamingMode = data.gaming_mode !== false;
        this._showPerformanceStats = data.show_performance_stats === true;
        const kbps = data.stream_bitrate || 20000;
        this._streamBitrateMbps = Math.round(kbps / 1000);
        this._streamHeight = data.stream_height || 1080;
        this._streamFps = data.stream_fps || 60;
        this._hdrEnabled = data.hdr_enabled === true;
        this._touchSensitivity = typeof data.touch_sensitivity === 'number' && data.touch_sensitivity > 0
            ? data.touch_sensitivity : 2.0;
        this._vsync = data.vsync_enabled !== false;
        // Back-compat: older saves stored a boolean; map it onto the tri-state.
        const vw = data.video_worker;
        this._videoWorker = (vw === true || vw === 'on') ? 'on'
            : (vw === false || vw === 'off') ? 'off' : 'auto';
    }

    /**
     * Recommended bitrate derived from the 1080p@60 SDR reference (20 Mbps):
     *   × pixel-count ratio vs 1080p (e.g. 1440p → 1.78)
     *   × framerate ratio vs 60 fps (e.g. 120 fps → 2.00)
     *   × 1.5 when HDR is enabled
     * Clamped to the slider range [5, 150] Mbps.
     * "Same as Host" (height 0) uses the 1080p reference.
     */
    _computeAutoBitrate(height, fps, hdr) {
        const REF_BITRATE = 20;   // Mbps at 1080p / 60fps / SDR
        const h = height > 0 ? height : 1080;
        // 16:9 assumed: pixel count scales with the square of the height ratio
        const pixelRatio = (h / 1080) * (h / 1080);
        const fpsRatio = (fps > 0 ? fps : 60) / 60;
        const hdrRatio = hdr ? 1.5 : 1.0;
        const mbps = Math.round(REF_BITRATE * pixelRatio * fpsRatio * hdrRatio);
        return Math.max(5, Math.min(150, mbps));
    }

    /** Recompute the bitrate from current selects and sync the slider UI. */
    _applyAutoBitrate() {
        const height = parseInt(this.container.querySelector('#settings-stream-height')?.value, 10);
        const fps = parseInt(this.container.querySelector('#settings-stream-fps')?.value, 10);
        const hdr = this.container.querySelector('#settings-hdr')?.checked === true;
        const mbps = this._computeAutoBitrate(
            isNaN(height) ? this._streamHeight : height,
            isNaN(fps) ? this._streamFps : fps,
            hdr);

        const slider = this.container.querySelector('#settings-stream-bitrate');
        const label = this.container.querySelector('#settings-bitrate-value');
        if (slider) slider.value = mbps;
        if (label) label.textContent = mbps;
        this._streamBitrateMbps = mbps;
    }

    /** Persist current internal state to localStorage (all clients)
     *  and to the server (localhost only — updates defaults for all browsers). */
    async _saveToStorage() {
        const settings = {
            video_codec: this._videoCodec,
            gaming_mode: this._gamingMode,
            show_performance_stats: this._showPerformanceStats,
            stream_bitrate: this._streamBitrateMbps * 1000,
            stream_height: this._streamHeight,
            stream_fps: this._streamFps,
            hdr_enabled: this._hdrEnabled,
            touch_sensitivity: this._touchSensitivity,
            vsync_enabled: this._vsync,
            video_worker: this._videoWorker
        };
        localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));

        // Persist to server when on localhost, so settings.json is updated
        // and all other clients get these defaults on first visit.
        const host = window.location.hostname;
        if (host === 'localhost' || host === '127.0.0.1') {
            try {
                await BackendClient.saveStreamingSettings(settings);
            } catch (err) {
                console.warn('[Settings] Failed to save to server:', err);
            }
        }
    }

    /**
     * Test browser codec support via VideoDecoder.isConfigSupported() for H.264,
     * HEVC and AV1. Uses minimal codec strings (no bitstream description needed).
     *
     * Each codec is tested with a list of fallback codec strings. The codec is
     * marked supported if ANY string in its list returns supported=true.
     *
     * If VideoDecoder.isConfigSupported is not available (old browser), all
     * codecs are assumed supported (graceful fallback).
     */
    async _checkCodecSupport() {
        const support = { h264: false, hevc: false, av1: false };

        if (typeof VideoDecoder?.isConfigSupported !== 'function') {
            console.warn('[Settings] VideoDecoder.isConfigSupported not available — ' +
                'assuming all codecs supported');
            support.h264 = true;
            support.hevc = true;
            support.av1 = true;
            return support;
        }

        // Test a list of codec strings, return true if ANY is supported.
        const testCodec = async (codecs) => {
            for (const codec of codecs) {
                try {
                    const result = await VideoDecoder.isConfigSupported({ codec });
                    if (result?.supported) return true;
                } catch (_) {
                    // Individual codec string rejected — try next fallback
                }
            }
            return false;
        };

        support.h264 = await testCodec([
            'avc1.64002A',  // High 4.2
            'avc1.42001E',  // Baseline 3.0
            'avc1.64001E',  // High 3.0
        ]);
        support.hevc = await testCodec([
            'hev1.1.6.L153.B0',  // hev1 used for actual streaming (Annex B fmt)
            'hvc1.1.6.L153.B0',  // Main, High tier, Level 5.1
            'hvc1.1.6.L120.B0',  // Main, High tier, Level 4.0
            'hvc1.1.2.L153.B0',  // Main, Main tier, Level 5.1
        ]);
        support.av1  = await testCodec([
            'av01.0.08M.08',    // Main, 1080p, 8-bit
            'av01.0.04M.08',    // Main, 720p, 8-bit
            'av01.0.01M.08',    // Main, 480p, 8-bit
        ]);

        console.log('[Settings] Browser codec support:', JSON.stringify(support));

        if (!support.h264) {
            console.error('[Settings] CRITICAL: H.264 not supported by this browser');
        }

        return support;
    }

    /**
     * Return the effective codec to display in the dropdown, considering:
     * 1. MediaTrack transport forces H.264
     * 2. Browser codec support (preferred codec may be unsupported)
     * 3. Fallback chain: h264 > hevc > av1
     *
     * Does NOT modify this._videoCodec (user preference remains in storage).
     */
    _getEffectiveCodec() {
        if (this._mediaTrackOnlyH264) return 'h264';
        if (!this._codecSupport) return this._videoCodec;
        if (this._codecSupport[this._videoCodec]) return this._videoCodec;
        if (this._codecSupport.h264) return 'h264';
        if (this._codecSupport.hevc) return 'hevc';
        if (this._codecSupport.av1) return 'av1';
        return this._videoCodec;
    }

    destroy() {
        if (this._saveTimer) {
            clearTimeout(this._saveTimer);
            this._saveTimer = null;
        }
    }

    // --- Auto-save ---

    _autoSave() {
        if (this._saveTimer) {
            clearTimeout(this._saveTimer);
        }

        this._saveTimer = setTimeout(async () => {
            this._saveTimer = null;

            // When MediaTrack transport is forced, only H.264 is available
            const codec = this._mediaTrackOnlyH264
                ? 'h264'
                : (this.container.querySelector('#settings-video-codec')?.value || this._videoCodec);
            const gamingMode = this.container.querySelector('#settings-gaming-mode')?.checked ?? this._gamingMode;
            const showPerf = this.container.querySelector('#settings-show-perf-stats')?.checked ?? this._showPerformanceStats;
            const bitrateMbps = parseInt(this.container.querySelector('#settings-stream-bitrate')?.value, 10) || this._streamBitrateMbps;
            const heightRaw = this.container.querySelector('#settings-stream-height')?.value;
            const height = heightRaw !== undefined ? parseInt(heightRaw, 10) : this._streamHeight;
            const fps = parseInt(this.container.querySelector('#settings-stream-fps')?.value, 10) || this._streamFps;
            const hdr = this.container.querySelector('#settings-hdr')?.checked ?? this._hdrEnabled;
            const sensRaw = parseFloat(this.container.querySelector('#settings-sensitivity')?.value);
            const sensitivity = isNaN(sensRaw) ? this._touchSensitivity : sensRaw;
            const vsync = this.container.querySelector('#settings-vsync')?.checked ?? this._vsync;
            const videoWorker = this.container.querySelector('#settings-video-worker')?.value ?? this._videoWorker;

            // Update internal state
            this._videoCodec = codec;
            this._gamingMode = gamingMode;
            this._showPerformanceStats = showPerf;
            this._streamBitrateMbps = bitrateMbps;
            this._streamHeight = isNaN(height) ? this._streamHeight : height;
            this._streamFps = fps;
            this._hdrEnabled = hdr;
            this._touchSensitivity = sensitivity;
            this._vsync = vsync;
            this._videoWorker = videoWorker;

            // Save to localStorage and server (if localhost)
            await this._saveToStorage();

            Toast.success('Saved');
        }, 300);
    }

    /** Reset all streaming preferences to their default values. */
    async _resetDefaults() {
        this._videoCodec = 'hevc';
        this._gamingMode = true;
        this._showPerformanceStats = false;
        this._streamHeight = 1080;
        this._streamFps = 60;
        this._hdrEnabled = false;
        this._touchSensitivity = 2.0;
        this._vsync = true;
        this._videoWorker = 'auto';
        // Bitrate follows the 1080p60 SDR reference
        this._streamBitrateMbps = this._computeAutoBitrate(1080, 60, false);

        await this._saveToStorage();

        // Re-render with defaults and re-bind the controls
        this.render();
        this.bindEvents();
        Toast.success('Settings reset to defaults');
    }

    // --- Rendering ---

    render() {
        // Codec options (explicit, no "Auto")
        const codecs = [
            { value: 'h264', label: 'H.264 (Wide compatibility)' },
            { value: 'hevc', label: 'HEVC (Efficient compression, recommended)' },
            { value: 'av1',  label: 'AV1 (Best compression for modern GPUs)' }
        ];
        const effectiveCodec = this._getEffectiveCodec();
        const codecOptions = codecs.map(c => {
            const browserDisabled = this._codecSupport && !this._codecSupport[c.value];
            const mediaTrackDisabled = this._mediaTrackOnlyH264 &&
                (c.value === 'hevc' || c.value === 'av1');
            const disabled = browserDisabled || mediaTrackDisabled;
            const selected = c.value === effectiveCodec ? ' selected' : '';

            let label = c.label;
            if (browserDisabled) {
                label = `${c.value.toUpperCase()} (unavailable)`;
            } else if (mediaTrackDisabled) {
                label = `${c.value.toUpperCase()} (unavailable)`;
            }

            return `<option value="${c.value}"${selected}${disabled ? ' disabled' : ''}>${this.esc(label)}</option>`;
        }).join('');

        // Warning when codec preference is overridden due to browser support
        const codecChanged = this._codecSupport &&
            this._codecSupport[this._videoCodec] === false &&
            effectiveCodec !== this._videoCodec;
        let codecHintHtml = '';
        if (codecChanged) {
            codecHintHtml = `<div class="settings-note">
                ${this._videoCodec.toUpperCase()} was selected but is not supported
                by this browser. Falling back to ${effectiveCodec.toUpperCase()}.
            </div>`;
        }

        // Critical warning when no codec is supported at all
        const noCodecSupported = this._codecSupport &&
            !this._codecSupport.h264 && !this._codecSupport.hevc && !this._codecSupport.av1;
        if (noCodecSupported) {
            codecHintHtml = `<div class="settings-status settings-status-pending" style="margin-bottom:8px">
                <strong>No codec supported.</strong> This browser does not support H.264,
                HEVC, or AV1 decoding. Streaming is not possible.
            </div>`;
        }

        // Resolution options (short labels: "1080p")
        const heights = [
            { value: 720,  label: '720p' },
            { value: 1080, label: '1080p' },
            { value: 1440, label: '1440p' },
            { value: 2160, label: '2160p' },
            { value: 0,    label: 'Same as Host' }
        ];
        const heightOptions = heights.map(h =>
            `<option value="${h.value}" ${h.value === this._streamHeight ? 'selected' : ''}>${this.esc(h.label)}</option>`
        ).join('');

        // FPS options
        const fpsValues = [1, 30, 60, 75, 90, 120, 144, 165, 240];
        const fpsOptions = fpsValues.map(f =>
            `<option value="${f}" ${f === this._streamFps ? 'selected' : ''}>${f} FPS</option>`
        ).join('');

        this.container.innerHTML = `
            <div class="settings-view" id="view-settings">
                <div class="settings-header">
                    <h2>Streaming Settings</h2>
                    <button class="view-close-btn" id="btn-settings-close"
                            title="Close">&times;</button>
                </div>

                <!-- ── Video ─────────────────────────────────────────────── -->
                <div class="settings-section">
                    <h3 class="settings-section-title">Video</h3>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-video-codec">
                            Video Codec
                        </label>
                        <span class="setting-desc">HEVC for best balance of quality and compatibility</span>
                        <select id="settings-video-codec" class="settings-select">
                            ${codecOptions}
                        </select>
                        ${codecHintHtml}
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-height">
                            Streamed Resolution
                        </label>
                        <span class="setting-desc">Higher resolution needs more bandwidth and GPU power</span>
                        <select id="settings-stream-height" class="settings-select">
                            ${heightOptions}
                        </select>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-fps">
                            Frame Rate
                        </label>
                        <span class="setting-desc">Higher frame rates provide smoother motion but need more bandwidth</span>
                        <select id="settings-stream-fps" class="settings-select">
                            ${fpsOptions}
                        </select>
                    </div>

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-hdr"
                                ${this._hdrEnabled ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>HDR</strong>
                            </span>
                        </label>
                        <span class="setting-desc">High Dynamic Range — richer colors and contrast (coming soon, not yet applied to the video stream)</span>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-bitrate">
                            Bitrate: <strong id="settings-bitrate-value">${this._streamBitrateMbps}</strong> Mbps
                        </label>
                        <span class="setting-desc">Auto-adjusted from resolution, frame rate and HDR (reference: 20 Mbps for 1080p 60fps SDR) — drag to override</span>
                        <input type="range" id="settings-stream-bitrate"
                               class="settings-slider"
                               min="5" max="150" step="1"
                               value="${this._streamBitrateMbps}" />
                        <div class="settings-slider-labels">
                            <span>5 Mbps</span>
                            <span>150 Mbps</span>
                        </div>
                    </div>
                </div>

                <!-- ── Advanced ────────────────────────────────────────────── -->
                <div class="settings-section">
                    <h3 class="settings-section-title">Advanced</h3>

                    ${IS_TOUCH_DEVICE ? '' : `
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-gaming-mode"
                                ${this._gamingMode ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>Mouse Gaming Mode</strong>
                            </span>
                        </label>
                        <span class="setting-desc">Locks mouse pointer for seamless camera control in games</span>
                    </div>`}

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-show-perf-stats"
                                ${this._showPerformanceStats ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>Show Performance Stats</strong>
                            </span>
                        </label>
                        <span class="setting-desc">Overlays FPS, bitrate, frame loss and latency stats during streaming</span>
                    </div>

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-vsync"
                                ${this._vsync ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>VSync</strong>
                            </span>
                        </label>
                        <span class="setting-desc">Synchronizes frames to the display refresh. Uncheck to allow tearing for lower latency (all transports)</span>
                    </div>

                    <!-- Hidden: too technical for the average user. Kept in the DOM
                         (display:none) as a debug override; defaults to 'auto'. -->
                    <div class="settings-field" style="display:none">
                        <label class="settings-label" for="settings-video-worker">Decode on worker thread</label>
                        <select id="settings-video-worker" class="settings-select">
                            <option value="auto" ${this._videoWorker === 'auto' ? 'selected' : ''}>Auto (by core count)</option>
                            <option value="on" ${this._videoWorker === 'on' ? 'selected' : ''}>On</option>
                            <option value="off" ${this._videoWorker === 'off' ? 'selected' : ''}>Off</option>
                        </select>
                        <span class="setting-desc">Decodes &amp; renders video off the UI thread (OffscreenCanvas). <strong>Auto</strong> enables it above 4 logical cores. Falls back automatically if unsupported. DataChannel/WSS transports only.</span>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-sensitivity">
                            Pointer Sensitivity: <strong id="settings-sensitivity-value">${this._touchSensitivity.toFixed(1)}</strong>×
                        </label>
                        <span class="setting-desc">Trackpad/touch cursor speed (higher = faster pointer movement)</span>
                        <input type="range" id="settings-sensitivity"
                               class="settings-slider"
                               min="0.5" max="5" step="0.1"
                               value="${this._touchSensitivity}" />
                        <div class="settings-slider-labels">
                            <span>0.5×</span>
                            <span>5×</span>
                        </div>
                    </div>
                </div>

                <!-- ── Reset ───────────────────────────────────────────────── -->
                <div class="settings-section">
                    <button class="btn btn-neutral" id="btn-settings-reset">
                        Reset Default Parameters
                    </button>
                    <span class="setting-desc">Restore all streaming settings to their default values</span>
                </div>
            </div>
        `;

        // Live bitrate label updates on slider input
        const slider = this.container.querySelector('#settings-stream-bitrate');
        const label = this.container.querySelector('#settings-bitrate-value');
        if (slider && label) {
            slider.addEventListener('input', () => {
                label.textContent = slider.value;
            });
            this._dragOnlySlider(slider);
        }

        // Live sensitivity label updates while dragging
        const sensSlider = this.container.querySelector('#settings-sensitivity');
        const sensLabel = this.container.querySelector('#settings-sensitivity-value');
        if (sensSlider && sensLabel) {
            sensSlider.addEventListener('input', () => {
                sensLabel.textContent = parseFloat(sensSlider.value).toFixed(1);
            });
            this._dragOnlySlider(sensSlider);
        }
    }

    // Block click-to-jump on the track: only a press on the thumb starts a drag.
    _dragOnlySlider(slider) {
        const THUMB = 20; // thumb diameter (CSS .settings-slider::-..-thumb)
        slider.addEventListener('pointerdown', (e) => {
            const rect = slider.getBoundingClientRect();
            const min = parseFloat(slider.min) || 0;
            const max = parseFloat(slider.max) || 100;
            const ratio = (parseFloat(slider.value) - min) / (max - min);
            const track = rect.width - THUMB;
            const thumbCenter = rect.left + THUMB / 2 + ratio * track;
            // Outside the thumb → cancel the native jump (drag from thumb still works).
            if (Math.abs(e.clientX - thumbCenter) > THUMB / 2) {
                e.preventDefault();
            }
        });
    }

    bindEvents() {
        const codecSelect = this.container.querySelector('#settings-video-codec');
        if (codecSelect) codecSelect.addEventListener('change', () => this._autoSave());

        // Resolution / FPS / HDR changes recompute the recommended bitrate
        // from the 1080p60 SDR reference before saving.
        const heightSelect = this.container.querySelector('#settings-stream-height');
        if (heightSelect) heightSelect.addEventListener('change', () => {
            this._applyAutoBitrate();
            this._autoSave();
        });

        const fpsSelect = this.container.querySelector('#settings-stream-fps');
        if (fpsSelect) fpsSelect.addEventListener('change', () => {
            this._applyAutoBitrate();
            this._autoSave();
        });

        const hdrCheck = this.container.querySelector('#settings-hdr');
        if (hdrCheck) hdrCheck.addEventListener('change', () => {
            this._applyAutoBitrate();
            this._autoSave();
        });

        const bitrateSlider = this.container.querySelector('#settings-stream-bitrate');
        if (bitrateSlider) bitrateSlider.addEventListener('change', () => this._autoSave());

        const gamingCheck = this.container.querySelector('#settings-gaming-mode');
        if (gamingCheck) gamingCheck.addEventListener('change', () => this._autoSave());

        const perfCheck = this.container.querySelector('#settings-show-perf-stats');
        if (perfCheck) perfCheck.addEventListener('change', () => this._autoSave());

        const vsyncCheck = this.container.querySelector('#settings-vsync');
        if (vsyncCheck) vsyncCheck.addEventListener('change', () => this._autoSave());

        const workerCheck = this.container.querySelector('#settings-video-worker');
        if (workerCheck) workerCheck.addEventListener('change', () => this._autoSave());

        const sensSlider = this.container.querySelector('#settings-sensitivity');
        if (sensSlider) sensSlider.addEventListener('change', () => this._autoSave());

        const resetBtn = this.container.querySelector('#btn-settings-reset');
        if (resetBtn) resetBtn.addEventListener('click', () => this._resetDefaults());

        const closeBtn = this.container.querySelector('#btn-settings-close');
        if (closeBtn) closeBtn.addEventListener('click', () => this.onClose());
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
