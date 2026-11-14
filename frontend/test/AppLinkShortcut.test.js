// The banner of a page being made into a link's home-screen icon
// (js/ui/AppLinkShortcut.js, issue #63): no share button of the page's own,
// whose sheet never offers "Add to Home Screen"; Done puts the page back and
// reloads it; a touch anywhere else puts it back without a reload, and what was
// touched still happens.
// @vitest-environment jsdom

import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { showAppLinkShortcut, closeAppLinkShortcut } from '../js/ui/AppLinkShortcut.js';

const banner = () => document.querySelector('.app-link-banner');

describe('the banner of a link’s icon', () => {
    let onClose;

    beforeEach(() => {
        document.body.innerHTML = '<button id="card">Display 1</button>';
        onClose = vi.fn();
        showAppLinkShortcut({
            title: 'Display 1 · DualRTX',
            hint: 'Use the browser’s Share button',
            onClose,
        });
    });

    afterEach(() => {
        closeAppLinkShortcut();
        document.body.innerHTML = '';
    });

    it('says the app, the host and the browser’s button, with Done alone', () => {
        expect(banner().textContent).toContain('Display 1 · DualRTX');
        expect(banner().textContent).toContain('Use the browser’s Share button');
        const buttons = banner().querySelectorAll('button');
        expect(buttons).toHaveLength(1);
        expect(buttons[0].dataset.act).toBe('done');
        expect(banner().querySelector('[data-act="share"]')).toBeNull();
    });

    it('Done puts the page back and reloads it', () => {
        banner().querySelector('[data-act="done"]').click();
        expect(banner()).toBeNull();
        expect(onClose).toHaveBeenCalledTimes(1);
        expect(onClose).toHaveBeenCalledWith({ reload: true });
    });

    it('a touch outside puts it back without a reload, and the touch goes on', () => {
        const card = document.getElementById('card');
        const clicked = vi.fn();
        card.addEventListener('click', clicked);
        const down = new Event('pointerdown', { bubbles: true, cancelable: true });
        card.dispatchEvent(down);
        card.click();
        expect(banner()).toBeNull();
        expect(onClose).toHaveBeenCalledWith({ reload: false });
        expect(down.defaultPrevented).toBe(false);
        expect(clicked).toHaveBeenCalledTimes(1);
        // Once: a second touch has no banner to close.
        card.dispatchEvent(new Event('pointerdown', { bubbles: true }));
        expect(onClose).toHaveBeenCalledTimes(1);
    });

    it('a touch on the banner itself leaves it up', () => {
        banner()
            .querySelector('strong')
            .dispatchEvent(new Event('pointerdown', { bubbles: true }));
        expect(banner()).not.toBeNull();
        expect(onClose).not.toHaveBeenCalled();
    });

    it('a launch closes it as a touch elsewhere does', () => {
        closeAppLinkShortcut({ reload: false });
        expect(banner()).toBeNull();
        expect(onClose).toHaveBeenCalledWith({ reload: false });
    });
});
