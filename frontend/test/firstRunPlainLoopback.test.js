/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The first-run wizard on plain HTTP loopback (server/FirstRunLink.h). A first
 * launch on macOS or Linux opens http://localhost:<port>/setup, so that the
 * first page is the wizard and not the self-signed certificate's warning. That
 * origin belongs to whoever holds the port at the time, so it keeps nothing:
 * no pairing key is made or read there, a request that would carry one is not
 * sent at all (never sent without it), a host key reaching it is dropped
 * unspent, and the page does nothing but the wizard.
 */

// @vitest-environment-options {"url": "http://localhost:8080/setup"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import {
    PLAIN_LOOPBACK,
    clearIdentity,
    loadOrCreateIdentity,
    onPlainLoopback,
    rememberHostIdentity,
} from '../js/util/pairingCrypto.js';
import { startTunnel } from '../js/net/tunnelBridge.js';

describe('which pages are the plain-loopback origin', () => {
    const at = (href) => {
        const u = new URL(href);
        return { protocol: u.protocol, hostname: u.hostname };
    };

    it('is this page', () => {
        expect(onPlainLoopback()).toBe(true);
    });

    it.each([
        'http://localhost:8080/setup',
        'http://LOCALHOST/setup',
        'http://127.0.0.1:8080/',
        'http://127.4.5.6/',
        'http://[::1]:8080/',
    ])('holds for %s', (href) => {
        expect(onPlainLoopback(at(href))).toBe(true);
    });

    it.each([
        // HTTPS loopback: where the key is made, after the wizard.
        'https://localhost:8443/',
        'https://127.0.0.1/',
        // A TV on plain HTTP (issue #42) is on the LAN, and left as it is.
        'http://192.168.1.66:8080/',
        'http://gaming-pc.local:8080/',
        'http://localhost.evil.example/',
        'http://127.0.0.1.evil.example/',
    ])('does not hold for %s', (href) => {
        expect(onPlainLoopback(at(href))).toBe(false);
    });

    it('does not hold without a location', () => {
        expect(onPlainLoopback(null)).toBe(false);
    });
});

describe('no key there, and nothing sent in its place', () => {
    let post;
    let fetchSpy;
    let opened;
    let savedFetch;

    beforeEach(() => {
        post = vi.spyOn(BackendClient, 'post').mockResolvedValue({});
        savedFetch = globalThis.fetch;
        fetchSpy = vi.fn(async () => ({ ok: true, json: async () => ({}) }));
        globalThis.fetch = fetchSpy;
        opened = vi.fn();
        globalThis.indexedDB = { open: opened };
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
    });
    afterEach(() => {
        delete globalThis.indexedDB;
        globalThis.fetch = savedFetch;
        vi.restoreAllMocks();
    });

    it('makes and opens no key', async () => {
        await expect(loadOrCreateIdentity()).rejects.toThrow(PLAIN_LOOPBACK);
        await expect(loadOrCreateIdentity({ forHost: 'other' })).rejects.toThrow(PLAIN_LOOPBACK);
        expect(await rememberHostIdentity('a2V5', 'host')).toBe(false);
        await clearIdentity();
        expect(opened).not.toHaveBeenCalled();
    });

    it.each([
        ['pairing-key', () => BackendClient.ensurePairingKey()],
        ['validate (PIN)', () => BackendClient.validatePin('1234', 'pc')],
        ['validate (certificate)', () => BackendClient.validateCertificate('cert', 'pc')],
        ['host-key', () => BackendClient.redeemHostKey('k', { direct: true })],
        ['home-screen', () => BackendClient.redeemHomeScreenKey('hs-k', 'pc')],
        ['pair-request approve', () => BackendClient.approvePairRequest('id', '123456', 'host')],
        ['pair-request refuse', () => BackendClient.refusePairRequest('id', 'host')],
    ])('sends no %s request', async (_name, call) => {
        await expect(call()).rejects.toThrow(PLAIN_LOOPBACK);
        expect(post).not.toHaveBeenCalled();
        expect(fetchSpy).not.toHaveBeenCalled();
        expect(opened).not.toHaveBeenCalled();
    });

    it('looks for no rendezvous worker', async () => {
        const worker = { addEventListener: vi.fn(), controller: null };
        Object.defineProperty(navigator, 'serviceWorker', { value: worker, configurable: true });
        try {
            expect(await startTunnel()).toBe(false);
            expect(worker.addEventListener).not.toHaveBeenCalled();
        } finally {
            delete navigator.serviceWorker;
        }
    });
});

describe('the page does nothing but the wizard', () => {
    let saved;

    beforeEach(() => {
        saved = { setupView: MoonlightApp.setupView };
        document.body.innerHTML = '<div id="main-content"></div>';
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
    });
    afterEach(() => {
        Object.assign(MoonlightApp, saved);
        vi.restoreAllMocks();
    });

    const spies = () => ({
        handoff: vi.spyOn(MoonlightApp, '_offerHomeScreenHandoff').mockResolvedValue(null),
        nav: vi.spyOn(MoonlightApp, '_initNavButtons').mockImplementation(() => {}),
        router: vi.spyOn(MoonlightApp, '_initRouter').mockImplementation(() => {}),
        control: vi.spyOn(MoonlightApp, '_initControlChannel').mockImplementation(() => {}),
        housekeeping: vi.spyOn(MoonlightApp, '_initAsync').mockImplementation(() => {}),
        gate: vi.spyOn(MoonlightApp, '_renderInsecureGate').mockResolvedValue(undefined),
    });

    it('shows the wizard when it is due, and starts nothing else', async () => {
        const s = spies();
        vi.spyOn(MoonlightApp, '_maybeShowSetup').mockResolvedValue(true);
        await MoonlightApp._startSignedIn();
        expect(s.handoff).not.toHaveBeenCalled();
        expect(s.nav).not.toHaveBeenCalled();
        expect(s.router).not.toHaveBeenCalled();
        expect(s.control).not.toHaveBeenCalled();
        expect(s.housekeeping).not.toHaveBeenCalled();
        expect(s.gate).not.toHaveBeenCalled();
    });

    it('past the wizard, offers HTTPS and starts nothing else', async () => {
        const s = spies();
        vi.spyOn(MoonlightApp, '_maybeShowSetup').mockResolvedValue(false);
        await MoonlightApp._startSignedIn();
        expect(s.gate).toHaveBeenCalledTimes(1);
        expect(s.handoff).not.toHaveBeenCalled();
        expect(s.nav).not.toHaveBeenCalled();
        expect(s.router).not.toHaveBeenCalled();
        expect(s.control).not.toHaveBeenCalled();
        expect(s.housekeeping).not.toHaveBeenCalled();
    });

    it('compares no session key, and makes none', async () => {
        const ensure = vi.spyOn(BackendClient, 'ensurePairingKey');
        expect(await MoonlightApp._sessionHasAnotherKey({ is_localhost: true })).toBe(false);
        expect(ensure).not.toHaveBeenCalled();
    });
});
