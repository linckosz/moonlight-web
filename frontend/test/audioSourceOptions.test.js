import { describe, expect, it } from 'vitest';

import { audioSourceOptions, normalizeAudioSource } from '../js/ui/audioSourceOptions.js';

describe('normalizeAudioSource', () => {
    it('keeps the known values', () => {
        expect(normalizeAudioSource('all')).toBe('all');
        expect(normalizeAudioSource('focused')).toBe('focused');
        expect(normalizeAudioSource('app:Discord.EXE')).toBe('app:discord.exe');
    });

    it('reads anything else as all apps', () => {
        for (const value of [
            undefined,
            null,
            1,
            '',
            'Focused',
            'app:',
            'app:discord',
            'app:a\\b.exe',
            'app:a/b.exe',
        ]) {
            expect(normalizeAudioSource(value)).toBe('all');
        }
    });
});

describe('audioSourceOptions', () => {
    const base = { apps: [], followed: '', allLabel: 'All apps', focusedLabel: 'Automatic' };

    it('puts All apps first, Automatic second, then the apps', () => {
        const options = audioSourceOptions({
            ...base,
            selected: 'all',
            apps: [{ exe: 'discord.exe', name: 'Discord' }],
        });
        expect(options).toEqual([
            { value: 'all', label: 'All apps' },
            { value: 'focused', label: 'Automatic' },
            { value: 'app:discord.exe', label: 'Discord' },
        ]);
    });

    it('names the app Automatic follows', () => {
        const options = audioSourceOptions({
            ...base,
            selected: 'focused',
            followed: 'huntgame.exe',
        });
        expect(options[1]).toEqual({ value: 'focused', label: 'Automatic — huntgame.exe' });
    });

    it('keeps the picked app listed while it plays nothing', () => {
        const options = audioSourceOptions({ ...base, selected: 'app:spotify.exe' });
        expect(options[2]).toEqual({ value: 'app:spotify.exe', label: 'spotify.exe' });
    });

    it('does not list the picked app twice, and falls back on its file name', () => {
        const options = audioSourceOptions({
            ...base,
            selected: 'app:vlc.exe',
            apps: [{ exe: 'VLC.exe' }, null, { name: 'no exe' }],
        });
        expect(options.slice(2)).toEqual([{ value: 'app:vlc.exe', label: 'VLC.exe' }]);
    });
});
