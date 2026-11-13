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
 * TouchPadLayoutEditor — the on-screen controller as a viewer wants it, set
 * from the Settings page on the screen they are holding: its preset, its size,
 * how see-through it is, and where each control sits.
 *
 * The controller itself is drawn by TouchPadView, so what is arranged is
 * exactly what a stream shows; the editor lays transparent handles over the
 * controls, and the layer stops taking touches meanwhile (`.is-editing`). A
 * drag writes a place as shares of the layer (`touchPadLayouts`), which is what
 * makes the same arrangement survive a rotation, a resolution change or a
 * different tablet. Portrait and landscape are two separate saves, chosen by
 * the shape of the stage.
 *
 * The stage fills the screen. Everything else is in one bar that floats over
 * the controller: it opens unfolded, folds to a pill, and moves by its grip,
 * so no control is ever out of reach under it. Its three backgrounds are a
 * preview only: they show the controller over a dark and a bright picture,
 * and are not kept.
 */
import { t } from '../i18n/i18n.js';
import { TouchPadState, layoutOrientation } from '../stream/TouchPadState.js';
import { TouchPadView } from './TouchPadView.js';

/** How small or large a control may be made, as `layoutTouchPad` reads it. */
const SIZE_MIN = 0.8;
const SIZE_MAX = 1.3;
const SIZE_STEP = 0.05;

/** The whole controller's size and opacity, in percent, as Settings kept them. */
const SCALE_MIN = 80;
const SCALE_MAX = 140;
const OPACITY_MIN = 20;
const OPACITY_MAX = 100;

const PRESETS = [
    ['default', 'settings.touchPadLayoutDefault'],
    ['left', 'settings.touchPadLayoutLeft'],
    ['compact', 'settings.touchPadLayoutCompact'],
];

/**
 * The pictures the controller can be tried over: the grid, a game at night, a
 * game by day (assets/touchpad). A preview, never kept.
 */
const BACKGROUNDS = [
    ['default', 'settings.touchPadEditorBgDefault'],
    ['night', 'settings.touchPadEditorBgNight'],
    ['day', 'settings.touchPadEditorBgDay'],
];

/**
 * What each drawn control is called, in the bar and in a screen reader: the
 * letters as printed on the pad, the rest in the viewer's language.
 */
const NAME_KEYS = {
    view: 'settings.touchPadControlView',
    menu: 'settings.touchPadControlMenu',
    dpad: 'settings.touchPadControlDpad',
    'stick-left': 'settings.touchPadControlStickLeft',
    'stick-right': 'settings.touchPadControlStickRight',
};
const nameOf = (id) => (NAME_KEYS[id] ? t(NAME_KEYS[id]) : id.toUpperCase());

const GRIP = `<svg width="12" height="18" viewBox="0 0 12 18" fill="currentColor" aria-hidden="true">
    <circle cx="3" cy="3" r="1.6"/><circle cx="9" cy="3" r="1.6"/>
    <circle cx="3" cy="9" r="1.6"/><circle cx="9" cy="9" r="1.6"/>
    <circle cx="3" cy="15" r="1.6"/><circle cx="9" cy="15" r="1.6"/></svg>`;
const CHEVRON = `<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor"
    stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
    <path d="m6 9 6 6 6-6"/></svg>`;

const clone = (layout) => JSON.parse(JSON.stringify(layout));
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const percent = (v, lo, hi) => clamp(Math.round(Number(v) * 100), lo, hi);

export class TouchPadLayoutEditor {
    /**
     * @param {HTMLElement} container the element the overlay is added to.
     * @param {{ layout: ReturnType<import('../stream/TouchPadState.js').touchPadLayouts>,
     *           scale?: number, opacity?: number,
     *           onChange?: (layout: any, look: {scale: number, opacity: number}) => void,
     *           onClosed?: () => void }} options
     *   `layout`, `scale` and `opacity` are what the viewer had (scale and
     *   opacity as fractions, 1 = 100%); `onChange` is told of all three once
     *   they press Done; `onClosed` when the overlay is gone.
     */
    constructor(container, { layout, scale, opacity, onChange, onClosed } = {}) {
        this._container = container;
        this._onChange = onChange || null;
        this._onClosed = onClosed || null;
        this._before = clone(layout);
        this._draft = clone(layout);
        this._scale = percent(scale === undefined ? 1 : scale, SCALE_MIN, SCALE_MAX);
        this._opacity = percent(opacity === undefined ? 1 : opacity, OPACITY_MIN, OPACITY_MAX);
        this._background = 'default';
        this._folded = false;
        this._barAt = null;
        this._el = null;
        this._stage = null;
        this._bar = null;
        this._view = null;
        this._state = null;
        this._handles = [];
        this._selected = null;
        this._drag = null;
        this._barDrag = null;
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
        this._bar = el.querySelector('.tp-layout-editor-bar');
        this._buildLayer();
        this._bind();
        this._showBackground();
        this._showLook();
        this._refresh();
        this._placeBar();
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
        this._bar = null;
        if (this._onClosed) this._onClosed();
    }

    /** True while the overlay is up. */
    isOpen() {
        return !!this._el;
    }

    _html() {
        const presets = PRESETS.map(
            ([id, key]) =>
                `<button type="button" class="tp-bar-preset" data-preset="${id}">${t(key)}</button>`,
        ).join('');
        const backgrounds = BACKGROUNDS.map(
            ([id, key]) =>
                `<button type="button" class="tp-bar-bg tp-bg-${id}" data-bg="${id}"
                         aria-label="${t(key)}" title="${t(key)}"></button>`,
        ).join('');
        const range = (cls, label, min, max, step) => `
                    <label class="${cls}">
                        <span>${t(label)} <strong></strong>%</span>
                        <input type="range" min="${min}" max="${max}" step="${step}" />
                    </label>`;
        return `
            <div class="tp-layout-editor-stage"></div>
            <div class="tp-layout-editor-bar" role="group"
                 aria-label="${t('settings.touchPadEditorTitle')}">
                <div class="tp-bar-head">
                    <span class="tp-bar-grip">${GRIP}</span>
                    <strong class="tp-layout-editor-title">${t('settings.touchPadEditorTitle')}</strong>
                    <span class="tp-layout-editor-orientation"></span>
                    <button type="button" class="tp-bar-unfold" data-act="unfold">
                        ${t('settings.touchPadEditorUnfold')}
                    </button>
                    <button type="button" class="tp-bar-fold" data-act="fold"
                            aria-label="${t('settings.touchPadEditorFold')}"
                            title="${t('settings.touchPadEditorFold')}">${CHEVRON}</button>
                    <button type="button" class="btn btn-primary tp-bar-done-folded" data-act="done">
                        ${t('settings.touchPadEditorDone')}
                    </button>
                </div>
                <div class="tp-bar-body">
                    <div class="tp-bar-presets" role="group"
                         aria-label="${t('settings.touchPadEditorPreset')}">${presets}</div>
                    <div class="tp-bar-look">
                        ${range('tp-layout-editor-scale', 'settings.touchPadSize', SCALE_MIN, SCALE_MAX, 5)}
                        ${range('tp-layout-editor-opacity', 'settings.touchPadOpacity', OPACITY_MIN, OPACITY_MAX, 5)}
                    </div>
                    <label class="tp-layout-editor-size">
                        <span class="tp-layout-editor-size-text"></span>
                        <input type="range" min="${SIZE_MIN}" max="${SIZE_MAX}" step="${SIZE_STEP}"
                               value="1" disabled />
                    </label>
                    <div class="tp-bar-foot">
                        <span class="tp-bar-bg-label">${t('settings.touchPadEditorBackground')}</span>
                        ${backgrounds}
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
                </div>
            </div>`;
    }

    /** The controller, drawn but deaf: the handles take the fingers. */
    _buildLayer() {
        this._state = new TouchPadState();
        this._view = new TouchPadView(this._stage, this._state, {
            layout: this._draft,
            scale: this._scale / 100,
            opacity: this._opacity / 100,
        });
        this._view.mount();
        const layer = this._stage.querySelector('.touch-pad');
        if (layer) layer.classList.add('is-editing');
        this._view.setVisible(true);
        if (typeof ResizeObserver === 'function') {
            this._resizeObserver = new ResizeObserver(() => {
                this._refresh();
                this._placeBar();
            });
            this._resizeObserver.observe(this._stage);
        }
    }

    _bind() {
        const act = (name, fn) => {
            for (const b of this._el.querySelectorAll(`[data-act="${name}"]`)) {
                b.addEventListener('click', fn);
            }
        };
        act('done', () => {
            if (this._onChange) {
                this._onChange(clone(this._draft), {
                    scale: this._scale / 100,
                    opacity: this._opacity / 100,
                });
            }
            this.close();
        });
        act('cancel', () => this.close());
        act('reset', () => {
            this._draft[this._orientation()] = {};
            this._selected = null;
            this._view.setLayout(this._draft);
            this._refresh();
        });
        act('fold', () => this._fold(true));
        act('unfold', () => this._fold(false));
        for (const b of this._el.querySelectorAll('[data-preset]')) {
            b.addEventListener('click', () => this._pickPreset(b.dataset.preset));
        }
        for (const b of this._el.querySelectorAll('[data-bg]')) {
            b.addEventListener('click', () => {
                this._background = b.dataset.bg;
                this._showBackground();
            });
        }
        this._el.querySelector('.tp-layout-editor-size input').addEventListener('input', (e) => {
            const id = this._selected;
            if (!id) return;
            this._write(id, { s: Number(e.target.value) });
            this._view.setLayout(this._draft);
            this._refresh();
        });
        this._el.querySelector('.tp-layout-editor-scale input').addEventListener('input', (e) => {
            this._scale = clamp(Number(e.target.value), SCALE_MIN, SCALE_MAX);
            this._showLook();
            this._view.setLook({ scale: this._scale / 100 });
            this._refresh();
        });
        this._el.querySelector('.tp-layout-editor-opacity input').addEventListener('input', (e) => {
            this._opacity = clamp(Number(e.target.value), OPACITY_MIN, OPACITY_MAX);
            this._showLook();
            this._view.setLook({ opacity: this._opacity / 100 });
        });
        this._el
            .querySelector('.tp-bar-grip')
            .addEventListener('pointerdown', (e) => this._grabBar(e));
        this._keydown = (e) => {
            if (e.key === 'Escape') {
                e.preventDefault();
                this.close();
            }
        };
        document.addEventListener('keydown', this._keydown);
    }

    /**
     * A preset is a starting point, not a nudge: the places the viewer moved
     * were their own, and the preset arranges the rest afresh, in both
     * orientations. Nothing is kept before Done.
     */
    _pickPreset(preset) {
        this._draft = { ...this._draft, preset, landscape: {}, portrait: {} };
        this._selected = null;
        this._view.setLayout(this._draft);
        this._refresh();
    }

    _fold(folded) {
        this._folded = folded;
        this._bar.classList.toggle('is-folded', folded);
        const focus = this._bar.querySelector(folded ? '.tp-bar-unfold' : '.tp-bar-fold');
        if (focus && typeof focus.focus === 'function') focus.focus();
        this._placeBar();
    }

    _showBackground() {
        for (const [id] of BACKGROUNDS) {
            this._stage.classList.toggle('tp-bg-' + id, id === this._background);
        }
        for (const b of this._el.querySelectorAll('[data-bg]')) {
            b.setAttribute('aria-pressed', String(b.dataset.bg === this._background));
        }
    }

    /** The two sliders of the whole controller, and the preset in use. */
    _showLook() {
        const show = (cls, value) => {
            const label = this._el.querySelector(cls);
            label.querySelector('strong').textContent = String(value);
            label.querySelector('input').value = String(value);
        };
        show('.tp-layout-editor-scale', this._scale);
        show('.tp-layout-editor-opacity', this._opacity);
        for (const b of this._el.querySelectorAll('[data-preset]')) {
            b.setAttribute('aria-pressed', String(b.dataset.preset === this._draft.preset));
        }
    }

    /**
     * Keep the bar on the screen: in the middle the first time, then where it
     * was put, pulled back inside when the screen turns or the bar unfolds.
     */
    _placeBar() {
        if (!this._bar) return;
        const room = this._el.getBoundingClientRect();
        const box = this._bar.getBoundingClientRect();
        if (!room.width || !room.height) return;
        const at = this._barAt || {
            x: (room.width - box.width) / 2,
            y: (room.height - box.height) / 2,
        };
        const x = clamp(at.x, 0, Math.max(0, room.width - box.width));
        const y = clamp(at.y, 0, Math.max(0, room.height - box.height));
        this._bar.style.left = x + 'px';
        this._bar.style.top = y + 'px';
    }

    /** The bar follows its grip. */
    _grabBar(e) {
        if (e.button !== undefined && e.button !== 0) return;
        e.preventDefault();
        e.stopPropagation();
        const grip = e.currentTarget;
        const box = this._bar.getBoundingClientRect();
        const room = this._el.getBoundingClientRect();
        const drag = {
            pointerId: e.pointerId,
            dx: e.clientX - box.left,
            dy: e.clientY - box.top,
            room,
        };
        drag.onMove = (ev) => {
            if (ev.pointerId !== drag.pointerId) return;
            ev.preventDefault();
            this._barAt = {
                x: ev.clientX - drag.dx - drag.room.left,
                y: ev.clientY - drag.dy - drag.room.top,
            };
            this._placeBar();
        };
        drag.onUp = () => {
            grip.removeEventListener('pointermove', drag.onMove);
            grip.removeEventListener('pointerup', drag.onUp);
            grip.removeEventListener('pointercancel', drag.onUp);
            this._barDrag = null;
        };
        this._barDrag = drag;
        if (typeof grip.setPointerCapture === 'function') grip.setPointerCapture(e.pointerId);
        grip.addEventListener('pointermove', drag.onMove);
        grip.addEventListener('pointerup', drag.onUp);
        grip.addEventListener('pointercancel', drag.onUp);
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
        this._showLook();
        this._syncSize();
    }

    _makeHandle(id, stick) {
        const handle = document.createElement('div');
        handle.className = 'tp-layout-editor-handle' + (stick ? ' is-stick' : '');
        handle.dataset.control = id;
        handle.setAttribute('role', 'button');
        handle.setAttribute('aria-label', nameOf(id));
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
            x: x === undefined ? before.x : clamp(x, 0, 1),
            y: y === undefined ? before.y : clamp(y, 0, 1),
            s: s === undefined ? before.s : clamp(s, SIZE_MIN, SIZE_MAX),
        };
    }

    /**
     * The size slider follows the control it resizes, and is dead with none:
     * its line then says how the editor is used.
     */
    _syncSize() {
        const input = this._el.querySelector('.tp-layout-editor-size input');
        const text = this._el.querySelector('.tp-layout-editor-size-text');
        const saved = this._selected
            ? (this._draft[this._orientation()] || {})[this._selected]
            : null;
        const size = saved ? saved.s : 1;
        input.disabled = !this._selected;
        input.value = String(size);
        text.textContent = this._selected
            ? t('settings.touchPadEditorControlSize', {
                  name: nameOf(this._selected),
                  size: Math.round(size * 100),
              })
            : t('settings.touchPadEditorHint');
    }
}
