/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { MoonlightApp } from '../js/app.js';
import { HostListView } from '../js/ui/HostListView.js';
import { Host } from '../js/models/Host.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';
import { t } from '../js/i18n/i18n.js';

/**
 * A host that does not show the certificate of its pairing: the card says so in
 * place of the app grid, and neither the grid nor the transport chain asks again.
 * The grid comes back by itself once the server has seen the right machine.
 */

const LOCALES = join(dirname(fileURLToPath(import.meta.url)), '..', 'locales');
const PAIRED = { uuid: 'host-1', name: 'Desk', state: 'online', pairState: 'paired' };
const refusal = () =>
    Object.assign(new Error('not the certificate of the pairing'), {
        statusCode: 409,
        responseBody: { code: 'certificate_changed' },
    });

describe('the host card', () => {
    let view;
    let list;

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="root"></div>';
        vi.spyOn(console, 'error').mockImplementation(() => {});
        list = vi.spyOn(BackendClient, 'getAppList');
    });
    afterEach(() => {
        if (view) view.destroy();
        view = null;
        vi.restoreAllMocks();
        document.body.innerHTML = '';
    });

    const mount = (data) => {
        view = new HostListView(document.getElementById('root'));
        view.hosts = [new Host(data)];
        view.renderList();
        return document.querySelector('.host-card');
    };

    it('says so, in place of the grid, and asks the host nothing', () => {
        const card = mount({ ...PAIRED, certificateChanged: true });
        expect(card.querySelector('.status-badge').textContent).toBe(
            t('hosts.statusCertificateChanged'),
        );
        expect(card.querySelector('.host-card-body').textContent.trim()).toBe(
            t('hosts.certificateChangedMessage'),
        );
        expect(card.querySelector('.host-apps')).toBeNull();
        expect(list).not.toHaveBeenCalled();
    });

    it('keeps the way out its text names, and offers no share', () => {
        const card = mount({ ...PAIRED, certificateChanged: true });
        expect(card.querySelector('.btn-remove')).not.toBeNull();
        expect(card.querySelector('.btn-share')).toBeNull();
    });

    it('stays as it was on a host off the network', () => {
        const card = mount({ ...PAIRED, state: 'offline', certificateChanged: true });
        expect(card.querySelector('.status-badge').textContent).toBe(t('hosts.statusOffline'));
        expect(card.querySelector('.host-card-body').textContent).not.toContain(
            t('hosts.certificateChangedMessage'),
        );
    });

    it('is repainted by the poll, both ways', () => {
        view = new HostListView(document.getElementById('root'));
        expect(view._cardFingerprint(new Host(PAIRED))).not.toBe(
            view._cardFingerprint(new Host({ ...PAIRED, certificateChanged: true })),
        );
    });

    it('takes a refused app list to the message, with no retry and no red line', async () => {
        list.mockRejectedValue(refusal());
        const card = mount(PAIRED);
        expect(card.querySelector('.host-apps')).not.toBeNull();
        await vi.waitFor(() =>
            expect(document.querySelector('.status-badge').textContent).toBe(
                t('hosts.statusCertificateChanged'),
            ),
        );
        expect(document.querySelector('.host-apps-error')).toBeNull();
        expect(view._appRetryTimers['host-1']).toBeUndefined();
        expect(list).toHaveBeenCalledTimes(1);
    });

    it('brings the grid back by itself when the poll says the right machine answers', async () => {
        list.mockRejectedValueOnce(refusal());
        mount(PAIRED);
        await vi.waitFor(() => expect(document.querySelector('.host-apps')).toBeNull());

        list.mockResolvedValue({ status: 'ok', apps: [{ id: 7, name: 'Desktop' }] });
        view.hosts = [new Host(PAIRED)];
        view.renderList();
        await vi.waitFor(() => expect(document.querySelector('.app-card')).not.toBeNull());
        expect(document.querySelector('.host-apps-error')).toBeNull();
    });

    it('leaves any other refusal of the list as it was', async () => {
        list.mockRejectedValue(
            Object.assign(new Error('no longer paired'), { statusCode: 409, responseBody: {} }),
        );
        mount(PAIRED);
        await vi.waitFor(() =>
            expect(document.querySelector('.host-apps-error')?.textContent).toBe(
                'no longer paired',
            ),
        );
    });
});

describe('the transport chain', () => {
    let saved;
    let launch;
    let toast;
    let quit;

    const outgoing = () => ({
        onQuit: () => {},
        captureViewState: () => null,
        captureLastFrameAsync: async () => null,
        quit: vi.fn(async () => {}),
        _firstFrameRendered: true,
    });

    beforeEach(() => {
        saved = { ...MoonlightApp };
        document.body.innerHTML = '<div id="app"></div>';
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_noteLiveQuality').mockImplementation(() => {});
        launch = vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(refusal());
        toast = vi.spyOn(Toast, 'error').mockImplementation(() => {});
        quit = vi.spyOn(MoonlightApp, '_onStreamingQuit').mockImplementation(() => {});
        MoonlightApp._lastStreamHost = { uuid: 'host-1' };
        MoonlightApp._lastStreamApp = { id: 7 };
        MoonlightApp._lastStreamingSettings = {};
        MoonlightApp._degradeOverrides = {};
        MoonlightApp._transportChain = ['webrtc-dc-udp', 'webrtc-dc-tcp', 'wss'];
        MoonlightApp._transportIndex = 0;
        MoonlightApp._relaunching = false;
        MoonlightApp._standbyView = null;
        MoonlightApp._startupAborted = false;
        MoonlightApp._launchRetried = false;
        MoonlightApp._firstTransportRetried = false;
        MoonlightApp._reconnectOf = null;
        MoonlightApp.hostListView = { confirmAppExists: vi.fn(async () => true) };
        MoonlightApp._nav = { ...(MoonlightApp._nav || {}), overlay: 'streaming' };
    });
    afterEach(() => {
        vi.restoreAllMocks();
        Object.assign(MoonlightApp, saved);
        document.body.innerHTML = '';
    });

    const settle = () => new Promise((resolve) => setTimeout(resolve, 20));

    it('stops at the first refusal, on the first rung', async () => {
        MoonlightApp.streamView = outgoing();
        await MoonlightApp._relaunchTransport(0);
        await settle();
        expect(launch).toHaveBeenCalledTimes(1);
        expect(toast).toHaveBeenCalledTimes(1);
        expect(toast).toHaveBeenCalledWith(t('hosts.certificateChangedMessage'));
        expect(quit).toHaveBeenCalledTimes(1);
    });

    it('does not walk on where a failed launch would have', async () => {
        const warn = vi.spyOn(Toast, 'warning').mockImplementation(() => {});
        MoonlightApp._firstTransportRetried = true;
        MoonlightApp._launchRetried = true;
        MoonlightApp.streamView = outgoing();
        await MoonlightApp._relaunchTransport(0);
        await settle();
        expect(launch).toHaveBeenCalledTimes(1);
        expect(warn).not.toHaveBeenCalled();
        expect(quit).toHaveBeenCalledTimes(1);
    });

    it('puts the refusal on the card', async () => {
        MoonlightApp.streamView = outgoing();
        await MoonlightApp._relaunchTransport(0);
        await settle();
        expect(MoonlightApp.hostListView.confirmAppExists).toHaveBeenCalledWith(
            MoonlightApp._lastStreamHost,
            MoonlightApp._lastStreamApp,
        );
    });

    it('asks once on a quality step too, without the classic relaunch behind it', async () => {
        const relaunch = vi.spyOn(MoonlightApp, '_relaunchTransport');
        const live = outgoing();
        MoonlightApp.streamView = live;
        MoonlightApp._activeSlot = 0;
        await MoonlightApp._seamlessRelaunch(0);
        await settle();
        expect(launch).toHaveBeenCalledTimes(1);
        expect(relaunch).not.toHaveBeenCalled();
        expect(toast).toHaveBeenCalledWith(t('hosts.certificateChangedMessage'));
        expect(live.quit).toHaveBeenCalledWith({ silent: true });
    });

    it('says it in the viewer language on the first launch, and goes no further', async () => {
        const transition = vi.spyOn(MoonlightApp, 'transition').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_enterLaunchFullscreen').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_probeUdpCached').mockImplementation(async () => null);
        vi.spyOn(console, 'log').mockImplementation(() => {});
        MoonlightApp.hostListView.clearLaunching = vi.fn();
        MoonlightApp.hostListView.start = vi.fn();
        await MoonlightApp.launchApp(
            new Host(PAIRED),
            { id: 7, name: 'Desktop' },
            undefined,
            undefined,
            { skipSelfStreamWarn: true, runningChecked: true },
        );
        await settle();
        expect(launch).toHaveBeenCalledTimes(1);
        expect(toast).toHaveBeenCalledTimes(1);
        expect(toast).toHaveBeenCalledWith(t('hosts.certificateChangedMessage'));
        expect(MoonlightApp.hostListView.confirmAppExists).toHaveBeenCalledTimes(1);
        expect(transition).toHaveBeenLastCalledWith('app_list');
    });

    it('still retries a launch that failed for any other reason', async () => {
        launch.mockReset();
        launch
            .mockRejectedValueOnce(Object.assign(new Error('Bad gateway'), { statusCode: 502 }))
            .mockResolvedValueOnce({ status: 'streaming' });
        vi.spyOn(MoonlightApp, '_startStreamView').mockImplementation(() => {
            MoonlightApp.streamView = { restoreViewState: vi.fn() };
        });
        MoonlightApp.streamView = outgoing();
        await MoonlightApp._relaunchTransport(0);
        await vi.waitFor(() => expect(launch).toHaveBeenCalledTimes(2));
        expect(toast).not.toHaveBeenCalled();
    });
});

describe('its texts', () => {
    it('exist in every language, as they were agreed', () => {
        const read = (lang) =>
            JSON.parse(readFileSync(join(LOCALES, `${lang}.json`), 'utf8')).hosts;
        expect(read('en').statusCertificateChanged).toBe('Certificate changed');
        expect(read('fr').statusCertificateChanged).toBe('Certificat changé');
        expect(read('zh').statusCertificateChanged).toBe('证书已更改');
        expect(read('en').certificateChangedMessage).toBe(
            'This host is not showing the certificate it was paired with, so nothing is sent to it. If you reinstalled its software, remove it and add it again, then pair it.',
        );
        expect(read('fr').certificateChangedMessage).toBe(
            'Cet hôte ne présente pas le certificat de son appairage : rien ne lui est envoyé. Si vous avez réinstallé son logiciel, retirez-le, ajoutez-le de nouveau puis appairez-le.',
        );
        expect(read('zh').certificateChangedMessage).toBe(
            '此主机出示的证书与配对时的证书不一致，因此不会向其发送任何内容。如果您重新安装了它的软件，请将其移除并重新添加，然后再次配对。',
        );
    });
});
