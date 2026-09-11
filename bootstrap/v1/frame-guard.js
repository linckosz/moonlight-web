// MoonlightWeb — bootstrap. Copyright (C) 2026 Bruno Martin. GPLv3.
//
// Stands in for frame-ancestors on the GitHub Pages copy.
//
// Caddy sends that directive as a real header on stream.{domain}, but a <meta>
// CSP cannot express it and Pages sends no headers at all — so on that host this
// is the only thing keeping the page out of someone else's frame.
//
// A separate, classic script rather than part of boot.js: a module is deferred,
// and by the time a deferred script runs the framing page has already had the
// document. This one runs the moment it is parsed.
//
// An external file rather than an inline <script> so the policy can say
// `script-src 'self'` with no 'unsafe-inline'. That matters more here than on an
// ordinary page: this is the code that will hold the pairing key.
'use strict';

if (window.top !== window.self) {
    window.top.location = window.self.location;
}
