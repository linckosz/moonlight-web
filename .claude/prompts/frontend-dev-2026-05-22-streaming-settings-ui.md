Agent frontend-dev, session ID: `2026-05-22-streaming-settings-ui-adjustments`

Tu dois appliquer 3 ajustements UI sur la modale "Streaming Settings" (SettingsView).

## Fichiers à modifier

- `d:\Code\moonlight-web-deepseek\frontend\js\ui\SettingsView.js`
- `d:\Code\moonlight-web-deepseek\frontend\css\style.css`

## Changement 1 — Tooltips => texte sous le label

Actuellement, chaque option de settings a un `title` (tooltip au survol). Remplace les tooltips par une courte phrase descriptive directement sous le `<label>` de chaque option.

Dans `SettingsView.js`, pour chaque champ de la modale de streaming :

- Bitrate : au lieu d'un tooltip, ajoute un `<span class="setting-desc">` sous le label avec un texte comme "Target bitrate in kbps for video encoding"
- Resolution : "Select the streaming resolution (width x height)"
- FPS : "Frames per second — higher is smoother but more demanding"
- Codec : "Video codec — some codecs require hardware support"
- Gaming mode : "Enables low-latency optimizations for gaming"

Ces `<span>` doivent être insérés juste après le `<label>` de chaque champ, dans le template HTML généré par `renderSettings()`.

Supprime les attributs `title` sur les `<select>` correspondants (si présents) — on ne veut plus les tooltips.

## Changement 2 — Labels complets dans le dropdown Video Codec

Les options du `<select id="codec-select">` devaient avant avoir des labels descriptifs comme "H.264 (widest compatibility)", "HEVC (H.265, efficient)", "AV1 (open, modern)".

Remets ces labels complets au lieu des noms courts actuels. Les valeurs (`value`) restent inchangées : `h264`, `h265`, `av1`.

## Changement 3 — Espacement vertical

Dans `style.css`, les champs de la modale sont trop collés. Ajoute du margin-bottom entre les `.setting-row` (ou l'équivalent qui wrapper chaque champ) dans la modale streaming.

Ajoute une règle CSS :

```css
#settings-modal .setting-row {
    margin-bottom: 20px;
}
```

Si la classe utilisée est différente (ex: `.settings-field`, `.form-group`, `.streaming-option`), adapte à la structure réelle. Le but est d'avoir ~20px entre chaque champ pour les aérer.

## Rendu

Lis d'abord les fichiers concernés pour comprendre la structure actuelle, puis applique les 3 changements.

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-05-22-streaming-settings-ui-adjustments/Resume-2026-05-22.md`. Inclus : fichiers modifiés, changements apportés, lignes exactes modifiées.
