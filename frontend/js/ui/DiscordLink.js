/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * MoonlightWeb — floating link to the community server.
 *
 * A button pinned to the bottom right of the application: support, questions,
 * and the place release news is posted. It is present on every screen the shell
 * draws — host list, apps, settings, admin, the invited player's page — and on
 * none of the streaming ones: `body.streaming-active` hides it in css/stream.css
 * along with the rest of the shell, because a stream owns the whole screen and a
 * blurple pill floating over a game is a misclick waiting to happen.
 *
 * Nothing here talks to Discord. It is one anchor to an invite URL, opened in a
 * new tab, so a machine with no Internet access simply has a link that does not
 * resolve — the application itself never depends on it.
 */
import { t } from '../i18n/i18n.js';

/* The invite, in ONE place for the whole application. Empty string means the
   server does not exist yet and NO button is drawn: a dead link to a server
   nobody can join is worse than no link at all.

   Fill in the permanent invite (Discord: never expires, unlimited uses). The
   marketing site carries the same constant in website/assets/chrome.js — both
   are edited together. */
const DISCORD_INVITE = 'https://discord.gg/wfbesPx4UB';

/* The official Discord mark, inlined: an <img> would be one more request that
   can fail, and currentColor lets the glyph follow the button's text colour.
   It is the button's only content — the round blurple circle is the label, the
   way every live-chat launcher on the web is drawn, and the words live in the
   title / aria-label instead. */
const DISCORD_GLYPH =
    '<svg viewBox="0 0 127.14 96.36" width="26" height="26" fill="currentColor" aria-hidden="true">' +
    '<path d="M107.7 8.07A105.15 105.15 0 0 0 81.47 0a72.06 72.06 0 0 0-3.36 6.83 97.68 97.68 0 0 0-29.11 0A72.37 72.37 0 0 0 45.64 0a105.89 105.89 0 0 0-26.25 8.09C2.79 32.65-1.71 56.6.54 80.21a105.73 105.73 0 0 0 32.17 16.15 77.7 77.7 0 0 0 6.89-11.11 68.42 68.42 0 0 1-10.85-5.18c.91-.66 1.8-1.34 2.66-2a75.57 75.57 0 0 0 64.32 0c.87.71 1.76 1.39 2.66 2a68.68 68.68 0 0 1-10.87 5.19 77 77 0 0 0 6.89 11.1 105.25 105.25 0 0 0 32.19-16.14c2.64-27.38-4.51-51.11-18.9-72.15ZM42.45 65.69C36.18 65.69 31 60 31 53s5-12.74 11.43-12.74S54 46 53.89 53s-5.05 12.69-11.44 12.69Zm42.24 0C78.41 65.69 73.25 60 73.25 53s5-12.74 11.44-12.74S96.23 46 96.12 53s-5.04 12.69-11.43 12.69Z"/>' +
    '</svg>';

export class DiscordLink {
    /**
     * Draw the button, once, inside the footer.
     *
     * The footer is where it hangs FROM, not where it sits: the shell is a flex
     * column whose middle pane scrolls, so the footer is pinned to the bottom of
     * the window, and a child positioned at `bottom: 100%` therefore floats just
     * above it whatever height it happens to have — no measuring, and nothing to
     * re-tune the day the footer gains a line. A viewport-anchored button landed
     * on top of that bar instead.
     *
     * Falls back to <body> if the footer is missing, and the CSS keeps it
     * viewport-anchored in that case.
     *
     * Silent when no invite is configured, and idempotent: a second call finds
     * the element already there and does nothing.
     */
    static mount() {
        if (!DISCORD_INVITE) return;
        if (document.getElementById('discord-fab')) return;

        const a = document.createElement('a');
        a.id = 'discord-fab';
        a.className = 'discord-fab';
        a.href = DISCORD_INVITE;
        a.target = '_blank';
        a.rel = 'noopener';
        a.title = t('header.community');
        a.setAttribute('aria-label', t('header.community'));
        a.innerHTML = DISCORD_GLYPH;
        (document.querySelector('.app-footer') || document.body).appendChild(a);
    }
}
