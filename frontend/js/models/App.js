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
 * MoonlightWeb — Remote application data model
 */
export class App {
    constructor(data, hostUuid = null) {
        this.id = data.id || 0;
        this.name = data.name || 'Unknown App';
        this.hdrSupported = data.hdrSupported || false;
        // Absent means yes: every GameStream host serves cover art, and only a
        // host that knows it has none — the native host, whose "apps" are
        // monitors — says so. An older host that says nothing keeps asking.
        this.hasBoxArt = data.boxArt !== false;
        this.hostUuid = hostUuid;
    }

    get displayName() {
        return this.name;
    }

    get boxArtUrl() {
        if (!this.hostUuid || !this.id || !this.hasBoxArt) return null;
        return `/api/hosts/${encodeURIComponent(this.hostUuid)}/appasset?appid=${this.id}`;
    }
}
