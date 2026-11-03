/*
 * Settings → "Stream audio from": the setting's values and the select's options.
 * Pure, so the rules are tested without a page.
 *
 * Values: 'all' (default), 'focused' (Automatic: the app in focus), or
 * 'app:<exe>' for one app picked by its executable.
 */

const APP_PREFIX = 'app:';

/** The stored or received value in its one form; anything else is 'all'. */
export function normalizeAudioSource(value) {
    if (value === 'focused') return 'focused';
    if (typeof value !== 'string' || !value.startsWith(APP_PREFIX)) return 'all';
    const exe = value.slice(APP_PREFIX.length).trim().toLowerCase();
    if (!exe.endsWith('.exe') || /[\\/]/.test(exe)) return 'all';
    return APP_PREFIX + exe;
}

/**
 * The select's options, in order: All apps, Automatic, then the apps with sound.
 * The app already picked stays listed while it plays nothing, so a choice never
 * silently moves to another option.
 *
 * @param {object} o
 * @param {string} o.selected the normalized value
 * @param {{exe: string, name: string}[]} o.apps from GET /api/audio/apps
 * @param {string} o.followed the app a running stream follows in Automatic mode
 * @param {string} o.allLabel
 * @param {string} o.focusedLabel
 * @returns {{value: string, label: string}[]}
 */
export function audioSourceOptions({ selected, apps, followed, allLabel, focusedLabel }) {
    const options = [
        { value: 'all', label: allLabel },
        { value: 'focused', label: followed ? `${focusedLabel} — ${followed}` : focusedLabel },
    ];
    for (const app of apps) {
        if (!app || typeof app.exe !== 'string' || !app.exe) continue;
        options.push({ value: APP_PREFIX + app.exe.toLowerCase(), label: app.name || app.exe });
    }
    if (!options.some((option) => option.value === selected)) {
        options.push({ value: selected, label: selected.slice(APP_PREFIX.length) });
    }
    return options;
}
