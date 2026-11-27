/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { nonceOf, openableTarget, handOff } from '../js/ownerHandoff.js';

// The page a link of this machine opens first (owner-handoff.html,
// server/OwnerHandoff.h): it trades the nonce in its fragment, once, for the
// owner's link, and opens that link in its own place.

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const NONCE = 'AbCdEfGhIjKlMnOpQrSt_-';
const ORIGIN = 'http://127.0.0.1:48080';
const LINK = 'https://localhost:48443/admin#k=KEY';

/** A window of the page as the browser opens it, its fetch answering `answer`. */
function pageWindow(answer, hash = `#n=${NONCE}`) {
    const steps = [];
    document.body.innerHTML = '<main class="handoff"><p id="handoff-reopen" hidden>EN</p></main>';
    const win = {
        location: {
            hash,
            pathname: '/owner-handoff',
            origin: ORIGIN,
            replace: vi.fn((url) => steps.push(['replace', url])),
        },
        history: {
            replaceState: vi.fn((state, title, url) => steps.push(['replaceState', url])),
        },
        fetch: vi.fn(async (url, init) => {
            steps.push(['fetch', url, init]);
            if (answer instanceof Error) throw answer;
            return answer;
        }),
        document,
    };
    return { win, steps };
}

const ok = (body) => ({ ok: true, status: 200, json: async () => body });
const refused = { ok: false, status: 403, json: async () => ({ error: 'reopen' }) };

beforeEach(() => {
    // The catalogs the refusal's sentence is read from (i18n.js asks the global fetch).
    const en = JSON.parse(readFileSync(join(ROOT, 'locales', 'en.json'), 'utf8'));
    vi.stubGlobal(
        'fetch',
        vi.fn(async () => ({ ok: true, status: 200, json: async () => en })),
    );
});

afterEach(() => {
    vi.unstubAllGlobals();
    document.body.innerHTML = '';
});

describe('nonceOf', () => {
    it('reads the nonce of the fragment, and nothing else', () => {
        expect(nonceOf(`#n=${NONCE}`)).toBe(NONCE);
        expect(nonceOf('')).toBeNull();
        expect(nonceOf(undefined)).toBeNull();
        expect(nonceOf(`#n=${NONCE}A`)).toBeNull();
        expect(nonceOf(`#n=${NONCE.slice(1)}`)).toBeNull();
        expect(nonceOf(`#n=${NONCE.slice(1)}+`)).toBeNull();
        expect(nonceOf(`#k=${NONCE}`)).toBeNull();
        expect(nonceOf(`#n=${NONCE}&k=x`)).toBeNull();
    });
});

describe('openableTarget', () => {
    it('opens an https: link, or one on the page’s own origin, and nothing else', () => {
        expect(openableTarget(LINK, ORIGIN)).toBe(LINK);
        expect(openableTarget('https://abc.moonlightweb.top/#k=K&p=/admin', ORIGIN)).toBe(
            'https://abc.moonlightweb.top/#k=K&p=/admin',
        );
        expect(openableTarget(`${ORIGIN}/admin#k=K`, ORIGIN)).toBe(`${ORIGIN}/admin#k=K`);
        expect(openableTarget('http://evil.example/#k=K', ORIGIN)).toBeNull();
        expect(openableTarget('http://127.0.0.1:48081/#k=K', ORIGIN)).toBeNull();
        expect(openableTarget('javascript:alert(1)', ORIGIN)).toBeNull();
        expect(openableTarget('data:text/html,<p>x</p>', 'null')).toBeNull();
        expect(openableTarget('not a url', ORIGIN)).toBeNull();
        expect(openableTarget('', ORIGIN)).toBeNull();
        expect(openableTarget(undefined, ORIGIN)).toBeNull();
        expect(openableTarget({ href: LINK }, ORIGIN)).toBeNull();
    });
});

describe('handOff', () => {
    it('clears its address first, trades the nonce once, then opens the link in its place', async () => {
        const { win, steps } = pageWindow(ok({ url: LINK }));
        expect(await handOff(win)).toBe(true);
        expect(steps.map((s) => s[0])).toEqual(['replaceState', 'fetch', 'replace']);
        expect(steps[0][1]).toBe('/owner-handoff');
        const [, url, init] = steps[1];
        expect(url).toBe('/api/auth/owner-handoff');
        expect(init.method).toBe('POST');
        expect(init.headers['Content-Type']).toBe('application/json');
        expect(JSON.parse(init.body)).toEqual({ n: NONCE });
        expect(init.credentials).toBe('omit');
        expect(init.referrerPolicy).toBe('no-referrer');
        expect(init.cache).toBe('no-store');
        expect(win.location.replace).toHaveBeenCalledWith(LINK);
        expect(document.getElementById('handoff-reopen').hidden).toBe(true);
    });

    it('says to open the page again when the trade is refused', async () => {
        const { win } = pageWindow(refused);
        expect(await handOff(win)).toBe(false);
        expect(win.location.replace).not.toHaveBeenCalled();
        expect(win.fetch).toHaveBeenCalledTimes(1);
        const note = document.getElementById('handoff-reopen');
        expect(note.hidden).toBe(false);
        expect(note.textContent).toContain('taskbar');
    });

    it('opens no link it may not open', async () => {
        for (const url of ['http://evil.example/#k=K', 'javascript:alert(1)', '', null]) {
            const { win } = pageWindow(ok({ url }));
            expect(await handOff(win)).toBe(false);
            expect(win.location.replace).not.toHaveBeenCalled();
            expect(document.getElementById('handoff-reopen').hidden).toBe(false);
        }
    });

    it('asks nothing without a nonce, and says so', async () => {
        const { win } = pageWindow(ok({ url: LINK }), '#k=KEY');
        expect(await handOff(win)).toBe(false);
        expect(win.fetch).not.toHaveBeenCalled();
        expect(win.history.replaceState).toHaveBeenCalledWith(null, '', '/owner-handoff');
        expect(document.getElementById('handoff-reopen').hidden).toBe(false);
    });

    it('says so when the server cannot be reached, and keeps its English if no catalog loads', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => {
                throw new Error('offline');
            }),
        );
        const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
        const { win } = pageWindow(new Error('connection refused'));
        expect(await handOff(win)).toBe(false);
        const note = document.getElementById('handoff-reopen');
        expect(note.hidden).toBe(false);
        expect(note.textContent).toBe('EN');
        warn.mockRestore();
    });
});

describe('owner-handoff.html', () => {
    const html = readFileSync(join(ROOT, 'owner-handoff.html'), 'utf8');

    it('runs nothing inline: its policy allows its own files and nothing else', () => {
        const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)];
        expect(scripts.length).toBe(1);
        for (const [tag, body] of scripts) {
            expect(tag).toContain('src="/js/ownerHandoff.js');
            expect(body.trim()).toBe('');
        }
        expect(html).not.toMatch(/\son[a-z]+=/i);
        expect(html).not.toMatch(/\sstyle=/i);
        for (const [, ref] of html.matchAll(/(?:src|href)="([^"]+)"/g))
            expect(ref.startsWith('/') && !ref.startsWith('//')).toBe(true);
    });

    it('has the refusal’s sentence, hidden until it is needed', () => {
        expect(html).toMatch(/<p id="handoff-reopen" hidden>/);
        expect(html).toContain('<meta name="referrer" content="no-referrer" />');
    });
});
