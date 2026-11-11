/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * AV1 and a decoder that does not keep up (11/11/2026).
 *
 * H.264 and HEVC stop feeding a decoder whose queue stays full: the deltas are
 * dropped until a keyframe comes, and a decoder that stays stuck is rebuilt.
 * AV1 had none of it. On a N95 decoding 1440p60 in software, 60 pictures a
 * second went in and 39 came out; every one of the others waited: a thousand
 * after 50 s, 27 s behind, with no picture dropped and nothing said.
 *
 * Pinned down: the rule itself (stream/DecodeBrake.js), and the AV1 path of
 * the main thread taking it — deltas waiting for a keyframe while the
 * reference is lost, the queue held short under a decoder that slow, the
 * stale pictures flushed in front of a keyframe, the decoder's config kept for
 * that flush.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { queueBrake } from '../js/stream/DecodeBrake.js';

const P = StreamView.prototype;
// One frame OBU with its size field: nothing to strip.
const PICTURE = new Uint8Array([0x32, 0x01, 0x00]);

describe('the brake on a decoder queue that stays full', () => {
    const at = (over) => queueBrake({ queueMax: 8, stallMs: 200, resetMs: 1000, ...over });
    const delta = { isKeyframe: false };

    it('lets pictures through a queue that is not full, and forgets a saturation that ended', () => {
        expect(at({ ...delta, queued: 7, stallStart: 0, now: 5000 })).toEqual({
            verdict: 'decode',
            stallStart: 0,
        });
        expect(at({ ...delta, queued: 2, stallStart: 4900, now: 5000 })).toEqual({
            verdict: 'decode',
            stallStart: 0,
        });
    });

    it('decodes through a burst, and keeps when it began', () => {
        expect(at({ ...delta, queued: 8, stallStart: 0, now: 5000 })).toEqual({
            verdict: 'decode',
            stallStart: 5000,
        });
        expect(at({ ...delta, queued: 12, stallStart: 5000, now: 5200 })).toEqual({
            verdict: 'decode',
            stallStart: 5000,
        });
    });

    it('drops the picture once the queue stayed full', () => {
        expect(at({ ...delta, queued: 12, stallStart: 5000, now: 5201 })).toEqual({
            verdict: 'drop',
            stallStart: 5000,
        });
    });

    it('has the decoder rebuilt when it stayed full a second, and starts over', () => {
        expect(at({ ...delta, queued: 12, stallStart: 5000, now: 6001 })).toEqual({
            verdict: 'reset',
            stallStart: 0,
        });
    });

    it('always lets a keyframe in, and starts over at it', () => {
        expect(at({ isKeyframe: true, queued: 40, stallStart: 5000, now: 9000 })).toEqual({
            verdict: 'decode',
            stallStart: 0,
        });
    });
});

describe('the AV1 path takes the brakes of the other codecs', () => {
    let now = 0;
    beforeEach(() => {
        now = 0;
        vi.spyOn(performance, 'now').mockImplementation(() => now);
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.stubGlobal(
            'EncodedVideoChunk',
            class {
                constructor(init) {
                    Object.assign(this, init);
                }
            },
        );
    });
    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    /** A stream view decoding AV1, its decoder's queue at @p queued. */
    function av1View(queued = 0, over = {}) {
        return {
            decoderConfigured: true,
            pendingFrames: [],
            decoder: { state: 'configured', decodeQueueSize: queued, decode: vi.fn() },
            frameCount: 0,
            _referenceValid: true,
            _idrRequested: false,
            stats: { dropped: 0, received: 0 },
            _diag: { noteDecodeQueue: vi.fn(), noteDrop: vi.fn(), arrivalAvgMs: 16.7 },
            DECODE_QUEUE_MAX: 8,
            QUEUE_STALL_MS: 200,
            QUEUE_RESET_MS: 1000,
            _queueStallStart: 0,
            _lastQueueFlushMs: -Infinity,
            _renderer: null,
            _decoderTrail: null,
            _trackChunkSubmit: vi.fn(),
            _tellDecodeQueue: vi.fn(),
            _requestIdr: vi.fn(),
            _handleDecoderError: vi.fn(),
            _flushDecodeQueue: vi.fn(),
            ...over,
        };
    }
    const feed = (v, isKeyframe) => P.decodeAv1Frame.call(v, PICTURE, isKeyframe);

    it('drops the deltas while the reference is lost, asks for a keyframe, and takes it', () => {
        const v = av1View(0, { _referenceValid: false, _idrRequested: true });
        feed(v, false);
        feed(v, false);
        expect(v.decoder.decode).not.toHaveBeenCalled();
        expect(v.stats.dropped).toBe(2);
        expect(v._diag.noteDrop).toHaveBeenCalledWith('backpressure');
        expect(v._requestIdr).toHaveBeenCalledWith('reference invalid');

        feed(v, true);
        expect(v.decoder.decode).toHaveBeenCalledOnce();
        expect(v._referenceValid).toBe(true);
        expect(v._idrRequested).toBe(false);
        feed(v, false);
        expect(v.decoder.decode).toHaveBeenCalledTimes(2);
    });

    it('decodes through a burst, then drops and asks for a keyframe', () => {
        const v = av1View(9);
        now = 1000;
        feed(v, false);
        now = 1150;
        feed(v, false);
        expect(v.decoder.decode).toHaveBeenCalledTimes(2);
        expect(v.stats.dropped).toBe(0);

        now = 1250;
        feed(v, false);
        expect(v.decoder.decode).toHaveBeenCalledTimes(2);
        expect(v.stats.dropped).toBe(1);
        expect(v._referenceValid).toBe(false);
        expect(v._requestIdr).toHaveBeenCalledWith('decode queue overflow');

        // Nine waiting are not worth a flush: the keyframe goes in behind them.
        now = 1400;
        feed(v, true);
        expect(v._flushDecodeQueue).not.toHaveBeenCalled();
        expect(v.decoder.decode).toHaveBeenCalledTimes(3);
        expect(v._referenceValid).toBe(true);
        expect(v._queueStallStart).toBe(0);
    });

    it('has a decoder rebuilt whose queue is still full a second on', () => {
        const v = av1View(9);
        now = 1000;
        feed(v, false);
        now = 2100;
        feed(v, false);
        expect(v._handleDecoderError).toHaveBeenCalledOnce();
        expect(v._handleDecoderError.mock.calls[0][0].message).toBe('Decode queue stalled');
        expect(v.decoder.decode).toHaveBeenCalledOnce();
        expect(v._queueStallStart).toBe(0);
    });

    it('flushes the stale pictures in front of a keyframe when they are a quarter of a second', () => {
        const order = [];
        const v = av1View(30, { _flushDecodeQueue: vi.fn(() => order.push('flush')) });
        v.decoder.decode = vi.fn(() => order.push('decode'));
        feed(v, true);
        expect(order).toEqual(['flush', 'decode']);
    });

    it('keeps the config of the decoder, for that flush to apply again', async () => {
        vi.stubGlobal('VideoDecoder', {
            isConfigSupported: vi.fn(async () => ({ supported: true })),
        });
        const v = av1View(0, {
            decoderConfigured: false,
            decoderConfiguring: false,
            _hdrYuv: false,
            _activeDecoderCfg: null,
            decodeAv1Frame: vi.fn(),
        });
        v.decoder.configure = vi.fn();
        P.configureAv1Decoder.call(v, undefined);
        await vi.waitFor(() => expect(v.decoderConfigured).toBe(true));
        expect(v._activeDecoderCfg).toBe(v.decoder.configure.mock.calls[0][0]);
        expect(v._activeDecoderCfg.codec).toMatch(/^av01\./);
    });

    it('holds the queue of a decoder that gives 39 pictures of the 60 a second', () => {
        // The N95 on 1440p60 in software. The host answers a keyframe request
        // a tenth of a second later.
        let queued = 0;
        let credit = 0;
        let last = 0;
        let keyframeDue = null;
        let deepest = 0;
        const askKeyframe = () => {
            if (keyframeDue === null) keyframeDue = now + 100;
        };
        const v = av1View(0, {
            decoder: {
                state: 'configured',
                get decodeQueueSize() {
                    return queued;
                },
                decode: () => {
                    queued++;
                },
            },
            _requestIdr: askKeyframe,
            _flushDecodeQueue: () => {
                queued = 0;
            },
        });
        v._handleDecoderError = () => {
            queued = 0;
            v._referenceValid = false;
            askKeyframe();
        };
        for (let i = 0; i < 60 * 60; i++) {
            now = (i * 1000) / 60;
            credit += ((now - last) * 39) / 1000;
            last = now;
            const able = Math.floor(credit);
            credit -= able;
            queued -= Math.min(queued, able);
            const isKeyframe = keyframeDue !== null && now >= keyframeDue;
            if (isKeyframe) keyframeDue = null;
            feed(v, isKeyframe);
            deepest = Math.max(deepest, queued);
        }
        // A thousand waiting without the brake; a quarter of a second with it.
        expect(deepest).toBeLessThan(30);
        expect(v.stats.dropped).toBeGreaterThan(100);
        // And the decoder kept busy: most of what it can do went through.
        expect(v.stats.received).toBeGreaterThan(60 * 30);
    });

    it('leaves a decoder that keeps up alone: nothing dropped, nothing asked', () => {
        const v = av1View(1);
        for (let i = 0; i < 600; i++) {
            now = (i * 1000) / 60;
            feed(v, i === 0);
        }
        expect(v.stats.dropped).toBe(0);
        expect(v.decoder.decode).toHaveBeenCalledTimes(600);
        expect(v._requestIdr).not.toHaveBeenCalled();
        expect(v._flushDecodeQueue).not.toHaveBeenCalled();
    });
});
