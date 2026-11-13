/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach } from 'vitest';

import { MIN_NAME_TEXT_PX, fitStreamBrand, planStreamBrand } from '../js/ui/streamBrandFit.js';

/**
 * The stream bar's mark (title, machine, version) gives up room to the
 * controls centred in the bar: the version first, then the title, then the
 * end of the name, and the name only once too little of it is left to read.
 */

// An iPhone 13 held sideways, as measured on the bench (05/11): the title
// 141 px, "DualRTX" 76 px with its 16 px of padding, a dev build's version
// 96 px, 12 px between the parts.
const PHONE = { title: 141, name: 76, namePad: 16, version: 96, gap: 12 };

describe('what the mark keeps', () => {
    it('keeps everything where there is room', () => {
        expect(planStreamBrand({ ...PHONE, room: 400 })).toEqual({
            name: 'full',
            version: true,
            title: true,
        });
    });

    it('gives up the version before any of the name', () => {
        expect(planStreamBrand({ ...PHONE, room: 229 })).toEqual({
            name: 'full',
            version: false,
            title: true,
        });
    });

    it('gives up the title before any of the name', () => {
        expect(planStreamBrand({ ...PHONE, room: 228 })).toEqual({
            name: 'full',
            version: false,
            title: false,
        });
        expect(planStreamBrand({ ...PHONE, room: PHONE.name })).toEqual({
            name: 'full',
            version: false,
            title: false,
        });
    });

    it('cuts the name while enough of it is left to read', () => {
        const room = PHONE.namePad + MIN_NAME_TEXT_PX;
        expect(planStreamBrand({ ...PHONE, room })).toEqual({
            name: 'cut',
            version: false,
            title: false,
        });
    });

    it('shows nothing rather than "D…"', () => {
        const room = PHONE.namePad + MIN_NAME_TEXT_PX - 1;
        expect(planStreamBrand({ ...PHONE, room })).toEqual({
            name: 'none',
            version: false,
            title: false,
        });
    });

    it('keeps the version when there is no name to show', () => {
        expect(planStreamBrand({ ...PHONE, name: 0, namePad: 0, room: 260 })).toEqual({
            name: 'none',
            version: true,
            title: true,
        });
    });

    it('hides the title, without a name, once it does not fit', () => {
        expect(planStreamBrand({ ...PHONE, name: 0, namePad: 0, room: 140 })).toEqual({
            name: 'none',
            version: false,
            title: false,
        });
    });
});

/** A laid-out rect of `width` at `left`. */
function at(el, left, width) {
    el.getBoundingClientRect = () => ({
        left,
        right: left + width,
        top: 0,
        bottom: 30,
        width,
        height: 30,
        x: left,
        y: 0,
    });
}

/** The bar as an iPhone 13 draws it sideways: 844 px, 47 px of inset a side. */
function bar() {
    const header = document.createElement('div');
    header.className = 'stream-header';
    header.style.padding = '0 47px';
    header.innerHTML = `
        <div class="stream-brand" style="column-gap: 12px">
            <span class="stream-brand-title">MoonlightWeb</span>
            <span class="instance-menu"><span class="instance-name"
                style="padding: 3px 8px">DualRTX</span></span>
            <span class="version">v0.4.2</span>
        </div>`;
    document.body.appendChild(header);
    Object.defineProperty(header, 'clientWidth', { value: 844 });
    const brand = /** @type {HTMLElement} */ (header.querySelector('.stream-brand'));
    const name = /** @type {HTMLElement} */ (header.querySelector('.instance-name'));
    // The keyboard button before it: the mark starts at 104.
    at(brand, 104, 300);
    at(/** @type {HTMLElement} */ (header.querySelector('.stream-brand-title')), 104, 141);
    Object.defineProperty(name, 'scrollWidth', { value: 76 });
    at(/** @type {HTMLElement} */ (header.querySelector('.version')), 0, 60);
    return {
        brand,
        title: /** @type {HTMLElement} */ (header.querySelector('.stream-brand-title')),
        box: /** @type {HTMLElement} */ (header.querySelector('.instance-menu')),
        version: /** @type {HTMLElement} */ (header.querySelector('.version')),
    };
}

describe('the mark on the bar', () => {
    afterEach(() => {
        document.body.innerHTML = '';
    });

    it('stops short of the microphone docked left of Fullscreen', () => {
        const b = bar();
        // The microphone starts at 345: the mark must end by 333.
        fitStreamBrand(b.brand, 333);
        expect(b.version.style.display).toBe('none');
        expect(b.title.style.display).toBe('');
        expect(b.box.style.display).toBe('');
        expect(b.brand.style.visibility).toBe('');
    });

    it('keeps the name, not the title, when both no longer fit', () => {
        const b = bar();
        fitStreamBrand(b.brand, 331);
        expect(b.title.style.display).toBe('none');
        expect(b.box.style.display).toBe('');
        expect(b.version.style.display).toBe('none');
        expect(b.brand.style.maxWidth).toBe('');
        expect(b.brand.style.visibility).toBe('');
    });

    it('cuts the name to the room it has', () => {
        const b = bar();
        fitStreamBrand(b.brand, 104 + 16 + 50);
        expect(b.title.style.display).toBe('none');
        expect(b.box.style.display).toBe('');
        expect(b.brand.style.maxWidth).toBe(`${16 + 50}px`);
    });

    it('shows nothing where only a letter of the name would show', () => {
        const b = bar();
        fitStreamBrand(b.brand, 104 + 16 + 20);
        expect(b.box.style.display).toBe('none');
        expect(b.brand.style.visibility).toBe('hidden');
    });

    it('shows the whole mark again once there is room', () => {
        const b = bar();
        fitStreamBrand(b.brand, 104 + 16 + 20);
        expect(b.brand.style.visibility).toBe('hidden');
        fitStreamBrand(b.brand, 200);
        expect(b.brand.style.visibility).toBe('');
        expect(b.title.style.display).toBe('none');
        // Within the CSS's 40 % too: 300 px of a 750 px bar.
        at(b.version, 0, 40);
        fitStreamBrand(b.brand, 700);
        expect(b.brand.style.visibility).toBe('');
        expect(b.title.style.display).toBe('');
        expect(b.box.style.display).toBe('');
        expect(b.version.style.display).toBe('');
        expect(b.brand.style.maxWidth).toBe('');
    });

    it('leaves a mark the CSS hides alone', () => {
        const b = bar();
        at(b.brand, 0, 0);
        fitStreamBrand(b.brand, 200);
        expect(b.brand.style.visibility).toBe('');
        expect(b.version.style.display).toBe('');
    });
});
