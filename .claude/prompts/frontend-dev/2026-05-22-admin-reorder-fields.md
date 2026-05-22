# Tâche : Réordonnancer HTTPS Port + Grouper Save & Reload

## Fichier cible
`d:\Code\moonlight-web-deepseek\frontend\js\ui\AdminView.js`

## Changement 1 — Réordonner "HTTPS Port" en dernier

Dans la section "Server Configuration" du renderServerConfig(), les champs sont actuellement dans un ordre quelconque. Il faut que **Transport Mode** apparaisse avant **HTTPS Port**.

L'ordre final doit être dans le cadre "Server Configuration" :
1. Listening Port (inchangé)
2. Localhost Only (inchangé) 
3. Transport Mode (déplacé avant HTTPS Port)
4. HTTPS Port (déplacé après Transport Mode)
5. DuckDNS (inchangé, déjà après)

## Changement 2 — Grouper "Save & Reload" avec HTTPS Port

Actuellement le bouton "Save & Reload" est un élément indépendant. Il doit être **visuellement groupé/attaché** au champ HTTPS Port.

Approche :
- Déplacer le bouton "Save & Reload" à l'intérieur du container HTML du champ HTTPS Port
- Le bouton doit apparaître juste après l'input du HTTPS Port, dans le même `.config-field`
- Tu peux utiliser un style inline `margin-top: 8px` ou une classe CSS pour espacer le bouton du champ

## Comportement (inchangé)

- Le bouton "Save & Reload" continue d'appeler `this.saveServerConfig()` puis `this.restartServer()`
- Le champ HTTPS Port continue d'utiliser `this.settings.httpsPort` et d'appeler `this.onConfigChange()`
- Le Transport Mode continue d'utiliser `this.settings.transportMode`

## Instructions

1. Lis d'abord le fichier pour trouver la section renderServerConfig()
2. Identifie les blocs HTML pour HTTPS Port, Transport Mode, et Save & Reload
3. Déplace Transport Mode avant HTTPS Port dans l'ordre du DOM
4. Déplace le bouton Save & Reload à l'intérieur du container du HTTPS Port
5. Vérifie que le rendu est correct

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-05-22-admin-reorder-fields/Resume-2026-05-22.md`.
