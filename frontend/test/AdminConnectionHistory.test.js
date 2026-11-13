/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Active Sessions and plain HTTP (issue #42): a TV signed in over plain HTTP
 * wears its tag in the table, and the connection history is downloaded by
 * the administrator, with the admin key, as a CSV file.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
// No locale is loaded in the TNR: render the key instead.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
    getLanguage: () => 'en',
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('AdminView: a TV on plain HTTP in Active Sessions', () => {
    it('wears the Plain HTTP tag, and the other devices do not', () => {
        const view = new AdminView(document.body, () => {});
        const html = view._buildSessionsTableHtml([
            { token: 'a', machine_name: 'Living room TV', ip: '192.168.1.50', lan_http: true },
            { token: 'b', machine_name: 'Bruno', ip: '192.168.1.177' },
        ]);
        document.body.innerHTML = html;
        const rows = document.querySelectorAll('tbody tr');
        expect(rows).toHaveLength(2);
        expect(rows[0].querySelector('.session-plain-http-badge')?.textContent).toBe(
            'admin.plainHttp',
        );
        expect(rows[1].querySelector('.session-plain-http-badge')).toBeNull();
    });
});

describe('BackendClient.downloadConnectionHistory', () => {
    beforeEach(() => {
        vi.restoreAllMocks();
    });

    it('asks with a POST and the admin key, and hands back the file', async () => {
        vi.spyOn(BackendClient, '_adminKey').mockResolvedValue('admin-key');
        const fetchMock = vi.fn(async () => new Response('"Start"\r\n', { status: 200 }));
        vi.stubGlobal('fetch', fetchMock);
        const blob = await BackendClient.downloadConnectionHistory();
        expect(fetchMock).toHaveBeenCalledTimes(1);
        const [path, init] = fetchMock.mock.calls[0];
        expect(path).toBe('/api/admin/connection-history');
        expect(init.method).toBe('POST');
        expect(init.headers['X-MW-Admin-Key']).toBe('admin-key');
        expect(blob.size).toBe('"Start"\r\n'.length);
        vi.unstubAllGlobals();
    });

    it('says so when the download fails', async () => {
        vi.spyOn(BackendClient, 'downloadConnectionHistory').mockRejectedValue(new Error('403'));
        const view = new AdminView(document.body, () => {});
        await view._downloadConnectionHistory();
        expect(Toast.error).toHaveBeenCalledWith('admin.connectionHistoryFailed');
    });
});
