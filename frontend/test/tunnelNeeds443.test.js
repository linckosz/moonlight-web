/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The tunnel's hello says the page needs TCP 443 (needs443: true) when UDP
 * does not get out of this network, or when a try of this page found no way
 * through: a yes, and nothing else (no port, no address). Otherwise the hello
 * is the one it always was.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

let tunnelModule;

beforeEach(async () => {
    vi.resetModules();
    tunnelModule = await import('../../bootstrap/v1/tunnel.js');
    tunnelModule.forgetNoRoute();
});

afterEach(() => {
    vi.unstubAllGlobals();
});

const hello = { type: 'hello', protocol: 1, key_id: 'k', public_key: 'p', nonce: 'n' };

describe('the hello and TCP 443', () => {
    it('is the same hello where UDP gets out, or where the browser cannot ask', () => {
        for (const udpOut of [true, null]) {
            const out = tunnelModule.helloWith443(hello, udpOut, false);
            expect(out).toEqual(hello);
            expect('needs443' in out).toBe(false);
        }
    });

    it('says it needs 443, and only that, when UDP does not get out', () => {
        const out = tunnelModule.helloWith443(hello, false, false);
        expect(out).toEqual({ ...hello, needs443: true });
        expect(Object.keys(out).sort()).toEqual([...Object.keys(hello), 'needs443'].sort());
        // The hello it was made from is left as it was
        expect('needs443' in hello).toBe(false);
    });

    it('says so after a try that found no way through, whatever UDP does', () => {
        for (const udpOut of [true, false, null])
            expect(tunnelModule.helloWith443(hello, udpOut, true).needs443).toBe(true);
    });
});

describe('the STUN probe of the hello', () => {
    it('cannot ask without WebRTC: null', async () => {
        vi.stubGlobal('RTCPeerConnection', undefined);
        expect(await tunnelModule.udpGetsOut('example.test', 10)).toBe(null);
    });

    it('a server-reflexive UDP candidate is a yes; nothing in time is a no', async () => {
        const made = [];
        class FakePc {
            constructor(config) {
                this.config = config;
                made.push(this);
            }
            createDataChannel() {}
            createOffer() {
                return Promise.resolve({});
            }
            setLocalDescription() {
                return Promise.resolve();
            }
            close() {
                this.closed = true;
            }
        }
        vi.stubGlobal('RTCPeerConnection', FakePc);
        const yes = tunnelModule.udpGetsOut('example.test', 1000);
        await Promise.resolve();
        // Asked of the introduction server's own STUN, and of no other
        expect(made[0].config).toEqual({ iceServers: [{ urls: 'stun:example.test:3478' }] });
        made[0].onicecandidate({ candidate: { type: 'srflx', protocol: 'udp', candidate: '' } });
        expect(await yes).toBe(true);
        expect(made[0].closed).toBe(true);

        const no = tunnelModule.udpGetsOut('example.test', 5);
        expect(await no).toBe(false);
        const ended = tunnelModule.udpGetsOut('example.test', 1000);
        await Promise.resolve();
        made[2].onicecandidate({ candidate: null });
        expect(await ended).toBe(false);
    });
});
