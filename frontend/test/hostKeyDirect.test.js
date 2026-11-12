// Spending the key of a link on a page the host serves itself: this browser's
// own key goes with it, and the host's identity comes back to be kept
// (BackendClient.redeemHostKey). Through the rendezvous, neither.

import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/util/pairingCrypto.js', async (original) => {
    const real = await original();
    return {
        ...real,
        loadOrCreateIdentity: vi.fn(),
        rememberHostIdentity: vi.fn(),
    };
});

import { BackendClient } from '../js/api/BackendClient.js';
import { loadOrCreateIdentity, rememberHostIdentity } from '../js/util/pairingCrypto.js';

function jsonResponse(body, { ok = true, status = 200 } = {}) {
    return {
        ok,
        status,
        json: async () => body,
        text: async () => JSON.stringify(body),
    };
}

/** fetch, answering the admin-key probe a write performs and `answer` to the rest. */
function mockFetch(answer) {
    const fetchMock = vi.fn().mockImplementation((path) => {
        if (path === '/api/admin/token') return Promise.resolve(jsonResponse({ token: 'ADMIN' }));
        return Promise.resolve(answer);
    });
    fetchMock.sent = () => {
        const call = fetchMock.mock.calls.find((c) => c[0] === '/api/auth/host-key');
        return call ? JSON.parse(call[1].body) : null;
    };
    vi.stubGlobal('fetch', fetchMock);
    return fetchMock;
}

describe('redeemHostKey on a page the host serves itself', () => {
    beforeEach(() => {
        vi.clearAllMocks();
        BackendClient._hadSession = false;
    });

    it("sends this browser's key with the key of the link", async () => {
        loadOrCreateIdentity.mockResolvedValue({ publicKeyBase64: 'BROWSER-KEY' });
        const fetchMock = mockFetch(jsonResponse({ status: 'ok' }));
        await BackendClient.redeemHostKey('LINK-KEY', { direct: true });
        expect(fetchMock.sent()).toEqual({ key: 'LINK-KEY', public_key: 'BROWSER-KEY' });
        expect(BackendClient._hadSession).toBe(true);
    });

    it("keeps the host's identity the answer carries", async () => {
        loadOrCreateIdentity.mockResolvedValue({ publicKeyBase64: 'BROWSER-KEY' });
        mockFetch(
            jsonResponse({
                status: 'ok',
                pairing_key: 'bound',
                host_public_key: 'HOST-KEY',
                host_id: 'HOST-ID',
            }),
        );
        await BackendClient.redeemHostKey('LINK-KEY', { direct: true });
        expect(rememberHostIdentity).toHaveBeenCalledTimes(1);
        expect(rememberHostIdentity).toHaveBeenCalledWith('HOST-KEY', 'HOST-ID');
    });

    it('keeps nothing when the answer names no host', async () => {
        loadOrCreateIdentity.mockResolvedValue({ publicKeyBase64: 'BROWSER-KEY' });
        mockFetch(jsonResponse({ status: 'ok' }));
        await BackendClient.redeemHostKey('LINK-KEY', { direct: true });
        expect(rememberHostIdentity).not.toHaveBeenCalled();
    });

    it('sends the key of the link alone from a browser that can hold no key', async () => {
        // No WebCrypto, or no storage for it: the host answers that, not the page.
        loadOrCreateIdentity.mockResolvedValue(null);
        const fetchMock = mockFetch(jsonResponse({ status: 'ok' }));
        await BackendClient.redeemHostKey('LINK-KEY', { direct: true });
        expect(fetchMock.sent()).toEqual({ key: 'LINK-KEY' });
    });
});

describe('redeemHostKey through the rendezvous', () => {
    beforeEach(() => {
        vi.clearAllMocks();
    });

    it("sends the key of the link alone: the line's key is the browser's", async () => {
        loadOrCreateIdentity.mockResolvedValue({ publicKeyBase64: 'BROWSER-KEY' });
        const fetchMock = mockFetch(
            jsonResponse({ status: 'ok', host_public_key: 'HOST-KEY', host_id: 'HOST-ID' }),
        );
        await BackendClient.redeemHostKey('LINK-KEY');
        expect(fetchMock.sent()).toEqual({ key: 'LINK-KEY' });
        expect(loadOrCreateIdentity).not.toHaveBeenCalled();
        // The page pinned the host as it arrived on the line: nothing is rewritten.
        expect(rememberHostIdentity).not.toHaveBeenCalled();
    });
});
