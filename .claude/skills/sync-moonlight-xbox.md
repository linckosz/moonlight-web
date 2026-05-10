---
name: sync-moonlight-xbox
description: Référence croisée moonlight-xbox — lit un composant dans moonlight-xbox et explique son fonctionnement, compare avec moonlight-web
---

# Skill — Sync Moonlight-Xbox

Ce skill consulte l'expert moonlight-xbox pour comprendre comment un composant est implémenté dans le client Xbox, puis compare avec l'implémentation moonlight-web.

## Procédure

### 1. Identifier le composant

Domaines où moonlight-xbox est particulièrement pertinent :
- `DirectX video rendering` → `Streaming/VideoRenderer.h/.cpp`
- `Shader-based color conversion` → `Streaming/ShaderStructures.h`, `Assets/Shader/`
- `Frame pacing / VSync` → `Streaming/Pacer.h/.cpp`, `Streaming/FrameCadence.h/.cpp`
- `Frame queue management` → `Streaming/FrameQueue.h/.cpp`
- `FFmpeg hardware decoding` → `Streaming/FFmpegDecoder.h/.cpp`
- `Audio rendering (WASAPI/XAudio2)` → `Streaming/AudioPlayer.h/.cpp`
- `Stats overlay rendering` → `Streaming/StatsRenderer.h/.cpp`
- `UWP app lifecycle` → `App.xaml.cpp`, `moonlight_xbox_dxMain.h/.cpp`

### 2. Appeler l'expert

Utiliser l'agent `expert-moonlight-xbox` avec un prompt précis :
```
Explique comment fonctionne [composant] dans moonlight-xbox.
Fichier(s) à regarder : [chemins]
Je veux comprendre : [question spécifique]
Contexte : je travaille sur l'équivalent web (WebGL/Canvas/WebCodecs)
```

### 3. Si demandé, comparer avec moonlight-web

Lire le(s) fichier(s) équivalent(s) dans moonlight-web et noter :
- Ce qui est transposable directement (ex: logique de pacing)
- Ce qui nécessite une adaptation (DirectX → WebGL)
- Ce qui n'a pas d'équivalent web (ex: accès direct aux textures GPU)
- Les optimisations pertinentes (ex: double/triple buffering)

### 4. Formater le rapport

## Format de rapport

```
## Sync moonlight-xbox — [Composant]

### Source moonlight-xbox
- Fichier(s) : [chemins]
- Résumé : [2-3 phrases sur le fonctionnement]

### Équivalent moonlight-web
- Fichier(s) : [chemins]
- Statut : ✅ Implémenté / ⚠️ Partiel / ❌ Manquant

### Transposition Web
| Concept Xbox | Équivalent Web | Faisabilité |
|-------------|----------------|-------------|
| [concept DirectX] | [API web équivalente] | ✅/⚠️/❌ |

### Recommandations
- [action suggérée ou "Pas d'action nécessaire"]
```

## Notes

- moonlight-xbox est particulièrement utile pour les sujets de rendu vidéo et shaders
- Les concepts DirectX se transposent souvent bien en WebGL
- Le pacer VSync DirectX peut inspirer l'utilisation de `requestAnimationFrame` + timestamps
- Toujours passer par l'agent `expert-moonlight-xbox` pour les explications détaillées
