---
name: audit-webrtc-hevc-2026-06-11
description: Audit pipeline HEVC WebRTC terminé — causes racines Android identifiées, plan OPT-0..20 validé, implémentation pas encore commencée
metadata:
  type: project
---

L'audit complet du pipeline vidéo HEVC sur WebRTC DataChannel est livré dans `docs/audit-webrtc-hevc-2026-06-11/` (4 livrables + résumé de session). Validé par code-reviewer, aucun code modifié.

**Causes racines** : (A) latence = 3 buffers non bornés (file Qt worker→main, SCTP gonflé par `maxRetransmits=3`, file interne VideoDecoder — `decodeQueueSize` jamais consulté) ; (B) pixélisation = aucun drop des deltas orphelines après perte (drop non-sticky backend + drop silencieux frontend), le décodeur HW Android ne signale pas les frames corrompues. Fait vérifié important : le zero-fill des P-frames décrit dans le commentaire WebRtcDataChannel.js l.47 N'EXISTE PAS dans le code.

**Why:** le stream fonctionne partout sauf Android (latence accumulée + macro-blocks persistants) ; tests réseau dégradé pas encore faits.

**How to apply:** toute implémentation doit suivre `04-plan-implementation.md` : étape 0 (coalescing IDR backend, prérequis anti-flood) avant tout ; étapes 2 (problème B) avant 3 (problème A) ; keyframes jamais droppées ; tester à chaque étape le trio à risque Chrome Windows HEVC / Safari iOS / réseau lossy mobile.
