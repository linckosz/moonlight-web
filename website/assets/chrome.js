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
        '<span class="lang">' +
          '<select id="lang-select" aria-label="Language" data-umami-event="lang">' +
            '<option value="en">English</option>' +
            '<option value="fr">Français</option>' +
            '<option value="zh" lang="zh">中文</option>' +
          '</select>' +
        '</span>' +
        '<a class="btn btn-primary gh-btn" href="https://github.com/linckosz/moonlight-web" target="_blank" rel="noopener" aria-label="GitHub" data-umami-event="github" data-umami-event-loc="nav">★ <span class="gh-label">GitHub</span></a>' +
      '</nav>' +
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
        '<a href="https://github.com/LizardByte/Sunshine" target="_blank" rel="noopener">Sunshine</a> · ' +
        '<a href="https://buymeacoffee.com/brunoocto" target="_blank" rel="noopener" data-umami-event="buy-me-a-coffee" data-umami-event-loc="footer" data-i18n="nav.support">Support</a>' +
      '</span>' +
    '</div>';

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
