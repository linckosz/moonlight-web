/**
 * Moonlight-Web — Host list view component
 */
import { BackendClient } from '../api/BackendClient.js';
import { Host } from '../models/Host.js';
import { PairDialog } from './PairDialog.js';

export class HostListView {
    constructor(container) {
        this.container = container;
        this.hosts = [];
        this.pollTimer = null;
        this.eventsBound = false;
        this.render();

        // Pair button delegation — bound once in constructor
        this.container.addEventListener('click', (e) => {
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
            }

        });
    }

    // --- Public API ---

    start() {
        this.scheduleNextPoll(0); // immediate first refresh
    }

    stop() {
        if (this.pollTimer) {
            clearTimeout(this.pollTimer);
            this.pollTimer = null;
        }
    }

    scheduleNextPoll(delay = 3000) {
        this.pollTimer = setTimeout(async () => {
            await this.refresh();
            if (this.pollTimer)
                this.scheduleNextPoll();
        }, delay);
    }

    async refresh() {
        try {
            const data = await BackendClient.getHosts();
            this.hosts = (data.hosts || []).map(h => new Host(h));
            this.render();
        } catch (err) {
            console.error('[MW] Failed to refresh hosts:', err);
            this.renderError(err.message);
        }
    }

    // --- Rendering ---

    render() {
        this.container.innerHTML = `
            <div class="hosts-view" id="view-hosts">
                <div class="hosts-header">
                    <h2>Hosts</h2>
                    <div class="hosts-actions">
                        <button class="btn" id="btn-scan">Scan Network</button>
                        <button class="btn btn-secondary" id="btn-manual">Add Manually</button>
                    </div>
                </div>
                <div class="hosts-list" id="hosts-list">
                    ${this.hosts.length === 0
                        ? `<div class="hosts-empty">
                             <span class="empty-icon">\u{1F5B4}</span>
                             <p>No hosts found</p>
                             <p class="hint">Click "Scan Network" or add a host manually</p>
                           </div>`
                        : this.hosts.map(h => this.renderCard(h)).join('')
                    }
                </div>
            </div>
        `;
        this.bindEvents();
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
                        ? `<button class="btn btn-launch" data-uuid="${host.uuid}">Launch</button>`
                        : host.isLocked
                            ? `<button class="btn btn-secondary btn-pair" data-uuid="${host.uuid}">Pair</button>`
                            : `<span class="host-unreachable">Unreachable</span>`
                    }
                </div>
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
        const scanBtn = this.container.querySelector('#btn-scan');
        if (scanBtn) {
            scanBtn.addEventListener('click', async () => {
                scanBtn.disabled = true;
                scanBtn.textContent = 'Scanning...';
                try {
                    await BackendClient.scanHosts();
                } catch (err) {
                    console.error('[MW] Scan failed:', err);
                }
                setTimeout(() => {
                    scanBtn.disabled = false;
                    scanBtn.textContent = 'Scan Network';
                    this.refresh();
                }, 2000);
            });
        }

        const manualBtn = this.container.querySelector('#btn-manual');
        if (manualBtn) {
            manualBtn.addEventListener('click', () => {
                const addr = prompt('Enter host IP address (e.g., 192.168.1.5):');
                if (addr && addr.trim()) {
                    BackendClient.addManualHost(addr.trim())
                        .then(() => this.refresh())
                        .catch(err => console.error('[MW] Add host failed:', err));
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
