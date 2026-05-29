# Task: Read StreamView.js — Reorder Buffer Analysis

Lis le fichier `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js` et repère tous les endroits qui concernent le reorder buffer.

Cherche spécifiquement:
1. Les variables `_expectedFrameId`, `_firstFrameIdSeen`, `_reorderBuffer`, `_gapStartTime`, `_gapTimeoutMs`, `_reorderGapSkips`
2. Les méthodes `_skipReorderGap()`, `_drainReorderBuffer()`
3. La logique de réordonnancement dans `handleVideoFrame()`
4. Le check de gap timeout dans `loop()`
5. Les appels à `_reorderBuffer.clear()` dans `quit()` et `cleanup()`
6. Les stats reorderBuf/gapStart/gapSkips/expFrameId dans le DIAG

Pour chaque occurrence, note la ligne exacte et le contexte (5 lignes avant/après).

En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-05-29-reorder-buffer-remove/Resume-2026-05-29.md`.
