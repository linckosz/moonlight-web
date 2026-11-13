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
 * The QR code a TV on plain HTTP shows to sign in (issue #42).
 *
 * The encoder is Project Nayuki's QR Code generator, v1.8.0, the file
 * js/vendor/qrcodegen-v1.8.0-es5.js as published (MIT, SHA-256
 * 348b0d876f9893f6f2bafae6c9d4ddb76b69651e214f2211034922a8207da3bf). It is a
 * plain script that declares `var qrcodegen` and nothing else: loaded as a
 * script, that name lands on the page; imported as a module, it would stay
 * inside the module. So it is loaded as a script, by the TV's page only, once.
 *
 * The drawing is numbers only: the address goes into the modules, never into
 * the markup.
 */

const SRC = new URL('../vendor/qrcodegen-v1.8.0-es5.js', import.meta.url).href;
/** On the local network the file comes at once; past this, the address is shown instead. */
const LOAD_WAIT_MS = 4000;
/** The light margin a scanner needs around the code, in modules. */
const QUIET_ZONE = 4;

/**
 * @typedef {{
 *   QrCode: {
 *     encodeText: (text: string, ecl: object) => {size: number, getModule: (x: number, y: number) => boolean},
 *     Ecc: {MEDIUM: object},
 *   },
 * }} QrCodeGen
 */

/** @type {Promise<QrCodeGen|null>|null} */
let loading = null;

/** @returns {QrCodeGen|null} */
function loaded() {
    const lib = /** @type {any} */ (globalThis).qrcodegen;
    return lib?.QrCode?.encodeText ? lib : null;
}

/**
 * The encoder, loaded once. Null when the file did not come: the TV then
 * shows the address as text.
 * @param {Document} [doc]
 * @param {number} [waitMs]
 * @returns {Promise<QrCodeGen|null>}
 */
export function loadQrEncoder(doc = document, waitMs = LOAD_WAIT_MS) {
    if (loaded()) return Promise.resolve(loaded());
    if (loading) return loading;
    loading = new Promise((resolve) => {
        const script = doc.createElement('script');
        let timer = null;
        const settle = (lib) => {
            clearTimeout(timer);
            // A failed load may be asked again later.
            if (!lib) loading = null;
            resolve(lib);
        };
        timer = setTimeout(() => settle(null), waitMs);
        script.addEventListener('load', () => settle(loaded()));
        script.addEventListener('error', () => settle(null));
        script.src = SRC;
        doc.head.appendChild(script);
    });
    return loading;
}

/**
 * @p text as a QR code, in SVG markup: dark on light whatever the theme, as
 * every scanner reads it, with its quiet zone.
 * @param {QrCodeGen} lib
 * @param {string} text
 * @returns {string}
 */
export function svgOf(lib, text) {
    const qr = lib.QrCode.encodeText(String(text), lib.QrCode.Ecc.MEDIUM);
    const size = Number(qr.size) | 0;
    const side = size + QUIET_ZONE * 2;
    let path = '';
    for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
            if (qr.getModule(x, y)) path += `M${x + QUIET_ZONE} ${y + QUIET_ZONE}h1v1h-1z`;
        }
    }
    return (
        `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${side} ${side}" ` +
        `shape-rendering="crispEdges" aria-hidden="true" focusable="false">` +
        `<rect width="${side}" height="${side}" fill="#fff"/>` +
        `<path d="${path}" fill="#000"/></svg>`
    );
}

/**
 * What TvSignInView draws its QR code with, or null without the encoder.
 * One that cannot be drawn (too long an address) gives '', and the view
 * shows the address instead.
 * @returns {Promise<((text: string) => string)|null>}
 */
export async function qrDrawer() {
    const lib = await loadQrEncoder();
    if (!lib) return null;
    return (text) => {
        try {
            return svgOf(lib, text);
        } catch {
            return '';
        }
    };
}

/** For the tests: forget a load in progress. */
export function _resetQrLoad() {
    loading = null;
}
