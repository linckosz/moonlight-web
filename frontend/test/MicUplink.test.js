/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 MoonlightWeb contributors.
 * GPLv3 — see repository LICENSE.
 *
 * The viewer's microphone. What matters: nothing is opened or sent unless the
 * host's offer asked for it; when it did, only the stream's audio section is
 * answered sendrecv and gets the track; a standby leg sends nothing until it is
 * promoted, and keeps the mute it inherits; a refused permission is reported,
 * never thrown.
 */
import { describe, it, expect, vi } from 'vitest';
import { MicUplink, micSupported, offerTakesMic } from '../js/audio/MicUplink.js';
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

const fakeTrack = () => ({ enabled: true, readyState: 'live', label: 'Mic', stop: vi.fn() });

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
        expect(await mic.attach()).toBe(true);
        expect(audio.sender.track).toBe(track);
        expect(video.sender.replaceTrack).not.toHaveBeenCalled();
        expect(getUserMedia.mock.calls[0][0].audio.echoCancellation).toBe(true);
        expect(states).toEqual(['live']);
        // Muting sends silence: the track stays on the sender, disabled.
        mic.toggleMuted();
        expect(track.enabled).toBe(false);
        expect(audio.sender.track).toBe(track);
        expect(mic.state).toBe('muted');
        mic.close();
        expect(track.stop).toHaveBeenCalled();
    });

    it('a standby leg sends nothing until released, with the mute it inherits', async () => {
        const live = new MicUplink({ getUserMedia: gum(fakeTrack()) });
        live.setMuted(true);
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const track = fakeTrack();
        const standby = new MicUplink({ held: true, getUserMedia: gum(track) });
        expect(standby.prepare(pcWith([audio]), OFFER_SENDRECV)).toBe(true);
        expect(await standby.attach()).toBe(false);
        expect(audio.sender.replaceTrack).not.toHaveBeenCalled();
        expect(await standby.release()).toBe(true);
        expect(audio.sender.track).toBe(track);
        expect(track.enabled).toBe(false);
        expect(standby.muted).toBe(true);
        // A new stream starts live again.
        expect(new MicUplink({ getUserMedia: gum(fakeTrack()) }).muted).toBe(false);
    });

    it('reports a refused permission instead of throwing', async () => {
        const audio = { mid: 'audio', direction: 'recvonly', sender: sender() };
        const denied = Object.assign(new Error('no'), { name: 'NotAllowedError' });
        const mic = new MicUplink({ getUserMedia: vi.fn(async () => Promise.reject(denied)) });
        mic.prepare(pcWith([audio]), OFFER_SENDRECV);
        expect(await mic.attach()).toBe(false);
        expect(mic.state).toBe('denied');
        expect(audio.sender.replaceTrack).not.toHaveBeenCalled();
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
        const attached = mic.attach();
        mic.close();
        resolve({ getAudioTracks: () => [track], getTracks: () => [track] });
        expect(await attached).toBe(false);
        expect(track.stop).toHaveBeenCalled();
        expect(audio.sender.replaceTrack).not.toHaveBeenCalled();
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
        expect(await mic.attach()).toBe(true);
        expect(params.encodings[0].maxBitrate).toBe(128000);
    });
});

describe('the WebSocket transport', () => {
    const fakeSender = () => {
        const made = [];
        class Fake {
            constructor(track, send, quality) {
                Object.assign(this, { track, send, quality, started: false, stopped: false });
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
        expect(await mic.attachSocket(send)).toBe(true);
        expect(made).toHaveLength(1);
        expect(made[0].started).toBe(true);
        expect(made[0].send).toBe(send);
        expect(made[0].quality.bitrate).toBe(128000);
        expect(mic.state).toBe('live');
        mic.toggleMuted();
        expect(track.enabled).toBe(false);
        mic.close();
        expect(made[0].stopped).toBe(true);
        expect(track.stop).toHaveBeenCalled();
    });

    it('a standby leg starts on the socket only when promoted', async () => {
        const { Fake, made } = fakeSender();
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
