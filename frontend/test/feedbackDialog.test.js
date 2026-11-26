/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Send feedback (Admin → Feedback, A174). Pinned down: Send stays off below
 * ten characters and on a malformed e-mail address; the count reads against
 * the server's bound, which is also the box's maxlength; the "Include version"
 * line shows exactly what the server reported and there is no box when it
 * reported nothing; Send hands over the message, the trimmed address, the box
 * and the trap field; a refusal says "too many" on 429 and "unavailable"
 * otherwise, and keeps what was typed; nothing typed ever reaches the console;
 * the section is last on the page, and absent when the server cannot send.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
// No locale is loaded in the TNR: render the key so the assertions can name it.
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
    getLanguage: () => 'en',
}));

import {
    showFeedbackDialog,
    attachmentText,
    feedbackErrorText,
    emailAccepted,
    privacyUrl,
} from '../js/ui/FeedbackDialog.js';
import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const flush = () => new Promise((r) => setTimeout(r, 0));
const MESSAGE = 'The stream stutters on my TV after ten minutes.';
const EMAIL = 'someone@example.org';

/** A BackendError as BackendClient throws it. */
function refusal(status, error) {
    const e = /** @type {any} */ (new Error(error));
    e.statusCode = status;
    e.responseBody = { error };
    return e;
}

function type(el, value) {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
}

function open(overrides = {}) {
    const send = vi.fn().mockResolvedValue({ status: 'ok' });
    const onSent = vi.fn();
    const d = showFeedbackDialog({
        maxMessage: 1600,
        attachment: { version: '0.4.4', edition: 'release', os: 'windows' },
        send,
        onSent,
        ...overrides,
    });
    const q = (sel) => d.el.querySelector(sel);
    return {
        d,
        send: overrides.send || send,
        onSent,
        message: /** @type {HTMLTextAreaElement} */ (q('#feedback-message')),
        email: /** @type {HTMLInputElement} */ (q('#feedback-email')),
        include: /** @type {HTMLInputElement|null} */ (q('#feedback-include-version')),
        website: /** @type {HTMLInputElement} */ (q('input[name="website"]')),
        sendBtn: /** @type {HTMLButtonElement} */ (q('.feedback-send')),
        cancelBtn: /** @type {HTMLButtonElement} */ (q('.feedback-cancel')),
        count: q('.feedback-count'),
        status: q('.feedback-status'),
        form: /** @type {HTMLFormElement} */ (q('form')),
    };
}

describe('the feedback dialog', () => {
    afterEach(() => {
        document.body.innerHTML = '';
    });

    it('keeps Send off below ten characters, and counts against the bound', () => {
        const f = open();
        expect(f.message.maxLength).toBe(1600);
        expect(f.count.textContent).toBe('0 / 1600');
        expect(f.sendBtn.disabled).toBe(true);
        type(f.message, '   short   ');
        expect(f.sendBtn.disabled).toBe(true);
        type(f.message, MESSAGE);
        expect(f.count.textContent).toBe(`${MESSAGE.length} / 1600`);
        expect(f.sendBtn.disabled).toBe(false);
    });

    it('keeps Send off on a malformed address, on when empty or valid', () => {
        const f = open();
        type(f.message, MESSAGE);
        type(f.email, 'not an address');
        expect(f.sendBtn.disabled).toBe(true);
        type(f.email, EMAIL);
        expect(f.sendBtn.disabled).toBe(false);
        type(f.email, '');
        expect(f.sendBtn.disabled).toBe(false);
    });

    it("keeps Send off on what the server would refuse, though the browser's check passes it", () => {
        // The server's and the relay's rule, not type=email's.
        expect(emailAccepted('')).toBe(true);
        expect(emailAccepted('  ' + EMAIL + '  ')).toBe(true);
        expect(emailAccepted('first.last+tag@mail.example.co.uk')).toBe(true);
        expect(emailAccepted('a@b')).toBe(false); // no top-level domain
        expect(emailAccepted('josé@example.org')).toBe(false); // not ASCII
        expect(emailAccepted('someone@exämple.org')).toBe(false);
        expect(emailAccepted('a@b.c')).toBe(false);
        expect(emailAccepted('a b@example.org')).toBe(false);
        expect(emailAccepted('`@example.org')).toBe(false);
        expect(emailAccepted('a'.repeat(64) + '@' + 'b'.repeat(63) + '.org')).toBe(true);
        expect(emailAccepted('a'.repeat(65) + '@example.org')).toBe(false);
        // Within every label's bound, over the 254 of the whole.
        const domain = ['b', 'c', 'd', 'e'].map((c) => c.repeat(60)).join('.');
        const long = 'a'.repeat(64) + '@' + domain + '.org';
        expect(long.length).toBeGreaterThan(254);
        expect(emailAccepted(long)).toBe(false);

        const f = open();
        type(f.message, MESSAGE);
        for (const bad of ['a@b', 'josé@example.org']) {
            type(f.email, bad);
            expect(f.sendBtn.disabled).toBe(true);
        }
        type(f.email, EMAIL);
        expect(f.sendBtn.disabled).toBe(false);
    });

    it('shows exactly what the box would attach, ticked, and no box without it', () => {
        expect(attachmentText({ version: '0.4.4', edition: 'release', os: 'windows' })).toBe(
            '0.4.4, release, windows',
        );
        expect(attachmentText({ edition: 'dev', os: 'linux' })).toBe('dev, linux');
        expect(attachmentText(null)).toBe('');

        const f = open();
        expect(f.include.checked).toBe(true);
        expect(f.d.el.querySelector('.feedback-include').textContent).toBe(
            'feedback.includeVersion {"details":"0.4.4, release, windows"}',
        );
        document.body.innerHTML = '';
        const g = open({ attachment: null });
        expect(g.d.el.querySelector('.feedback-include')).toBeNull();
    });

    it("ends its notice on a Privacy link to the FAQ's answer, in a new tab", () => {
        expect(privacyUrl('en')).toBe('https://moonlightweb.top/faq.html#q16');
        expect(privacyUrl('fr')).toBe('https://moonlightweb.top/fr/faq.html#q16');
        expect(privacyUrl('zh')).toBe('https://moonlightweb.top/zh/faq.html#q16');
        // A language the site does not have reads the English page.
        expect(privacyUrl('tr')).toBe('https://moonlightweb.top/faq.html#q16');

        const f = open();
        const a = /** @type {HTMLAnchorElement} */ (f.d.el.querySelector('.feedback-notice a'));
        expect(a.textContent).toBe('feedback.privacy');
        expect(a.href).toBe('https://moonlightweb.top/faq.html#q16');
        expect(a.target).toBe('_blank');
        expect(a.rel).toBe('noopener noreferrer');
        expect(a.href).not.toContain('?');
    });

    it('sends the message, the trimmed address, the box and the trap field', async () => {
        const f = open();
        type(f.message, MESSAGE);
        type(f.email, '  ' + EMAIL + '  ');
        f.include.checked = false;
        f.form.dispatchEvent(new Event('submit', { cancelable: true }));
        expect(f.status.textContent).toBe('feedback.sending');
        expect(f.message.disabled && f.cancelBtn.disabled && f.sendBtn.disabled).toBe(true);
        await flush();
        expect(f.send).toHaveBeenCalledTimes(1);
        expect(f.send.mock.calls[0][0]).toEqual({
            message: MESSAGE,
            email: EMAIL,
            include_version: false,
            website: '',
        });
        expect(document.body.contains(f.d.el)).toBe(false);
        expect(f.onSent).toHaveBeenCalledTimes(1);
    });

    it('passes the trap field as a bot filled it', async () => {
        const f = open();
        type(f.message, MESSAGE);
        f.website.value = 'http://spam.example';
        f.form.dispatchEvent(new Event('submit', { cancelable: true }));
        await flush();
        expect(f.send.mock.calls[0][0].website).toBe('http://spam.example');
        expect(f.website.tabIndex).toBe(-1);
        expect(f.website.closest('[aria-hidden="true"]')).not.toBeNull();
    });

    it('says "too many" on 429 and "unavailable" otherwise, and keeps the text', async () => {
        expect(feedbackErrorText(refusal(429, 'rate_limited'))).toBe('feedback.rateLimited');
        for (const [s, e] of [
            [503, 'unavailable'],
            [502, 'unreachable'],
            [409, 'busy'],
            [400, 'message_short'],
        ])
            expect(feedbackErrorText(refusal(s, e))).toBe('feedback.unavailable');
        expect(feedbackErrorText(new Error('server_timeout'))).toBe('feedback.unavailable');

        const send = vi.fn().mockRejectedValue(refusal(429, 'rate_limited'));
        const f = open({ send });
        type(f.message, MESSAGE);
        f.form.dispatchEvent(new Event('submit', { cancelable: true }));
        await flush();
        expect(f.status.textContent).toBe('feedback.rateLimited');
        expect(f.status.classList.contains('err')).toBe(true);
        expect(document.body.contains(f.d.el)).toBe(true);
        expect(f.message.value).toBe(MESSAGE);
        expect(f.sendBtn.disabled).toBe(false);
        expect(f.onSent).not.toHaveBeenCalled();
    });

    it('closes on Cancel and Escape, but not while sending', async () => {
        let release;
        const send = vi.fn(() => new Promise((r) => (release = r)));
        const f = open({ send });
        type(f.message, MESSAGE);
        f.form.dispatchEvent(new Event('submit', { cancelable: true }));
        document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
        f.cancelBtn.click();
        expect(document.body.contains(f.d.el)).toBe(true);
        release({ status: 'ok' });
        await flush();
        expect(document.body.contains(f.d.el)).toBe(false);

        const g = open();
        document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
        expect(document.body.contains(g.d.el)).toBe(false);
        const h = open();
        h.cancelBtn.click();
        expect(document.body.contains(h.d.el)).toBe(false);
    });
});

describe('nothing typed reaches the console (F3)', () => {
    const methods = /** @type {const} */ (['log', 'info', 'warn', 'error', 'debug', 'trace']);
    let spies;
    beforeEach(() => {
        spies = methods.map((m) => vi.spyOn(console, m).mockImplementation(() => {}));
    });
    afterEach(() => {
        spies.forEach((s) => s.mockRestore());
        vi.restoreAllMocks();
        document.body.innerHTML = '';
    });

    it('through a success and a refusal of the real client', async () => {
        const responses = [
            new Response(JSON.stringify({ status: 'ok' }), { status: 200 }),
            new Response(JSON.stringify({ error: 'unavailable' }), { status: 503 }),
        ];
        vi.spyOn(BackendClient, '_adminKey').mockResolvedValue('');
        const fetchSpy = vi.spyOn(globalThis, 'fetch').mockImplementation(() =>
            Promise.resolve(responses.shift()),
        );
        for (let i = 0; i < 2; i++) {
            const f = open({ send: (b) => BackendClient.sendFeedback(b) });
            type(f.message, MESSAGE);
            type(f.email, EMAIL);
            f.form.dispatchEvent(new Event('submit', { cancelable: true }));
            await flush();
            await flush();
        }
        expect(fetchSpy).toHaveBeenCalledTimes(2);
        expect(fetchSpy.mock.calls[0][0]).toBe('/api/feedback');
        const said = spies.flatMap((s) => s.mock.calls.flat()).map((a) => String(a));
        for (const line of said) {
            expect(line).not.toContain(MESSAGE);
            expect(line).not.toContain(EMAIL);
        }
    });
});

describe('the Feedback section', () => {
    afterEach(() => {
        vi.restoreAllMocks();
        document.body.innerHTML = '';
    });

    it('is absent when the server cannot send, or is too old for the route', async () => {
        const view = new AdminView(document.body, () => {});
        vi.spyOn(BackendClient, 'getAdminSettings').mockResolvedValue({});
        const status = vi.spyOn(BackendClient, 'getFeedbackStatus');

        status.mockResolvedValue({ available: false, max_message: 1600, attachment: {} });
        await view._loadState();
        expect(view._feedback).toBeNull();
        expect(view._renderFeedback()).toBe('');

        status.mockRejectedValue(refusal(404, 'not_found'));
        await view._loadState();
        expect(view._feedback).toBeNull();

        // Even a call that throws at once leaves the settings loaded.
        status.mockImplementation(() => {
            throw new TypeError('not a function');
        });
        vi.mocked(BackendClient.getAdminSettings).mockResolvedValue({ https_port: 8443 });
        await view._loadState();
        expect(view._feedback).toBeNull();
        expect(view._httpsPort).toBe(8443);

        const f = { available: true, max_message: 1600, attachment: { os: 'linux' } };
        status.mockResolvedValue(f);
        await view._loadState();
        expect(view._feedback).toBe(f);
        expect(view._renderFeedback()).toContain('id="btn-send-feedback"');
    });

    it('comes after Advanced, and its button opens the dialog with the bound', () => {
        const view = new AdminView(document.body, () => {});
        view._feedback = { available: true, max_message: 1234, attachment: null };
        const html = view._renderAdvanced() + view._renderFeedback();
        expect(html.indexOf('admin-section-feedback')).toBeGreaterThan(
            html.indexOf('admin-section-advanced'),
        );
        view._openFeedback();
        const box = /** @type {HTMLTextAreaElement} */ (document.querySelector('#feedback-message'));
        expect(box.maxLength).toBe(1234);
        expect(Toast.success).not.toHaveBeenCalled();
    });
});
