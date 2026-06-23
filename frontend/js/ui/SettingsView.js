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
 * Moonlight-Web — Streaming Settings view
 *
 * Streaming-related user preferences (video codec, resolution, bitrate, etc.).
 * Settings are stored in localStorage (per-browser) with default values
 * fetched from the server on first visit.
 *
 * Layout:
 *   - Video section: resolution, FPS, HDR, VSync, bitrate
 *   - Advanced section: video enhancement, codec, performance stats, gaming mode
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { t, getLanguage, setLanguage, AVAILABLE_LANGUAGES } from '../i18n/i18n.js';

/** True when the browser supports touch events (mobile/tablet, or touchscreen laptop). */
const IS_TOUCH_DEVICE =
    'ontouchstart' in window ||
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
        this._streamAspect = 'auto';
        this._streamFps = 60;
        this._hdrEnabled = false;
        this._chroma444 = false;
        this._touchSensitivity = 2.2;
        this._vsync = true;
        // Worker decode mode: 'auto' (heuristic, default), 'on' or 'off' (explicit).
        this._videoWorker = 'auto';
        this._mediaTrackOnlyH264 = false;
        // Video enhancement (WebGPU upscale/sharpen): 'off'|'on' + algo selector.
        // The algo selector is exposed only in debug builds (server reports it);
        // in production the algo is forced to 'auto'.
        this._videoEnhancement = 'off';
        this._videoEnhancementAlgo = 'auto';
        this._debugBuild = false;

        // Power Saving mode (mobile only): forces the lightest pipeline.
        // _powerSaveBackup holds the values present before enabling, so unchecking
        // can restore the ones the user didn't manually change in the meantime.
        this._powerSave = false;
        this._powerSaveBackup = null;

        // Per-codec browser support map: { h264:bool, hevc:bool, av1:bool } or null
        this._codecSupport = null;

        // Debounce timer to avoid rapid repeated saves
        this._saveTimer = null;
    }

    async start() {
        await this._loadState();
        this._codecSupport = await this._checkCodecSupport();
        this._webgpuUsable = await this._checkWebGpuSupport();
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
            this._debugBuild = data.debug_build === true;

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
        this._streamAspect = ['auto', '16:9', '21:9', '32:9'].includes(data.stream_aspect)
            ? data.stream_aspect
            : 'auto';
        this._streamFps = data.stream_fps || 60;
        this._hdrEnabled = data.hdr_enabled === true;
        this._chroma444 = data.chroma_444_enabled === true;
        this._touchSensitivity =
            typeof data.touch_sensitivity === 'number' && data.touch_sensitivity > 0
                ? data.touch_sensitivity
                : 2.2;
        this._vsync = data.vsync_enabled !== false;
        // Back-compat: older saves stored a boolean; map it onto the tri-state.
        const vw = data.video_worker;
        this._videoWorker =
            vw === true || vw === 'on' ? 'on' : vw === false || vw === 'off' ? 'off' : 'auto';
        this._videoEnhancement = data.video_enhancement === 'on' ? 'on' : 'off';
        const algo = data.video_enhancement_algo;
        this._videoEnhancementAlgo =
            algo === 'sgsr' || algo === 'fsr1' || algo === 'force2d' ? algo : 'auto';
        this._powerSave = data.power_save === true;
        this._powerSaveBackup =
            data.power_save_backup && typeof data.power_save_backup === 'object'
                ? data.power_save_backup
                : null;
    }

    /**
     * Recommended bitrate derived from the 1080p@60 SDR reference (20 Mbps):
     *   × pixel-count ratio vs 1080p (e.g. 1440p → 1.78)
     *   × framerate ratio vs 60 fps (e.g. 120 fps → 2.00)
     *   × 1.5 when HDR and/or YUV 4:4:4 is enabled (not cumulative — single 1.5×)
     * Clamped to the slider range [5, 150] Mbps.
     * "Same as Host" (height 0) uses the 1080p reference.
     */
    /** Parse a "W:H" aspect string into a numeric ratio. "auto" / unknown → 16:9
     *  (baseline for the bitrate estimate; the real width is derived from the
     *  host's screen format on the backend at launch). */
    _aspectToNumber(aspect) {
        const m = /^(\d+):(\d+)$/.exec(aspect || '');
        if (m && +m[2] > 0) return +m[1] / +m[2];
        return 16 / 9;
    }

    _computeAutoBitrate(height, fps, aspect, chroma444) {
        const REF_BITRATE = 20; // Mbps at 1920×1080 / 60fps / SDR
        const h = height > 0 ? height : 1080;
        // Pixel count = (h × aspectRatio) × h, normalised to the 1920×1080 ref.
        // Ultrawide (21:9, 32:9) therefore scales the bitrate up accordingly.
        const aspRatio = this._aspectToNumber(aspect);
        const pixelRatio = (h * h * aspRatio) / (1080 * 1080 * (16 / 9));
        const fpsRatio = (fps > 0 ? fps : 60) / 60;
        // 4:4:4 (4× chroma samples) justifies ~1.5× bitrate.
        const richRatio = chroma444 ? 1.5 : 1.0;
        const mbps = Math.round(REF_BITRATE * pixelRatio * fpsRatio * richRatio);
        return Math.max(1, Math.min(150, mbps));
    }

    /** Recompute the bitrate from current selects and sync the slider UI. */
    _applyAutoBitrate() {
        const height = parseInt(this.container.querySelector('#settings-stream-height')?.value, 10);
        const fps = parseInt(this.container.querySelector('#settings-stream-fps')?.value, 10);
        const chroma444 = this.container.querySelector('#settings-chroma-444')?.checked === true;
        const aspect =
            this.container.querySelector('#settings-stream-aspect')?.value || this._streamAspect;
        const mbps = this._computeAutoBitrate(
            isNaN(height) ? this._streamHeight : height,
            isNaN(fps) ? this._streamFps : fps,
            aspect,
            chroma444,
        );

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
            stream_aspect: this._streamAspect,
            stream_fps: this._streamFps,
            hdr_enabled: this._hdrEnabled,
            chroma_444_enabled: this._chroma444,
            touch_sensitivity: this._touchSensitivity,
            vsync_enabled: this._vsync,
            video_worker: this._videoWorker,
            video_enhancement: this._videoEnhancement,
            video_enhancement_algo: this._videoEnhancementAlgo,
            // Per-device only (server ignores these unknown fields).
            power_save: this._powerSave,
            power_save_backup: this._powerSaveBackup,
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
            console.warn(
                '[Settings] VideoDecoder.isConfigSupported not available — ' +
                    'assuming all codecs supported',
            );
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
            'avc1.64002A', // High 4.2
            'avc1.42001E', // Baseline 3.0
            'avc1.64001E', // High 3.0
        ]);
        support.hevc = await testCodec([
            'hev1.1.6.L153.B0', // hev1 used for actual streaming (Annex B fmt)
            'hvc1.1.6.L153.B0', // Main, High tier, Level 5.1
            'hvc1.1.6.L120.B0', // Main, High tier, Level 4.0
            'hvc1.1.2.L153.B0', // Main, Main tier, Level 5.1
        ]);
        support.av1 = await testCodec([
            'av01.0.08M.08', // Main, 1080p, 8-bit
            'av01.0.04M.08', // Main, 720p, 8-bit
            'av01.0.01M.08', // Main, 480p, 8-bit
        ]);

        console.log('[Settings] Browser codec support:', JSON.stringify(support));

        if (!support.h264) {
            console.error('[Settings] CRITICAL: H.264 not supported by this browser');
        }

        return support;
    }

    /**
     * Probe WebGPU usability for the Video Enhancement feature (consumed by the
     * UI graying in a later commit). Probes the adapter, NOT the device — the
     * device is created by the renderer at launch (Canvas2D fallback on failure).
     */
    async _checkWebGpuSupport() {
        try {
            if (!navigator.gpu) return false;
            const adapter = await navigator.gpu.requestAdapter();
            return !!adapter;
        } catch (_) {
            return false;
        }
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
                : this.container.querySelector('#settings-video-codec')?.value || this._videoCodec;
            const gamingMode =
                this.container.querySelector('#settings-gaming-mode')?.checked ?? this._gamingMode;
            const showPerf =
                this.container.querySelector('#settings-show-perf-stats')?.checked ??
                this._showPerformanceStats;
            const bitrateMbps =
                parseInt(this.container.querySelector('#settings-stream-bitrate')?.value, 10) ||
                this._streamBitrateMbps;
            const heightRaw = this.container.querySelector('#settings-stream-height')?.value;
            const height = heightRaw !== undefined ? parseInt(heightRaw, 10) : this._streamHeight;
            const aspect =
                this.container.querySelector('#settings-stream-aspect')?.value ||
                this._streamAspect;
            const fps =
                parseInt(this.container.querySelector('#settings-stream-fps')?.value, 10) ||
                this._streamFps;
            const hdr = this.container.querySelector('#settings-hdr')?.checked ?? this._hdrEnabled;
            const chroma444 =
                this.container.querySelector('#settings-chroma-444')?.checked ?? this._chroma444;
            const sensRaw = parseFloat(
                this.container.querySelector('#settings-sensitivity')?.value,
            );
            const sensitivity = isNaN(sensRaw) ? this._touchSensitivity : sensRaw;
            const vsync = this.container.querySelector('#settings-vsync')?.checked ?? this._vsync;
            const videoWorker =
                this.container.querySelector('#settings-video-worker')?.value ?? this._videoWorker;
            const veCheck = this.container.querySelector('#settings-video-enhancement');
            const videoEnhancement = veCheck
                ? veCheck.checked
                    ? 'on'
                    : 'off'
                : this._videoEnhancement;
            // Algo dropdown only exists in debug builds; production forces 'auto'.
            const veAlgoEl = this.container.querySelector('#settings-video-enhancement-algo');
            const videoEnhancementAlgo = this._debugBuild && veAlgoEl ? veAlgoEl.value : 'auto';

            // Update internal state
            this._videoCodec = codec;
            this._gamingMode = gamingMode;
            this._showPerformanceStats = showPerf;
            this._streamBitrateMbps = bitrateMbps;
            this._streamHeight = isNaN(height) ? this._streamHeight : height;
            this._streamAspect = aspect;
            this._streamFps = fps;
            this._hdrEnabled = hdr;
            this._chroma444 = chroma444;
            this._touchSensitivity = sensitivity;
            this._vsync = vsync;
            this._videoWorker = videoWorker;
            this._videoEnhancement = videoEnhancement;
            this._videoEnhancementAlgo = videoEnhancementAlgo;

            // Save to localStorage and server (if localhost)
            await this._saveToStorage();

            Toast.success(t('settings.saved'));
        }, 300);
    }

    /** Reset all streaming preferences to their default values. */
    async _resetDefaults() {
        this._videoCodec = 'hevc';
        this._gamingMode = true;
        this._showPerformanceStats = false;
        this._streamHeight = 1080;
        this._streamAspect = 'auto';
        this._streamFps = 60;
        this._hdrEnabled = false;
        this._chroma444 = false;
        this._touchSensitivity = 2.2;
        this._vsync = true;
        this._videoWorker = 'auto';
        this._videoEnhancement = 'off';
        this._videoEnhancementAlgo = 'auto';
        this._powerSave = false;
        this._powerSaveBackup = null;
        // Bitrate follows the 1080p60 SDR 16:9 reference
        this._streamBitrateMbps = this._computeAutoBitrate(1080, 60, false, '16:9');

        await this._saveToStorage();

        // Re-render with defaults and re-bind the controls
        this.render();
        this.bindEvents();
        Toast.success(t('settings.settingsReset'));
    }

    /**
     * Toggle Power Saving mode (mobile only). Enabling forces the lightest
     * pipeline (native video / UDP-first transport, H.264, no HDR, no
     * enhancement, VSync on, 720p / 60fps, auto bitrate) and grays out the
     * forced controls. Resolution / FPS / bitrate stay editable.
     *
     * The pre-enable values are snapshotted; disabling restores each one ONLY
     * if it is still at its power-save default (i.e. the user didn't change it).
     */
    async _togglePowerSave(enabled) {
        if (enabled) {
            this._powerSaveBackup = {
                video_codec: this._videoCodec,
                hdr_enabled: this._hdrEnabled,
                chroma_444_enabled: this._chroma444,
                video_enhancement: this._videoEnhancement,
                vsync_enabled: this._vsync,
                stream_height: this._streamHeight,
                stream_fps: this._streamFps,
                stream_bitrate_mbps: this._streamBitrateMbps,
            };
            this._videoCodec = 'h264';
            this._hdrEnabled = false;
            this._chroma444 = false;
            this._videoEnhancement = 'off';
            this._vsync = true;
            this._streamHeight = 720;
            this._streamFps = 60;
            this._streamBitrateMbps = this._computeAutoBitrate(720, 60, false, this._streamAspect);
            this._powerSave = true;
        } else {
            const b = this._powerSaveBackup;
            if (b) {
                const psBitrate = this._computeAutoBitrate(720, 60, false, this._streamAspect);
                // Restore each value only if untouched (still at the power-save default).
                if (this._videoCodec === 'h264') this._videoCodec = b.video_codec;
                if (this._hdrEnabled === false) this._hdrEnabled = b.hdr_enabled;
                if (this._chroma444 === false) this._chroma444 = b.chroma_444_enabled;
                if (this._videoEnhancement === 'off') this._videoEnhancement = b.video_enhancement;
                if (this._vsync === true) this._vsync = b.vsync_enabled;
                if (this._streamHeight === 720) this._streamHeight = b.stream_height;
                if (this._streamFps === 60) this._streamFps = b.stream_fps;
                if (this._streamBitrateMbps === psBitrate)
                    this._streamBitrateMbps = b.stream_bitrate_mbps;
            }
            this._powerSave = false;
            this._powerSaveBackup = null;
        }

        await this._saveToStorage();
        this.render();
        this.bindEvents();
        Toast.success(t('settings.saved'));
    }

    // --- Rendering ---

    render() {
        // Power Saving forces (and grays out) HDR, VSync, codec and enhancement.
        // psDisabled disables the control; psLocked dims the whole field + adds a
        // lock icon so it's clearly read-only (not just subtly greyed).
        const psDisabled = this._powerSave ? ' disabled' : '';
        const psLocked = this._powerSave ? ' settings-field-locked' : '';

        const hdrDisabled = psDisabled;
        const hdrLocked = psLocked;
        const hdrChecked = this._hdrEnabled ? 'checked' : '';

        // Codec options (explicit, no "Auto")
        const codecs = [
            { value: 'h264', label: t('settings.codecH264') },
            { value: 'hevc', label: t('settings.codecHevc') },
            { value: 'av1', label: t('settings.codecAv1') },
        ];
        const effectiveCodec = this._getEffectiveCodec();
        const codecOptions = codecs
            .map((c) => {
                const browserDisabled = this._codecSupport && !this._codecSupport[c.value];
                const mediaTrackDisabled =
                    this._mediaTrackOnlyH264 && (c.value === 'hevc' || c.value === 'av1');
                const disabled = browserDisabled || mediaTrackDisabled;
                const selected = c.value === effectiveCodec ? ' selected' : '';

                let label = c.label;
                if (browserDisabled || mediaTrackDisabled) {
                    label = t('settings.codecUnavailable', { codec: c.value.toUpperCase() });
                }

                return `<option value="${c.value}"${selected}${disabled ? ' disabled' : ''}>${this.esc(label)}</option>`;
            })
            .join('');

        // Warning when codec preference is overridden due to browser support
        const codecChanged =
            this._codecSupport &&
            this._codecSupport[this._videoCodec] === false &&
            effectiveCodec !== this._videoCodec;
        let codecHintHtml = '';
        if (codecChanged) {
            codecHintHtml = `<div class="settings-note">
                ${this.esc(
                    t('settings.codecFallback', {
                        selected: this._videoCodec.toUpperCase(),
                        effective: effectiveCodec.toUpperCase(),
                    }),
                )}
            </div>`;
        }

        // Critical warning when no codec is supported at all
        const noCodecSupported =
            this._codecSupport &&
            !this._codecSupport.h264 &&
            !this._codecSupport.hevc &&
            !this._codecSupport.av1;
        if (noCodecSupported) {
            codecHintHtml = `<div class="settings-status settings-status-pending u-mb-2">
                <strong>${t('settings.noCodecSupportedTitle')}</strong> ${t('settings.noCodecSupportedBody')}
            </div>`;
        }

        // Resolution options (short labels: "1080p")
        const heights = [
            { value: 720, label: '720p' },
            { value: 1080, label: '1080p' },
            { value: 1440, label: '1440p' },
            { value: 2160, label: '2160p' },
        ];
        const heightOptions = heights
            .map(
                (h) =>
                    `<option value="${h.value}" ${h.value === this._streamHeight ? 'selected' : ''}>${this.esc(h.label)}</option>`,
            )
            .join('');

        // FPS options
        const fpsValues = [15, 30, 60, 75, 90, 120, 144, 165, 240];
        const fpsOptions = fpsValues
            .map(
                (f) =>
                    `<option value="${f}" ${f === this._streamFps ? 'selected' : ''}>${this.esc(t('settings.fpsSuffix', { fps: f }))}</option>`,
            )
            .join('');

        // Video Enhancement (WebGPU upscale/sharpen) — grayed out if WebGPU is
        // unavailable, like the per-codec graying.
        const webgpuUnavailable = !this._webgpuUsable;
        const veDisabledAttr = webgpuUnavailable ? ' disabled' : '';
        const veAlgos = [
            { value: 'auto', label: t('settings.algoAuto'), disabled: false },
            { value: 'fsr1', label: t('settings.algoFsr1'), disabled: false },
            { value: 'sgsr', label: t('settings.algoSgsr'), disabled: false },
            { value: 'force2d', label: t('settings.algoForce2d'), disabled: false },
        ];
        const veAlgoOptions = veAlgos
            .map(
                (a) =>
                    `<option value="${a.value}" ${a.value === this._videoEnhancementAlgo ? 'selected' : ''}${a.disabled ? ' disabled' : ''}>${this.esc(a.label)}</option>`,
            )
            .join('');
        const veNote = webgpuUnavailable
            ? `<div class="settings-note">${t('settings.webgpuUnavailable')}</div>`
            : '';

        this.container.innerHTML = `
            <div class="settings-view" id="view-settings">
                <div class="settings-header">
                    <h2>${t('settings.title')}</h2>
                    <button class="view-close-btn" id="btn-settings-close"
                            title="${this.esc(t('common.close'))}">&times;</button>
                </div>

                <!-- ── Video ─────────────────────────────────────────────── -->
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('settings.video')}</h3>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-height">
                            ${t('settings.resolution')}
                        </label>
                        <span class="setting-desc">${t('settings.resolutionDesc')}</span>
                        <select id="settings-stream-height" class="settings-select">
                            ${heightOptions}
                        </select>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-fps">
                            ${t('settings.frameRate')}
                        </label>
                        <span class="setting-desc">${t('settings.frameRateDesc')}</span>
                        <select id="settings-stream-fps" class="settings-select">
                            ${fpsOptions}
                        </select>
                    </div>

                    <!-- HDR: requires HEVC (or AV1 HDR profile) and a WebGPU-capable browser.
                         Shown when the app supports HDR; otherwise grayed out with the noWebgpu message. -->
                    <div class="settings-field${hdrLocked}">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-hdr"
                                ${hdrChecked}${hdrDisabled} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.hdr')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.hdrDesc')}</span>
                    </div>

                    <div class="settings-field${psLocked}">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-vsync"
                                ${this._vsync ? 'checked' : ''}${psDisabled} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.vsync')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.vsyncDesc')}</span>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-bitrate">
                            ${t('settings.bitrate')} <strong id="settings-bitrate-value">${this._streamBitrateMbps}</strong> ${t('settings.bitrateUnit')}
                        </label>
                        <span class="setting-desc">${t('settings.bitrateDesc')}</span>
                        <input type="range" id="settings-stream-bitrate"
                               class="settings-slider"
                               min="1" max="150" step="1"
                               value="${this._streamBitrateMbps}" />
                        <div class="settings-slider-labels">
                            <span>1 Mbps</span>
                            <span>150 Mbps</span>
                        </div>
                    </div>
                </div>

                <!-- ── Advanced ────────────────────────────────────────────── -->
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('settings.advanced')}</h3>

                    <div class="settings-field${psLocked}">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-video-enhancement"
                                ${this._videoEnhancement === 'on' ? 'checked' : ''}${veDisabledAttr}${psDisabled} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.videoEnhancement')}${webgpuUnavailable ? t('settings.unavailableSuffix') : ''}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.videoEnhancementDesc')}</span>
                        ${
                            this._debugBuild
                                ? `<select id="settings-video-enhancement-algo" class="settings-select u-mt-2"${veDisabledAttr}>
                            ${veAlgoOptions}
                        </select>`
                                : ''
                        }
                        ${veNote}
                    </div>

                    <div class="settings-field${psLocked}">
                        <label class="settings-label" for="settings-video-codec">
                            ${t('settings.videoCodec')}
                        </label>
                        <span class="setting-desc">${t('settings.videoCodecDesc')}</span>
                        <select id="settings-video-codec" class="settings-select"${psDisabled}>
                            ${codecOptions}
                        </select>
                        ${codecHintHtml}
                    </div>

                    <div class="settings-field${psLocked}">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-chroma-444"
                                ${this._chroma444 ? 'checked' : ''}${psDisabled} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.chroma444')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.chroma444Desc')}</span>
                    </div>

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-show-perf-stats"
                                ${this._showPerformanceStats ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.showPerfStats')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.showPerfStatsDesc')}</span>
                    </div>

                    ${
                        IS_TOUCH_DEVICE
                            ? ''
                            : `
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-gaming-mode"
                                ${this._gamingMode ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.gamingMode')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.gamingModeDesc')}</span>
                    </div>`
                    }

                    <!-- Hidden: too technical for the average user. Kept in the DOM
                         (display:none) as a debug override; defaults to 'auto'. -->
                    <div class="settings-field u-hidden">
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
                            ${t('settings.pointerSensitivity')} <strong id="settings-sensitivity-value">${this._touchSensitivity.toFixed(1)}</strong>×
                        </label>
                        <span class="setting-desc">${t('settings.pointerSensitivityDesc')}</span>
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

                <!-- ── Language ────────────────────────────────────────────── -->
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('settings.language')}</h3>
                    <div class="settings-field">
                        <select id="settings-language" class="settings-select">
                            ${AVAILABLE_LANGUAGES.map(
                                (l) =>
                                    `<option value="${l.code}" ${l.code === getLanguage() ? 'selected' : ''}>${this.esc(l.label)}</option>`,
                            ).join('')}
                        </select>
                        <span class="setting-desc">${t('settings.languageDesc')}</span>
                    </div>
                </div>

                ${
                    IS_TOUCH_DEVICE
                        ? `
                <!-- ── Power Saving (mobile only, isolated above Reset) ──────── -->
                <div class="settings-section settings-section-powersave">
                    <h3 class="settings-section-title">${t('settings.powerSave')}</h3>
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-power-save"
                                ${this._powerSave ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.powerSaveToggle')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.powerSaveDesc')}</span>
                    </div>
                </div>`
                        : ''
                }

                <!-- ── Reset ───────────────────────────────────────────────── -->
                <div class="settings-section">
                    <button class="btn btn-neutral" id="btn-settings-reset">
                        ${t('settings.resetDefaults')}
                    </button>
                    <span class="setting-desc">${t('settings.resetDefaultsDesc')}</span>
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
        if (heightSelect)
            heightSelect.addEventListener('change', () => {
                this._applyAutoBitrate();
                this._autoSave();
            });

        const fpsSelect = this.container.querySelector('#settings-stream-fps');
        if (fpsSelect)
            fpsSelect.addEventListener('change', () => {
                this._applyAutoBitrate();
                this._autoSave();
            });

        const hdrCheck = this.container.querySelector('#settings-hdr');
        if (hdrCheck)
            hdrCheck.addEventListener('change', () => {
                this._applyAutoBitrate();
                this._autoSave();
            });

        // 4:4:4 chroma bumps the recommended bitrate (higher bandwidth) before saving.
        const chroma444Check = this.container.querySelector('#settings-chroma-444');
        if (chroma444Check)
            chroma444Check.addEventListener('change', () => {
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

        const veSelect = this.container.querySelector('#settings-video-enhancement');
        if (veSelect) veSelect.addEventListener('change', () => this._autoSave());

        const veAlgoSelect = this.container.querySelector('#settings-video-enhancement-algo');
        if (veAlgoSelect) veAlgoSelect.addEventListener('change', () => this._autoSave());

        const sensSlider = this.container.querySelector('#settings-sensitivity');
        if (sensSlider) sensSlider.addEventListener('change', () => this._autoSave());

        // Language selector — changing it persists the choice and reloads.
        const langSelect = this.container.querySelector('#settings-language');
        if (langSelect) langSelect.addEventListener('change', () => setLanguage(langSelect.value));

        const powerSaveCheck = this.container.querySelector('#settings-power-save');
        if (powerSaveCheck)
            powerSaveCheck.addEventListener('change', () =>
                this._togglePowerSave(powerSaveCheck.checked),
            );

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
