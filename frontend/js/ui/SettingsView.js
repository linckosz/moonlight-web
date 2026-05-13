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

        // Dirty tracking
        this._cleanState = {};
        this._dirty = false;
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
        } catch (err) {
            console.warn('[Settings] Failed to load streaming settings:', err);
        }
    }

    destroy() {
        // Cleanup if needed
    }

    // --- Dirty tracking ---

    _markClean() {
        const codecSelect = this.container.querySelector('#settings-video-codec');
        const gamingCheck = this.container.querySelector('#settings-gaming-mode');
        this._cleanState = {
            videoCodec: codecSelect ? codecSelect.value : this._videoCodec,
            gamingMode: gamingCheck ? gamingCheck.checked : this._gamingMode
        };
        this._dirty = false;
        this._updateSaveButton();
    }

    _onFieldChange() {
        const codecSelect = this.container.querySelector('#settings-video-codec');
        const gamingCheck = this.container.querySelector('#settings-gaming-mode');
        if (!codecSelect && !gamingCheck) return;
        const codecDirty = codecSelect && (codecSelect.value !== this._cleanState.videoCodec);
        const gamingDirty = gamingCheck && (gamingCheck.checked !== this._cleanState.gamingMode);
        this._dirty = codecDirty || gamingDirty;
        this._updateSaveButton();
    }

    _updateSaveButton() {
        const saveBtn = this.container.querySelector('#btn-settings-save');
        if (saveBtn) {
            saveBtn.disabled = !this._dirty;
        }
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
                            title="Close (discards unsaved changes)">&times;</button>
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

                    <div class="settings-actions">
                        <button class="btn btn-save" id="btn-settings-save" disabled>
                            Save Changes
                        </button>
                    </div>
                </div>
            </div>
        `;

        this._markClean();
    }

    bindEvents() {
        // ── Codec selector dirty tracking ─────────────────────────────────────
        const codecSelect = this.container.querySelector('#settings-video-codec');
        if (codecSelect) {
            codecSelect.addEventListener('change', () => this._onFieldChange());
        }

        // ── Gaming mode checkbox dirty tracking ───────────────────────────────
        const gamingCheck = this.container.querySelector('#settings-gaming-mode');
        if (gamingCheck) {
            gamingCheck.addEventListener('change', () => this._onFieldChange());
        }

        // ── Save button ───────────────────────────────────────────────────────
        const saveBtn = this.container.querySelector('#btn-settings-save');
        if (saveBtn) {
            saveBtn.addEventListener('click', async () => {
                const codec = codecSelect.value;
                const gamingMode = gamingCheck ? gamingCheck.checked : true;

                saveBtn.disabled = true;
                saveBtn.classList.add('btn-loading');
                saveBtn.textContent = 'Saving...';

                try {
                    const result = await BackendClient.saveStreamingSettings({
                        video_codec: codec,
                        gaming_mode: gamingMode
                    });
                    if (result.status === 'saved') {
                        this._videoCodec = result.video_codec || this._videoCodec;
                        this._gamingMode = result.gaming_mode !== false;
                        Toast.success('Settings saved');
                        this._markClean();
                    }
                } catch (err) {
                    console.error('[Settings] Failed to save:', err);
                    Toast.error('Failed to save: ' + err.message);
                } finally {
                    saveBtn.classList.remove('btn-loading');
                    saveBtn.textContent = 'Save Changes';
                    this._updateSaveButton();
                }
            });
        // ── Close button ───────────────────────────────────────────────────────
        const closeBtn = this.container.querySelector('#btn-settings-close');
        if (closeBtn) {
            closeBtn.addEventListener('click', () => {
                if (this._dirty) {
                    Toast.info('Settings changes discarded');
                }
                this.onClose();
            });
        }
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
