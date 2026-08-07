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
 *
 * Both drawings share the same stick figure, so the two dead ends read as the
 * same little story rather than two unrelated icons.
 */
export const PlayerArt = {
    /**
     * "The void of video games" — a dead link. The spiral is the whole point:
     * without it the figure was just standing in the dark next to a controller
     * and nobody could tell where they were. Kept very faint so it reads as a
     * pull, not as a pattern; the figure tumbles into it head-first and the
     * controller follows one arm behind.
     */
    void: `
<svg viewBox="0 0 240 150" role="img" aria-hidden="true" class="player-art-svg">
  <defs>
    <radialGradient id="mw-void-bg" cx="50%" cy="50%" r="55%">
      <stop offset="0%" stop-color="currentColor" stop-opacity="0.16"/>
      <stop offset="100%" stop-color="currentColor" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <rect x="0" y="0" width="240" height="150" fill="url(#mw-void-bg)"/>
  <!-- the drain: an Archimedean spiral, barely there -->
  <g fill="none" stroke="currentColor" stroke-width="1.2" stroke-linecap="round"
     stroke-linejoin="round" opacity="0.16">
    <path d="M126.2 76.0 L126.7 76.7 L127.0 77.4 L127.1 78.2 L127.0 79.1 L126.6 80.0
             L126.0 80.9 L125.0 81.7 L123.9 82.5 L122.4 83.2 L120.8 83.7 L118.9 84.1
             L116.9 84.2 L114.8 84.2 L112.7 83.9 L110.5 83.4 L108.5 82.6 L106.6 81.6
             L104.9 80.4 L103.5 79.0 L102.4 77.4 L101.7 75.7 L101.4 73.9 L101.6 72.0
             L102.2 70.1 L103.3 68.2 L104.9 66.4 L107.0 64.7 L109.5 63.3 L112.4 62.1
             L115.6 61.1 L119.1 60.5 L122.8 60.2 L126.6 60.3 L130.4 60.8 L134.1 61.6
             L137.7 62.9 L141.0 64.5 L143.9 66.5 L146.4 68.8 L148.3 71.3 L149.7 74.1
             L150.4 77.0 L150.4 80.0 L149.6 83.0 L148.2 86.0 L146.0 88.8 L143.1 91.5
             L139.6 93.8 L135.4 95.9 L130.8 97.5 L125.8 98.7 L120.5 99.3 L114.9 99.5
             L109.4 99.1 L103.9 98.1 L98.6 96.6 L93.6 94.6 L89.1 92.1 L85.3 89.1
             L82.1 85.7 L79.7 82.0 L78.2 78.0 L77.6 73.9 L78.0 69.8 L79.4 65.6
             L81.8 61.6 L85.1 57.8 L89.3 54.4 L94.4 51.4 L100.2 48.8 L106.6 46.8
             L113.4 45.5 L120.6 44.8 L127.9 44.9 L135.3 45.7 L142.4 47.2 L149.2 49.4
             L155.5 52.2 L161.1 55.7 L165.9 59.7 L169.7 64.2 L172.4 69.1 L174.0 74.2
             L174.4 79.5 L173.5 84.9 L171.3 90.1 L168.0 95.1 L163.4 99.8 L157.8 104.0
             L151.1 107.6 L143.6 110.6 L135.5 112.9 L126.8 114.3 L117.7 114.9
             L108.6 114.6 L99.5 113.4 L90.8 111.4 L82.5 108.4 L75.0 104.7 L68.3 100.2
             L62.8 95.1 L58.4 89.5 L55.4 83.4 L53.9 77.1 L53.8 70.6 L55.2 64.1
             L58.2 57.8 L62.7 51.9 L68.5 46.3 L75.6 41.4 L83.9 37.2 L93.1 33.8
             L103.1 31.3 L113.7 29.8 L124.6 29.3 L135.5 29.9 L146.3 31.6 L156.6 34.3
             L166.2 38.0 L175.0 42.7 L182.6 48.2 L188.8 54.4 L193.6 61.2 L196.8 68.5
             L198.3 76.0 L198.0 83.6 L195.8 91.2 L192.0 98.6 L186.4 105.5 L179.2 111.8
             L170.5 117.5 L160.6 122.2 L149.5 126.0 L137.7 128.6 L125.3 130.1
             L112.6 130.4 L99.9 129.4 L87.4 127.2 L75.6 123.8 L64.6 119.2 L54.7 113.6
             L46.2 107.0 L39.3 99.7 L34.2 91.7 L30.9 83.3 L29.6 74.5 L30.4 65.7
             L33.3 57.0 L38.2 48.7 L45.0 40.9"/>
  </g>
  <!-- dead pixels, also caught in the current -->
  <g fill="currentColor" opacity="0.45">
    <rect x="30" y="26" width="3" height="3"/>
    <rect x="208" y="40" width="3" height="3"/>
    <rect x="46" y="124" width="2" height="2"/>
    <rect x="196" y="120" width="3" height="3"/>
    <rect x="120" y="14" width="2" height="2"/>
    <rect x="16" y="86" width="2" height="2"/>
  </g>
  <!-- the controller, one arm behind, tumbling in -->
  <g transform="translate(74 106) rotate(-34) scale(0.9)" fill="none" stroke="currentColor"
     stroke-width="2.5" stroke-linejoin="round" opacity="0.8">
    <path d="M-20 -6 h40 a10 10 0 0 1 10 10 v2 a8 8 0 0 1 -14 5 l-6 -5 h-20 l-6 5
             a8 8 0 0 1 -14 -5 v-2 a10 10 0 0 1 10 -10 z"/>
    <path d="M-12 0 v6 M-15 3 h6"/>
    <circle cx="12" cy="1" r="2"/>
    <circle cx="18" cy="5" r="2"/>
  </g>
  <!-- the player, head-first down the drain, arms and legs everywhere -->
  <g transform="translate(154 54) rotate(202)" fill="none" stroke="currentColor" stroke-width="3"
     stroke-linecap="round" stroke-linejoin="round">
    <circle cx="0" cy="-16" r="9"/>
    <path d="M0 -7 v20"/>
    <path d="M0 -2 l-15 -6 M0 -2 l13 -10"/>
    <path d="M0 13 l-12 12 M0 13 l9 15"/>
  </g>
</svg>`,

    /**
     * "Unplugged" — the owner ended the session. Two halves of a cable pulled
     * apart, which is exactly what the copy says happened ("nothing broke, you
     * were just unplugged"), and the same figure shrugging at it. The rotary
     * phone this replaces read as a webcam at this size.
     */
    unplugged: `
<svg viewBox="0 0 240 150" role="img" aria-hidden="true" class="player-art-svg">
  <!-- the guest, shrugging -->
  <g transform="translate(44 68)" fill="none" stroke="currentColor" stroke-width="3"
     stroke-linecap="round" stroke-linejoin="round">
    <circle cx="0" cy="-16" r="9"/>
    <path d="M0 -7 v20"/>
    <!-- arms up, palms out: the universal "well, that's that" -->
    <path d="M0 -2 l-13 -6 l-4 -9 M0 -2 l13 -6 l4 -9"/>
    <path d="M0 13 l-9 14 M0 13 l10 14"/>
  </g>

  <!-- plug half, pulled out -->
  <g fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"
     stroke-linejoin="round">
    <path d="M98 66 h22 a4 4 0 0 1 4 4 v16 a4 4 0 0 1 -4 4 h-22 z"/>
    <path d="M124 72 h11 M124 84 h11"/>
    <!-- its cord, trailing back to the guest -->
    <path d="M98 78 q-14 0 -20 10 q-6 10 -16 12" opacity="0.75"/>
  </g>

  <!-- socket half, still live -->
  <g fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"
     stroke-linejoin="round">
    <path d="M172 62 h20 a5 5 0 0 1 5 5 v22 a5 5 0 0 1 -5 5 h-20 z"/>
    <path d="M172 72 h-6 M172 84 h-6"/>
    <!-- and its cord, off toward the host -->
    <path d="M197 78 q14 0 20 -11 q5 -9 15 -11" opacity="0.75"/>
  </g>

  <!-- the gap, sparking -->
  <g stroke="currentColor" stroke-width="2.2" stroke-linecap="round" opacity="0.85">
    <path d="M144 66 l6 -7"/>
    <path d="M148 78 h8"/>
    <path d="M144 90 l6 7"/>
  </g>
</svg>`,
};
