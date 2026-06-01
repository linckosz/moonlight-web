---
name: HEVC x4 Stretch Chrome Mac Fix
description: Fix 4x horizontal stretch of HEVC NV12 frames on Chrome macOS/iOS by using drawImage(VideoFrame) instead of createImageBitmap
metadata:
  type: project
---

**Probleme:** Les frames HEVC NV12 (1920x1080) sont etirees x4 en largeur sur Chrome macOS et iOS. H.264 OK, Safari OK, Windows Chrome OK (apres le fix green-tint existant).

**Cause racine:** `createImageBitmap(VideoFrame)` sur Chrome macOS/iOS pour HEVC NV12 retourne un bitmap de largeur 480px au lieu de 1920px. Le ratio exact x4 (1920/4=480) indique un stride mal interprete dans le pipeline de decodage hardware HEVC de Chrome (VideoToolbox → NV12). H.264 utilise un chemin de decodage different sans ce bug.

**Why:** Chromium sur macOS utilise le decodeur hardware HEVC de VideoToolbox qui produit des surfaces NV12 avec un stride interne que `createImageBitmap` interprete mal comme etant la largeur/4. Le meme bug affecte `frame.copyTo({format:'RGBA'})`. En revanche, `ctx.drawImage(VideoFrame`) emprunte le pipeline GPU compositeur (Metal) qui gere correctement ce stride.

**How to apply:** La detection `isChromeNonWin` (UserAgent Chrome, ni Edge, ni Opera, non-Windows) combinee avec `CODEC_HEVC && format NV12` active la permutation de l'ordre de rendu: `drawImage(VideoFrame)` en premier, `createImageBitmap` en fallback.

**Fichier modifie:** `frontend/js/ui/StreamView.js` — fonction `_drawFrameWithBitmap()`

**Commit/PR:** Non merge — branche master, modification directe.
