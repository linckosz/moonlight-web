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
        this.state = data.state || 'unknown';
        this.pairState = data.pairState || 'unknown';
        this.activeAddress = data.activeAddress || '';
        this.port = data.port || 47989;
        this.gpuModel = data.gpuModel || '';
        this.gfeVersion = data.gfeVersion || '';
        this.appVersion = data.appVersion || '';
        this.currentGameId = data.currentGameId || 0;
        this.displayModes = data.displayModes || [];
        this.localAddress = data.localAddress || '';
        this.remoteAddress = data.remoteAddress || '';
        this.manualAddress = data.manualAddress || '';
        this.serverCodecModeSupport = data.serverCodecModeSupport || 1;
        this.macAddress = data.macAddress || '';
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

    // Wake-on-LAN is offered for offline hosts with a known (non-zero) MAC.
    get canWake() {
        return !this.isOnline && !!this.macAddress && this.macAddress !== '00:00:00:00:00:00';
    }

    get displayName() {
        // Never fall back to the IP address — internal addresses stay hidden.
        if (this.name && this.name !== 'UNKNOWN') return this.name;
        return 'Unknown Host';
    }

    get displayGpu() {
        return this.gpuModel || '';
    }

    get statusLabel() {
        if (!this.isOnline) return t('hosts.statusOffline');
        if (this.isPaired) return t('hosts.statusReady');
        return t('hosts.statusNotPaired');
    }

    get statusClass() {
        if (!this.isOnline) return 'offline';
        if (this.isPaired) return 'ready';
        return 'locked';
    }

    get statusIcon() {
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
