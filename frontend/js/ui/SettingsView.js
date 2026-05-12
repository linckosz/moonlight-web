/**
 * Moonlight-Web — Streaming Settings view
 *
 * Streaming-related user preferences (video codec, etc.).
 * All settings are stored server-side per the project principle.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';

export class SettingsView {
    constructor(container) {
        this.container = container;

        this._videoCodec = 'auto';

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
        this._cleanState = {
            videoCodec: codecSelect ? codecSelect.value : this._videoCodec
        };
        this._dirty = false;
        this._updateSaveButton();
    }

    _onFieldChange() {
        const codecSelect = this.container.querySelector('#settings-video-codec');
        if (!codecSelect) return;
        this._dirty = (codecSelect.value !== this._cleanState.videoCodec);
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

        // ── Save button ───────────────────────────────────────────────────────
        const saveBtn = this.container.querySelector('#btn-settings-save');
        if (saveBtn) {
            saveBtn.addEventListener('click', async () => {
                const codec = codecSelect.value;

                saveBtn.disabled = true;
                saveBtn.classList.add('btn-loading');
                saveBtn.textContent = 'Saving...';

                try {
                    const result = await BackendClient.saveStreamingSettings({ video_codec: codec });
                    if (result.status === 'saved') {
                        this._videoCodec = result.video_codec;
                        Toast.success('Video codec set to ' + result.video_codec.toUpperCase());
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
        }
    }

    // --- Helpers ---

    esc(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
}
