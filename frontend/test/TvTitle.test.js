/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A TV on plain HTTP (issue #42) names the PC in its header, which the
 * stream's bar repeats: its name and version, as a PC's page shows them. The
 * TV's start ended before the step that reads them, and its bar said
 * "MoonlightWeb" alone (14/11/2026).
 */

// A TV's page: plain HTTP, on the PC's address.
// @vitest-environment-options {"url": "http://192.168.1.10:8080/"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { listInstances, rememberInstance } from '../js/util/instances.js';

describe('a TV on plain HTTP names the PC in its header', () => {
    let saved;
    let get;

    beforeEach(() => {
        saved = {
            instanceMenu: MoonlightApp.instanceMenu,
            _plainHttpTv: MoonlightApp._plainHttpTv,
            _hasAdminAccess: MoonlightApp._hasAdminAccess,
        };
        MoonlightApp.instanceMenu = null;
        localStorage.clear();
        document.body.className = '';
        document.body.innerHTML =
            '<header class="app-header"><h1>MoonlightWeb</h1>' +
            '<div class="instance-menu" id="instance-menu" hidden></div>' +
            '<span class="version" hidden></span></header>' +
            '<div id="main-content"></div>';
        // Signed in: the status route says so.
        vi.stubGlobal(
            'fetch',
            vi.fn(async (url) => {
                if (url === '/api/lan-http/status')
                    return { ok: true, json: async () => ({ signed_in: true }) };
                throw new Error(`not asked here: ${url}`);
            }),
        );
        get = vi.spyOn(BackendClient, 'get').mockImplementation(async (path) => {
            if (path === '/api/health') return { status: 'ok', version: '0.4.4', name: 'DualRTX' };
            throw new Error(`not asked here: ${path}`);
        });
        // The rest of the start is not what this is about.
        vi.spyOn(MoonlightApp, '_initRouter').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
        Object.assign(MoonlightApp, saved);
        document.body.innerHTML = '';
        document.body.className = '';
        localStorage.clear();
    });

    it('shows the PC name and version once signed in, as on a PC', async () => {
        expect(await MoonlightApp._initPlainHttpTv()).toBe(true);
        const version = /** @type {HTMLElement} */ (document.querySelector('.app-header .version'));
        await vi.waitFor(() => expect(version.textContent).toBe('v0.4.4'));
        expect(version.hidden).toBe(false);
        expect(
            document.querySelector('.app-header #instance-menu .instance-name')?.textContent,
        ).toBe('DualRTX');
    });

    it('gives the name alone: no menu of other machines, nothing in its register', async () => {
        // A machine this browser knew before: a PC's page would offer it.
        rememberInstance({
            id: 'https://other.example',
            url: 'https://other.example/',
            name: 'Other PC',
        });
        await MoonlightApp._initPlainHttpTv();
        await vi.waitFor(() =>
            expect(document.querySelector('#instance-menu .instance-name')?.textContent).toBe(
                'DualRTX',
            ),
        );
        expect(document.querySelector('#instance-menu .instance-btn')).toBeNull();
        expect(listInstances().map((e) => e.name)).toEqual(['Other PC']);
    });

    it('leaves the header as it was when the PC does not answer', async () => {
        get.mockImplementation(async () => {
            throw new Error('offline');
        });
        expect(await MoonlightApp._initPlainHttpTv()).toBe(true);
        await vi.waitFor(() => expect(console.warn).toHaveBeenCalled());
        expect(
            /** @type {HTMLElement} */ (document.querySelector('.app-header .version')).hidden,
        ).toBe(true);
        expect(document.getElementById('instance-menu')?.hidden).toBe(true);
    });
});
