/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/*
 * The Internet question in the first-run wizard, which is a consent and not a
 * preference. Three properties, each of which fails silently if it breaks: the
 * page cannot be applied while the question is unanswered, neither answer is
 * pre-selected, and what is sent as the agreement is the wording that was on
 * screen. A regression here does not throw — it records an answer nobody gave.
 *
 * It is also the first of the config step's two pages, and the reason there are
 * two: the consent is the only thing on this screen that must be answered, so
 * it is asked alone and everything after it is optional.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getSetupStatus: vi.fn(),
        applySetup: vi.fn(async () => ({ ok: true, internet_active: false })),
        verifySunshineCreds: vi.fn(async () => ({ ok: true })),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { SetupView } from '../js/ui/SetupView.js';
import { BackendClient } from '../js/api/BackendClient.js';

describe('SetupView — the Internet question', () => {
    let container;
    let view;

    // Drop straight into the config step with an instance that is asking (no
    // live Internet link, Sunshine already paired so nothing else holds Start).
    function showConfig() {
        view = new SetupView(container, () => {});
        view._step = 'config';
        view._internetActive = false;
        view._sunshineInstalled = true;
        view._sunshinePaired = true;
        view._autostartInstalled = true;
        view.render();
        view.bindEvents();
        return view;
    }

    const skipBtn = () => container.querySelector('#btn-internet-skip');
    const acceptBtn = () => container.querySelector('#btn-internet-accept');
    const startBtn = () => container.querySelector('#btn-setup-start');

    beforeEach(() => {
        vi.clearAllMocks();
        document.body.innerHTML = '<div id="setup"></div>';
        container = document.getElementById('setup');
    });

    it('offers two buttons and pre-selects neither', () => {
        showConfig();
        expect(skipBtn()).not.toBeNull();
        expect(acceptBtn()).not.toBeNull();
        expect(skipBtn().getAttribute('aria-pressed')).toBe('false');
        expect(acceptBtn().getAttribute('aria-pressed')).toBe('false');
        expect(container.querySelector('.setup-choice-btn.is-chosen')).toBeNull();
    });

    it('puts Skip before Accept, so the affirmative answer is on the right', () => {
        showConfig();
        const buttons = [...container.querySelectorAll('.setup-choice-btn')].map((b) => b.id);
        expect(buttons).toEqual(['btn-internet-skip', 'btn-internet-accept']);
    });

    it('asks the question alone, with nothing else on the page to press', async () => {
        showConfig();
        // Page one carries the consent and no way past it: the guard is the
        // shape of the page, not a greyed-out button several sections below the
        // reason it is grey.
        expect(startBtn()).toBeNull();
        expect(container.querySelector('#chk-autostart')).toBeNull();
        expect(container.querySelector('#chk-display-awake')).toBeNull();

        // And it still refuses to apply if something reaches _apply() another way.
        await view._apply();
        expect(BackendClient.applySetup).not.toHaveBeenCalled();
    });

    it('turns the page on either answer, with Done live straight away', () => {
        showConfig();
        skipBtn().click();
        expect(view._internetAuth).toBe(false);
        expect(skipBtn()).toBeNull();
        expect(startBtn().disabled).toBe(false);
    });

    it('restates the answer on page two, and lets it be changed', () => {
        showConfig();
        acceptBtn().click();
        expect(container.innerHTML).toContain('text:setup.internetChosenYes');

        container.querySelector('#btn-internet-back').click();
        // Back on the question, with the previous answer still chosen — going
        // back must not silently clear a consent that was given.
        expect(acceptBtn().getAttribute('aria-pressed')).toBe('true');
        expect(view._internetAuth).toBe(true);
    });

    it('sends the agreement text with an accepted answer, and none with a refusal', async () => {
        showConfig();
        acceptBtn().click();
        expect(view._internetAuth).toBe(true);

        await view._apply();
        expect(BackendClient.applySetup).toHaveBeenCalledTimes(1);
        const sent = BackendClient.applySetup.mock.calls[0][0];
        expect(sent.internet_access_authorized).toBe(true);
        // What is recorded is what was displayed: the body plus the sentence
        // the Accept button agrees to.
        expect(sent.consent_message).toBe('text:setup.internetBody / text:setup.internetOption');
    });

    it('records no agreement when the answer was Skip', async () => {
        showConfig();
        skipBtn().click();
        await view._apply();
        const sent = BackendClient.applySetup.mock.calls[0][0];
        expect(sent.internet_access_authorized).toBe(false);
        expect(sent.consent_message).toBe('');
    });

    it('asks nothing when the link is already up', () => {
        view = new SetupView(container, () => {});
        view._step = 'config';
        view._internetActive = true;
        view._sunshineInstalled = true;
        view._sunshinePaired = true;
        view._autostartInstalled = true;
        view.render();
        view.bindEvents();

        expect(skipBtn()).toBeNull();
        expect(acceptBtn()).toBeNull();
        // …and an instance with nothing to ask must not be stuck on Start.
        expect(startBtn().disabled).toBe(false);
    });
});
