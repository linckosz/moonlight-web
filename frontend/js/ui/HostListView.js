/**
 * Moonlight-Web — Host list view component
 */
import { BackendClient } from '../api/BackendClient.js';
import { Host } from '../models/Host.js';
import { PairDialog } from './PairDialog.js';
import { Toast } from './Toast.js';

export class HostListView {
    constructor(container) {
        this.container = container;
        this.hosts = [];
        this.pollTimer = null;
        this.eventsBound = false;
        this.onLaunch = null;  // called with host when user clicks Launch
        this._active = false;
        this._destroyed = false;
        this.renderShell();

        // Auto-scan when page gains focus
        this._visibilityHandler = () => {
            if (!document.hidden && !this._destroyed)
                this._autoScan();
        };
        document.addEventListener('visibilitychange', this._visibilityHandler);

        // Button delegation — bound once, cleaned up in destroy()
        this._clickHandler = (e) => {
            if (this._destroyed) return;

            const pairBtn = e.target.closest('.btn-pair');
            if (pairBtn) {
                const uuid = pairBtn.dataset.uuid;
                const host = this.hosts.find(h => h.uuid === uuid);
                if (host) {
                    this.stop();
                    const dialog = new PairDialog(host);
                    dialog.onComplete = () => {
                        this.start();  // start() already calls refresh()
                    };
                    dialog.onCancel = () => this.start();
                    dialog.show();
                }
                return;
            }

            const launchBtn = e.target.closest('.btn-open');
            if (launchBtn) {
                const uuid = launchBtn.dataset.uuid;
                const host = this.hosts.find(h => h.uuid === uuid);
                if (host && host.isAvailable && this.onLaunch) {
                    this.stop();
                    this.onLaunch(host);
                }
                return;
            }

            const wolBtn = e.target.closest('.btn-wol');
            if (wolBtn) {
                const uuid = wolBtn.dataset.uuid;
                const host = this.hosts.find(h => h.uuid === uuid);
                wolBtn.disabled = true;
                BackendClient.wakeHost(uuid)
                    .then(() => {
                        Toast.show(`Wake-on-LAN sent to "${host ? host.displayName : uuid}"`, 'success');
                    })
                    .catch(err => {
                        console.error('[MW] Wake-on-LAN failed:', err);
                        Toast.show(err.message, 'error');
                    })
                    .finally(() => { wolBtn.disabled = false; });
                return;
            }

            const removeBtn = e.target.closest('.btn-remove');
            if (removeBtn) {
                removeBtn.disabled = true;
                removeBtn.textContent = 'Removing...';
                const uuid = removeBtn.dataset.uuid;
                BackendClient.removeHost(uuid)
                    .then(() => {
                        this.hosts = this.hosts.filter(h => h.uuid !== uuid);
                        this.renderList();
                    })
                    .catch(err => {
                        console.error('[MW] Remove host failed:', err);
                        this.refresh();
                    });
            }
        };
        this.container.addEventListener('click', this._clickHandler);
    }

    // --- Public API ---

    start() {
        this._active = true;
        this._autoScan();
        this.scheduleNextPoll(0); // immediate first refresh
    }

    async _autoScan() {
        if (!this._active) return;
        try {
            await BackendClient.scanHosts();
        } catch (err) {
            // Silent — scan is best-effort, errors are expected
        }
    }

    stop() {
        this._active = false;
        if (this.pollTimer) {
            clearTimeout(this.pollTimer);
            this.pollTimer = null;
        }
    }

    destroy() {
        this._active = false;
        this._destroyed = true;
        this.stop();
        if (this._clickHandler) {
            this.container.removeEventListener('click', this._clickHandler);
            this._clickHandler = null;
        }
        if (this._visibilityHandler) {
            document.removeEventListener('visibilitychange', this._visibilityHandler);
            this._visibilityHandler = null;
        }
    }

    scheduleNextPoll(delay = 3000) {
        if (!this._active || this._destroyed) return;
        this.pollTimer = setTimeout(async () => {
            if (!this._active || this._destroyed) return;
            await this.refresh();
            // Don't re-schedule if stop() was called during refresh
            if (this._active && !this._destroyed)
                this.scheduleNextPoll();
        }, delay);
    }

    async refresh() {
        if (!this._active) return;
        try {
            const data = await BackendClient.getHosts();
            // Re-check after await: stop()/destroy() may have run while the
            // request was in flight (e.g. a stream was launched) — a late
            // render here would overwrite the current view.
            if (!this._active || this._destroyed) return;
            const serverHosts = data.hosts || [];
            const before = this._fingerprint();
            // Merge — add/update, never remove
            for (const h of serverHosts) {
                const host = new Host(h);
                const idx = this.hosts.findIndex(ex => ex.uuid === host.uuid);
                if (idx >= 0)
                    this.hosts[idx] = host;
                else
                    this.hosts.push(host);
            }
            const after = this._fingerprint();
            if (before !== after)
                this.renderList();
        } catch (err) {
            console.error('[MW] Failed to refresh hosts:', err);
            // Show error only on first load; keep existing data on refresh
            if (this.hosts.length === 0)
                this.renderError(err.message);
        }
    }

    // Stable fingerprint of host list — skips re-render when nothing changed
    _fingerprint() {
        return JSON.stringify(this.hosts.map(h => [
            h.uuid, h.state, h.pairState, h.name,
            h.activeAddress, h.port, h.gpuModel,
            h.displayModes
        ]));
    }

    _cardFingerprint(host) {
        return [
            host.uuid, host.state, host.pairState, host.name,
            host.activeAddress, host.port, host.gpuModel,
            host.macAddress,
            JSON.stringify(host.displayModes)
        ].join('|');
    }

    // --- Rendering ---

    // Render the outer shell once (header + empty list container)
    renderShell() {
        this.container.innerHTML = `
            <div class="hosts-view" id="view-hosts">
                <div class="hosts-header">
                    <h2>Hosts</h2>
                    <div class="hosts-actions">
                        <button class="btn btn-neutral" id="btn-manual">Add Manually</button>
                    </div>
                </div>
                <div class="hosts-list" id="hosts-list"></div>
            </div>
        `;
        this.bindEvents();
    }

    // Patch only the host cards — never touches the header
    renderList() {
        const list = this.container.querySelector('#hosts-list');
        if (!list) return;

        // Empty state
        if (this.hosts.length === 0) {
            list.innerHTML = `
                <div class="hosts-empty">
                    <span class="empty-icon">\u{1F5B4}</span>
                    <p>No hosts found</p>
                    <p class="hint">Click "Scan Network" or add a host manually</p>
                </div>`;
            return;
        }

        // Remove empty placeholder if present
        const empty = list.querySelector('.hosts-empty');
        if (empty) empty.remove();

        const currentUuids = new Set(this.hosts.map(h => h.uuid));

        // Remove cards for hosts no longer in the list
        const existingCards = list.querySelectorAll('.host-card');
        for (const card of existingCards) {
            if (!currentUuids.has(card.dataset.uuid))
                card.remove();
        }

        // Insert or update cards in order
        for (let i = 0; i < this.hosts.length; i++) {
            const host = this.hosts[i];
            const fp = this._cardFingerprint(host);
            let card = list.querySelector(`.host-card[data-uuid="${host.uuid}"]`);

            if (!card) {
                // New host — insert at correct position
                const tmp = document.createElement('div');
                tmp.innerHTML = this.renderCard(host);
                card = tmp.firstElementChild;
                card.dataset.fingerprint = fp;
                // Insert at the right position
                const allCards = list.querySelectorAll('.host-card');
                if (i < allCards.length) {
                    list.insertBefore(card, allCards[i]);
                } else {
                    list.appendChild(card);
                }
            } else if (card.dataset.fingerprint !== fp) {
                // Data changed — replace just this card
                const tmp = document.createElement('div');
                tmp.innerHTML = this.renderCard(host);
                const newCard = tmp.firstElementChild;
                newCard.dataset.fingerprint = fp;
                card.replaceWith(newCard);
            }
        }
    }

    renderCard(host) {
        const cls = host.statusClass;
        return `
            <div class="host-card ${cls}" data-uuid="${host.uuid}">
                <div class="host-card-icon">
                    <span class="status-icon ${cls}">${host.statusIcon}</span>
                </div>
                <div class="host-card-info">
                    <div class="host-name">${this.esc(host.displayName)}</div>
                    <div class="host-address">${this.esc(host.displayAddress)}</div>
                    ${host.displayGpu
                        ? `<div class="host-gpu">${this.esc(host.displayGpu)}</div>`
                        : ''}
                    ${host.resolutionText
                        ? `<div class="host-resolution">${this.esc(host.resolutionText)}</div>`
                        : ''}
                </div>
                <div class="host-card-status">
                    <span class="status-badge ${cls}">${host.statusLabel}</span>
                </div>
                <div class="host-card-actions">
                    ${host.isAvailable
                        ? `<button class="btn btn-open" data-uuid="${host.uuid}">Open</button>`
                        : host.isLocked
                            ? `<button class="btn btn-secondary btn-pair" data-uuid="${host.uuid}">Pair</button>`
                            : host.canWake
                                ? `<button class="btn btn-secondary btn-small btn-wol" data-uuid="${host.uuid}" title="Wake On LAN">⏻ Wake</button>`
                                : ''
                    }
                </div>
                ${!host.isAvailable && !host.isLocked
                    ? `<div class="host-card-remove">
                         <button class="btn btn-secondary btn-remove" data-uuid="${host.uuid}">Remove</button>
                       </div>`
                    : ''
                }
            </div>
        `;
    }

    renderError(message) {
        this.container.innerHTML = `
            <div class="hosts-view">
                <div class="hosts-header">
                    <h2>Hosts</h2>
                    <button class="btn" onclick="location.reload()">Retry</button>
                </div>
                <div class="hosts-error">
                    <p>Connection lost</p>
                    <p class="hint">${this.esc(message)}</p>
                </div>
            </div>
        `;
    }

    // --- Events ---

    bindEvents() {
        const manualBtn = this.container.querySelector('#btn-manual');
        if (manualBtn) {
            manualBtn.addEventListener('click', async () => {
                const addr = prompt('Enter host IP address (e.g., 192.168.1.5):');
                if (!addr || !addr.trim()) return;
                manualBtn.disabled = true;
                manualBtn.classList.add('btn-loading');
                try {
                    const data = await BackendClient.addManualHost(addr.trim());
                    if (data.hosts && data.hosts.length > 0) {
                        const newHost = new Host(data.hosts[0]);
                        const idx = this.hosts.findIndex(h => h.uuid === newHost.uuid);
                        if (idx >= 0) {
                            this.hosts[idx] = newHost;
                            this.renderList();
                            Toast.show(`"${newHost.displayName}" already in list`, 'warning');
                        } else {
                            this.hosts.push(newHost);
                            this.renderList();
                            Toast.show(`"${newHost.displayName}" added successfully`, 'success');
                        }
                    } else {
                        Toast.show('No host was returned from the server', 'error');
                    }
                } catch (err) {
                    console.error('[MW] Add host failed:', err);
                    Toast.show(err.message, 'error');
                } finally {
                    manualBtn.disabled = false;
                    manualBtn.classList.remove('btn-loading');
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
