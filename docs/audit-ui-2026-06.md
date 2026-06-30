# Audit UI — MoonlightWeb (2026-06-17)

> Audit **sans modification de code**. Contraintes invariantes imposées :
> **même navigation, même wording, même comportement, mêmes informations
> visuelles**. Seuls le *look & feel*, l'architecture des fichiers et
> l'ajout de l'i18n sont ouverts.

---

## 1. État des lieux

### 1.1 Arborescence frontend actuelle

```
frontend/
  index.html                 (54 l)   shell : header + main + footer
  css/
    style.css               (1500 l)  tokens :root + toutes les vues hors stream
    stream.css               (657 l)  overlay de streaming
  js/
    app.js                   (988 l)  routeur History API, orchestration overlays
    ui/
      HostListView.js        (349 l)  liste des hôtes + cartes
      AppListView.js         (181 l)  grille d'applications
      SettingsView.js        (706 l)  préférences streaming (localStorage)
      AdminView.js          (1066 l)  réglages serveur (localhost)
      LoginView.js           (503 l)  auth PIN / certificat
      PairDialog.js          (157 l)  dialogue de pairing
      StreamView.js         (4722 l)  overlay de streaming (vidéo/input/stats/clavier)
      Toast.js                (80 l)  notifications
    api/   …                          BackendClient, WebRtc*, WebSocket
    stream/ …                         renderers, worker, JitterController
    models/, util/, utils/, audio/, vendor/
```

### 1.2 Pattern de rendu

Toutes les vues suivent le même modèle : **classe ES6** avec une méthode
`render()` qui assigne une **template string** à `container.innerHTML`, puis
`bindEvents()` qui rattache les écouteurs. Le texte est **codé en dur en
anglais** directement dans ces templates (et dans `index.html`).

Conséquences pour la refonte :
- Le *look & feel* se pilote presque entièrement par le **CSS** + le balisage
  de classes → une refonte visuelle est possible **sans toucher au JS** dans
  ~80 % des cas (il reste du style inline et des emojis dans le JS).
- L'i18n impose en revanche de **remplacer chaque chaîne** par un appel
  `t('clé')` — touche à tous les fichiers de vue, mais **mécaniquement**, sans
  changer le wording (mêmes textes, juste externalisés).

### 1.3 Navigation (à préserver telle quelle)

`app.js` implémente un routeur History API maison :
- **Vues principales** : `hosts` (`/`), `apps` (`/apps`).
- **Overlays** : `admin` (`/admin`), `settings` (`/settings`), `streaming`
  (plein écran, sans URL). Un *guard state* gère le retour arrière.
- Boutons header : `#btn-admin` (localhost), `#btn-settings`.

→ **Aucune modification de cette logique n'est nécessaire** pour la refonte
visuelle ni pour l'i18n.

---

## 2. Audit Look & Feel — constats

### 2.1 Système de design : tokens actuels

```css
--bg-primary:#1a1a2e  --bg-secondary:#16213e  --bg-card:#0f3460
--text-primary:#e0e0e0  --text-secondary:#a0a0b0
--accent:#e94560 (rouge/rose)  --accent-hover:#ff6b81
--success:#4caf50  --warning:#ff9800  --error:#f44336
--border:#2a2a4a  --radius:12px
```

**Problèmes :**
1. **Palette datée** — le combo navy `#1a1a2e` + accent rouge framboise
   `#e94560` est un thème « dark Bootstrap 2018 ». Peu différenciant, peu
   moderne.
2. **Trop de couleurs d'action concurrentes** : l'accent rouge (`.btn`),
   le bleu (`.btn-neutral`, sliders `#4a90d9`), le vert (`.btn-open`,
   `#1976d2`)… Aucune hiérarchie claire « action primaire vs secondaire ».
   Le rouge sert à la fois d'accent **et** de couleur d'erreur (`--error`
   `#f44336` très proche) → ambiguïté sémantique.
3. **Échelle d'espacement absente** : valeurs en px ad hoc partout (12/16/20/24/28…).
   Pas de variables `--space-*`.
4. **Échelle typographique absente** : tailles en `rem`/`px` dispersées
   (`1.2rem`, `1.25rem`, `0.875rem`, `0.8rem`, `0.78rem`, `0.72rem`…) sans
   système. `font-size` de base 16px, une seule famille système.
5. **Rayons & ombres** semi-tokenisés (`--radius`, `--shadow-card`) mais
   beaucoup de `border-radius: 6/8/12px` en dur à côté.

### 2.2 Incohérences de composants

- **Boutons** : `.btn`, `.btn-secondary`, `.btn-neutral`, `.btn-open`,
  `.btn-pair`, `.btn-save`, `.btn-danger`, `.btn-small`, `.btn-icon`,
  `.btn-pagination`, `.btn-launch`, `.btn-stream-fs`, `.btn-stream-kbd`,
  `.stream-quit-btn`, `.view-close-btn` → **15 variantes** sans grammaire
  commune (certaines avec gradient, d'autres plates, tailles incohérentes).
- **Champs** : `.settings-input`, `.settings-select`, `.settings-slider`,
  `.login-input`, `.login-pin-input` → styles dupliqués et divergents
  (login utilise `#222240` en dur, settings utilise `--bg-primary`).
- **Selects natifs** en `appearance:auto` → rendu OS non maîtrisé, jure avec
  le reste.
- **Cartes** : `.host-card`, `.app-card`, `.settings-section` partagent le même
  motif (gradient + border + shadow) mais redéfini 3×.
- **Icônes incohérentes** : SVG dans le header/login, mais **emojis** ailleurs
  (`🖴` hosts vides, `📦` apps vides, `🎮` app sans jaquette, `⛶`/`⌨` boutons
  stream, `●` streaming). Rendu emoji variable selon OS/navigateur.

### 2.3 Ruptures d'expérience

- **`prompt()` natif** pour « Add Manually » (`HostListView` l.310) — boîte de
  dialogue système brute, en total décalage avec le design soigné du reste.
  *Comportement à préserver, mais l'habillage peut devenir un vrai dialogue.*
- **`confirm()` natif** pour la régénération de certificat (`AdminView` l.896).
- **Style inline dans le JS** : nombreux `style="..."` (Login, Admin, Settings)
  → impossible à thématiser proprement, fragilise la refonte.

### 2.4 Accessibilité / finitions

- Pas d'état **`:focus-visible`** cohérent (navigation clavier peu lisible).
- Pas de `prefers-reduced-motion` (animations spinner/pulse/toast permanentes).
- Pas de `prefers-color-scheme` ni de thème clair (dark imposé — acceptable,
  mais à acter comme choix).
- Contrastes `--text-secondary` `#a0a0b0` sur `--bg-secondary` parfois sous le
  seuil AA pour le petit texte (`0.72–0.78rem`).
- Cibles tactiles : certains boutons stream < 44px (cf. directives mobiles).

### 2.5 Ce qui fonctionne bien (à conserver)

- **Glassmorphism** du header et de l'overlay stats (`backdrop-filter: blur`)
  — moderne, à **généraliser** comme langage visuel.
- Gestion fine des **safe-areas iOS** (`env(safe-area-inset-*)`) — soignée.
- **`#main-content` seul conteneur scrollable** sous header fixe — bon choix.
- Loader de démarrage stream (arc conique masqué) — élégant, à garder/étendre.
- Le **wording** est clair et cohérent en anglais → bonne base pour l'i18n.

---

## 3. Proposition de refonte visuelle

> Objectif : UI **claire et moderne**, sans changer un seul libellé ni le
> positionnement des informations. On remplace le *langage visuel*, pas la
> structure.

### 3.1 Nouveau jeu de tokens (proposition)

Direction recommandée : **dark « slate » neutre + accent unique indigo/cyan**,
surfaces élevées par luminosité (pas par teinte), glassmorphism généralisé.

```css
:root {
  /* Surfaces — neutres froids, élévation par clarté croissante */
  --surface-0:#0b0e14;  --surface-1:#11161f;  --surface-2:#171d28;
  --surface-3:#1e2532;  --surface-glass:rgba(20,26,38,.6);

  /* Texte */
  --text-1:#eef2f8;  --text-2:#a7b0c0;  --text-3:#6b7588;

  /* Accent UNIQUE (primaire) + sémantiques distinctes */
  --accent:#5b8def;          /* indigo — action primaire */
  --accent-strong:#3f6fd6;   --accent-soft:rgba(91,141,239,.14);
  --success:#3ec98a;  --warning:#f0b429;  --danger:#ef5d64;  --info:#56b6e6;

  --border:rgba(255,255,255,.08);  --border-strong:rgba(255,255,255,.16);

  /* Échelle d'espacement (4px base) */
  --space-1:4px; --space-2:8px; --space-3:12px; --space-4:16px;
  --space-5:24px; --space-6:32px; --space-8:48px;

  /* Échelle typographique */
  --fs-xs:.75rem; --fs-sm:.875rem; --fs-md:1rem; --fs-lg:1.125rem;
  --fs-xl:1.375rem; --fs-2xl:1.75rem;
  --font-ui: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  --font-mono:'SF Mono','Cascadia Code','Consolas',monospace;

  /* Rayons & ombres */
  --radius-sm:8px; --radius-md:12px; --radius-lg:16px; --radius-pill:999px;
  --shadow-1:0 1px 2px rgba(0,0,0,.3);
  --shadow-2:0 4px 16px rgba(0,0,0,.35);
  --shadow-3:0 12px 40px rgba(0,0,0,.45);
}
```

> L'accent reste paramétrable : on peut proposer 2-3 thèmes d'accent
> (indigo / teal / violet) via une seule variable, sans retoucher les composants.

### 3.2 Grammaire de composants normalisée

Un seul système, mappé sur les classes existantes (on **garde les noms** quand
c'est possible, on **fusionne** les doublons) :

| Composant | Variantes | Remplace |
|---|---|---|
| `.btn` | `--primary` `--secondary` `--ghost` `--danger` + `--sm`/`--icon` | les 15 variantes actuelles |
| `.card` | `--interactive` | `.host-card`, `.app-card`, `.settings-section` |
| `.field` | `input` / `select` / `range` / `checkbox` unifiés | inputs login + settings |
| `.badge` | `--ready` `--locked` `--offline` | `.status-badge`, codec badge |
| `.dialog` | overlay + panneau | `.pairing-dialog` + nouveaux (Add host, confirm) |
| `.toast` | success/error/warning/info | identique, restylé |
| `.table` | — | `.sessions-table` |

Principes :
- **Boutons** : un seul gabarit (hauteur, rayon, typo), couleur par variante.
  Action primaire = accent indigo plein ; secondaire = surface + bordure ;
  ghost = transparent ; danger = rouge. Supprime les gradients multiples.
- **Selects** : `appearance:none` + chevron SVG custom → rendu identique partout.
- **Focus** : `:focus-visible { outline:2px solid var(--accent); outline-offset:2px }`
  global.
- **Iconographie** : remplacer les emojis par un petit set **SVG inline**
  cohérent (même glyphe, même emplacement = « même information visuelle »
  respectée). Lot d'icônes : serveur, app, manette, clavier, plein écran,
  check, lock, offline.
- **Mouvement** : encapsuler les animations dans `@media (prefers-reduced-motion:
  no-preference)`.

### 3.3 Application par écran (inchangé fonctionnellement)

- **Header** : conserver titre + version + 2 boutons icône ; restyler en glass,
  accent sur l'état actif (`.nav-active`).
- **Hosts** : cartes plus aérées, icône statut SVG, badge sémantique, hover
  subtil (élévation par ombre, pas par couleur rouge agressive).
- **Apps** : grille identique, jaquettes avec ratio préservé, fallback SVG
  manette au lieu de l'emoji.
- **Settings / Admin** : sections en `.card`, champs unifiés, sliders restylés
  (piste + thumb accent), tableau sessions lisible.
- **Login** : champs unifiés, logo conservé.
- **Stream overlay** : garder la structure (header discret, stats glass, loader,
  slide raccourcis, barre clavier tactile) ; harmoniser couleurs/typo avec les
  nouveaux tokens. L'accent jaune `#f0c040` du loader/stats peut rester (signal
  « live ») ou s'aligner sur `--warning`.

> **Garanties** : aucune chaîne modifiée, aucun élément déplacé, aucun flux
> changé. Seuls couleurs, rayons, ombres, espacements, typo et la forme des
> icônes évoluent.

---

## 4. Revue de l'architecture des fichiers

### 4.1 Problèmes détectés

1. **Doublons de modules** :
   - `js/api/BackendClient.js` **et** `js/utils/BackendClient.js` (lequel fait
     foi ? les vues importent `../api/BackendClient.js`). → `utils/` est
     probablement mort.
   - `js/ui/StreamView.js` (utilisé par `app.js`) **et** `js/stream/StreamView.js`
     (non référencé par `app.js`). → vérifier/supprimer le mort.
   - Dossiers `js/util/` **et** `js/utils/` coexistent → confusion.
   - Fichiers parasites à la racine repo : `find_streamview.bat`,
     `tmp_find_streamview.sh`, `find-stream-files.sh`, `temp_find.sh`,
     `temp-ls.sh`, `temp-ls.bat`, `git-diff-check.bat`. → nettoyage.
2. **CSS monolithique** : `style.css` (1500 l) mélange tokens, base, et 6 vues.
   Pas de séparation tokens / composants / vues.
3. **Style inline dans le JS** : empêche une thématisation centralisée.
4. **`StreamView.js` à 4722 lignes** : hors périmètre de cet audit (logique
   streaming, pas UI pure), mais c'est un point de dette à part.

### 4.2 Arborescence cible proposée (CSS + i18n)

```
frontend/css/
  tokens.css        :root — couleurs, espacements, typo, rayons, ombres
  base.css          reset, html/body, scrollbar, focus-visible, a11y
  components.css    .btn, .card, .field, .badge, .dialog, .toast, .table, .icon
  layout.css        header, footer, #main-content, safe-areas
  views/
    hosts.css  apps.css  settings.css  admin.css  login.css  pairing.css
  stream.css        (conservé, restylé sur les tokens)
```

`index.html` charge `tokens → base → layout → components → views/* → stream`.

```
frontend/i18n/
  index.js          init i18next, détection langue, helper t()
  locales/
    en.json  fr.json  (+ langues ajoutées via Tolgee)
```

> Le découpage CSS est **mécanique** (déplacement de règles), sans risque
> fonctionnel, et grandement facilité si on supprime d'abord le style inline.

---

## 5. Internationalisation (i18n)

### 5.1 Constat

**100 % des chaînes utilisateur sont codées en dur en anglais** dans les
templates JS et `index.html`. Aucune infrastructure i18n. Quelques chaînes sont
déjà en **français** dans `app.js` (toasts de fallback transport l.784-790) →
incohérence linguistique actuelle.

Inventaire des sources de chaînes (toutes à externaliser) :
- `index.html` : titre, version, tooltips boutons, footer.
- `HostListView` : « Hosts », « Add Manually », « Open », « Pair », « Remove »,
  « No hosts found », hint, prompt « Enter host IP… », toasts.
- `AppListView` : « Back to Hosts », « Launch », « No applications found »,
  « Loading applications… », erreurs.
- `SettingsView` : ~30 libellés (sections, labels, descriptions, options codec/
  résolution/FPS/algo, boutons).
- `AdminView` : ~40 chaînes (PIN, sessions, certificat, internet, ports, toasts,
  confirm).
- `LoginView` : titres, sous-titres, labels, erreurs, états boutons.
- `PairDialog` : titre, instruction, statuts.
- `StreamView` : « Stop »/« Stop Streaming », « Click to capture… », étapes du
  loader, libellés stats, raccourcis clavier, gestes tactiles, hint « Add to
  Home Screen », boutons.
- `Toast` : messages (passés par les appelants).

### 5.2 Stack retenue : **i18next** (moteur) + **Tolgee self-hosted** (gestion)

**Moteur — i18next** (ESM/CDN, vanilla) :
- Interpolation `t('host.added', { name })`, pluriels (`{{count}} attempt(s)` →
  vraie pluralisation), fallback de langue, détection
  (`navigator.language` + override `localStorage`).
- Helper unique exposé : `import { t, setLanguage } from './i18n/index.js'`.

**Pattern d'intégration (sans changer le wording) :**
- Dans les templates JS : remplacer le littéral par `t('clé')`. Ex.
  `<h2>Hosts</h2>` → `<h2>${t('hosts.title')}</h2>` avec
  `"hosts.title": "Hosts"` dans `en.json`.
- Dans `index.html` (statique) : attribut `data-i18n="app.title"` +
  une passe `applyTranslations(root)` au boot qui remplit `textContent`/`title`.
- Re-render des vues sur changement de langue (les vues ont déjà `render()` →
  il suffit de rappeler `render()`), ou simple `location.reload()` au switch
  (acceptable en v1).
- Les chaînes FR déjà présentes dans `app.js` rejoignent les dictionnaires.

**Gestion — Tolgee self-hosted** (interface web demandée) :
- Conteneur Tolgee (Docker) hébergé à côté du backend → **éditeur web** des
  clés et des langues, ajout de langue, in-context editing, mémoire de
  traduction, export/import **JSON compatible i18next**.
- Deux modes d'usage possibles :
  1. **Build-time** : Tolgee CLI `pull` → écrit `frontend/i18n/locales/*.json`
     versionnés en Git (simple, offline en prod).
  2. **Runtime/in-context** : SDK Tolgee + i18next backend pour éditer
     directement depuis l'app en dev. Recommandé : mode build-time pour la
     prod, in-context activé uniquement en dev (`MW_DEBUG`).
- Sélecteur de langue : un contrôle dans **Settings** (et/ou header), valeur
  persistée en `localStorage` (`mw-lang`), défaut = langue navigateur → fallback
  `en`.

### 5.3 Langues initiales

Bootstrap avec **`en`** (source) + **`fr`** (déjà partiellement présent).
Toute langue supplémentaire s'ajoute ensuite via l'éditeur Tolgee sans toucher
au code.

### 5.4 Effort i18n (indicatif, hors périmètre « ne pas coder »)

- Extraction des ~200 chaînes → `en.json` (mécanique, 1 passe par fichier).
- Câblage `t()` dans 8 vues + `index.html`.
- Init i18next + sélecteur de langue + `data-i18n` loader.
- Setup Tolgee (Docker) + pipeline `pull` JSON.
- Traduction `fr` (la moitié existe déjà).

---

## 6. Synthèse & ordre d'exécution recommandé

> Tout est **réalisable sans changer navigation / wording / comportement /
> informations visuelles**.

1. **Nettoyage architecture** (risque nul) : supprimer doublons
   (`utils/BackendClient`, `stream/StreamView` si morts), scripts `.bat/.sh`
   parasites ; vérifier les imports.
2. **Découpage CSS** : extraire `tokens.css` / `base.css` / `components.css` /
   `layout.css` / `views/*`, sans changer le rendu (refactor à iso-visuel).
3. **Sortir le style inline** du JS vers les classes.
4. **Refonte des tokens + composants** : appliquer le nouveau design system
   (couleurs, espacements, typo, focus, icônes SVG, dialogues custom pour
   `prompt()`/`confirm()`).
5. **i18n** : i18next + extraction des chaînes + `data-i18n` + sélecteur de
   langue + Tolgee self-hosted (Docker) avec pull JSON.

Décisions déjà actées : **i18next** (moteur) + **Tolgee self-hosted** (gestion).
Décisions ouvertes : teinte d'accent finale (indigo proposé), conserver ou non
l'accent jaune « live » du stream, périmètre du sélecteur de langue (Settings
seul vs header).
```
