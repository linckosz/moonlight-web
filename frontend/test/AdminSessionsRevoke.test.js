/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Revoking a session from the admin table. The table re-renders on a timer, so
 * the row under the pointer can change between reading it and clicking it —
 * these tests pin down that a click always revokes the row it landed on, and
 * that the user is told which device that is before anything is destroyed.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { revokeSession: vi.fn(async () => ({ status: 'revoked' })) },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
// No locale is loaded in the TNR, so t() would swallow the interpolation the
// confirmation depends on. Render the parameters instead.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';

describe('AdminView session revoke', () => {
    let view;
    let container;

    const sessions = [
        { token: 'tok-host', machine_name: 'Host machine', created_at: 300, current: true },
        { token: 'tok-linux', machine_name: 'Linux Chrome', created_at: 200 },
    ];

    const revokeButton = (name) =>
        Array.from(container.querySelectorAll('.btn-session-revoke')).find(
            (b) => b.dataset.machine === name,
        );

    beforeEach(() => {
        vi.clearAllMocks();
        document.body.innerHTML = '<div id="admin-sessions-table"></div>';
        container = document.body;
        view = new AdminView(container, () => {});
        view._sessions = sessions.map((s) => ({ ...s }));
        view._renderSessionsTable();
    });

    afterEach(() => vi.unstubAllGlobals());

    it('revokes the session of the button that was clicked', async () => {
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true),
        );
        await view._revokeSession(revokeButton('Linux Chrome'));
        expect(BackendClient.revokeSession).toHaveBeenCalledWith('tok-linux');
    });

    it('names the clicked device in the confirmation', async () => {
        const confirmMock = vi.fn(() => true);
        vi.stubGlobal('confirm', confirmMock);
        await view._revokeSession(revokeButton('Linux Chrome'));
        expect(confirmMock.mock.calls[0][0]).toContain('Linux Chrome');
    });

    it('destroys nothing when the confirmation is declined', async () => {
        vi.stubGlobal(
            'confirm',
            vi.fn(() => false),
        );
        await view._revokeSession(revokeButton('Linux Chrome'));
        expect(BackendClient.revokeSession).not.toHaveBeenCalled();
    });

    it('warns differently for the browser reading the page', async () => {
        const confirmMock = vi.fn(() => false);
        vi.stubGlobal('confirm', confirmMock);
        await view._revokeSession(revokeButton('Host machine'));
        const ownQuestion = confirmMock.mock.calls[0][0];

        confirmMock.mockClear();
        await view._revokeSession(revokeButton('Linux Chrome'));
        expect(confirmMock.mock.calls[0][0]).not.toBe(ownQuestion);
    });

    it('marks its own session in the table', () => {
        const rows = container.querySelectorAll('.sessions-table tbody tr');
        expect(rows[0].classList.contains('session-row-current')).toBe(true);
        expect(rows[0].querySelector('.session-current-badge')).not.toBeNull();
        expect(rows[1].querySelector('.session-current-badge')).toBeNull();
    });

    it('does not rebuild identical rows, so the click target stays put', () => {
        const before = revokeButton('Linux Chrome');
        // Same data → the poll must leave the DOM alone.
        expect(view._sessionsSignature()).toBe(view._sessionsRendered);

        // A change the table shows → the signature moves and a re-render is due.
        view._sessions[1].streaming = true;
        expect(view._sessionsSignature()).not.toBe(view._sessionsRendered);
        view._renderSessionsTable();
        expect(revokeButton('Linux Chrome')).not.toBe(before);
    });
});
