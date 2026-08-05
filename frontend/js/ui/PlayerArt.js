/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * Illustrations for the invited player's dead ends. Inline SVG rather than
 * image files: they inherit currentColor, so they follow the theme, and they
 * cost no extra request on a page a guest opens once.
 */
export const PlayerArt = {
    /**
     * "The void of video games" — a small figure drifting past a broken
     * controller and a few dead pixels, for a link that leads nowhere.
     */
    void: `
<svg viewBox="0 0 240 150" role="img" aria-hidden="true" class="player-art-svg">
  <defs>
    <radialGradient id="mw-void-bg" cx="50%" cy="45%" r="60%">
      <stop offset="0%" stop-color="currentColor" stop-opacity="0.14"/>
      <stop offset="100%" stop-color="currentColor" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <rect x="0" y="0" width="240" height="150" fill="url(#mw-void-bg)"/>
  <!-- drifting stars / dead pixels -->
  <g fill="currentColor" opacity="0.55">
    <rect x="26" y="30" width="3" height="3"/>
    <rect x="205" y="44" width="3" height="3"/>
    <rect x="52" y="112" width="3" height="3"/>
    <rect x="180" y="118" width="3" height="3"/>
    <rect x="120" y="18" width="3" height="3"/>
    <rect x="88" y="58" width="2" height="2"/>
    <rect x="158" y="86" width="2" height="2"/>
  </g>
  <!-- tumbling controller -->
  <g transform="translate(168 96) rotate(18)" fill="none" stroke="currentColor"
     stroke-width="2.5" stroke-linejoin="round" opacity="0.75">
    <path d="M-20 -6 h40 a10 10 0 0 1 10 10 v2 a8 8 0 0 1 -14 5 l-6 -5 h-20 l-6 5
             a8 8 0 0 1 -14 -5 v-2 a10 10 0 0 1 10 -10 z"/>
    <path d="M-12 0 v6 M-15 3 h6"/>
    <circle cx="12" cy="1" r="2"/>
    <circle cx="18" cy="5" r="2"/>
  </g>
  <!-- floating player -->
  <g transform="translate(84 70)" fill="none" stroke="currentColor" stroke-width="3"
     stroke-linecap="round" stroke-linejoin="round">
    <circle cx="0" cy="-16" r="9"/>
    <path d="M0 -7 v20"/>
    <path d="M0 -2 l-14 -8 M0 -2 l15 -5"/>
    <path d="M0 13 l-10 14 M0 13 l11 12"/>
  </g>
</svg>`,

    /**
     * "Calling the owner" — a rotary phone and a speech bubble, for a session
     * the owner has ended.
     */
    phone: `
<svg viewBox="0 0 240 150" role="img" aria-hidden="true" class="player-art-svg">
  <!-- speech bubble -->
  <g fill="none" stroke="currentColor" stroke-width="2.5" stroke-linejoin="round" opacity="0.7">
    <path d="M138 22 h74 a8 8 0 0 1 8 8 v26 a8 8 0 0 1 -8 8 h-40 l-14 12 v-12 h-20
             a8 8 0 0 1 -8 -8 v-26 a8 8 0 0 1 8 -8 z"/>
  </g>
  <g fill="currentColor" opacity="0.7">
    <circle cx="160" cy="43" r="3"/>
    <circle cx="175" cy="43" r="3"/>
    <circle cx="190" cy="43" r="3"/>
  </g>
  <!-- rotary phone -->
  <g transform="translate(30 58)" fill="none" stroke="currentColor" stroke-width="3"
     stroke-linecap="round" stroke-linejoin="round">
    <!-- body -->
    <path d="M6 46 h72 a8 8 0 0 0 8 -8 v-8 a10 10 0 0 0 -10 -10 h-68
             a10 10 0 0 0 -10 10 v8 a8 8 0 0 0 8 8 z"/>
    <!-- dial -->
    <circle cx="42" cy="30" r="12"/>
    <circle cx="42" cy="30" r="3"/>
    <!-- handset -->
    <path d="M2 12 h84"/>
    <path d="M2 12 a7 7 0 0 1 -7 -7 a7 7 0 0 1 14 0"/>
    <path d="M86 12 a7 7 0 0 0 7 -7 a7 7 0 0 0 -14 0"/>
    <!-- cord -->
    <path d="M86 46 q10 8 4 16 q-6 8 4 14" stroke-dasharray="0" opacity="0.8"/>
  </g>
</svg>`,
};
