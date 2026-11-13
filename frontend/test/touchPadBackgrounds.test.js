/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The pictures the controller's editor shows behind the controller: a game at
 * night and a game by day. They ship with the application, and the tunnel's
 * bootstrap pulls every file it serves before the page starts, so each must be
 * there and stay small.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const FRONTEND = join(dirname(fileURLToPath(import.meta.url)), '..');
const CSS = readFileSync(join(FRONTEND, 'css', 'stream.css'), 'utf8');

/** The picture a background class names, as a path under css/. */
function pictureOf(cls) {
    const rule = CSS.match(new RegExp(`\\.${cls}\\s*\\{([^}]*)\\}`));
    expect(rule, cls).not.toBeNull();
    const url = rule[1].match(/url\('([^']+)'\)/);
    expect(url, cls).not.toBeNull();
    return join(FRONTEND, 'css', url[1]);
}

describe("the controller editor's backgrounds", () => {
    for (const cls of ['tp-bg-night', 'tp-bg-day']) {
        it(`${cls} names a WebP that ships, at 300 KB at most`, () => {
            const file = pictureOf(cls);
            expect(file.endsWith('.webp')).toBe(true);
            const bytes = readFileSync(file);
            expect(bytes.subarray(0, 4).toString('latin1')).toBe('RIFF');
            expect(bytes.subarray(8, 12).toString('latin1')).toBe('WEBP');
            expect(statSync(file).size).toBeLessThanOrEqual(300 * 1024);
        });
    }
});
