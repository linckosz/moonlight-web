// MoonlightWeb bootstrap — placeholder logic.
//
// An external file rather than an inline <script> so the Content-Security-Policy
// can say `script-src 'self'` with no 'unsafe-inline'. That matters more here
// than on an ordinary page: this is the one piece of code a browser loads from a
// server, and it is the code that will hold the pairing key.
//
// The real bootstrap ships with phase 2b: Service Worker install, MW-BIND-v1
// handshake, then the interface itself fetched from the user's own machine over
// WebRTC.
'use strict';

// Stands in for frame-ancestors on the GitHub Pages copy. Caddy sends that
// directive as a real header on stream.{domain}, but a <meta> CSP cannot express
// it, and Pages sends no headers — so on that host this check is the only thing
// keeping the page out of someone else's frame.
if (window.top !== window.self) {
  window.top.location = window.self.location;
}
