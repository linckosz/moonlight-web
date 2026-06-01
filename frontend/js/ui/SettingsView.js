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
            stream_fps: this._streamFps
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
            const height = parseInt(this.container.querySelector('#settings-stream-height')?.value, 10) || this._streamHeight;
            const fps = parseInt(this.container.querySelector('#settings-stream-fps')?.value, 10) || this._streamFps;

            // Update internal state
            this._videoCodec = codec;
            this._gamingMode = gamingMode;
            this._showPerformanceStats = showPerf;
            this._streamBitrateMbps = bitrateMbps;
            this._streamHeight = height;
            this._streamFps = fps;

            // Save to localStorage and server (if localhost)
            await this._saveToStorage();

            Toast.success('Saved');
        }, 300);
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
        const fpsValues = [30, 60, 75, 90, 120, 144, 165, 240];
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
                        <label class="settings-label" for="settings-stream-bitrate">
                            Bitrate: <strong id="settings-bitrate-value">${this._streamBitrateMbps}</strong> Mbps
                        </label>
                        <span class="setting-desc">Higher bitrate = better quality but more network bandwidth</span>
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

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-gaming-mode"
                                ${this._gamingMode ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>Mouse Gaming Mode</strong>
                            </span>
                        </label>
                        <span class="setting-desc">Locks mouse pointer for seamless camera control in games</span>
                    </div>

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
        }
    }

    bindEvents() {
        const codecSelect = this.container.querySelector('#settings-video-codec');
        if (codecSelect) codecSelect.addEventListener('change', () => this._autoSave());

        const heightSelect = this.container.querySelector('#settings-stream-height');
        if (heightSelect) heightSelect.addEventListener('change', () => this._autoSave());

        const fpsSelect = this.container.querySelector('#settings-stream-fps');
        if (fpsSelect) fpsSelect.addEventListener('change', () => this._autoSave());

        const bitrateSlider = this.container.querySelector('#settings-stream-bitrate');
        if (bitrateSlider) bitrateSlider.addEventListener('change', () => this._autoSave());

        const gamingCheck = this.container.querySelector('#settings-gaming-mode');
        if (gamingCheck) gamingCheck.addEventListener('change', () => this._autoSave());

        const perfCheck = this.container.querySelector('#settings-show-perf-stats');
        if (perfCheck) perfCheck.addEventListener('change', () => this._autoSave());

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
