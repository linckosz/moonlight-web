/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Coming back to the entry page (bootstrap/v1/boot.js) after it had failed
 * (01/11/2026: Chrome on an iPhone showed the last visit's "Not connected" for a
 * couple of seconds over a machine that was on by then). Pinned down: put away,
 * a failed page goes back to its opening look, which is what the phone's picture
 * of it shows next time; shown again, it starts over; a page that had not failed
 * is left alone.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

vi.mock('../../bootstrap/v1/tunnel.js', async (original) => ({
    ...(await original()),
    hostIdFromLocation: () => 'a'.repeat(26),
    knownInstances: () => [],
    rememberLastHost: () => {},
}));

let visibility = 'visible';
/** Each start is navigated away by a reload, which jsdom reports and skips. */
let reloads = 0;

function setVisibility(value) {
    visibility = value;
    document.dispatchEvent(new Event('visibilitychange'));
}

async function startPage() {
    vi.resetModules();
    await import('../../bootstrap/v1/boot.js');
    await vi.waitFor(() => expect(document.body.dataset.state).toBe('failed'));
}

beforeEach(() => {
    reloads = 0;
    visibility = 'visible';
    Object.defineProperty(document, 'visibilityState', {
        configurable: true,
        get: () => visibility,
    });
    vi.stubGlobal('requestAnimationFrame', () => 0);
    // No service worker: the quickest failure the page has, said at once.
    vi.stubGlobal('navigator', { userAgent: 'test' });
    document.body.innerHTML =
        '<h1 id="stage"></h1><p id="detail"></p><div id="bar"></div><p id="status"></p>' +
        '<p id="note"></p><div id="instance-menu" hidden></div>' +
        '<button id="forget" hidden></button>';
    delete document.body.dataset.state;
    const error = console.error;
    vi.spyOn(console, 'error').mockImplementation((...a) => {
        if (String(a[0]).includes('Not implemented: navigation')) reloads++;
        else error(...a);
    });
});

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    delete document.visibilityState;
});

// One page for the whole walk: every import of boot.js leaves its listeners on
// the same document, and a second page would hear the first one's events.
describe('the entry page, coming back to it', () => {
    it('starts over only a page that had failed, and shows it as an opening meanwhile', async () => {
        await startPage();
        const failure = document.getElementById('note').textContent;
        expect(failure).not.toBe('');

        // Working when put away: left alone. (Hand-set: the same page, as it
        // looks while it works.)
        document.body.dataset.state = 'working';
        setVisibility('hidden');
        setVisibility('visible');
        expect(reloads).toBe(0);
        expect(document.body.dataset.state).toBe('working');

        // Failed when put away: the opening look for the phone's picture...
        document.body.dataset.state = 'failed';
        document.getElementById('note').textContent = failure;
        setVisibility('hidden');
        expect(document.body.dataset.state).toBe('working');
        expect(document.getElementById('note').textContent).toBe('');
        expect(reloads).toBe(0);

        // ...and a fresh start when shown again.
        setVisibility('visible');
        expect(reloads).toBe(1);

        // Brought back failed from the back-forward cache: the same.
        document.body.dataset.state = 'failed';
        const shown = new Event('pageshow');
        Object.defineProperty(shown, 'persisted', { value: true });
        window.dispatchEvent(shown);
        expect(reloads).toBe(2);
    });
});
