/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What the browser does with the stream's sound, once a second (plan « le son
 * et la priorité des paquets », A0). What a bench relies on: the stream's own
 * sound is read and never an audio-road track, the cumulative counters become
 * the last interval's values, a counter that went backwards anchors again
 * instead of giving a negative row, and the CSV holds one line per second.
 */
import { describe, it, expect } from 'vitest';
import {
    AUDIO_STATS_COLUMNS,
    AudioStatsSampler,
    findAudioPlayout,
    findStreamAudioInbound,
} from '../js/stream/AudioStats.js';

const report = (...stats) => new Map(stats.map((s, i) => [String(i), s]));
const inbound = (extra) => ({ type: 'inbound-rtp', kind: 'audio', ...extra });

describe('findStreamAudioInbound', () => {
    it('takes the stream sound by its mid, never an audio-road track', () => {
        const road = inbound({ mid: 'vaudio' });
        const sound = inbound({ mid: 'audio' });
        const video = { type: 'inbound-rtp', kind: 'video', mid: 'video' };
        expect(findStreamAudioInbound(report(road, video, sound))).toBe(sound);
    });

    it('without mids, tells them apart by the transceivers track ids', () => {
        const road = inbound({ trackIdentifier: 't-road' });
        const sound = inbound({ trackIdentifier: 't-sound' });
        const pc = {
            getTransceivers: () => [
                { mid: 'uaudio', receiver: { track: { id: 't-road' } } },
                { mid: 'audio', receiver: { track: { id: 't-sound' } } },
            ],
        };
        expect(findStreamAudioInbound(report(road, sound), pc)).toBe(sound);
    });

    it('is null without any audio, and the only one when there is one', () => {
        expect(findStreamAudioInbound(report({ type: 'outbound-rtp' }))).toBe(null);
        const only = inbound({});
        expect(findStreamAudioInbound(report(only))).toBe(only);
    });
});

describe('AudioStatsSampler', () => {
    const cumul = (k) => ({
        jitterBufferDelay: 0.06 * 48000 * k, // 60 ms a sample
        jitterBufferTargetDelay: 0.06 * 48000 * k,
        jitterBufferMinimumDelay: 0.06 * 48000 * k,
        jitterBufferEmittedCount: 48000 * k,
        jitter: 0.0021,
        packetsReceived: 200 * k,
        packetsLost: k > 1 ? 1 : 0,
        totalSamplesReceived: 48000 * k,
        concealedSamples: 480 * k,
        silentConcealedSamples: 0,
        concealmentEvents: k,
        insertedSamplesForDeceleration: 0,
        removedSamplesForAcceleration: 96 * k,
        packetsDiscarded: 3 * k,
        totalAudioEnergy: 0.0025 * k,
    });

    it('turns the cumulative counters into one second of values', () => {
        const s = new AudioStatsSampler();
        expect(s.sample(cumul(1), 1000)).toBe(null); // the first one anchors
        const row = s.sample(cumul(2), 2000);
        expect(row.t).toBe(1000);
        expect(row.bufferMs).toBeCloseTo(60, 6);
        expect(row.targetMs).toBeCloseTo(60, 6);
        expect(row.minimumMs).toBeCloseTo(60, 6);
        expect(row.jitterMs).toBeCloseTo(2.1, 6);
        expect(row.packets).toBe(200);
        expect(row.lost).toBe(1);
        expect(row.concealed).toBe(480);
        expect(row.concealmentEvents).toBe(1);
        expect(row.removed).toBe(96);
        expect(row.discarded).toBe(3);
        expect(row.energy).toBeCloseTo(0.0025, 9);
        expect(row.flushes).toBe(-1); // Chrome only, absent here
        expect(AudioStatsSampler.concealedPercent(row)).toBeCloseTo(1, 6);
        expect(s.last).toBe(row);
    });

    it('anchors again when a counter goes backwards (a new track)', () => {
        const s = new AudioStatsSampler();
        s.sample(cumul(5), 0);
        expect(s.sample(cumul(1), 1000)).toBe(null);
        expect(s.sample(cumul(2), 2000).packets).toBe(200);
    });

    it('marks what the browser does not report with -1', () => {
        const s = new AudioStatsSampler();
        s.sample({ jitterBufferDelay: 0, jitterBufferEmittedCount: 0 }, 0);
        const row = s.sample({ jitterBufferDelay: 48, jitterBufferEmittedCount: 960 }, 1000);
        expect(row.bufferMs).toBeCloseTo(50, 6);
        expect(row.targetMs).toBe(-1);
        expect(row.minimumMs).toBe(-1);
        expect(row.jitterMs).toBe(-1);
        expect(row.discarded).toBe(-1);
        expect(row.flushes).toBe(-1);
        expect(row.energy).toBe(-1);
    });

    it('counts the flushes where the browser reports them', () => {
        const s = new AudioStatsSampler();
        s.sample({ jitterBufferEmittedCount: 0, jitterBufferFlushes: 2 }, 0);
        expect(
            s.sample({ jitterBufferEmittedCount: 960, jitterBufferFlushes: 3 }, 1000).flushes,
        ).toBe(1);
    });

    it('keeps the newest rows and writes them as CSV', () => {
        const s = new AudioStatsSampler({ maxRows: 2 });
        for (let k = 1; k <= 4; k++) s.sample(cumul(k), k * 1000);
        expect(s.rows.length).toBe(2);
        const lines = s.csv().trim().split('\n');
        expect(lines[0]).toBe(AUDIO_STATS_COLUMNS.join(','));
        expect(lines.length).toBe(3);
        expect(lines[2]).toBe(
            '3000,60.00,60.00,60.00,2.10,200,0,48000,480,0,1,0,96,3,-1,0.00250000,-1,-1,-1,-1',
        );
    });

    const playout = (k, extra) => ({
        type: 'media-playout',
        totalSamplesDuration: 1.0 * k,
        totalSamplesCount: 48000 * k,
        synthesizedSamplesDuration: 0.01 * k,
        synthesizedSamplesEvents: 2 * k,
        totalPlayoutDelay: 0.02 * 48000 * k, // 20 ms a sample
        ...extra,
    });

    it('reads what the audio device took from the browser', () => {
        const s = new AudioStatsSampler();
        s.sample(cumul(1), 1000, playout(1));
        const row = s.sample(cumul(2), 2000, playout(2));
        expect(row.playedMs).toBeCloseTo(1000, 6);
        expect(row.synthMs).toBeCloseTo(10, 6);
        expect(row.synthEvents).toBe(2);
        expect(row.outputMs).toBeCloseTo(20, 6);
    });

    it('shows an output that stopped asking as less than a second played', () => {
        const s = new AudioStatsSampler();
        s.sample(cumul(1), 1000, playout(1));
        const row = s.sample(
            cumul(2),
            2000,
            playout(1, { totalSamplesDuration: 1.25, totalSamplesCount: 60000 }),
        );
        expect(row.playedMs).toBeCloseTo(250, 6);
    });

    it('marks the playout -1 without it, or when its count went back', () => {
        const s = new AudioStatsSampler();
        s.sample(cumul(1), 1000);
        expect(s.sample(cumul(2), 2000, playout(2)).playedMs).toBe(-1); // nothing before
        expect(s.sample(cumul(3), 3000, playout(1)).playedMs).toBe(-1); // went back
    });
});

describe('findAudioPlayout', () => {
    it('takes the playout the sound plays through, else the only one', () => {
        const a = { type: 'media-playout', id: 'AP1' };
        const b = { type: 'media-playout', id: 'AP2' };
        expect(findAudioPlayout(report(a, b), inbound({ playoutId: 'AP2' }))).toBe(b);
        expect(findAudioPlayout(report(a), inbound({}))).toBe(a);
        expect(findAudioPlayout(report(a, b), inbound({}))).toBe(null);
        expect(findAudioPlayout(report(inbound({})), inbound({}))).toBe(null);
    });
});
