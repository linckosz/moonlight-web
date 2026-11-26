/* Download links — resolve the latest GitHub release's assets for this visitor.

   Used by the home page and the three install guides. Every link keeps its
   static href (/releases, or #download for the hero) until the release is
   known, and keeps it for good on any failure (offline, rate-limit, no release
   yet). No version is written anywhere: the asset names are version-stamped,
   so they are matched by their platform suffix in the latest release.

     a[data-dl="hero"]       hero button: names the visitor's OS and downloads
                             its file; a phone or an unknown OS keeps its text
                             and its #download link.
     a[data-dl="main"]       the download section's big button, with #dl-meta
                             (release · file · size) and #dl-hint under it.
     a[data-asset]           one format of one card (win-x64, mac, linux-deb…).
     a[data-dl-os="win"|"mac"|"linux"]
                             a guide's button: always that OS's file, whatever
                             the visitor's OS; only its href changes.

   Text follows the page language (<html lang>). */
(async () => {
  const REPO = 'linckosz/moonlight-web';
  const $ = (s) => document.querySelector(s);
  const hero = $('a[data-dl="hero"]'), btn = $('a[data-dl="main"]'),
        meta = $('#dl-meta'), hint = $('#dl-hint'),
        guides = document.querySelectorAll('a[data-dl-os]'),
        cards = document.querySelectorAll('[data-asset]');
  if (!hero && !btn && !guides.length && !cards.length) return;

  const ua = navigator.userAgent;
  let os = null;
  if (/Android|iPhone|iPad|iPod/i.test(ua) || navigator.maxTouchPoints > 2 && /Mac/.test(ua)) os = 'mobile';
  else if (/Windows/i.test(ua)) os = 'win';
  else if (/Mac/i.test(ua)) os = 'mac';
  else if (/Linux|X11/i.test(ua)) os = 'linux';
  // Linux package family. The UA rarely names the distro (Firefox does on
  // Ubuntu/Fedora, Chrome never), so the fallback is .deb: it covers the
  // Debian family — by far the biggest desktop share — and it installs by
  // double-click. The AppImage is NOT a safe default: it needs chmod +x
  // (without it GNOME just offers to "search for software") and FUSE 2,
  // which Ubuntu >= 22.04 no longer ships.
  const pkg = /Fedora|Red Hat|CentOS|Rocky|Alma|SUSE|Nobara/i.test(ua) ? 'rpm'
            : /Arch|Manjaro|EndeavourOS|SteamOS|Bazzite/i.test(ua) ? 'appimage'
            : 'deb';
  let arch = /aarch64|arm64/i.test(ua) ? 'arm64' : 'x64';
  try {
    const hints = await navigator.userAgentData?.getHighEntropyValues?.(['architecture']);
    if (/arm/i.test(hints?.architecture || '')) arch = 'arm64';
  } catch {}
  // No Linux package is built for ARM: such a visitor gets the generic button.
  if (os === 'linux' && arch === 'arm64') os = null;

  const T = {
    en: {
      base: 'Download MoonlightWeb',
      os: {win: 'Download for Windows', mac: 'Download for Mac', linux: 'Download for Linux'},
      latest: 'Latest release on GitHub',
      mobile: 'On this device, just open the URL of your server — the download goes on the gaming PC.',
      all: (t) => `${t} — all downloads on GitHub`,
      hint: {
        deb: 'Debian · Ubuntu · Mint · Pop!_OS — other distros: .rpm or .AppImage below',
        rpm: 'Fedora · RHEL · openSUSE — other distros: .deb or .AppImage below',
        appimage: 'Portable — run chmod +x first. Debian/Ubuntu: prefer the .deb below',
      },
      mac: 'Apple Silicon (M1 or later)', blocked: 'Blocked by macOS? See the guide',
    },
    fr: {
      base: 'Télécharger MoonlightWeb',
      os: {win: 'Télécharger pour Windows', mac: 'Télécharger pour Mac', linux: 'Télécharger pour Linux'},
      latest: 'Dernière version sur GitHub',
      mobile: 'Sur cet appareil, ouvrez simplement l’URL de votre serveur — le téléchargement se fait sur le PC gamer.',
      all: (t) => `${t} — tous les téléchargements sur GitHub`,
      hint: {
        deb: 'Debian · Ubuntu · Mint · Pop!_OS — autres distros : .rpm ou .AppImage ci‑dessous',
        rpm: 'Fedora · RHEL · openSUSE — autres distros : .deb ou .AppImage ci‑dessous',
        appimage: 'Portable — faites d’abord chmod +x. Debian/Ubuntu : préférez le .deb ci‑dessous',
      },
      mac: 'Apple Silicon (M1 ou plus récent)', blocked: 'Bloqué par macOS ? Voir le guide',
    },
    de: {
      base: 'MoonlightWeb herunterladen',
      os: {win: 'Für Windows herunterladen', mac: 'Für Mac herunterladen', linux: 'Für Linux herunterladen'},
      latest: 'Neueste Version auf GitHub',
      mobile: 'Auf diesem Gerät öffnest du einfach die URL deines Servers — der Download gehört auf den Gaming-PC.',
      all: (t) => `${t} — alle Downloads auf GitHub`,
      hint: {
        deb: 'Debian · Ubuntu · Mint · Pop!_OS — andere Distros: .rpm oder .AppImage unten',
        rpm: 'Fedora · RHEL · openSUSE — andere Distros: .deb oder .AppImage unten',
        appimage: 'Portabel — zuerst chmod +x ausführen. Debian/Ubuntu: lieber die .deb unten',
      },
      mac: 'Apple Silicon (M1 oder neuer)', blocked: 'Von macOS blockiert? Siehe die Anleitung',
    },
    zh: {
      base: '下载 MoonlightWeb',
      os: {win: '下载 Windows 版', mac: '下载 Mac 版', linux: '下载 Linux 版'},
      latest: 'GitHub 上的最新版本',
      mobile: '在本设备上，只需打开你服务器的 URL——下载在游戏 PC 上进行。',
      all: (t) => `${t} — 全部下载见 GitHub`,
      hint: {
        deb: 'Debian · Ubuntu · Mint · Pop!_OS — 其他发行版：下方的 .rpm 或 .AppImage',
        rpm: 'Fedora · RHEL · openSUSE — 其他发行版：下方的 .deb 或 .AppImage',
        appimage: '便携版——请先执行 chmod +x。Debian/Ubuntu：建议用下方的 .deb',
      },
      mac: 'Apple Silicon（M1 或更新机型）', blocked: '被 macOS 拦截？请看安装指南',
    },
  };
  const L = T[document.documentElement.lang] || T.en;

  let best = null, rel = null;                       // filled once the fetch resolves
  const render = () => {
    if (hero && os && L.os[os]) hero.textContent = L.os[os];
    if (btn) btn.textContent = (os && L.os[os]) || L.base;
    if (meta) {
      if (best && rel) meta.textContent = `${rel.tag_name} · ${best.name} · ${(best.size / 1048576).toFixed(0)} MB`;
      else if (os === 'mobile') meta.textContent = L.mobile;
      else if (rel) meta.textContent = L.all(rel.tag_name);
      else meta.textContent = L.latest;
    }
    if (hint) {
      hint.textContent = '';
      if (os === 'linux') hint.textContent = L.hint[pkg];
      else if (os === 'mac') {
        // The .pkg is not notarized: a browser download meets Gatekeeper, and
        // the macOS guide says how to get past it (or to use Homebrew instead).
        const a = document.createElement('a');
        a.href = 'guides/macos.html#blocked';
        a.textContent = L.blocked;
        hint.append(`${L.mac} · `, a);
      }
      hint.hidden = !hint.textContent;               // no empty flex row elsewhere
    }
  };
  render();

  try {
    const r = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`);
    if (!r.ok) throw new Error(r.status);
    rel = await r.json();
    const find = (re) => (rel.assets || []).find((a) => re.test(a.name));
    // Asset names produced by .github/workflows/release.yml (version-stamped,
    // matched here by their platform/arch/extension suffix):
    //   MoonlightWeb-installer-<ver>-win-{x64,arm64}.exe
    //   moonlightweb-<ver>-linux-x64.{deb,rpm}, moonlightweb-<ver>-x86_64.AppImage
    //   moonlightweb-<ver>-macos-arm64.pkg
    const assets = {
      'win-x64': find(/win-x64\.exe$/i),
      'win-arm64': find(/win-arm64\.exe$/i),
      'mac': find(/\.pkg$/i),
      'linux-deb': find(/\.deb$/i),
      'linux-rpm': find(/\.rpm$/i),
      'linux-appimage': find(/\.appimage$/i),
    };
    const pick = {
      // Only a Windows browser's architecture says which Windows file fits.
      win: (os === 'win' && assets[`win-${arch}`]) || assets['win-x64'],
      mac: assets.mac,
      linux: assets[`linux-${pkg}`] || assets['linux-deb'],
    };
    cards.forEach((a) => {
      const m = assets[a.dataset.asset];
      if (m) a.href = m.browser_download_url;
    });
    guides.forEach((a) => {
      const m = pick[a.dataset.dlOs];
      if (m) a.href = m.browser_download_url;
    });
    best = pick[os] || null;
    if (best) {
      if (btn) btn.href = best.browser_download_url;
      if (hero) hero.href = best.browser_download_url;
    }
  } catch { /* keep the static links */ }
  render();
})();
