/**
 * Moonlight-Web — Host data model
 */
import { t } from '../i18n/i18n.js';

export class Host {
    constructor(data) {
        this.uuid          = data.uuid || '';
        this.name          = data.name || 'Unknown Host';
        this.state         = data.state || 'unknown';
        this.pairState     = data.pairState || 'unknown';
        this.activeAddress = data.activeAddress || '';
        this.port          = data.port || 47989;
        this.gpuModel      = data.gpuModel || '';
        this.gfeVersion    = data.gfeVersion || '';
        this.appVersion    = data.appVersion || '';
        this.currentGameId = data.currentGameId || 0;
        this.displayModes  = data.displayModes || [];
        this.localAddress  = data.localAddress || '';
        this.remoteAddress = data.remoteAddress || '';
        this.manualAddress = data.manualAddress || '';
        this.serverCodecModeSupport = data.serverCodecModeSupport || 1;
        this.macAddress    = data.macAddress || '';
    }

    get isOnline()  { return this.state === 'online'; }
    get isPaired()  { return this.pairState === 'paired'; }
    get isLocked()  { return this.isOnline && !this.isPaired; }
    get isAvailable() { return this.isOnline && this.isPaired; }

    // Wake-on-LAN is offered for offline hosts with a known (non-zero) MAC.
    get canWake() {
        return !this.isOnline
            && !!this.macAddress
            && this.macAddress !== '00:00:00:00:00:00';
    }

    get displayName() {
        if (this.name && this.name !== 'UNKNOWN')
            return this.name;
        if (this.activeAddress)
            return this.activeAddress;
        return 'Unknown Host';
    }

    get displayAddress() {
        return this.activeAddress
            ? `${this.activeAddress}:${this.port}`
            : '';
    }

    get displayGpu() {
        return this.gpuModel || '';
    }

    get statusLabel() {
        if (!this.isOnline) return t('hosts.statusOffline');
        if (this.isPaired)  return t('hosts.statusReady');
        return t('hosts.statusNotPaired');
    }

    get statusClass() {
        if (!this.isOnline) return 'offline';
        if (this.isPaired)  return 'ready';
        return 'locked';
    }

    get statusIcon() {
        if (!this.isOnline) return '⏻';   // ⏻ power off
        if (this.isPaired)  return '✔';   // ✔ checkmark
        return '\u{1F512}';                    // 🔒 lock
    }

    get resolutionText() {
        if (this.displayModes.length === 0)
            return '';
        const best = this.displayModes[0];  // sorted desc by pixels*Hz
        const hz = best.refreshRate || 60;
        return `${best.width}\xd7${best.height} @ ${hz}Hz`;
    }
}
