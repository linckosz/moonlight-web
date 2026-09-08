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
 * MoonlightWeb — the name of the machine you are looking at, in the header.
 *
 * Someone running this on three PCs had no way to tell, from the page, which of
 * the three they had open. The name answers that. When the browser knows only
 * one machine it stays a name and nothing else — no arrow, no hover, nothing
 * that suggests a click that would do nothing. The moment a second machine is
 * known it becomes a menu, and the arrow is what says so.
 *
 * That progression is the whole design, and it is why this is a component rather
 * than a span: the affordance has to appear and disappear on its own, from a
 * register that grows as its owner visits machines.
 *
 * Choosing another machine is a NAVIGATION, not a view switch. Each install is a
 * separate program on a separate PC holding its own hosts, its own session and
 * its own settings; the browser leaves this one and goes to that one, through
 * the bootstrap, exactly as a bookmark would.
 */
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { Icons } from './icons.js';
import { listInstances } from '../util/instances.js';

export class InstanceMenu {
    /**
     * @param {HTMLElement} mount the placeholder element in the header
     */
    constructor(mount) {
        this.mount = mount;
        this.currentId = '';
        this.open = false;
        this._onDocClick = null;
        this._onKeyDown = null;
    }

    /**
     * Draw (or redraw) the name and, if there is more than one machine, its menu.
     *
     * @param {string} currentId  this install's key in the register
     * @param {string} name       what this install calls itself
     */
    render(currentId, name) {
        if (!this.mount || !name) return;
        this.currentId = currentId;

        const instances = listInstances();
        // The register is written before this runs, so the current machine is
        // always in it — but a browser that refused storage gives an empty list,
        // and the header must still show the name it was handed.
        const others = instances.filter((e) => e.id !== currentId);

        this._closeMenu();
        this.mount.hidden = false;

        if (others.length === 0) {
            this.mount.className = 'instance-menu';
            this.mount.innerHTML = `<span class="instance-name" title="${escapeHtml(name)}">${escapeHtml(name)}</span>`;
            return;
        }

        this.mount.className = 'instance-menu has-menu';
        this.mount.innerHTML = `
            <button class="instance-btn" type="button" aria-haspopup="menu" aria-expanded="false"
                    title="${escapeHtml(t('header.instanceSwitch'))}">
                <span class="instance-name">${escapeHtml(name)}</span>
                <span class="instance-caret">${Icons.chevronDown}</span>
            </button>
            <div class="instance-panel" role="menu" hidden>
                <p class="instance-panel-title">${escapeHtml(t('header.instances'))}</p>
                ${instances
                    .map((e) => {
                        const isCurrent = e.id === this.currentId;
                        return `
                    <button class="instance-item${isCurrent ? ' is-current' : ''}" role="menuitem"
                            type="button" data-url="${escapeHtml(e.url)}"
                            ${isCurrent ? 'aria-current="true"' : ''}>
                        <span class="instance-item-icon">${Icons.monitor}</span>
                        <span class="instance-item-name">${escapeHtml(e.name)}</span>
                        <span class="instance-item-mark">${isCurrent ? Icons.check : ''}</span>
                    </button>`;
                    })
                    .join('')}
            </div>`;

        this.btn = /** @type {HTMLButtonElement} */ (this.mount.querySelector('.instance-btn'));
        this.panel = /** @type {HTMLElement} */ (this.mount.querySelector('.instance-panel'));

        this.btn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._toggleMenu();
        });

        this.panel.addEventListener('click', (e) => {
            const item = /** @type {HTMLElement|null} */ (
                /** @type {HTMLElement} */ (e.target).closest('.instance-item')
            );
            if (!item) return;
            // Picking the machine already on screen closes the menu and does
            // nothing else. Reloading the page someone is already looking at is
            // never what "which machine am I on?" meant.
            if (item.classList.contains('is-current')) {
                this._closeMenu();
                return;
            }
            window.location.href = item.dataset.url || '/';
        });
    }

    destroy() {
        this._closeMenu();
        if (this.mount) {
            this.mount.innerHTML = '';
            this.mount.hidden = true;
        }
    }

    _toggleMenu() {
        if (this.open) this._closeMenu();
        else this._openMenu();
    }

    _openMenu() {
        if (!this.panel || this.open) return;
        this.open = true;
        this.panel.hidden = false;
        this.btn.setAttribute('aria-expanded', 'true');
        this.mount.classList.add('is-open');

        // Bound on the document rather than on a backdrop element: this menu
        // hangs off the header, which sits above every view, and a full-screen
        // backdrop would have to be above that in turn — a lot of z-index for a
        // list of four names.
        this._onDocClick = (e) => {
            if (!this.mount.contains(e.target)) this._closeMenu();
        };
        this._onKeyDown = (e) => {
            if (e.key === 'Escape') {
                this._closeMenu();
                this.btn?.focus();
            }
        };
        document.addEventListener('click', this._onDocClick);
        document.addEventListener('keydown', this._onKeyDown);
    }

    _closeMenu() {
        if (this._onDocClick) document.removeEventListener('click', this._onDocClick);
        if (this._onKeyDown) document.removeEventListener('keydown', this._onKeyDown);
        this._onDocClick = null;
        this._onKeyDown = null;
        this.open = false;
        if (this.panel) this.panel.hidden = true;
        if (this.btn) this.btn.setAttribute('aria-expanded', 'false');
        this.mount?.classList.remove('is-open');
    }
}
