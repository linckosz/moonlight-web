/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The QR code a TV on plain HTTP shows (issue #42): drawn by the vendored
 * encoder, loaded as a script by the TV's page only, and the address shown as
 * text whenever it cannot be drawn.
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { _resetQrLoad, loadQrEncoder, qrDrawer, svgOf } from '../js/ui/qrSvg.js';

// The encoder itself, as the TV's page runs it: a plain script. Its one name
// is returned instead of set on the page.
const VENDOR = readFileSync(
    join(dirname(fileURLToPath(import.meta.url)), '..', 'js', 'vendor', 'qrcodegen-v1.8.0-es5.js'),
    'utf8',
);
const lib = new Function(`${VENDOR}\nreturn qrcodegen;`)();

const URL_SHOWN = 'https://192.168.1.10:8443/tv-pair-AbCdEfGhIjKlMnOp';
const scripts = () => [...document.head.querySelectorAll('script')];

afterEach(() => {
    _resetQrLoad();
    vi.unstubAllGlobals();
    for (const s of scripts()) s.remove();
});

describe('svgOf', () => {
    it('draws every module, its quiet zone around, and nothing of the address', () => {
        const svg = svgOf(lib, URL_SHOWN);
        const size = lib.QrCode.encodeText(URL_SHOWN, lib.QrCode.Ecc.MEDIUM).size;
        const side = size + 8;
        expect(svg).toContain(`viewBox="0 0 ${side} ${side}"`);
        // The top-left finder: its top row dark, seven modules from the corner
        // of the quiet zone, then the light separator.
        for (let x = 4; x <= 10; x++) expect(svg).toContain(`M${x} 4h1v1h-1z`);
        expect(svg).not.toContain('M11 4h1v1h-1z');
        // Dark on light whatever the theme.
        expect(svg).toContain('fill="#fff"');
        expect(svg).toContain('fill="#000"');
        // The address is in the modules, never in the markup.
        expect(svg).not.toContain('192.168');
        expect(svg).not.toContain('tv-pair');
    });

    it('fits a TV address in a small code', () => {
        const size = lib.QrCode.encodeText(URL_SHOWN, lib.QrCode.Ecc.MEDIUM).size;
        // Version 4 or below: 33 modules, read from across a room.
        expect(size).toBeLessThanOrEqual(33);
    });
});

describe('loadQrEncoder', () => {
    it('loads the vendored file as a script, once, and hands its encoder over', async () => {
        const first = loadQrEncoder(document, 1000);
        const second = loadQrEncoder(document, 1000);
        expect(scripts()).toHaveLength(1);
        expect(scripts()[0].src).toMatch(/\/js\/vendor\/qrcodegen-v1\.8\.0-es5\.js$/);
        expect(scripts()[0].type).toBe('');
        vi.stubGlobal('qrcodegen', lib);
        scripts()[0].dispatchEvent(new Event('load'));
        expect(await first).toBe(lib);
        expect(await second).toBe(lib);
        // Loaded: nothing more is asked.
        expect(await loadQrEncoder(document, 1000)).toBe(lib);
        expect(scripts()).toHaveLength(1);
    });

    it('gives nothing when the file does not come, and may ask again', async () => {
        const failed = loadQrEncoder(document, 1000);
        scripts()[0].dispatchEvent(new Event('error'));
        expect(await failed).toBeNull();
        expect(await loadQrEncoder(document, 5)).toBeNull();
        expect(scripts()).toHaveLength(2);
    });

    it('gives nothing for a file that loads without its encoder', async () => {
        const loading = loadQrEncoder(document, 1000);
        scripts()[0].dispatchEvent(new Event('load'));
        expect(await loading).toBeNull();
    });
});

describe('qrDrawer', () => {
    it('draws with the encoder once it is there', async () => {
        vi.stubGlobal('qrcodegen', lib);
        const draw = await qrDrawer();
        expect(draw(URL_SHOWN)).toBe(svgOf(lib, URL_SHOWN));
    });

    it('gives the empty drawing for what cannot be drawn, so the address is shown', async () => {
        vi.stubGlobal('qrcodegen', lib);
        const draw = await qrDrawer();
        expect(draw('x'.repeat(4000))).toBe('');
    });
});
