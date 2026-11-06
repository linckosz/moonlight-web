/**
 * The mwsr/ folder, MWSR for other programs: its shaders are generated from
 * the product's own module (mwsr/generate.mjs), so a change to MwsrShader.js
 * that is not followed by `node mwsr/generate.mjs` fails here; and every file
 * other developers take carries its author and the MIT licence.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { mwsrDist } from '../../mwsr/generate.mjs';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const MWSR = join(REPO, 'mwsr');
// a Windows checkout may have turned the line ends into CRLF
const read = (path) => readFileSync(path, 'utf8').replace(/\r\n/g, '\n');

describe('the mwsr/ folder', () => {
    const dist = mwsrDist();

    it('holds every file the generator writes, as it writes it today', () => {
        for (const [path, text] of Object.entries(dist))
            expect(read(join(MWSR, path)), 'mwsr/' + path + ': run node mwsr/generate.mjs').toBe(
                text.replace(/\r\n/g, '\n'),
            );
    });

    it('names its author and the MIT licence in every file', () => {
        for (const path of Object.keys(dist)) {
            const text = read(join(MWSR, path));
            expect(text, path).toContain('Copyright (c) 2026 Bruno Martin');
            expect(text, path).toContain('SPDX-License-Identifier: MIT');
            expect(text, path).toContain('Permission is hereby granted, free of charge');
        }
        expect(read(join(MWSR, 'LICENSE'))).toMatch(/^MIT License\n\nCopyright \(c\) 2026 Bruno Martin/);
        // the example is written by hand, under the same notice
        const example = read(join(MWSR, 'cpp', 'example_d3d11.cpp'));
        expect(example).toContain('Copyright (c) 2026 Bruno Martin');
        expect(example).toContain('SPDX-License-Identifier: MIT');
        expect(example).toContain('Permission is hereby granted, free of charge');
    });

    it('keeps #version first in each GLSL text, as GLSL ES requires', () => {
        for (const path of Object.keys(dist).filter((p) => p.startsWith('glsl/')))
            expect(read(join(MWSR, path)).startsWith('#version 300 es\n'), path).toBe(true);
    });

    it('carries one text per regime the renderers build, in each language, the C++ header, and the libplacebo and Metal upscales', () => {
        expect(Object.keys(dist).sort()).toEqual(
            [
                'cpp/mwsr.h',
                'libplacebo/mwsr-up.hook',
                'metal/mwsr-up.metal',
                'glsl/mwsr-down-16.frag',
                'glsl/mwsr-down-4.frag',
                'glsl/mwsr-down-9.frag',
                'glsl/mwsr-up.frag',
                'glsl/mwsr.vert',
                'hlsl/mwsr-down-1.hlsl',
                'hlsl/mwsr-down-16.hlsl',
                'hlsl/mwsr-down-4.hlsl',
                'hlsl/mwsr-down-9.hlsl',
                'hlsl/mwsr-up-half.hlsl',
                'hlsl/mwsr-up.hlsl',
                'js/MwsrShader.js',
                'wgsl/mwsr-down-1.wgsl',
                'wgsl/mwsr-down-16.wgsl',
                'wgsl/mwsr-down-4.wgsl',
                'wgsl/mwsr-down-9.wgsl',
                'wgsl/mwsr-up-f16.wgsl',
                'wgsl/mwsr-up.wgsl',
            ].sort(),
        );
    });
});
