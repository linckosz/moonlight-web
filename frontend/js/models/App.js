/**
 * Moonlight-Web — Remote application data model
 */
export class App {
    constructor(data, hostUuid = null) {
        this.id           = data.id || 0;
        this.name         = data.name || 'Unknown App';
        this.hdrSupported = data.hdrSupported || false;
        this.hostUuid     = hostUuid;
    }

    get displayName() {
        return this.name;
    }

    get boxArtUrl() {
        if (!this.hostUuid || !this.id)
            return null;
        return `/api/hosts/${encodeURIComponent(this.hostUuid)}/appasset?appid=${this.id}`;
    }
}
