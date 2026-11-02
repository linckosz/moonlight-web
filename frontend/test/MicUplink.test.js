/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 MoonlightWeb contributors.
 * GPLv3 — see repository LICENSE.
 *
 * The viewer's microphone. What matters: nothing is opened or sent unless the
 * host's offer asked for it AND the viewer opened it; every stream starts cut;
 * cutting it stops the track, so the browser's indicator goes out; only the
 * stream's audio section is answered sendrecv and gets the track; a standby leg
 * sends nothing until it is promoted, and keeps the state it inherits; a refused
 * permission is reported, never thrown.
 */
import { describe, it, expect, vi } from 'vitest';
import {
    MicUplink,
    loadMicDevice,
    micConstraints,
    micQuality,
    micRequested,
    micSupported,
    namedMicrophones,
    offerTakesMic,
    saveMicDevice,
} from '../js/audio/MicUplink.js';
import { MicSocketSender } from '../js/audio/MicSocketSender.js';
import { recvonlyExceptCarrier } from '../js/util/DscpCarrier.js';

const OFFER_SENDRECV =
    'v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio\r\na=sendrecv\r\n' +
    'm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:data\r\n';
const OFFER_SENDONLY =
    'v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio\r\na=sendonly\r\n' +
    'm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\na=sendrecv\r\n';

const sender = () => {
    const s = {
        track: null,
        replaceTrack: vi.fn(async (t) => {
            s.track = t;
        }),
    };
    return s;
};

const pcWith = (transceivers) => ({ getTransceivers: () => transceivers });

const fakeTrack = (label = 'Mic') => ({ enabled: true, readyState: 'live', label, stop: vi.fn() });

const gum = (track) =>
    vi.fn(async () => ({ getAudioTracks: () => [track], getTracks: () => [track] }));

describe('offerTakesMic', () => {
    it('is the audio section, sendrecv, and nothing else', () => {
        expect(offerTakesMic(OFFER_SENDRECV)).toBe(true);
        expect(offerTakesMic(OFFER_SENDONLY)).toBe(false);
        expect(offerTakesMic(undefined)).toBe(false);
        // Another section's sendrecv says nothing about the audio one.
        expect(offerTakesMic(OFFER_SENDONLY.replace('a=sendonly', 'a=recvonly'))).toBe(false);
    });
});

describe('micSupported', () => {
    it('needs getUserMedia in a secure context', () => {
        const nav = { mediaDevices: { getUserMedia: () => {} } };
        expect(micSupported(nav, true)).toBe(true);
        expect(micSupported(nav, false)).toBe(false);
        expect(micSupported({}, true)).toBe(false);
        expect(micSupported(undefined, true)).toBe(false);
    });
});

describe('MicUplink', () => {
    it('leaves a sendonly offer alone and never opens the microphone', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const getUserMedia = gum(fakeTrack());
        const mic = new MicUplink({ getUserMedia });
        expect(mic.prepare(pcWith([audio]), OFFER_SENDONLY)).toBe(false);
        expect(audio.direction).toBe('recvonly');
        expect(await mic.attach()).toBe(false);
        expect(getUserMedia).not.toHaveBeenCalled();
    });

    it('answers the audio section sendrecv and puts the track on it', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const video = { mid: 'video', direction: 'recvonly', sender: sender() };
        const track = fakeTrack();
        const getUserMedia = gum(track);
        const mic = new MicUplink({ getUserMedia });
        const states = [];
        mic.onStateChange = (s) => states.push(s);
        expect(mic.prepare(pcWith([video, audio]), OFFER_SENDRECV)).toBe(true);
        expect(audio.direction).toBe('sendrecv');
        expect(video.direction).toBe('recvonly');
        // Cut at the start: answered, but no prompt and no track.
        expect(await mic.attach()).toBe(false);
        expect(getUserMedia).not.toHaveBeenCalled();
        expect(audio.sender.track).toBe(null);
        expect(states).toEqual(['muted']);
        expect(await mic.toggleMuted()).toBe(true);
        expect(audio.sender.track).toBe(track);
        expect(video.sender.replaceTrack).not.toHaveBeenCalled();
        expect(getUserMedia.mock.calls[0][0].audio.echoCancellation).toBe(true);
        expect(states).toEqual(['muted', 'live']);
    });

    it('cutting stops the track, and opening again takes a new one', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const first = fakeTrack();
        const second = fakeTrack();
        const getUserMedia = vi
            .fn()
            .mockResolvedValueOnce({ getAudioTracks: () => [first], getTracks: () => [first] })
            .mockResolvedValueOnce({ getAudioTracks: () => [second], getTracks: () => [second] });
        const mic = new MicUplink({ getUserMedia });
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        await mic.setMuted(false);
        await mic.setMuted(true);
        // The indicator goes out only on a stopped track.
        expect(first.stop).toHaveBeenCalled();
        expect(audio.sender.replaceTrack).toHaveBeenLastCalledWith(null);
        expect(mic.state).toBe('muted');
        expect(await mic.setMuted(false)).toBe(true);
        expect(audio.sender.track).toBe(second);
        expect(mic.state).toBe('live');
        mic.close();
        expect(second.stop).toHaveBeenCalled();
    });

    it('a microphone cut while the prompt is up never reaches the sender', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const track = fakeTrack();
        let resolve;
        const getUserMedia = vi.fn(() => new Promise((r) => (resolve = r)));
        const mic = new MicUplink({ getUserMedia });
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        const opening = mic.setMuted(false);
        await mic.setMuted(true);
        resolve({ getAudioTracks: () => [track], getTracks: () => [track] });
        expect(await opening).toBe(false);
        expect(track.stop).toHaveBeenCalled();
        expect(audio.sender.track).toBe(null);
        expect(mic.state).toBe('muted');
    });

    it('cut then open again while the prompt is up asks only once', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const track = fakeTrack();
        let resolve;
        const getUserMedia = vi.fn(() => new Promise((r) => (resolve = r)));
        const mic = new MicUplink({ getUserMedia });
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        const first = mic.setMuted(false);
        mic.setMuted(true);
        const second = mic.setMuted(false);
        resolve({ getAudioTracks: () => [track], getTracks: () => [track] });
        await Promise.all([first, second]);
        expect(getUserMedia).toHaveBeenCalledTimes(1);
        expect(audio.sender.track).toBe(track);
        expect(track.stop).not.toHaveBeenCalled();
        expect(mic.state).toBe('live');
    });

    it('a standby leg of a cut microphone opens nothing, even promoted', async () => {
        const live = new MicUplink({ getUserMedia: gum(fakeTrack()) });
        await live.setMuted(true);
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const getUserMedia = gum(fakeTrack());
        const standby = new MicUplink({ held: true, getUserMedia });
        expect(standby.prepare(pcWith([audio]), OFFER_SENDRECV)).toBe(true);
        expect(await standby.attach()).toBe(false);
        expect(await standby.release()).toBe(false);
        expect(getUserMedia).not.toHaveBeenCalled();
        expect(standby.muted).toBe(true);
        expect(standby.state).toBe('muted');
    });

    it('a standby leg of an open microphone sends once promoted, not before', async () => {
        const live = new MicUplink({ getUserMedia: gum(fakeTrack()) });
        await live.setMuted(false);
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const track = fakeTrack();
        const standby = new MicUplink({ held: true, getUserMedia: gum(track) });
        standby.prepare(pcWith([audio]), OFFER_SENDRECV);
        expect(await standby.attach()).toBe(false);
        expect(audio.sender.replaceTrack).not.toHaveBeenCalled();
        expect(await standby.release()).toBe(true);
        expect(audio.sender.track).toBe(track);
        expect(standby.state).toBe('live');
        // A new stream starts cut again, whatever the last one did.
        expect(new MicUplink({ getUserMedia: gum(fakeTrack()) }).muted).toBe(true);
    });

    it('reports a refused permission instead of throwing', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const denied = Object.assign(new Error('no'), { name: 'NotAllowedError' });
        const mic = new MicUplink({ getUserMedia: vi.fn(async () => Promise.reject(denied)) });
        const states = [];
        mic.onStateChange = (s) => states.push(s);
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        expect(await mic.setMuted(false)).toBe(false);
        expect(mic.state).toBe('denied');
        expect(mic.muted).toBe(true);
        expect(audio.sender.replaceTrack).not.toHaveBeenCalled();
        // Another click is another try, and another answer.
        expect(await mic.toggleMuted()).toBe(false);
        expect(states).toEqual(['denied', 'muted', 'denied']);
    });

    it('a microphone that arrives after the stream ended is stopped at once', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const track = fakeTrack();
        let resolve;
        const getUserMedia = vi.fn(
            () =>
                new Promise((r) => {
                    resolve = r;
                }),
        );
        const mic = new MicUplink({ getUserMedia });
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        const attached = mic.setMuted(false);
        mic.close();
        resolve({ getAudioTracks: () => [track], getTracks: () => [track] });
        expect(await attached).toBe(false);
        expect(track.stop).toHaveBeenCalled();
        expect(audio.sender.replaceTrack).not.toHaveBeenCalled();
    });
});

describe('the microphone chosen', () => {
    const named = (label, id = label) => ({ kind: 'audioinput', label, deviceId: id });
    const stale = () => Object.assign(new Error('gone'), { name: 'OverconstrainedError' });
    const streamOf = (track) => ({ getAudioTracks: () => [track], getTracks: () => [track] });

    it('follows the system unless a device is named', () => {
        expect(micConstraints(micQuality('voice')).audio.deviceId).toBeUndefined();
        expect(micConstraints(micQuality('voice'), 'abc').audio.deviceId).toEqual({ exact: 'abc' });
    });

    it('asks for that device and no other', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const getUserMedia = gum(fakeTrack('USB'));
        const mic = new MicUplink({ device: { id: 'usb', label: 'USB' }, getUserMedia });
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        expect(await mic.setMuted(false)).toBe(true);
        expect(getUserMedia).toHaveBeenCalledTimes(1);
        expect(getUserMedia.mock.calls[0][0].audio.deviceId).toEqual({ exact: 'usb' });
    });

    it('finds it again by name when its id has expired', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const system = fakeTrack('Built-in');
        const usb = fakeTrack('USB');
        const getUserMedia = vi
            .fn()
            .mockRejectedValueOnce(stale())
            .mockResolvedValueOnce(streamOf(system))
            .mockResolvedValueOnce(streamOf(usb));
        const mic = new MicUplink({
            device: { id: 'old', label: 'USB' },
            getUserMedia,
            enumerateDevices: async () => [named('Built-in'), named('USB', 'new')],
        });
        const missing = vi.fn();
        mic.onDeviceMissing = missing;
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        expect(await mic.setMuted(false)).toBe(true);
        expect(getUserMedia.mock.calls[2][0].audio.deviceId).toEqual({ exact: 'new' });
        expect(system.stop).toHaveBeenCalled();
        expect(audio.sender.track).toBe(usb);
        expect(missing).not.toHaveBeenCalled();
    });

    it('takes the system one and says so when it is gone, once a stream', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const system = fakeTrack('Built-in');
        const getUserMedia = vi.fn(async (c) => {
            if (c.audio.deviceId) throw stale();
            return streamOf(system);
        });
        const mic = new MicUplink({
            device: { id: 'old', label: 'USB' },
            getUserMedia,
            enumerateDevices: async () => [named('Built-in')],
        });
        const missing = vi.fn();
        mic.onDeviceMissing = missing;
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        expect(await mic.setMuted(false)).toBe(true);
        expect(audio.sender.track).toBe(system);
        await mic.setMuted(true);
        await mic.setMuted(false);
        expect(missing).toHaveBeenCalledTimes(1);
    });

    it('a refusal on a named device is a refusal, not a missing device', async () => {
        const denied = Object.assign(new Error('no'), { name: 'NotAllowedError' });
        const getUserMedia = vi.fn(async () => Promise.reject(denied));
        const mic = new MicUplink({ device: { id: 'usb', label: 'USB' }, getUserMedia });
        mic.prepare(
            pcWith([{ mid: 'audio', direction: 'recvonly', sender: sender() }]),
            OFFER_SENDRECV,
        );
        await mic.setMuted(false);
        expect(getUserMedia).toHaveBeenCalledTimes(1);
        expect(mic.state).toBe('denied');
    });

    it('lists only the microphones that can be named', () => {
        expect(
            namedMicrophones([
                named('Default - USB', 'default'),
                named('Communications - USB', 'communications'),
                named('USB', 'usb'),
                named('', 'nameless'),
                { kind: 'audiooutput', label: 'Speakers', deviceId: 'out' },
            ]),
        ).toEqual([{ id: 'usb', label: 'USB' }]);
        expect(namedMicrophones(undefined)).toEqual([]);
    });

    it('keeps the device apart from the settings, and survives a bad entry', () => {
        const store = new Map();
        const storage = {
            getItem: (k) => (store.has(k) ? store.get(k) : null),
            setItem: (k, v) => store.set(k, v),
            removeItem: (k) => store.delete(k),
        };
        expect(loadMicDevice(storage)).toBe(null);
        saveMicDevice({ id: 'usb', label: 'USB' }, storage);
        expect(loadMicDevice(storage)).toEqual({ id: 'usb', label: 'USB' });
        saveMicDevice(null, storage);
        expect(loadMicDevice(storage)).toBe(null);
        store.set('mw-mic-device', 'not json');
        expect(loadMicDevice(storage)).toBe(null);
        store.set('mw-mic-device', '{"id":""}');
        expect(loadMicDevice(storage)).toBe(null);
    });

    it('asks the host by default, and not once the viewer turned it off', () => {
        expect(micRequested(undefined)).toBe(true);
        expect(micRequested({})).toBe(true);
        // Saved before the list existed: not a choice.
        expect(micRequested({ mic_enabled: false })).toBe(true);
        expect(micRequested({ mic_chosen: true, mic_enabled: false })).toBe(false);
        expect(micRequested({ mic_chosen: true, mic_enabled: true })).toBe(true);
    });
});

describe('recvonlyExceptCarrier with the microphone', () => {
    it("keeps the audio section sending under 'mic', and only it", () => {
        const out = recvonlyExceptCarrier(
            OFFER_SENDRECV + 'm=video 9 x\r\na=mid:video\r\na=sendrecv\r\n',
            'mic',
        );
        expect(out).toMatch(/a=mid:audio\r\na=sendrecv/);
        expect(out).toMatch(/a=mid:video\r\na=recvonly/);
    });
});

describe('microphone quality', () => {
    it('asks for a call-style cleanup in voice, and nothing in clean', async () => {
        const { micConstraints, micQuality } = await import('../js/audio/MicUplink.js');
        const voice = micConstraints(micQuality('voice')).audio;
        expect(voice.echoCancellation && voice.noiseSuppression && voice.autoGainControl).toBe(
            true,
        );
        const clean = micConstraints(micQuality('clean')).audio;
        expect(clean.echoCancellation || clean.noiseSuppression || clean.autoGainControl).toBe(
            false,
        );
        expect(clean.sampleRate).toBe(48000);
        // Anything unknown is the default.
        expect(micQuality('nonsense')).toBe(micQuality('voice'));
    });

    it("puts the quality's bitrate on the RTP sender", async () => {
        const s = sender();
        let params = { encodings: [{}] };
        s.getParameters = () => params;
        s.setParameters = vi.fn(async (p) => {
            params = p;
        });
        const audio = { mid: 'audio', direction: 'recvonly', sender: s };
        const mic = new MicUplink({ quality: 'clean', getUserMedia: gum(fakeTrack()) });
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        expect(await mic.setMuted(false)).toBe(true);
        expect(params.encodings[0].maxBitrate).toBe(128000);
    });
});

describe('the WebSocket transport', () => {
    const fakeSender = () => {
        const made = [];
        class Fake {
            constructor(track, send, quality, firstSequence) {
                Object.assign(this, {
                    track,
                    send,
                    quality,
                    firstSequence,
                    sequence: firstSequence + 100,
                    started: false,
                    stopped: false,
                });
                made.push(this);
            }
            async start() {
                this.started = true;
            }
            stop() {
                this.stopped = true;
            }
        }
        return { Fake, made };
    };

    it('sends on the socket with the quality, and stops with the stream', async () => {
        const { Fake, made } = fakeSender();
        const track = fakeTrack();
        const mic = new MicUplink({
            quality: 'clean',
            getUserMedia: gum(track),
            SocketSender: Fake,
        });
        const send = vi.fn(() => true);
        expect(await mic.attachSocket(send)).toBe(false);
        expect(made).toHaveLength(0);
        expect(await mic.setMuted(false)).toBe(true);
        expect(made).toHaveLength(1);
        expect(made[0].started).toBe(true);
        expect(made[0].send).toBe(send);
        expect(made[0].quality.bitrate).toBe(128000);
        expect(mic.state).toBe('live');
        await mic.toggleMuted();
        expect(made[0].stopped).toBe(true);
        expect(track.stop).toHaveBeenCalled();
        expect(mic.state).toBe('muted');
        mic.close();
    });

    it('numbers on across a cut: the host would drop a sender restarting at 0', async () => {
        const { Fake, made } = fakeSender();
        const mic = new MicUplink({ getUserMedia: gum(fakeTrack()), SocketSender: Fake });
        await mic.attachSocket(() => true);
        await mic.setMuted(false);
        await mic.setMuted(true);
        await mic.setMuted(false);
        expect(made.map((s) => s.firstSequence)).toEqual([0, 100]);
        mic.close();
    });

    it('two quick clicks start one sender', async () => {
        const { Fake, made } = fakeSender();
        const mic = new MicUplink({ getUserMedia: gum(fakeTrack()), SocketSender: Fake });
        await mic.attachSocket(() => true);
        const first = mic.setMuted(false);
        const second = mic.setMuted(false);
        await Promise.all([first, second]);
        expect(made).toHaveLength(1);
        mic.close();
    });

    it('the real sender goes on from the number it was given, and wraps', () => {
        const sent = [];
        const sender = new MicSocketSender(
            /** @type {any} */ ({}),
            (bytes) => {
                sent.push(bytes);
                return true;
            },
            { bitrate: 64000 },
            0xffff,
        );
        sender._emit(1, new Uint8Array([7]));
        sender._emit(1, new Uint8Array([8]));
        expect(Array.from(sent[0].slice(2, 4))).toEqual([0xff, 0xff]);
        expect(Array.from(sent[1].slice(2, 4))).toEqual([0, 0]);
        expect(sender.sequence).toBe(1);
    });

    it('a standby leg starts on the socket only when promoted', async () => {
        const { Fake, made } = fakeSender();
        // Inherits an open microphone from the live leg.
        await new MicUplink({ getUserMedia: gum(fakeTrack()) }).setMuted(false);
        const mic = new MicUplink({
            held: true,
            getUserMedia: gum(fakeTrack()),
            SocketSender: Fake,
        });
        expect(await mic.attachSocket(() => true)).toBe(false);
        expect(made).toHaveLength(0);
        expect(await mic.release()).toBe(true);
        expect(made).toHaveLength(1);
    });

    it('frames what it sends the way the host reads it', async () => {
        const { micFrame, pcm16, MIC_CODEC_OPUS } = await import('../js/audio/MicSocketSender.js');
        const f = micFrame(MIC_CODEC_OPUS, 0x1234, new Uint8Array([9, 8]));
        expect(Array.from(f)).toEqual([0x4d, 1, 0x12, 0x34, 9, 8]);
        // Little-endian, clipped at full scale.
        expect(Array.from(pcm16(new Float32Array([0, 1, -1, 2])))).toEqual([
            0, 0, 0xff, 0x7f, 0x00, 0x80, 0xff, 0x7f,
        ]);
    });
});
