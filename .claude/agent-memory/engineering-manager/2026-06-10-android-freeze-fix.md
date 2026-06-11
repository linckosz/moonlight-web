---
name: android-freeze-fix
description: Fix freeze WebRTC DC Android — keyframes bloquées par stale check en dirty mode, kMaxPayloadSize 14000→32000, auto-exit dirty mode 5s
metadata:
  type: project
---

# Android Freeze Fix — 2026-06-10

## Root cause
Triple verrouillage mortel sur Android WebRTC DC (ordered=false, maxRetransmits=3) :

1. **Stale check dans handleVideoFrame()** (ligne ~2109) : la condition `this.decoderConfigured || !isKeyframe` est TRUE pour les keyframes quand `decoderConfigured=true`. Les keyframes arrivees tardivement (SCTP reordering) ont un backend timestamp stale → DROPPEES. Dirty mode ne recoit jamais de keyframe → ne sort jamais → freeze permanent.

2. **kMaxPayloadSize=14000 trop petit** : keyframe 120KB → 9 chunks SCTP. Plus de chunks = plus de reordering unordered = plus de gaps frameId > 5 = dirty mode frequent.

3. **Pas d'auto-exit dirty mode** : une fois entre, permanent si les keyframes ne passent pas le stale check.

## Fix (3 corrections)

A) **DataChannelRelay.h** : kMaxPayloadSize 14000 → 32000
   - 120KB → 4 chunks au lieu de 9
   - Compromis entre 14000 (trop de reordering) et 64000 (trop gros pour SCTP)

B) **StreamView.js handleVideoFrame** : exemption keyframes en dirty mode dans le stale check
   ```js
   if (this._decoderDirty && isKeyframe) {
       // Let keyframe through in dirty mode to recover
   } else if (this.decoderConfigured || !isKeyframe) {
       this.stats.dropped++;
       return;
   }
   ```

C) **StreamView.js _processVideoFrame** : auto-exit dirty mode apres 5s
   - Safety net si les keyframes n'arrivent toujours pas
   - Re-entre si corruption reelle (decoder error handler)

## Fichiers modifies
- `backend/src/streaming/DataChannelRelay.h` line 97
- `frontend/js/ui/StreamView.js` : constructor (l.327), _enterDirtyMode (l.2070), handleVideoFrame (l.2121-2126), _processVideoFrame (l.2273-2291)

## Verification
- Le dirty mode existe toujours pour les vrais cas de corruption
- Seule exemption : keyframes stale en dirty mode
- Auto-exit 5s (decoder error handler re-entre dirty mode si corruption reelle)
- kMaxPayloadSize=32000 < 64KB SCTP message max
