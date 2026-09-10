/* Shared site chrome (header + footer) for every MoonlightWeb page.

   ONE source of truth for the nav / footer markup — injected into the
   <header id="site-header"> and <footer id="site-footer"> placeholders that
   each page carries. Add a nav link or change the footer here once and every
   page updates; the two headers can never drift apart again.

   LOAD ORDER MATTERS: include this as a *blocking* <script> placed AFTER the
   placeholders but BEFORE the page's i18n script (the inline block on the
   landing page, assets/i18n.js on the sub-pages). Both i18n systems snapshot
   every [data-i18n] element and wire up #lang-select on load, so the markup
   must already be in the DOM when they run. Styling lives in assets/chrome.css.

   All URLs are absolute (/...) so the same markup works from / and from
   /guides/. */
(function () {
  /* The community server, in ONE place for the whole site — the floating
     Discord button below and nothing else reads it. Empty string means the
     invite does not exist yet and NO button is drawn anywhere: a dead link to
     a server nobody can join is worse than no link at all.

     Fill in the permanent invite (Discord: never expires, unlimited uses) and
     every page grows the button on the next deploy. Same constant, same rule,
     in frontend/js/ui/DiscordLink.js for the application. */
  var DISCORD_INVITE = 'https://discord.gg/wfbesPx4UB';

  var header =
    '<div class="wrap">' +
      '<a class="brand" href="/">' +
        '<img src="/assets/icon-192.png" alt="MoonlightWeb logo" width="28" height="28">' +
        ' MOONLIGHTWEB' +
      '</a>' +
      '<nav aria-label="Main navigation">' +
        '<a href="/" data-nav="home" data-i18n="nav.home">Home</a>' +
        '<a href="/guides/" data-nav="guides" data-i18n="nav.guides">Guides</a>' +
        '<a href="/faq.html" data-nav="faq" data-i18n="nav.faq">FAQ</a>' +
        '<a href="/contact.html" data-nav="contact" data-i18n="nav.contact">Contact</a>' +
      '</nav>' +
      /* Controls, not nav links — kept OUTSIDE <nav> so mobile can hoist them
         onto the brand row (top right) while the links wrap to their own row. */
      '<div class="hdr-actions">' +
        '<span class="lang">' +
          '<select id="lang-select" aria-label="Language" data-umami-event="lang">' +
            '<option value="en">English</option>' +
            '<option value="fr">Français</option>' +
            '<option value="zh" lang="zh">中文</option>' +
          '</select>' +
        '</span>' +
        '<a class="btn btn-primary gh-btn" href="https://github.com/linckosz/moonlight-web" target="_blank" rel="noopener" aria-label="GitHub" data-umami-event="github" data-umami-event-loc="nav">★ <span class="gh-label">GitHub</span></a>' +
      '</div>' +
    '</div>';

  var footer =
    '<div class="wrap">' +
      '<span><img src="/assets/icon-192.png" alt="" width="18" height="18" style="border-radius:4px;vertical-align:-3px;margin-right:6px"> MoonlightWeb · © 2026 Bruno Martin · GPL‑3.0</span>' +
      '<span>' +
        '<a href="/" data-i18n="nav.home">Home</a> · ' +
        '<a href="/guides/" data-i18n="nav.guides">Guides</a> · ' +
        '<a href="/faq.html" data-i18n="nav.faq">FAQ</a> · ' +
        '<a href="/contact.html" data-i18n="nav.contact">Contact</a> · ' +
        '<a href="https://github.com/linckosz/moonlight-web" target="_blank" rel="noopener" data-umami-event="github" data-umami-event-loc="footer">GitHub</a> · ' +
        '<a href="https://buymeacoffee.com/brunoocto" target="_blank" rel="noopener" data-umami-event="buy-me-a-coffee" data-umami-event-loc="footer" data-i18n="nav.support">☕ Coffee</a>' +
      '</span>' +
    '</div>';

  /* Floating community button — bottom right of every page, above the fold and
     below it, because "where do I ask a question?" is asked at any point of a
     page and not only at its end (the footer already carries the links for the
     reader who scrolls to the bottom).

     Appended to <body> rather than to the footer: position:fixed inside the
     footer would still work, but the element belongs to the viewport, not to
     the page flow, and a future footer with a transform on it would trap it. */
  if (DISCORD_INVITE) {
    var fab = document.createElement('a');
    fab.className = 'discord-fab';
    fab.href = DISCORD_INVITE;
    fab.target = '_blank';
    fab.rel = 'noopener';
    /* No visible label: the round blurple mark IS the label, exactly like the
       chat launchers this shape is borrowed from. title + aria-label carry the
       word for the tooltip and for a screen reader — just "Discord", which
       needs no translation: the site's i18n only rewrites text nodes, never
       attributes, so a sentence here would stay English on the FR/ZH pages. */
    fab.title = 'Discord';
    fab.setAttribute('aria-label', 'Discord');
    fab.setAttribute('data-umami-event', 'discord');
    fab.setAttribute('data-umami-event-loc', 'fab');
    fab.innerHTML =
      '<svg viewBox="0 0 127.14 96.36" width="26" height="26" fill="currentColor" aria-hidden="true">' +
        '<path d="M107.7 8.07A105.15 105.15 0 0 0 81.47 0a72.06 72.06 0 0 0-3.36 6.83 97.68 97.68 0 0 0-29.11 0A72.37 72.37 0 0 0 45.64 0a105.89 105.89 0 0 0-26.25 8.09C2.79 32.65-1.71 56.6.54 80.21a105.73 105.73 0 0 0 32.17 16.15 77.7 77.7 0 0 0 6.89-11.11 68.42 68.42 0 0 1-10.85-5.18c.91-.66 1.8-1.34 2.66-2a75.57 75.57 0 0 0 64.32 0c.87.71 1.76 1.39 2.66 2a68.68 68.68 0 0 1-10.87 5.19 77 77 0 0 0 6.89 11.1 105.25 105.25 0 0 0 32.19-16.14c2.64-27.38-4.51-51.11-18.9-72.15ZM42.45 65.69C36.18 65.69 31 60 31 53s5-12.74 11.43-12.74S54 46 53.89 53s-5.05 12.69-11.44 12.69Zm42.24 0C78.41 65.69 73.25 60 73.25 53s5-12.74 11.44-12.74S96.23 46 96.12 53s-5.04 12.69-11.43 12.69Z"/>' +
      '</svg>';
    document.body.appendChild(fab);
  }

  var h = document.getElementById('site-header');
  if (h) h.innerHTML = header;
  var f = document.getElementById('site-footer');
  if (f) f.innerHTML = footer;

  // Highlight the nav link for the current page (matches the old per-page
  // aria-current="page"). Normalise "/index.html" and trailing "/" to "/".
  var path = location.pathname.replace(/index\.html$/, '');
  if (path === '' ) path = '/';
  var active = path === '/' ? 'home'
             : path.indexOf('/guides/') === 0 ? 'guides'
             : /\/faq\.html$/.test(path) ? 'faq'
             : /\/contact\.html$/.test(path) ? 'contact'
             : null;
  if (active && h) {
    var link = h.querySelector('nav a[data-nav="' + active + '"]');
    if (link) link.setAttribute('aria-current', 'page');
  }
})();
