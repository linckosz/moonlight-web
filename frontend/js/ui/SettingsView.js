/**
 * Moonlight-Web — Streaming Settings view
 *
 * Streaming-related user preferences (video codec, resolution, bitrate, etc.).
 * All settings are stored server-side per the project principle.
 *
 * Layout:
 *   - Video section: codec, resolution, FPS, bitrate (grouped)
 *   - Advanced section: gaming mode, performance stats
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';

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

        // Debounce timer to avoid rapid repeated saves
        this._saveTimer = null;
    }

    async start() {
        await this._loadState();
        this.render();
        this.bindEvents();
    }

    async _loadState() {
        try {
            const data = await BackendClient.getStreamingSettings();
            this._videoCodec = data.video_codec || 'hevc';
            this._gamingMode = data.gaming_mode === true;
            this._showPerformanceStats = data.show_performance_stats === true;

            const kbps = data.stream_bitrate || 20000;
            this._streamBitrateMbps = Math.round(kbps / 1000);

            this._streamHeight = data.stream_height || 1080;
            this._streamFps = data.stream_fps || 60;
        } catch (err) {
            console.warn('[Settings] Failed to load streaming settings:', err);
        }
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

            const codec = this.container.querySelector('#settings-video-codec')?.value || this._videoCodec;
            const gamingMode = this.container.querySelector('#settings-gaming-mode')?.checked ?? this._gamingMode;
            const showPerf = this.container.querySelector('#settings-show-perf-stats')?.checked ?? this._showPerformanceStats;
            const bitrateMbps = parseInt(this.container.querySelector('#settings-stream-bitrate')?.value, 10) || this._streamBitrateMbps;
            const height = parseInt(this.container.querySelector('#settings-stream-height')?.value, 10) || this._streamHeight;
            const fps = parseInt(this.container.querySelector('#settings-stream-fps')?.value, 10) || this._streamFps;

            try {
                const result = await BackendClient.saveStreamingSettings({
                    video_codec: codec,
                    gaming_mode: gamingMode,
                    show_performance_stats: showPerf,
                    stream_bitrate: bitrateMbps * 1000,
                    stream_height: height,
                    stream_fps: fps
                });
                if (result.status === 'saved') {
                    this._videoCodec = result.video_codec || this._videoCodec;
                    this._gamingMode = result.gaming_mode !== false;
                    this._showPerformanceStats = result.show_performance_stats === true;
                    if (result.stream_bitrate !== undefined)
                        this._streamBitrateMbps = Math.round(result.stream_bitrate / 1000);
                    if (result.stream_height !== undefined)
                        this._streamHeight = result.stream_height;
                    if (result.stream_fps !== undefined)
                        this._streamFps = result.stream_fps;
                    Toast.success('Saved');
                }
            } catch (err) {
                console.error('[Settings] Auto-save failed:', err);
                Toast.error('Save failed: ' + err.message);
            }
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
        const codecOptions = codecs.map(c =>
            `<option value="${c.value}" ${c.value === this._videoCodec ? 'selected' : ''}>${this.esc(c.label)}</option>`
        ).join('');

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
