#!/usr/bin/env node
/*
 * i18n catalog sanity check (run from frontend/):  node scripts/check-i18n.cjs
 *
 *  1. Every locale JSON is valid and has the same set of keys as en.json.
 *  2. Every t('key') referenced in the JS sources exists in en.json.
 *
 * Exit code 1 on any problem — usable as a pre-commit / CI gate.
 */
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const LOCALES = path.join(ROOT, 'locales');
const JS = path.join(ROOT, 'js');

const flat = (o, p = '') =>
    Object.entries(o).flatMap(([k, v]) =>
        v && typeof v === 'object' ? flat(v, p + k + '.') : [p + k],
    );

let failed = false;
const fail = (msg) => {
    console.error('✗ ' + msg);
    failed = true;
};

// --- Load locales ---
const localeFiles = fs.readdirSync(LOCALES).filter((f) => f.endsWith('.json'));
const catalogs = {};
for (const f of localeFiles) {
    try {
        catalogs[f] = JSON.parse(fs.readFileSync(path.join(LOCALES, f), 'utf8'));
    } catch (e) {
        fail(`${f} is not valid JSON: ${e.message}`);
    }
}

if (!catalogs['en.json']) {
    fail('en.json missing — it is the reference catalog');
    process.exit(1);
}

const enKeys = new Set(flat(catalogs['en.json']));

// --- Key parity across locales ---
for (const [f, cat] of Object.entries(catalogs)) {
    if (f === 'en.json') continue;
    const keys = new Set(flat(cat));
    const missing = [...enKeys].filter((k) => !keys.has(k));
    const extra = [...keys].filter((k) => !enKeys.has(k));
    if (missing.length) fail(`${f} missing ${missing.length} key(s): ${missing.join(', ')}`);
    if (extra.length) fail(`${f} has ${extra.length} unknown key(s): ${extra.join(', ')}`);
}

// --- t() usage vs catalog ---
const walk = (d) =>
    fs.readdirSync(d, { withFileTypes: true }).flatMap((e) => {
        const p = path.join(d, e.name);
        return e.isDirectory() ? walk(p) : p.endsWith('.js') ? [p] : [];
    });
// A t() argument is either a whole key — t('stream.statLatency') — or a prefix
// built at runtime — t('stream.' + leg.key). Group 3 catches the trailing '+'
// that tells them apart: without it, a prefix is read as a literal key and
// reported missing, which is a false failure that makes the whole gate useless.
const re = /\bt\(\s*(['"`])([a-zA-Z0-9_.]+)\1(\s*\+)?/g;
const enKeyList = [...enKeys];
for (const file of walk(JS)) {
    const s = fs.readFileSync(file, 'utf8');
    let m;
    while ((m = re.exec(s))) {
        const key = m[2];
        if (m[3]) {
            // The full key is only known at runtime, so verify what can be:
            // that the prefix still names a section of the catalog. That catches
            // a renamed or misspelled prefix, which is the realistic mistake.
            if (!enKeyList.some((k) => k.startsWith(key)))
                fail(`${path.relative(ROOT, file)}: t('${key}' + …) matches no key in en.json`);
        } else if (!enKeys.has(key)) {
            fail(`${path.relative(ROOT, file)}: t('${key}') has no entry in en.json`);
        }
    }
}

if (failed) process.exit(1);
console.log(
    `✓ i18n OK — ${enKeys.size} keys, ${localeFiles.length} locale(s): ${localeFiles.join(', ')}`,
);
