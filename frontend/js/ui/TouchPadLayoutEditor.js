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
 * TouchPadLayoutEditor — where a viewer puts the on-screen controller's
 * controls, from the Settings page, on the screen they are holding.
 *
 * The controller itself is drawn by TouchPadView, so what is arranged is
 * exactly what a stream shows; the editor lays transparent handles over the
 * controls, and the layer stops taking touches meanwhile (`.is-editing`). A
 * drag writes a place as shares of the layer (`touchPadLayouts`), which is what
 * makes the same arrangement survive a rotation, a resolution change or a
 * different tablet. Portrait and landscape are two separate saves, chosen by
 * the shape of the stage.
 */
import { t } from '../i18n/i18n.js';
import { TouchPadState, layoutOrientation } from '../stream/TouchPadState.js';
import { TouchPadView } from './TouchPadView.js';

/** How small or large a control may be made, as `layoutTouchPad` reads it. */
const SIZE_MIN = 0.8;
const SIZE_MAX = 1.3;
const SIZE_STEP = 0.05;

/** What each drawn control is called in a screen reader. */
const NAMES = {
    a: 'A',
    b: 'B',
    x: 'X',
    y: 'Y',
    lb: 'LB',
    rb: 'RB',
    lt: 'LT',
    rt: 'RT',
    l3: 'L3',
    r3: 'R3',
    view: 'View',
    menu: 'Menu',
    dpad: 'D-pad',
    'stick-left': 'Left stick',
    'stick-right': 'Right stick',
};

const clone = (layout) => JSON.parse(JSON.stringify(layout));

export class TouchPadLayoutEditor {
    /**
     * @param {HTMLElement} container the element the overlay is added to.
     * @param {{ layout: ReturnType<import('../stream/TouchPadState.js').touchPadLayouts>,
     *           onChange?: (layout: any) => void,
     *           onClosed?: () => void }} options
     *   `layout` is what the viewer had; `onChange` is told of every saved
     *   arrangement, once they press Done; `onClosed` when the overlay is gone.
     */
    constructor(container, { layout, onChange, onClosed } = {}) {
        this._container = container;
        this._onChange = onChange || null;
        this._onClosed = onClosed || null;
        this._before = clone(layout);
        this._draft = clone(layout);
        this._el = null;
        this._stage = null;
        this._view = null;
        this._state = null;
        this._handles = [];
        this._selected = null;
        this._drag = null;
        this._resizeObserver = null;
        this._keydown = null;
    }

    /** Build and show it. */
    open() {
        if (this._el) return;
        const el = document.createElement('div');
        el.className = 'tp-layout-editor';
        el.innerHTML = this._html();
        this._container.appendChild(el);
        this._el = el;
        this._stage = el.querySelector('.tp-layout-editor-stage');
        this._buildLayer();
        this._bind();
        this._refresh();
    }

    /** Take it away, saving nothing: Cancel and Done both come through here. */
    close() {
        if (!this._el) return;
        if (this._resizeObserver) this._resizeObserver.disconnect();
        this._resizeObserver = null;
        if (this._keydown) document.removeEventListener('keydown', this._keydown);
        this._keydown = null;
        if (this._view) this._view.destroy();
        this._view = null;
        this._state = null;
        this._handles = [];
        this._el.remove();
        this._el = null;
        if (this._onClosed) this._onClosed();
    }

    /** True while the overlay is up. */
    isOpen() {
        return !!this._el;
    }

    _html() {
        return `
            <div class="tp-layout-editor-bar">
                <strong class="tp-layout-editor-title">${t('settings.touchPadEditorTitle')}</strong>
                <span class="tp-layout-editor-orientation"></span>
                <label class="tp-layout-editor-size">
                    ${t('settings.touchPadEditorSize')}
                    <input type="range" min="${SIZE_MIN}" max="${SIZE_MAX}" step="${SIZE_STEP}"
                           value="1" disabled />
                </label>
                <span class="tp-layout-editor-actions">
                    <button type="button" class="btn btn-secondary" data-act="reset">
                        ${t('settings.touchPadEditorReset')}
                    </button>
                    <button type="button" class="btn btn-secondary" data-act="cancel">
                        ${t('settings.touchPadEditorCancel')}
                    </button>
                    <button type="button" class="btn btn-primary" data-act="done">
                        ${t('settings.touchPadEditorDone')}
                    </button>
                </span>
            </div>
            <p class="tp-layout-editor-hint">${t('settings.touchPadEditorHint')}</p>
            <div class="tp-layout-editor-stage"></div>`;
    }

    /** The controller, drawn but deaf: the handles take the fingers. */
    _buildLayer() {
        this._state = new TouchPadState();
        this._view = new TouchPadView(this._stage, this._state, { layout: this._draft });
        this._view.mount();
        const layer = this._stage.querySelector('.touch-pad');
        if (layer) layer.classList.add('is-editing');
        this._view.setVisible(true);
        if (typeof ResizeObserver === 'function') {
            this._resizeObserver = new ResizeObserver(() => this._refresh());
            this._resizeObserver.observe(this._stage);
        }
    }

    _bind() {
        this._el.querySelector('[data-act="done"]').addEventListener('click', () => {
            if (this._onChange) this._onChange(clone(this._draft));
            this.close();
        });
        this._el.querySelector('[data-act="cancel"]').addEventListener('click', () => {
            this.close();
        });
        this._el.querySelector('[data-act="reset"]').addEventListener('click', () => {
            this._draft[this._orientation()] = {};
            this._selected = null;
            this._view.setLayout(this._draft);
            this._refresh();
        });
        this._el.querySelector('.tp-layout-editor-size input').addEventListener('input', (e) => {
            const id = this._selected;
            if (!id) return;
            this._write(id, { s: Number(e.target.value) });
            this._view.setLayout(this._draft);
            this._refresh();
        });
        this._keydown = (e) => {
            if (e.key === 'Escape') {
                e.preventDefault();
                this.close();
            }
        };
        document.addEventListener('keydown', this._keydown);
    }

    /** The orientation the stage is in, which is the save the drags go to. */
    _orientation() {
        const rect = this._stage.getBoundingClientRect();
        return layoutOrientation(rect.width, rect.height);
    }

    /**
     * Where a control is, as the layout just drew it: the rect of a button or
     * cross, the ring of a stick. Handles are laid over them again after every
     * move, so what a finger grabs is always what the layer shows.
     */
    _refresh() {
        if (!this._el) return;
        const layout = this._view.lastLayout();
        if (!layout) return;
        const orientation = this._orientation();
        this._el.querySelector('.tp-layout-editor-orientation').textContent =
            orientation === 'portrait'
                ? t('settings.touchPadEditorPortrait')
                : t('settings.touchPadEditorLandscape');
        const rect = this._stage.getBoundingClientRect();
        const wants = layout.controls.map((c) => {
            if (c.kind === 'stick') {
                return {
                    id: c.id,
                    stick: true,
                    x: c.restX - c.radius,
                    y: c.restY - c.radius,
                    w: c.radius * 2,
                    h: c.radius * 2,
                };
            }
            return { id: c.id, x: c.x, y: c.y, w: c.w, h: c.h };
        });
        // Reuse the handles already on the stage: a drag must keep its element.
        const byId = new Map(this._handles.map((h) => [h.dataset.control, h]));
        for (const want of wants) {
            let handle = byId.get(want.id);
            if (!handle) handle = this._makeHandle(want.id, want.stick);
            byId.delete(want.id);
            handle.style.left = want.x + 'px';
            handle.style.top = want.y + 'px';
            handle.style.width = want.w + 'px';
            handle.style.height = want.h + 'px';
            handle.classList.toggle('is-selected', this._selected === want.id);
        }
        for (const gone of byId.values()) gone.remove();
        this._handles = Array.from(this._el.querySelectorAll('.tp-layout-editor-handle'));
        this._el.dataset.orientation = orientation;
        this._el.dataset.stageWidth = String(rect.width);
        this._el.dataset.stageHeight = String(rect.height);
        this._el.classList.toggle('is-portrait', orientation === 'portrait');
        this._syncSize();
    }

    _makeHandle(id, stick) {
        const handle = document.createElement('div');
        handle.className = 'tp-layout-editor-handle' + (stick ? ' is-stick' : '');
        handle.dataset.control = id;
        handle.setAttribute('role', 'button');
        handle.setAttribute('aria-label', NAMES[id] || id);
        handle.addEventListener('pointerdown', (e) => this._grab(e, handle, id));
        this._stage.appendChild(handle);
        return handle;
    }

    /** Take a control under the finger, and follow it from where it was grabbed. */
    _grab(e, handle, id) {
        if (e.button !== undefined && e.button !== 0) return;
        e.preventDefault();
        e.stopPropagation();
        this._selected = id;
        const rect = this._stage.getBoundingClientRect();
        const box = handle.getBoundingClientRect();
        const drag = {
            id,
            pointerId: e.pointerId,
            handle,
            // Where the finger is, within what it grabbed: the control must not
            // jump under it.
            dx: e.clientX - (box.left + box.width / 2),
            dy: e.clientY - (box.top + box.height / 2),
            left: rect.left,
            top: rect.top,
            width: rect.width,
            height: rect.height,
        };
        drag.onMove = (ev) => this._dragTo(ev);
        drag.onUp = () => this._drop();
        this._drag = drag;
        if (typeof handle.setPointerCapture === 'function') handle.setPointerCapture(e.pointerId);
        handle.addEventListener('pointermove', drag.onMove);
        handle.addEventListener('pointerup', drag.onUp);
        handle.addEventListener('pointercancel', drag.onUp);
        this._refresh();
        this._syncSize();
    }

    _dragTo(e) {
        const drag = this._drag;
        if (!drag || e.pointerId !== drag.pointerId) return;
        e.preventDefault();
        const x = (e.clientX - drag.dx - drag.left) / drag.width;
        const y = (e.clientY - drag.dy - drag.top) / drag.height;
        this._write(drag.id, { x, y });
        this._view.setLayout(this._draft);
        this._refresh();
    }

    _drop() {
        const drag = this._drag;
        if (!drag) return;
        this._drag = null;
        drag.handle.removeEventListener('pointermove', drag.onMove);
        drag.handle.removeEventListener('pointerup', drag.onUp);
        drag.handle.removeEventListener('pointercancel', drag.onUp);
    }

    /** The place a control has now, as shares of the stage, from what is drawn. */
    _placeOf(id) {
        const layout = this._view.lastLayout();
        const c = layout && layout.controls.find((control) => control.id === id);
        const rect = this._stage.getBoundingClientRect();
        if (!c || !rect.width || !rect.height) return { x: 0.5, y: 0.5 };
        const cx = c.kind === 'stick' ? c.restX : c.cx !== undefined ? c.cx : c.x + c.w / 2;
        const cy = c.kind === 'stick' ? c.restY : c.cy !== undefined ? c.cy : c.y + c.h / 2;
        return { x: cx / rect.width, y: cy / rect.height };
    }

    /**
     * Put a place or a size in the draft, keeping what was already saved — and
     * what is drawn, for a control this edit only resizes.
     */
    _write(id, { x, y, s }) {
        const orientation = this._orientation();
        const saved = this._draft[orientation] || (this._draft[orientation] = {});
        const here = this._placeOf(id);
        const before = saved[id] || { x: here.x, y: here.y, s: 1 };
        saved[id] = {
            x: x === undefined ? before.x : Math.min(1, Math.max(0, x)),
            y: y === undefined ? before.y : Math.min(1, Math.max(0, y)),
            s: s === undefined ? before.s : Math.min(SIZE_MAX, Math.max(SIZE_MIN, s)),
        };
    }

    /** The size slider follows the control it resizes, and is dead with none. */
    _syncSize() {
        const input = this._el.querySelector('.tp-layout-editor-size input');
        const saved = this._selected
            ? (this._draft[this._orientation()] || {})[this._selected]
            : null;
        input.disabled = !this._selected;
        input.value = saved ? saved.s : 1;
    }
}
