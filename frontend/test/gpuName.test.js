/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A GPU's name as a host card has room for it. Only the words that tell no two
 * GPUs apart may go: a laptop part, a memory size or a Ti is another GPU.
 */
import { describe, it, expect } from 'vitest';
import { shortGpuName, shortGpuNames } from '../js/util/gpuName.js';

describe('shortGpuName', () => {
    it.each([
        ['NVIDIA GeForce RTX 5060 Ti', 'RTX 5060 Ti'],
        ['NVIDIA GeForce GTX 1650 SUPER', 'GTX 1650 SUPER'],
        ['NVIDIA GeForce GT 1030', 'GT 1030'],
        ['NVIDIA GeForce MX450', 'MX450'],
        ['NVIDIA RTX A2000', 'RTX A2000'],
        ['AMD Radeon RX 9070 XT', 'RX 9070 XT'],
        ['Intel(R) Arc(TM) A380 Graphics', 'Arc A380'],
        ['Intel(R) UHD Graphics 630', 'UHD Graphics 630'],
        ['Intel(R) Iris(R) Xe Graphics', 'Iris Xe Graphics'],
        ['Apple M3 Pro', 'M3 Pro'],
    ])('%s reads %s', (full, short) => {
        expect(shortGpuName(full)).toBe(short);
    });

    it('keeps the words that name another GPU', () => {
        expect(shortGpuName('NVIDIA GeForce RTX 4060 Laptop GPU')).toBe('RTX 4060 Laptop');
        expect(shortGpuName('NVIDIA GeForce GTX 1060 6GB')).toBe('GTX 1060 6GB');
        expect(shortGpuName('NVIDIA GeForce RTX 5060 Ti 16GB')).toBe('RTX 5060 Ti 16GB');
    });

    it('keeps the brand where no family says what it is', () => {
        expect(shortGpuName('AMD Radeon(TM) Graphics')).toBe('Radeon Graphics');
        expect(shortGpuName('AMD Radeon 780M Graphics')).toBe('Radeon 780M');
        expect(shortGpuName('NVIDIA GeForce 940M')).toBe('GeForce 940M');
        expect(shortGpuName('NVIDIA Quadro RTX 4000')).toBe('Quadro RTX 4000');
    });

    it('leaves alone what it does not know, and never leaves nothing', () => {
        expect(shortGpuName('Microsoft Basic Render Driver')).toBe('Microsoft Basic Render Driver');
        expect(shortGpuName('Apple GPU')).toBe('Apple GPU');
        expect(shortGpuName('GPU')).toBe('GPU');
        expect(shortGpuName('')).toBe('');
        expect(shortGpuName(undefined)).toBe('');
    });
});

describe('shortGpuNames', () => {
    it('shortens the names of a list', () => {
        const shown = shortGpuNames(['NVIDIA GeForce RTX 5060 Ti', 'AMD Radeon(TM) Graphics']);
        expect(shown.get('NVIDIA GeForce RTX 5060 Ti')).toBe('RTX 5060 Ti');
        expect(shown.get('AMD Radeon(TM) Graphics')).toBe('Radeon Graphics');
    });

    it('keeps full the two different names that would read the same', () => {
        const a = 'NVIDIA GeForce RTX 3060';
        const b = 'NVIDIA RTX 3060';
        const c = 'AMD Radeon RX 7600';
        const shown = shortGpuNames([a, b, c]);
        expect(shown.get(a)).toBe(a);
        expect(shown.get(b)).toBe(b);
        expect(shown.get(c)).toBe('RX 7600');
    });

    it('does not count one name given twice as a collision', () => {
        const a = 'NVIDIA GeForce RTX 5060 Ti';
        expect(shortGpuNames([a, a]).get(a)).toBe('RTX 5060 Ti');
    });
});
