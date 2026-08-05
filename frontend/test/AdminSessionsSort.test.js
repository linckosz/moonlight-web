/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach } from 'vitest';
import { AdminView } from '../js/ui/AdminView.js';

// Column sorting of the admin "Active Sessions" table. Only the table helpers
// are exercised — no backend, no polling.
describe('AdminView sessions sort', () => {
    let view;
    let container;

    const sessions = [
        { token: 'a', machine_name: 'zeta', created_at: 300, ip: '192.168.1.10' },
        { token: 'b', machine_name: 'Alpha', created_at: 100, ip: '192.168.1.9' },
        { token: 'c', machine_name: 'mid', created_at: 200, location: 'Local', streaming: true },
    ];

    const names = () =>
        Array.from(container.querySelectorAll('.session-name-edit')).map((el) => el.textContent);

    const clickHeader = (key) => {
        container.querySelector(`.sessions-th-sortable[data-sort="${key}"]`).click();
    };

    beforeEach(() => {
        document.body.innerHTML = '<div id="admin-sessions-table"></div>';
        container = document.body;
        view = new AdminView(container, () => {});
        view._sessions = sessions.map((s) => ({ ...s }));
        view._renderSessionsTable();
    });

    it('keeps the backend order until a header is clicked', () => {
        expect(names()).toEqual(['zeta', 'Alpha', 'mid']);
    });

    it('sorts by machine name, case-insensitively, and flips on a second click', () => {
        clickHeader('machine');
        expect(names()).toEqual(['Alpha', 'mid', 'zeta']);
        clickHeader('machine');
        expect(names()).toEqual(['zeta', 'mid', 'Alpha']);
    });

    it('sorts the authorized column by timestamp, not by its formatted text', () => {
        clickHeader('authorized');
        expect(names()).toEqual(['Alpha', 'mid', 'zeta']);
    });

    it('orders IPv4 addresses numerically and puts the local label last', () => {
        clickHeader('ip');
        expect(names()).toEqual(['Alpha', 'zeta', 'mid']);
    });

    it('marks the active header and shows its direction', () => {
        clickHeader('machine');
        const th = container.querySelector('.sessions-th-sortable[data-sort="machine"]');
        expect(th.getAttribute('aria-sort')).toBe('ascending');
        expect(th.querySelector('.sessions-sort-arrow').textContent).toBe('▲');
        clickHeader('machine');
        const flipped = container.querySelector('.sessions-th-sortable[data-sort="machine"]');
        expect(flipped.getAttribute('aria-sort')).toBe('descending');
    });

    it('returns to the first page when the order changes', () => {
        view._sessionsPerPage = 2;
        view._sessionPage = 1;
        view._renderSessionsTable();
        clickHeader('machine');
        expect(view._sessionPage).toBe(0);
        expect(names()).toEqual(['Alpha', 'mid']);
    });
});
