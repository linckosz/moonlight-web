/**
 * MoonlightWeb — Backend management dialog
 *
 * Declares which stream backend drives a host (integration doc §5.3). Reached
 * from the host card's kebab menu, because "what drives this host" is an admin
 * fact about the host, not something a player chooses per session.
 *
 * Saving pairs immediately. That is the whole point of the feature: one admin
 * gesture here means no player ever sees a pairing PIN, since MoonlightWeb
 * posts the PIN to the backend's control API itself.
 *
 * The token is write-only. It is never sent to the browser, so the field starts
 * empty and an empty field means "keep the stored one" — that way the URL can
 * be corrected without retyping a secret nobody was shown.
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

export class BackendDialog {
    constructor(host, { onSaved } = {}) {
        this.host = host;
        this.onSaved = onSaved;
        this.overlay = null;
        this.busy = false;
    }

    esc(s) {
        return escapeHtml(s);
    }

    async show() {
        this.render();
        document.body.appendChild(this.overlay);
        this.bindEvents();

        // Ask the server which backends it can actually build, rather than
        // offering a list the build may not support.
        try {
            const { types } = await BackendClient.getBackendTypes();
            const select = this.overlay?.querySelector('.backend-type');
            if (!select) return; // closed while loading
            select.innerHTML = types
                // "gamestream" is the absence of management, offered as "none".
                .filter((ty) => ty !== 'gamestream')
                .map(
                    (ty) =>
                        `<option value="${this.esc(ty)}"${
                            ty === this.host.backendType ? ' selected' : ''
                        }>${this.esc(ty)}</option>`
                )
                .join('');
        } catch {
            this.setStatus(t('backend.typesFailed'), 'error');
        }

        // What this host's backend actually supports, read from the provider
        // itself. Shown rather than inferred from the type name, so the panel
        // cannot claim a capability the code does not implement.
        if (this.host.backendType) {
            try {
                const { capabilities } = await BackendClient.getHostBackend(this.host.uuid);
                this.renderCapabilities(capabilities);
            } catch {
                /* Not fatal: the form still works without the summary. */
            }
        }
    }

    renderCapabilities(caps) {
        const el = this.overlay?.querySelector('.backend-caps');
        if (!el || !caps) return;

        const labels = [
            [caps.multiUser, t('backend.capMultiUser')],
            [caps.provisioning, t('backend.capProvisioning')],
            [caps.lobbies, t('backend.capLobbies')],
        ];

        el.innerHTML = labels
            .map(
                ([on, label]) =>
                    `<span class="backend-cap${on ? ' is-on' : ''}">${on ? '✓' : '·'} ${this.esc(
                        label
                    )}</span>`
            )
            .join('');
        el.hidden = false;
    }

    close() {
        if (this.overlay) {
            this.overlay.remove();
            this.overlay = null;
        }
    }

    setStatus(message, kind = 'info') {
        const el = this.overlay?.querySelector('.backend-status-text');
        if (!el) return;
        el.textContent = message;
        el.className = `backend-status-text pairing-${kind}`;
    }

    setBusy(busy) {
        this.busy = busy;
        this.overlay?.querySelectorAll('button, input, select').forEach((el) => {
            el.disabled = busy;
        });
    }

    async save() {
        if (this.busy) return;

        const type = this.overlay.querySelector('.backend-type').value;
        const apiUrl = this.overlay.querySelector('.backend-url').value.trim();
        const apiToken = this.overlay.querySelector('.backend-token').value;

        if (!apiUrl) {
            this.setStatus(t('backend.urlRequired'), 'error');
            return;
        }

        this.setBusy(true);
        this.setStatus(t('backend.pairing'));
        try {
            await BackendClient.setHostBackend(this.host.uuid, { type, apiUrl, apiToken });
            if (!this.overlay) return;
            this.setStatus(t('backend.paired'), 'success');
            if (this.onSaved) this.onSaved();
            setTimeout(() => this.close(), 900);
        } catch (err) {
            if (!this.overlay) return;
            this.setBusy(false);
            this.setStatus(err?.message || t('backend.failed'), 'error');
        }
    }

    async clear() {
        if (this.busy) return;
        this.setBusy(true);
        this.setStatus(t('backend.removing'));
        try {
            await BackendClient.clearHostBackend(this.host.uuid);
            if (this.onSaved) this.onSaved();
            this.close();
        } catch (err) {
            if (!this.overlay) return;
            this.setBusy(false);
            this.setStatus(err?.message || t('backend.failed'), 'error');
        }
    }

    render() {
        const configured = !!this.host.backendType;

        this.overlay = document.createElement('div');
        this.overlay.className = 'pairing-overlay';
        this.overlay.innerHTML = `
            <div class="pairing-dialog backend-dialog">
                <h3>${this.esc(t('backend.title', { name: this.host.displayName || this.host.name }))}</h3>
                <p class="pairing-instruction">${t('backend.instruction')}</p>

                <label class="backend-field">
                    <span>${t('backend.type')}</span>
                    <select class="backend-type"></select>
                </label>

                <label class="backend-field">
                    <span>${t('backend.apiUrl')}</span>
                    <input class="backend-url" type="url" spellcheck="false"
                           placeholder="http://192.168.1.50:8080"
                           value="${this.esc(this.host.backendApiUrl || '')}">
                </label>

                <label class="backend-field">
                    <span>${t('backend.apiToken')}</span>
                    <input class="backend-token" type="password" autocomplete="off"
                           spellcheck="false"
                           placeholder="${
                               this.host.backendConfigured
                                   ? t('backend.tokenKeep')
                                   : t('backend.tokenPlaceholder')
                           }">
                </label>

                <div class="backend-caps" hidden></div>

                <div class="pairing-status">
                    <p class="backend-status-text pairing-info">${t('backend.ready')}</p>
                </div>

                <div class="pairing-actions">
                    ${
                        configured
                            ? `<button class="btn btn-danger btn-backend-clear">${t('backend.remove')}</button>`
                            : ''
                    }
                    <button class="btn btn-secondary btn-backend-cancel">${t('common.cancel')}</button>
                    <button class="btn btn-primary btn-backend-save">${t('common.save')}</button>
                </div>
            </div>
        `;
    }

    bindEvents() {
        this.overlay
            .querySelector('.btn-backend-cancel')
            .addEventListener('click', () => this.close());

        this.overlay.querySelector('.btn-backend-save').addEventListener('click', () => this.save());

        this.overlay
            .querySelector('.btn-backend-clear')
            ?.addEventListener('click', () => this.clear());

        // Click-outside closes, but not mid-pairing: the handshake is already in
        // flight on the host and the result is worth seeing.
        this.overlay.addEventListener('click', (e) => {
            if (e.target === this.overlay && !this.busy) this.close();
        });
    }
}
