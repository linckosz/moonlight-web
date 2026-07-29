/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

import { describe, it, expect } from 'vitest';
import { MAX_DRAWS_IN_FLIGHT, drawCapFor } from '../js/stream/RenderPacing.js';

describe('drawCapFor', () => {
    it('pipelines renderers that tolerate concurrent frames', () => {
        expect(drawCapFor({ serialDrawsOnly: false })).toBe(MAX_DRAWS_IN_FLIGHT);
    });

    it('forces one draw at a time when the renderer says so', () => {
        expect(drawCapFor({ serialDrawsOnly: true })).toBe(1);
    });

    it('never returns less than one, renderer or not', () => {
        expect(drawCapFor(null)).toBe(1);
        expect(drawCapFor(undefined)).toBe(1);
        expect(drawCapFor({})).toBeGreaterThanOrEqual(1);
    });

    it('ships pipelined by default', () => {
        expect(MAX_DRAWS_IN_FLIGHT).toBe(2);
    });
});
