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
                        }>${this.esc(ty)}</option>`,
                )
                .join('');
            this.syncPairCredsVisibility();
        } catch {
            this.setStatus(t('backend.typesFailed'), 'error');
        }

        // The form now reflects the stored config: snapshot it as the clean
        // baseline so SAVE starts disabled and only lights up on a real change.
        this.captureBaseline();

        // What this host's backend actually supports, read from the provider
        // itself. Shown rather than inferred from the type name, so the panel
        // cannot claim a capability the code does not implement.
        if (this.host.backendType) {
            try {
                const { capabilities } = await BackendClient.getHostBackend(this.host.uuid);
                this.capabilities = capabilities || {};
                this.renderCapabilities(capabilities);
            } catch {
                /* Not fatal: the form still works without the summary. */
            }
            this.loadSeats();
        }
    }

    async loadSeats() {
        const panel = this.overlay?.querySelector('.backend-seats');
        if (!panel) return;
        panel.hidden = false;
        panel.innerHTML = `<p class="backend-seats-empty">${t('backend.seatsLoading')}</p>`;

        try {
            const { seats } = await BackendClient.getHostSeats(this.host.uuid);
            if (!this.overlay) return;
            this.renderSeats(seats || []);
        } catch (err) {
            if (!this.overlay) return;
            // 501 means the backend simply has no seat concept — not a failure
            // worth alarming anyone about.
            const msg = err?.statusCode === 501 ? t('backend.seatsUnsupported') : err?.message;
            panel.innerHTML = `<p class="backend-seats-empty">${this.esc(msg || '')}</p>`;
        }
    }

    renderSeats(seats) {
        const panel = this.overlay?.querySelector('.backend-seats');
        if (!panel) return;

        const canProvision = !!this.capabilities?.provisioning;
        const rows = seats.length
            ? seats
                  .map(
                      (s) => `
                <div class="backend-seat">
                    <span class="backend-seat-name">${this.esc(s.name || s.id)}</span>
                    <span class="backend-seat-meta">${this.esc(s.address)}:${s.httpPort}${
                        s.busy ? ` · ${this.esc(t('backend.seatBusy'))}` : ''
                    }</span>
                    ${
                        canProvision
                            ? `<button class="btn-seat-free" data-seat="${this.esc(
                                  s.id,
                              )}" title="${this.esc(t('backend.seatFreeHint'))}">${this.esc(
                                  t('backend.seatFree'),
                              )}</button>
                               <button class="btn-seat-remove" data-seat="${this.esc(
                                   s.id,
                               )}" title="${this.esc(t('backend.seatRemove'))}">✕</button>`
                            : ''
                    }
                </div>`,
                  )
                  .join('')
            : `<p class="backend-seats-empty">${t('backend.seatsNone')}</p>`;

        panel.innerHTML = `
            <div class="backend-seats-head">
                <span>${t('backend.seats')}</span>
                ${
                    canProvision
                        ? `<button class="btn-seat-add">${t('backend.seatAdd')}</button>`
                        : ''
                }
            </div>
            ${rows}`;
    }

    async provisionSeat() {
        // accountName is SeatRequest's only required field; the service defaults
        // the rest. Asking for more here would bake a MultiSeat-shaped form into
        // a dialog that is meant to serve every backend.
        const account = prompt(t('backend.seatAccountPrompt'));
        if (!account) return;

        this.setBusy(true);
        this.setStatus(t('backend.seatProvisioning'));
        try {
            await BackendClient.provisionHostSeat(this.host.uuid, { accountName: account });
            if (!this.overlay) return;
            this.setStatus(t('backend.seatProvisioned'), 'success');
            this.loadSeats();
        } catch (err) {
            if (!this.overlay) return;
            this.setStatus(err?.message || t('backend.failed'), 'error');
        } finally {
            this.setBusy(false);
        }
    }

    async releaseSeatOwner(seatId) {
        this.setBusy(true);
        this.setStatus(t('backend.seatFreeing'));
        try {
            await BackendClient.releaseHostSeatOwner(this.host.uuid, seatId);
            if (!this.overlay) return;
            this.setStatus(t('backend.seatFreed'), 'success');
            this.loadSeats();
        } catch (err) {
            if (!this.overlay) return;
            // 404 here is not a failure worth alarming about: it just means the
            // seat was already free.
            this.setStatus(err?.message || t('backend.failed'), 'error');
        } finally {
            this.setBusy(false);
        }
    }

    async teardownSeat(seatId) {
        this.setBusy(true);
        this.setStatus(t('backend.seatRemoving'));
        try {
            await BackendClient.teardownHostSeat(this.host.uuid, seatId);
            if (!this.overlay) return;
            this.setStatus(t('backend.ready'));
            this.loadSeats();
        } catch (err) {
            if (!this.overlay) return;
            this.setStatus(err?.message || t('backend.failed'), 'error');
        } finally {
            this.setBusy(false);
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
                        label,
                    )}</span>`,
            )
            .join('');
        el.hidden = false;
    }

    /// MultiSeat pairs a seat through its Apollo web UI, so it needs those
    /// credentials; Wolf pairs through its own API and needs none. Asking every
    /// backend for them would be noise.
    syncPairCredsVisibility() {
        const select = this.overlay?.querySelector('.backend-type');
        const panel = this.overlay?.querySelector('.backend-pair-creds');
        if (!select || !panel) return;
        panel.hidden = select.value !== 'multiseat';
    }

    /// Current form values, as the tuple that decides whether anything changed.
    snapshotForm() {
        const q = (sel) => this.overlay?.querySelector(sel);
        return JSON.stringify({
            type: q('.backend-type')?.value || '',
            url: q('.backend-url')?.value.trim() || '',
            token: q('.backend-token')?.value || '',
            pairUser: q('.backend-pair-user')?.value.trim() || '',
            pairPassword: q('.backend-pair-password')?.value || '',
        });
    }

    /// Mark the current form state as the clean reference (on open, and again
    /// after a successful save so the button greys back out).
    captureBaseline() {
        this._baseline = this.snapshotForm();
        this.updateSaveEnabled();
    }

    /// SAVE is enabled only when the form differs from the baseline and no
    /// request is in flight. A URL is required, so an empty one never enables it.
    updateSaveEnabled() {
        const save = this.overlay?.querySelector('.btn-backend-save');
        if (!save) return;
        const hasUrl = !!this.overlay.querySelector('.backend-url')?.value.trim();
        save.disabled = this.busy || !hasUrl || this.snapshotForm() === this._baseline;
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
        // 'loading' shows a spinner but is otherwise styled like info.
        const tone = kind === 'loading' ? 'info' : kind;
        el.className = `backend-status-text pairing-${tone}${kind === 'loading' ? ' is-loading' : ''}`;
        el.textContent = message;
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
        const pairUser = this.overlay.querySelector('.backend-pair-user')?.value.trim() || '';
        const pairPassword = this.overlay.querySelector('.backend-pair-password')?.value || '';

        if (!apiUrl) {
            this.setStatus(t('backend.urlRequired'), 'error');
            return;
        }

        this.setBusy(true);
        this.setStatus(t('backend.pairing'), 'loading');
        try {
            await BackendClient.setHostBackend(this.host.uuid, {
                type,
                apiUrl,
                apiToken,
                pairUser,
                pairPassword,
            });
            if (!this.overlay) return;
            this.setStatus(t('backend.paired'), 'success');
        } catch (err) {
            if (!this.overlay) return;
            this.setStatus(err?.message || t('backend.failed'), 'error');
        } finally {
            if (this.overlay) {
                this.setBusy(false);
                // The entered config is persisted either way now, so tell the host
                // list to refresh and re-pull the seat panel — it reflects reality:
                // the real seats on success, empty/unreachable on a failed URL. The
                // dialog stays open; closing is the admin's own CLOSE click.
                if (this.onSaved) this.onSaved();
                this.captureBaseline();
                this.loadSeats();
            }
        }
    }

    render() {
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

                <div class="backend-pair-creds" hidden>
                    <label class="backend-field">
                        <span>${t('backend.pairUser')}</span>
                        <input class="backend-pair-user" type="text" autocomplete="off"
                               spellcheck="false"
                               value="${this.esc(this.host.backendPairUser || '')}">
                    </label>
                    <label class="backend-field">
                        <span>${t('backend.pairPassword')}</span>
                        <input class="backend-pair-password" type="password" autocomplete="off"
                               placeholder="${
                                   this.host.backendPairConfigured
                                       ? t('backend.tokenKeep')
                                       : t('backend.pairPasswordPlaceholder')
                               }">
                    </label>
                    <p class="backend-hint">${t('backend.pairHint')}</p>
                </div>

                <div class="backend-caps" hidden></div>
                <div class="backend-seats" hidden></div>

                <div class="pairing-status">
                    <p class="backend-status-text pairing-info">${t('backend.ready')}</p>
                </div>

                <div class="pairing-actions">
                    <button class="btn btn-secondary btn-backend-cancel">${t('common.close')}</button>
                    <button class="btn btn-primary btn-backend-save">${t('common.save')}</button>
                </div>
            </div>
        `;
    }

    bindEvents() {
        this.overlay
            .querySelector('.btn-backend-cancel')
            .addEventListener('click', () => this.close());

        this.overlay
            .querySelector('.btn-backend-save')
            .addEventListener('click', () => this.save());

        this.overlay.querySelector('.backend-type').addEventListener('change', () => {
            this.syncPairCredsVisibility();
            this.updateSaveEnabled();
        });

        // SAVE stays disabled until the form differs from what is stored, so an
        // accidental click cannot re-pair with unchanged values. Any edit to a
        // field re-evaluates it.
        this.overlay
            .querySelectorAll(
                '.backend-url, .backend-token, .backend-pair-user, .backend-pair-password',
            )
            .forEach((el) => {
                el.addEventListener('input', () => this.updateSaveEnabled());
                // Enter in any field triggers SAVE, but only when it is a valid
                // action (dirty + reachable URL + not mid-pairing) — same gate as
                // the button, so Enter never re-pairs unchanged values.
                el.addEventListener('keydown', (e) => {
                    if (e.key !== 'Enter') return;
                    e.preventDefault();
                    const save = this.overlay.querySelector('.btn-backend-save');
                    if (save && !save.disabled) this.save();
                });
            });

        // Seat controls are rendered after load, so they are handled by
        // delegation rather than bound per row.
        this.overlay.addEventListener('click', (e) => {
            const add = e.target.closest('.btn-seat-add');
            if (add) {
                this.provisionSeat();
                return;
            }
            const free = e.target.closest('.btn-seat-free');
            if (free) {
                this.releaseSeatOwner(free.dataset.seat);
                return;
            }

            const remove = e.target.closest('.btn-seat-remove');
            if (remove) {
                this.teardownSeat(remove.dataset.seat);
                return;
            }

            // Click-outside closes, but not mid-pairing: the handshake is
            // already in flight on the host and the result is worth seeing.
            if (e.target === this.overlay && !this.busy) this.close();
        });
    }
}
