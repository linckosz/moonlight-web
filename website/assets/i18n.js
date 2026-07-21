/* Shared language switcher for MoonlightWeb sub-pages (guides, FAQ).

   English is the source of truth: the page's own HTML. Other languages come from
   a per-page  window.MW_DICTS = { fr: {...}, zh: {...} }  defined BEFORE this
   script loads. Keys map to elements via  data-i18n  (textContent) and
   data-i18n-html  (innerHTML). The choice is shared with the landing page
   through localStorage 'mw_lang', so switching language anywhere sticks. */
(function () {
  var DICTS = window.MW_DICTS || {};

  // Snapshot the original English markup so switching back to EN restores it.
  var origText = new Map();
  var origHtml = new Map();
  document.querySelectorAll('[data-i18n]').forEach(function (el) {
    origText.set(el, el.textContent);
  });
  document.querySelectorAll('[data-i18n-html]').forEach(function (el) {
    origHtml.set(el, el.innerHTML);
  });

  function apply(lang) {
    if (lang !== 'en' && !DICTS[lang]) lang = 'en';
    var dict = DICTS[lang]; // undefined for EN -> restore original markup
    document.documentElement.lang = lang;
    document.querySelectorAll('[data-i18n]').forEach(function (el) {
      var v = dict ? dict[el.dataset.i18n] : null;
      el.textContent = v != null ? v : origText.get(el);
    });
    document.querySelectorAll('[data-i18n-html]').forEach(function (el) {
      var v = dict ? dict[el.dataset.i18nHtml] : null;
      el.innerHTML = v != null ? v : origHtml.get(el);
    });
    var sel = document.getElementById('lang-select');
    if (sel) sel.value = lang;
    try { localStorage.setItem('mw_lang', lang); } catch (e) { /* private mode */ }
    document.dispatchEvent(new CustomEvent('langchange', { detail: lang }));
  }

  var sel = document.getElementById('lang-select');
  if (sel) sel.addEventListener('change', function () { apply(sel.value); });

  var saved = 'en';
  try { saved = localStorage.getItem('mw_lang') || 'en'; } catch (e) { /* private mode */ }
  apply(saved);
})();
