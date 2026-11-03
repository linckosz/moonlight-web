#!/usr/bin/env node
/* Builds the static parts of the moonlightweb.top website (repo-root website/).

   The English pages are the source of truth and are edited by hand. This script
   stamps into them what must be plain HTML for a search engine to see it, and
   writes everything derived from them:

     - the shared header and footer, written into every page between their
       <header id="site-header"> / <footer id="site-footer"> tags (a crawler
       follows links in the HTML; links a script injects are found late or never);
     - the hreflang alternates of every page, in its <head>;
     - one translated copy of every page per language, under /<lang>/, its text
       taken from website/_i18n/<lang>/<page>.json (keys = the data-i18n and
       data-i18n-html attributes of the English page; meta.title and
       meta.description translate the <head>);
     - sitemap.xml, each URL dated by the last commit of its sources.

   Usage (from anywhere, no dependencies):
     node scripts/website/build-site.mjs           rewrite the files
     node scripts/website/build-site.mjs --check   exit 1 if a file is stale

   Run it after editing a page, a dictionary, or the header/footer below, and
   commit its output with the change. */
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../website');
const ORIGIN = 'https://moonlightweb.top';
const CHECK = process.argv.includes('--check');

/* Languages in the order of the switcher. English is the source and lives at
   the root; every other language is a copy under /<code>/. */
const LANGS = [
  { code: 'en', name: 'English', locale: 'en_US' },
  { code: 'fr', name: 'Français', locale: 'fr_FR' },
  { code: 'de', name: 'Deutsch', locale: 'de_DE' },
  { code: 'zh', name: '中文', locale: 'zh_CN' },
];
const TRANSLATED = LANGS.filter((l) => l.code !== 'en');

/* Every page of the site: its source file, its public path, and the nav entry
   it lights up. A page missing here is neither translated nor in the sitemap,
   unless it is in ENGLISH_ONLY below. */
const PAGES = [
  { src: 'index.html', url: '/', nav: 'home' },
  { src: 'guides/index.html', url: '/guides/', nav: 'guides' },
  { src: 'guides/windows.html', url: '/guides/windows.html', nav: 'guides' },
  { src: 'guides/macos.html', url: '/guides/macos.html', nav: 'guides' },
  { src: 'guides/linux.html', url: '/guides/linux.html', nav: 'guides' },
  { src: 'faq.html', url: '/faq.html', nav: 'faq' },
  { src: 'contact.html', url: '/contact.html', nav: 'contact' },
  { src: 'server-access/index.html', url: '/server-access/', nav: 'connect' },
  { src: 'play/chromebook.html', url: '/play/chromebook.html', nav: null },
  { src: 'play/iphone-ipad.html', url: '/play/iphone-ipad.html', nav: null },
  { src: 'play/tv.html', url: '/play/tv.html', nav: null },
  { src: 'sunshine-web-client.html', url: '/sunshine-web-client.html', nav: null },
  { src: 'browser-game-streaming.html', url: '/browser-game-streaming.html', nav: null },
  { src: 'controllers.html', url: '/controllers.html', nav: null },
];
const PAGE_URLS = new Set(PAGES.map((p) => p.url));

/* Pages in English alone, with a header of their own and outside the menu:
   in the sitemap, without alternates, and nothing else. */
const ENGLISH_ONLY = [{ src: 'mwsr/index.html', url: '/mwsr/' }];

const localUrl = (lang, url) => (lang === 'en' ? url : `/${lang}${url}`);
const pageId = (p) => p.src.replace(/\.html$/, '');

/* ── Header and footer ─────────────────────────────────────────────────────
   The Discord button and the Buy Me a Coffee logo stay in assets/chrome.js:
   neither is a link a search engine needs. */
const DISCORD_INVITE = 'https://discord.gg/wfbesPx4UB';
const GITHUB = 'https://github.com/linckosz/moonlight-web';

function navLinks(lang, t, current, sep) {
  const items = [
    ['home', '/', 'Home'],
    ['guides', '/guides/', 'Guides'],
    ['faq', '/faq.html', 'FAQ'],
    ['contact', '/contact.html', 'Contact'],
    // Opens a host by typing its code — for a device with no address bar,
    // which can reach this site from a search but cannot type the host's link.
    ['connect', '/server-access/', 'Connect'],
  ];
  return items
    .map(([key, url, en]) => {
      const cur = current === key ? ' aria-current="page"' : '';
      const nav = sep ? '' : ` data-nav="${key}"`;
      return `<a href="${localUrl(lang, url)}"${nav}${cur}>${esc(t[`nav.${key}`] ?? en)}</a>`;
    })
    .join(sep);
}

function header(lang, t, page) {
  const options = LANGS.map((l) => {
    const sel = l.code === lang ? ' selected' : '';
    const zh = l.code === 'zh' ? ' lang="zh"' : '';
    return `<option value="${l.code}" data-href="${localUrl(l.code, page.url)}"${zh}${sel}>${l.name}</option>`;
  }).join('');
  return (
    '<div class="wrap">' +
    `<a class="brand" href="${localUrl(lang, '/')}">` +
    '<img src="/assets/icon-192.png" alt="MoonlightWeb logo" width="28" height="28"> MOONLIGHTWEB</a>' +
    `<nav aria-label="Main navigation">${navLinks(lang, t, page.nav, '')}</nav>` +
    // Controls, not nav links — kept OUTSIDE <nav> so mobile can hoist them
    // onto the brand row (top right) while the links wrap to their own row.
    '<div class="hdr-actions">' +
    `<span class="lang"><select id="lang-select" aria-label="Language" data-umami-event="lang">${options}</select></span>` +
    `<a class="btn btn-primary gh-btn" href="${GITHUB}" target="_blank" rel="noopener" aria-label="GitHub" data-umami-event="github" data-umami-event-loc="nav">★ <span class="gh-label">GitHub</span></a>` +
    '</div></div>'
  );
}

function footer(lang, t) {
  return (
    '<div class="wrap">' +
    '<span><img src="/assets/icon-192.png" alt="" width="18" height="18" style="border-radius:4px;vertical-align:-3px;margin-right:6px"> MoonlightWeb · © 2026 Bruno Martin · GPL‑3.0</span>' +
    '<span>' +
    navLinks(lang, t, null, ' · ') +
    ` · <a href="${GITHUB}" target="_blank" rel="noopener" data-umami-event="github" data-umami-event-loc="footer">GitHub</a>` +
    ` · <a href="https://buymeacoffee.com/brunoocto" target="_blank" rel="noopener" data-umami-event="buy-me-a-coffee" data-umami-event-loc="footer" class="bmc-link"><span data-bmc-logo></span><span>${esc(t['nav.support'] ?? 'Coffee')}</span></a>` +
    '</span>' +
    // The pages for one device or one host, linked from every page: nothing
    // else on the site points at them, and a search engine weighs a page by
    // the links it finds to it.
    `<span class="foot-play">${esc(t['foot.play'] ?? 'Play from:')} ` +
    [
      ['/play/chromebook.html', 'foot.cb', 'Chromebook'],
      ['/play/iphone-ipad.html', 'foot.ios', 'iPhone & iPad'],
      ['/play/tv.html', 'foot.tv', 'TV'],
      ['/sunshine-web-client.html', 'foot.sunshine', 'Sunshine web client'],
      ['/browser-game-streaming.html', 'foot.why', 'Why a browser client'],
    ]
      .map(([url, key, en]) => `<a href="${localUrl(lang, url)}">${esc(t[key] ?? en)}</a>`)
      .join(' · ') +
    '</span></div>'
  );
}

/* ── HTML helpers ──────────────────────────────────────────────────────────
   The pages are hand-written, well-formed HTML: a tag scanner is enough, and
   keeps this script free of dependencies. */
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const escAttr = (s) => esc(s).replace(/"/g, '&quot;');

/** Replaces the content of the first <tag id="id"> … </tag>. */
function fillById(html, tag, id, inner) {
  const re = new RegExp(`(<${tag} id="${id}">)[\\s\\S]*?(</${tag}>)`);
  if (!re.test(html)) throw new Error(`<${tag} id="${id}"> not found`);
  return html.replace(re, (_, open, close) => open + inner + close);
}

/** End offset of the element whose open tag ends at `from` (just past its </tag>). */
function closeOf(html, tag, from) {
  const re = new RegExp(`<(/?)${tag}\\b[^>]*>`, 'gi');
  re.lastIndex = from;
  let depth = 1;
  for (let m; (m = re.exec(html)); ) {
    if (m[1]) {
      if (--depth === 0) return { innerEnd: m.index, end: re.lastIndex };
    } else if (!m[0].endsWith('/>')) depth++;
  }
  throw new Error(`unclosed <${tag}> at ${from}`);
}

/** Swaps the content of every data-i18n / data-i18n-html element for its translation. */
function translateBody(html, dict, missing) {
  const open = /<([a-zA-Z][\w-]*)\b[^>]*?\sdata-i18n(-html)?="([^"]+)"[^>]*>/g;
  const hits = [];
  for (let m; (m = open.exec(html)); ) hits.push({ at: m.index, len: m[0].length, tag: m[1], raw: !!m[2], key: m[3] });
  // Last first: an element is rewritten before any element that contains it,
  // and offsets before the one being rewritten stay valid.
  for (const h of hits.reverse()) {
    const v = dict[h.key];
    if (v == null) { missing.add(h.key); continue; }
    const start = h.at + h.len;
    const { innerEnd } = closeOf(html, h.tag, start);
    html = html.slice(0, start) + (h.raw ? v : esc(v)) + html.slice(innerEnd);
  }
  return html;
}

/** Points the page's own links at the same page in `lang`, and makes relative URLs absolute. */
function localiseUrls(html, lang, page) {
  const base = new URL(page.url, ORIGIN);
  const fix = (value) => {
    if (/^(#|mailto:|javascript:|data:|\{\{)/.test(value)) return value;
    if (/^[a-z]+:\/\//i.test(value) && !value.startsWith(ORIGIN + '/')) return value;
    // The query may hold a Caddy template ({{fileStat …}}): keep it verbatim.
    const cut = value.search(/[?#]/);
    const tail = cut < 0 ? '' : value.slice(cut);
    let p = new URL(cut < 0 ? value : value.slice(0, cut), base).pathname;
    if (PAGE_URLS.has(p) || PAGE_URLS.has(p.replace(/index\.html$/, ''))) p = localUrl(lang, p);
    return (value.startsWith(ORIGIN) ? ORIGIN : '') + p + tail;
  };
  html = html.replace(/\s(href|src)="([^"]*)"/g, (all, attr, v) => ` ${attr}="${fix(v)}"`);
  // Absolute page URLs in structured data and meta tags.
  html = html.replace(/(<script type="application\/ld\+json">[\s\S]*?<\/script>)/g, (block) =>
    block.replace(/"(https:\/\/moonlightweb\.top\/[^"]*)"/g, (all, v) => `"${fix(v)}"`),
  );
  html = html.replace(/(<meta property="og:url" content=")([^"]*)"/, (all, a, v) => `${a}${fix(v)}"`);
  return html;
}

function setMeta(html, sel, value) {
  const re = new RegExp(`(<meta ${sel} content=")[^"]*(")`);
  return html.replace(re, (_, a, b) => a + escAttr(value) + b);
}

function alternates(page) {
  const links = LANGS.map(
    (l) => `<link rel="alternate" hreflang="${l.code}" href="${ORIGIN}${localUrl(l.code, page.url)}">`,
  );
  links.push(`<link rel="alternate" hreflang="x-default" href="${ORIGIN}${page.url}">`);
  return `<!-- i18n:alternates (written by scripts/website/build-site.mjs) -->\n${links.join('\n')}\n<!-- /i18n:alternates -->`;
}

function stampAlternates(html, page) {
  const block = alternates(page);
  const re = /<!-- i18n:alternates[\s\S]*?<!-- \/i18n:alternates -->/;
  if (re.test(html)) return html.replace(re, block);
  return html.replace(/(<link rel="canonical"[^>]*>\n)/, `$1${block}\n`);
}

const readDict = (lang, id) => {
  const f = path.join(SITE, '_i18n', lang, id + '.json');
  return fs.existsSync(f) ? JSON.parse(fs.readFileSync(f, 'utf8')) : {};
};

/* ── Build ─────────────────────────────────────────────────────────────── */
const outputs = new Map(); // absolute path -> content
const problems = [];

const chromeEn = {};
for (const page of PAGES) {
  let html = fs.readFileSync(path.join(SITE, page.src), 'utf8');
  html = fillById(html, 'header', 'site-header', header('en', chromeEn, page));
  html = fillById(html, 'footer', 'site-footer', footer('en', chromeEn));
  html = stampAlternates(html, page);
  outputs.set(path.join(SITE, page.src), html);

  for (const { code, locale } of TRANSLATED) {
    const chrome = readDict(code, '_chrome');
    const dict = readDict(code, pageId(page));
    const missing = new Set();
    let out = translateBody(html, dict, missing);
    out = stampAlternates(localiseUrls(out, code, page), page);
    out = fillById(out, 'header', 'site-header', header(code, chrome, page));
    out = fillById(out, 'footer', 'site-footer', footer(code, chrome));
    out = out.replace(/<html lang="en">/, `<html lang="${code}">`);
    out = out.replace(
      /<link rel="canonical" href="[^"]*">/,
      `<link rel="canonical" href="${ORIGIN}${localUrl(code, page.url)}">`,
    );
    if (dict['meta.title']) {
      out = out.replace(/<title>[^<]*<\/title>/, `<title>${esc(dict['meta.title'])}</title>`);
      out = setMeta(out, 'property="og:title"', dict['meta.title']);
      out = setMeta(out, 'name="twitter:title"', dict['meta.title']);
    } else missing.add('meta.title');
    if (dict['meta.description']) {
      out = setMeta(out, 'name="description"', dict['meta.description']);
      out = setMeta(out, 'property="og:description"', dict['meta.description']);
      out = setMeta(out, 'name="twitter:description"', dict['meta.description']);
    } else missing.add('meta.description');
    out = setMeta(out, 'property="og:locale"', locale);
    out = out.replace(
      /^<!DOCTYPE html>\n/i,
      `<!DOCTYPE html>\n<!-- Generated from /${page.src} and _i18n/${code}/${pageId(page)}.json by scripts/website/build-site.mjs: edit those, then rebuild. -->\n`,
    );
    if (missing.size) problems.push(`${code}/${page.src}: ${missing.size} untranslated (${[...missing].join(', ')})`);
    outputs.set(path.join(SITE, code, page.src), out);
  }
}

/* sitemap.xml — each URL dated by the newest of its sources (the English page,
   the header/footer above, its dictionary). An uncommitted source dates today. */
function lastChange(files) {
  const today = new Date().toISOString().slice(0, 10);
  let best = '';
  for (const f of files) {
    if (!fs.existsSync(f)) continue;
    const rel = path.relative(path.dirname(SITE), f).replace(/\\/g, '/');
    const dirty = execFileSync('git', ['status', '--porcelain', '--', rel], { cwd: path.dirname(SITE) }).toString().trim();
    const d = dirty ? today : execFileSync('git', ['log', '-1', '--format=%cs', '--', rel], { cwd: path.dirname(SITE) }).toString().trim();
    if (d > best) best = d;
  }
  return best || today;
}
const builder = fileURLToPath(import.meta.url);
const urls = [];
for (const page of PAGES) {
  const alts = [...LANGS.map((l) => [l.code, localUrl(l.code, page.url)]), ['x-default', page.url]]
    .map(([h, u]) => `    <xhtml:link rel="alternate" hreflang="${h}" href="${ORIGIN}${u}"/>`)
    .join('\n');
  for (const { code } of LANGS) {
    const sources = [path.join(SITE, page.src), builder];
    if (code !== 'en') sources.push(path.join(SITE, '_i18n', code, pageId(page) + '.json'), path.join(SITE, '_i18n', code, '_chrome.json'));
    urls.push(`  <url>\n    <loc>${ORIGIN}${localUrl(code, page.url)}</loc>\n    <lastmod>${lastChange(sources)}</lastmod>\n${alts}\n  </url>`);
  }
}
for (const page of ENGLISH_ONLY)
  urls.push(`  <url>\n    <loc>${ORIGIN}${page.url}</loc>\n    <lastmod>${lastChange([path.join(SITE, page.src), builder])}</lastmod>\n  </url>`);
outputs.set(
  path.join(SITE, 'sitemap.xml'),
  '<?xml version="1.0" encoding="UTF-8"?>\n' +
    '<!-- Written by scripts/website/build-site.mjs — do not edit by hand. -->\n' +
    '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" xmlns:xhtml="http://www.w3.org/1999/xhtml">\n' +
    urls.join('\n') +
    '\n</urlset>\n',
);

/* ── The MWSR comparison (website/mwsr/) ───────────────────────────────────
   A tool page in English, outside PAGES: neither translated nor in the menu,
   in the sitemap through ENGLISH_ONLY. It runs what MoonlightWeb ships: its
   kernels are written here from the product's modules (and the lab's 16-bit
   ports of FSR1 and SGSR1, so that each runs at its best precision), so a
   shader changed in the product and not rebuilt here fails --check. */
{
  const REPO = path.dirname(SITE);
  const read = (rel) => fs.readFileSync(path.join(REPO, rel), 'utf8').replace(/\r\n/g, '\n');
  const renderer = read('frontend/js/stream/renderers/WebGpuRenderer.js');
  // A WGSL template literal of the renderer by the name of its const, the
  // vertex stage prepended to the ones built on it (the lab's extractWgsl).
  const wgsl = (name) => {
    const at = renderer.indexOf('const ' + name + ' =');
    if (at < 0) throw new Error(`${name} not found in WebGpuRenderer.js`);
    const b0 = renderer.indexOf('`', at);
    const b1 = renderer.indexOf('`', b0 + 1);
    const body = renderer.slice(b0 + 1, b1);
    if (body.includes('${')) throw new Error(`${name} has an interpolation`);
    return renderer.slice(at, b0).includes('FULLSCREEN_VS +') ? wgsl('FULLSCREEN_VS') + body : body;
  };
  const lab = (f) => import(new URL(`../bench/scaler-lab/${f}`, import.meta.url));
  const { EASU_H_WGSL, RCAS_H_WGSL } = await lab('fsr1-half.js');
  const { sgsrHalf } = await lab('sgsr1-half.js');
  const SGSR = wgsl('SGSR_WGSL');
  const kernels = {
    SGSR_WGSL: SGSR,
    SGSR_H_WGSL: sgsrHalf(SGSR),
    NIS_WGSL: wgsl('NIS_WGSL'),
    EASU_WGSL: wgsl('EASU_WGSL'),
    RCAS_WGSL: wgsl('RCAS_WGSL'),
    EASU_H_WGSL,
    RCAS_H_WGSL,
  };
  const notice = (rel) =>
    read(rel)
      .trim()
      .split('\n')
      .map((l) => (' * ' + l.replace(/^\/\/ ?/, '')).trimEnd())
      .join('\n');
  const tp = 'backend/native-host/tools/scaler-bench/third_party/';
  outputs.set(
    path.join(SITE, 'mwsr', 'kernels.js'),
    '/*\n' +
      ' * Written by scripts/website/build-site.mjs — do not edit by hand.\n' +
      ' *\n' +
      ' * The kernels the comparison runs, as MoonlightWeb ships them: the WGSL of\n' +
      ' * frontend/js/stream/renderers/WebGpuRenderer.js (GPL-3.0-or-later), which\n' +
      ' * ports FSR1, NIS and SGSR1, and the lab\'s 16-bit ports of FSR1 and SGSR1\n' +
      ' * (scripts/bench/scaler-lab/fsr1-half.js, sgsr1-half.js). The vendors\'\n' +
      ' * notices of the code they port:\n' +
      ' *\n' +
      ' * AMD FidelityFX Super Resolution 1 (EASU, RCAS):\n *\n' +
      notice(tp + 'ffx/LICENSE.txt') +
      '\n *\n * NVIDIA Image Scaling (NVScaler):\n *\n' +
      notice(tp + 'nis/LICENSE.txt') +
      '\n *\n * Snapdragon Game Super Resolution 1:\n *\n' +
      notice(tp + 'sgsr/LICENSE') +
      '\n */\n' +
      Object.entries(kernels)
        .map(([k, v]) => `export const ${k} = ${JSON.stringify(v)};\n`)
        .join(''),
  );
  // MWSR itself (MIT) and NIS's filter banks, copied as they are.
  for (const f of ['MwsrShader.js', 'NisTables.js'])
    outputs.set(path.join(SITE, 'mwsr', f), read('frontend/js/stream/renderers/' + f));
}

/* ── Write or check ────────────────────────────────────────────────────── */
let stale = 0;
for (const [file, content] of outputs) {
  const now = fs.existsSync(file) ? fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n') : null;
  // The sitemap's dates move with the working tree; --check ignores them.
  const norm = (s) => (file.endsWith('sitemap.xml') ? s?.replace(/<lastmod>[^<]*<\/lastmod>/g, '') : s);
  if (norm(now) === norm(content)) continue;
  stale++;
  const rel = path.relative(SITE, file);
  if (CHECK) console.error(`stale: website/${rel.replace(/\\/g, '/')}`);
  else {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, content);
    console.log(`wrote website/${rel.replace(/\\/g, '/')}`);
  }
}
for (const p of problems) console.warn(`warning: ${p}`);
if (CHECK && stale) {
  console.error(`${stale} file(s) out of date — run: node scripts/website/build-site.mjs`);
  process.exit(1);
}
