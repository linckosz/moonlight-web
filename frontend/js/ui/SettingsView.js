/**
 * Moonlight-Web — Streaming Settings view
 *
 * Streaming-related user preferences (video codec, etc.).
 * All settings are stored server-side per the project principle.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';

export class SettingsView {
    constructor(container, onClose) {
        this.container = container;
        this.onClose = onClose || (() => {});

        this._videoCodec = 'auto';
        this._gamingMode = true;
        this._transport = 'webrtc';

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
            this._videoCodec = data.video_codec || 'auto';
            this._gamingMode = data.gaming_mode !== false;
            this._transport = data.transport || 'webrtc';
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
        // Debounce: cancel any pending save
        if (this._saveTimer) {
            clearTimeout(this._saveTimer);
        }

        const codecSelect = this.container.querySelector('#settings-video-codec');
        const gamingCheck = this.container.querySelector('#settings-gaming-mode');
        const transportSelect = this.container.querySelector('#settings-transport');
        if (!codecSelect || !gamingCheck || !transportSelect) return;

        this._saveTimer = setTimeout(async () => {
            this._saveTimer = null;

            const codec = codecSelect.value;
            const gamingMode = gamingCheck.checked;
            const transport = transportSelect.value;

            try {
                const result = await BackendClient.saveStreamingSettings({
                    video_codec: codec,
                    gaming_mode: gamingMode,
                    transport: transport
                });
                if (result.status === 'saved') {
                    this._videoCodec = result.video_codec || this._videoCodec;
                    this._gamingMode = result.gaming_mode !== false;
                    this._transport = result.transport || this._transport;
                    // Subtle toast feedback
                    Toast.success('Saved');
                }
            } catch (err) {
                console.error('[Settings] Auto-save failed:', err);
                Toast.error('Save failed: ' + err.message);
            }
        }, 300); // 300ms debounce
    }

    // --- Rendering ---

    render() {
        const codecs = [
            { value: 'auto', label: 'Auto (let Sunshine decide)' },
            { value: 'h264', label: 'H.264 (widest compatibility)' },
            { value: 'hevc', label: 'H.265 / HEVC (better compression)' },
            { value: 'av1',  label: 'AV1 (best compression, newer hardware)' }
        ];

        const codecOptions = codecs.map(c =>
            `<option value="${c.value}" ${c.value === this._videoCodec ? 'selected' : ''}>
                ${this.esc(c.label)}
            </option>`
        ).join('');

        this.container.innerHTML = `
            <div class="settings-view" id="view-settings">
                <div class="settings-header">
                    <h2>Streaming Settings</h2>
                    <button class="view-close-btn" id="btn-settings-close"
                            title="Close">&times;</button>
                </div>

                <div class="settings-section">
                    <h3 class="settings-section-title">Video Codec</h3>
                    <p class="settings-section-desc">
                        Choose the preferred video codec for game streaming.
                        <strong>Auto</strong> lets Sunshine pick the best codec
                        supported by both your browser and the host GPU encoder.
                    </p>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-video-codec">
                            Preferred Video Codec
                        </label>
                        <select id="settings-video-codec" class="settings-select">
                            ${codecOptions}
                        </select>
                    </div>

                    <div class="settings-gaming-mode" style="margin-top: 20px; padding-top: 20px; border-top: 1px solid #ddd;">
                        <label class="setting-row" style="display: flex; align-items: center; justify-content: space-between;">
                            <span class="setting-label">
                                <strong>Gaming Mode</strong>
                                <br>
                                <span class="setting-description" style="font-size: 0.85em; color: #666;">
                                    Lock mouse pointer for seamless camera control in games.
                                    Disable for normal mouse behavior (useful with touchpads or multi-monitor setups).
                                </span>
                            </span>
                            <span class="setting-control">
                                <input type="checkbox" id="settings-gaming-mode"
                                    ${this._gamingMode ? 'checked' : ''} />
                            </span>
                        </label>
                    </div>

                    <div class="settings-transport" style="margin-top: 20px; padding-top: 20px; border-top: 1px solid #ddd;">
                        <label class="setting-row" style="display: flex; align-items: center; justify-content: space-between;">
                            <span class="setting-label">
                                <strong>Transport Protocol</strong>
                                <br>
                                <span class="setting-description" style="font-size: 0.85em; color: #666;">
                                    <strong>WebRTC</strong> (default): uses libdatachannel DataChannels for
                                    low-latency video/audio streaming.<br>
                                    <strong>WebSocket</strong>: legacy StreamRelay over plain WebSocket
                                    (for testing/diagnostics).
                                </span>
                            </span>
                            <span class="setting-control">
                                <select id="settings-transport" class="settings-select" style="min-width: 120px;">
                                    <option value="webrtc" ${this._transport === 'webrtc' ? 'selected' : ''}>WebRTC</option>
                                    <option value="wss" ${this._transport === 'wss' ? 'selected' : ''}>WebSocket</option>
                                </select>
                            </span>
                        </label>
                    </div>

                </div>
            </div>
        `;

    }

    bindEvents() {
        // ── Codec selector auto-save ──────────────────────────────────────────
        const codecSelect = this.container.querySelector('#settings-video-codec');
        if (codecSelect) {
            codecSelect.addEventListener('change', () => this._autoSave());
        }

        // ── Gaming mode checkbox auto-save ────────────────────────────────────
        const gamingCheck = this.container.querySelector('#settings-gaming-mode');
        if (gamingCheck) {
            gamingCheck.addEventListener('change', () => this._autoSave());
        }

        // ── Transport selector auto-save ────────────────────────────────────────
        const transportSelect = this.container.querySelector('#settings-transport');
        if (transportSelect) {
            transportSelect.addEventListener('change', () => this._autoSave());
        }

        // ── Close button ──────────────────────────────────────────────────────
        const closeBtn = this.container.querySelector('#btn-settings-close');
        if (closeBtn) {
            closeBtn.addEventListener('click', () => this.onClose());
        }
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
