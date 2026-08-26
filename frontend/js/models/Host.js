/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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
 * MoonlightWeb — Host data model
 */
import { t } from '../i18n/i18n.js';
import { Icons } from '../ui/icons.js';

export class Host {
    constructor(data) {
        this.uuid = data.uuid || '';
        this.name = data.name || 'Unknown Host';
        // A name given here, in MoonlightWeb. Wins over the reported one wherever
        // the host is shown, and is kept apart from it so the rename dialog can
        // still offer what the host calls itself when the field is cleared.
        this.customName = data.customName || '';
        this.state = data.state || 'unknown';
        // Machine answers at the IP level but the GameStream server isn't running
        // (host powered on, MoonlightWeb/Sunshine not started). Backend-derived.
        this.reachable = data.reachable === true;
        this.pairState = data.pairState || 'unknown';
        this.port = data.port || 47989;
        this.gpuModel = data.gpuModel || '';
        this.gfeVersion = data.gfeVersion || '';
        this.appVersion = data.appVersion || '';
        this.currentGameId = data.currentGameId || 0;
        this.displayModes = data.displayModes || [];
        this.serverCodecModeSupport = data.serverCodecModeSupport || 1;
        // The backend never sends host addresses or the MAC: the browser talks
        // only to this server, and Wake-on-LAN is sent server-side. All we get
        // is whether waking is possible at all.
        this.wakeSupported = data.wakeSupported === true;
        // Backend flag: this host is the very machine MoonlightWeb runs on.
        this.isLocalHost = data.isLocalHost === true;

        // The server holds a way to bounce this host's streaming service without
        // asking anyone for a password — its own Sunshine, or a backend control
        // API that offers it. False everywhere else, and the menu says nothing.
        this.restartSupported = data.restartSupported === true;

        // Which backend drives this host. Empty for a plain GameStream host,
        // which is the default and what every Sunshine card stays. The token is
        // never sent to the browser — backendConfigured only says one is stored.
        this.backendType = data.backendType || '';
        this.backendApiUrl = data.backendApiUrl || '';
        this.backendConfigured = data.backendConfigured === true;

        // This host shares a screen through its own native co-op — the player
        // joins the owner's lobby rather than opening a second view of one
        // desktop. Read from the backend's declared capabilities rather than
        // from its name: what the UI needs to know is what the host can do.
        this.supportsLobbies = data.capabilities?.lobbies === true;

        // The server found a MultiSeat control API on this host by itself. It is
        // what lets the UI offer that setup to the people who have it and stay
        // completely absent for everyone else — nobody names their own server.
        this.multiSeatDetected = data.multiSeatDetected === true;

        // The server found no Sunshine-family management API on this host, so
        // whatever runs it has a control API only its owner can point us at.
        // What this drives is an OFFER, never a label: the observation does not
        // name the product, and the product names itself once the address is
        // given.
        this.mayHaveControlApi = data.mayHaveControlApi === true;
    }

    get isOnline() {
        return this.state === 'online';
    }
    get isPaired() {
        return this.pairState === 'paired';
    }
    get isLocked() {
        return this.isOnline && !this.isPaired;
    }
    get isAvailable() {
        return this.isOnline && this.isPaired;
    }

    // Offline but the machine still answers at the IP level → the host is up,
    // it just isn't running the GameStream server. Shown as "Unavailable"
    // (no Wake-on-LAN — the machine is already awake).
    get isUnavailable() {
        return !this.isOnline && this.reachable;
    }

    // Wake-on-LAN is offered for offline hosts whose MAC the server knows —
    // but only when the machine is actually down (not merely service-down).
    get canWake() {
        return !this.isOnline && !this.reachable && this.wakeSupported;
    }

    get displayName() {
        // A name given here wins: it is the one the user chose to recognise this
        // machine by, and it is the only one that exists for a host that has
        // never answered.
        if (this.customName) return this.customName;
        // Never fall back to the IP address — internal addresses stay hidden.
        if (this.name && this.name !== 'UNKNOWN') return this.name;
        return 'Unknown Host';
    }

    /** What the host calls itself, ignoring any alias — shown as the rename
     *  dialog's placeholder so clearing the field has a visible meaning. */
    get reportedName() {
        if (this.name && this.name !== 'UNKNOWN') return this.name;
        return 'Unknown Host';
    }

    get displayGpu() {
        return this.gpuModel || '';
    }

    get statusLabel() {
        if (this.isUnavailable) return t('hosts.statusUnavailable');
        if (!this.isOnline) return t('hosts.statusOffline');
        if (this.isPaired) return t('hosts.statusReady');
        return t('hosts.statusNotPaired');
    }

    get statusClass() {
        if (this.isUnavailable) return 'unavailable';
        if (!this.isOnline) return 'offline';
        if (this.isPaired) return 'ready';
        return 'locked';
    }

    get statusIcon() {
        if (this.isUnavailable) return Icons.unavailable; // reachable, service down
        if (!this.isOnline) return Icons.power; // power off
        if (this.isPaired) return Icons.check; // checkmark
        return Icons.lock; // lock
    }

    get resolutionText() {
        if (this.displayModes.length === 0) return '';
        const best = this.displayModes[0]; // sorted desc by pixels*Hz
        const hz = best.refreshRate || 60;
        return `${best.width}\xd7${best.height} @ ${hz}Hz`;
    }
}
