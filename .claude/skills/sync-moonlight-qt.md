---
name: sync-moonlight-qt
description: Référence croisée moonlight-qt — lit un composant dans moonlight-qt et explique son fonctionnement, compare avec moonlight-web
---

# Skill — Sync Moonlight-QT

Ce skill consulte l'expert moonlight-qt pour comprendre comment un composant est implémenté dans le client de référence, puis compare avec l'implémentation moonlight-web.

## Procédure

### 1. Identifier le composant

L'utilisateur doit spécifier ce qu'il veut comprendre, par exemple :
- `keyboard mapping` → `app/streaming/input/keyboard.cpp`
- `RTSP handshake` → `moonlight-common-c/src/Connection.c`
- `video decoding pipeline` → `app/streaming/video/ffmpeg.h/.cpp` + `app/streaming/video/decoder.h`
- `ENet control stream` → `moonlight-common-c/src/ControlStream.c`
- `pairing protocol` → `app/backend/nvpairingmanager.h/.cpp`
- `mDNS discovery` → `app/backend/computerseeker.h/.cpp`

### 2. Appeler l'expert

Utiliser l'agent `expert-moonlight-qt` avec un prompt précis :
```
Explique comment fonctionne [composant] dans moonlight-qt.
Fichier(s) à regarder : [chemins]
Je veux comprendre : [question spécifique]
```

### 3. Si demandé, comparer avec moonlight-web

Lire le(s) fichier(s) équivalent(s) dans moonlight-web et noter :
- Ce qui est identique
- Ce qui est adapté différemment (contrainte web)
- Ce qui est manquant (oubli potentiel)
- Ce qui est simplifié (MVP vs production)

### 4. Formater le rapport

## Format de rapport

```
## Sync moonlight-qt — [Composant]

### Source moonlight-qt
- Fichier(s) : [chemins]
- Résumé : [2-3 phrases sur le fonctionnement]

### Équivalent moonlight-web
- Fichier(s) : [chemins]
- Statut : ✅ Implémenté / ⚠️ Partiel / ❌ Manquant

### Différences
| Aspect | moonlight-qt | moonlight-web | Raison |
|--------|-------------|---------------|--------|
| [aspect] | [description] | [description] | [pourquoi] |

### Recommandations
- [action suggérée ou "Pas d'action nécessaire"]
```

## Notes

- Toujours passer par l'agent `expert-moonlight-qt` pour les explications détaillées
- Ne pas lire directement moonlight-qt sans passer par l'expert (il a le contexte complet)
- Ce skill est souvent utilisé avant ou pendant l'implémentation d'une nouvelle fonctionnalité
