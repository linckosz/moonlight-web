/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The Auto bitrate an invited player gets is worked out on the host, in C++
 * (backend/src/server/GuestProfile.cpp), from the owner's rule here
 * (util/AutoBitrate.js). Both are held to one table,
 * fixtures/guest-profile-cases.json (backend/tests/test_guest_profile.cpp
 * reads it too), so a change to the rule on one side fails until the other
 * follows (issue #67).
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { computeAutoBitrate } from '../js/util/AutoBitrate.js';

const cases = JSON.parse(
    readFileSync(
        join(dirname(fileURLToPath(import.meta.url)), 'fixtures', 'guest-profile-cases.json'),
        'utf8',
    ),
);

describe("the guest's Auto bitrate, on the shared table", () => {
    it('has cases for both networks', () => {
        expect(cases.bitrate.some((c) => c.lan)).toBe(true);
        expect(cases.bitrate.some((c) => !c.lan)).toBe(true);
    });

    it.each(cases.bitrate)('$height p at $aspect, LAN $lan: $kbps kbps', (c) => {
        expect(computeAutoBitrate(c.height, c.aspect, false, false, c.lan) * 1000).toBe(c.kbps);
    });
});
