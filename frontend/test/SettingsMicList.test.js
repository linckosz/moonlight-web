/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

import { SettingsView } from '../js/ui/SettingsView.js';
import { Toast } from '../js/ui/Toast.js';
import { loadMicDevice } from '../js/audio/MicUplink.js';

/**
 * Settings → Microphone: off, the system's (the default), then the named ones.
 * A browser names its microphones only once one has been allowed, so the list
 * asks for names on demand, and what the viewer had chosen stands until they
 * pick something else.
 */

const proto = SettingsView.prototype;

/** Just what the list's methods read. */
function view(over = {}) {
    return {
        _micEnabled: true,
        _micChosen: false,
        _micDevice: null,
        _micValue: proto._micValue,
        _micListHtml: proto._micListHtml,
        _fillMicList: proto._fillMicList,
        _askMicNames: proto._askMicNames,
        _takeMicChoice: proto._takeMicChoice,
        ...over,
    };
}

function select(v, named = []) {
    const list = document.createElement('select');
    list.innerHTML = v._micListHtml(named);
    return list;
}

const values = (list) => Array.from(list.options).map((o) => o.value);

describe('Settings microphone list', () => {
    let devices;
    let getUserMedia;

    beforeEach(() => {
        localStorage.removeItem('mw-mic-device');
        devices = [{ kind: 'audioinput', label: '', deviceId: '' }];
        getUserMedia = vi.fn(async () => ({ getTracks: () => [{ stop: vi.fn() }] }));
        Object.defineProperty(navigator, 'mediaDevices', {
            configurable: true,
            value: { enumerateDevices: async () => devices, getUserMedia },
        });
    });

    afterEach(() => {
        delete navigator.mediaDevices;
        vi.restoreAllMocks();
    });

    it('offers off and the system, the system chosen, and asks for names', () => {
        const list = select(view());
        expect(values(list)).toEqual(['off', 'system', 'pick']);
        expect(list.value).toBe('system');
    });

    it('lists the named ones without the question once they are known', () => {
        const list = select(view(), [{ id: 'usb', label: 'USB' }]);
        expect(values(list)).toEqual(['off', 'system', 'device:usb']);
    });

    it('keeps the one picked earlier when it is not listed', () => {
        const v = view({ _micDevice: { id: 'usb', label: 'USB <mic>' } });
        const list = select(v);
        expect(values(list)).toEqual(['off', 'system', 'device:usb', 'pick']);
        expect(list.value).toBe('device:usb');
        // A device's name is the browser's text, not markup.
        expect(list.options[2].textContent).toBe('USB <mic>');
    });

    it('selects off when turned off, whatever device is remembered', () => {
        const list = select(view({ _micEnabled: false, _micDevice: { id: 'usb', label: 'USB' } }));
        expect(list.value).toBe('off');
    });

    it('keeps a named choice, apart from the settings', () => {
        const v = view();
        const list = select(v, [{ id: 'usb', label: 'USB' }]);
        list.value = 'device:usb';
        v._takeMicChoice(list);
        expect(v._micEnabled).toBe(true);
        expect(v._micChosen).toBe(true);
        expect(v._micDevice).toEqual({ id: 'usb', label: 'USB' });
        expect(loadMicDevice()).toEqual({ id: 'usb', label: 'USB' });
    });

    it('going back to the system forgets the device', () => {
        const v = view({ _micDevice: { id: 'usb', label: 'USB' } });
        const list = select(v);
        list.value = 'system';
        v._takeMicChoice(list);
        expect(v._micDevice).toBe(null);
        expect(loadMicDevice()).toBe(null);
        expect(v._micChosen).toBe(true);
    });

    it('turning it off is a choice, and remembers the device for later', () => {
        const v = view({ _micDevice: { id: 'usb', label: 'USB' } });
        const list = select(v);
        list.value = 'off';
        v._takeMicChoice(list);
        expect(v._micEnabled).toBe(false);
        expect(v._micChosen).toBe(true);
        expect(v._micDevice).toEqual({ id: 'usb', label: 'USB' });
    });

    it('opens a microphone only for the question, then shows the names', async () => {
        const stop = vi.fn();
        getUserMedia.mockResolvedValue({ getTracks: () => [{ stop }] });
        const v = view();
        const list = select(v);
        list.value = 'pick';
        devices = [{ kind: 'audioinput', label: 'USB', deviceId: 'usb' }];
        await v._askMicNames(list);
        expect(stop).toHaveBeenCalled();
        expect(values(list)).toEqual(['off', 'system', 'device:usb']);
        // Nothing chosen yet: the viewer picks in the list that came back.
        expect(list.value).toBe('system');
        expect(v._micChosen).toBe(false);
    });

    it('a refusal changes nothing and is said', async () => {
        const warning = vi.spyOn(Toast, 'warning').mockImplementation(() => {});
        getUserMedia.mockRejectedValue(Object.assign(new Error('no'), { name: 'NotAllowedError' }));
        const v = view();
        const list = select(v);
        list.value = 'pick';
        await v._askMicNames(list);
        expect(warning).toHaveBeenCalledTimes(1);
        expect(list.value).toBe('system');
        expect(values(list)).toEqual(['off', 'system', 'pick']);
    });
});
