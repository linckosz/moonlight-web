# Audit WebRTC HEVC — Résumé de session (Engineering Manager)

> **Session :** 2026-06-11-audit-webrtc-hevc
> **Mission :** docs/prompt-engineering-manager-webrtc-hevc-audit.md — audit complet du pipeline vidéo HEVC sur WebRTC DataChannel, problèmes Android (A) latence accumulée et (B) pixélisation persistante. Aucun code modifié (audit + plans uniquement).

## Sous-agents mobilisés (tous Opus)

| Agent | Apport principal |
|---|---|
| expert-moonlight-qt | Pacer keep-newest (MAX_QUEUED_FRAMES=3), decodeUnitQueue=15 overflow→flush+IDR, `waitingForIdrFrame` (deltas droppées AVANT le décodeur), coalescing IDR idempotent, anti "congestion collapse" (IDR demandé après la 1re frame complète post-perte) |
| expert-moonlight-xbox | FrameQueue=5/HWM=3, drop à l'enqueue (alternance newest/oldest), IDR jamais droppé (évince l'oldest), catch-up rendu, zero-copy GPU, recommandation explicite d'un état "waiting for IDR" pour Android |
| expert-moonlight-web-stream | `max_retransmits=0`, file d'envoi=3 avec deltas droppables, reset décodeur si backlog>200 ms + IDR, une seule VideoFrame vivante au rendu, flag `requestedIdr` par état |
| backend-dev | 20 findings (F1-F20) ; P0 : F5 drop non-sticky, F6 maxRetransmits=3, F1 file Qt non bornée |
| frontend-dev | P0 : decodeQueueSize jamais consulté, aucun état referenceValid, recovery dépendant d'une erreur jamais émise par le décodeur Android |
| code-reviewer (2 passes) | Verdict : approuvé avec amendements. Corrections factuelles : le zero-fill des P-frames N'EXISTE PAS (commentaire l.47 obsolète — frames incomplètes droppées silencieusement l.864-879) ; configs DC vidéo cohérentes des deux côtés (negotiated id=0, maxRetransmits=3) mais commentaires mensongers. Amendements : coalescing IDR backend obligatoire en prérequis, keyframes exclues de tout drop, seuil decodeQueueSize 3-4, trio de test à risque (Chrome Windows HEVC, Safari iOS, réseau lossy mobile) |

## Causes racines retenues

- **Problème A** — trois buffers non bornés/mal bornés en cascade : file de signaux Qt worker→main (invisible du watermark), buffer SCTP gonflé par `maxRetransmits=3`, et surtout la file interne du VideoDecoder (`decodeQueueSize` jamais consulté).
- **Problème B** — aucune des deux extrémités ne droppe les deltas après une perte de référence : drop non-sticky backend + drop silencieux non signalé frontend → deltas orphelines décodées sans erreur par le décodeur HW Android ; récupération lente (timers 200/500 ms) sans relance d'IDR.

## Livrables

1. `01-cartographie-pipeline.md` — pipeline frame-par-frame, threads, buffers, coûts, écarts vs 3 références
2. `02-rapport-audit.md` — findings par composant (backend F1-F20, frontend F1.x-F4.x), copies mémoire, résilience réseau
3. `03-plan-optimisation.md` — OPT-0 (prérequis) à OPT-20, matrice gain/risque
4. `04-plan-implementation.md` — 6 étapes séquencées avec dépendances, protocole de test, matrice plateformes

## Suite recommandée

Étapes 0+1 (XS, quasi sans risque) immédiatement, puis étapes 2 (problème B) et 3 (problème A) avec revue code-reviewer à chaque étape et tests sur le trio à risque.

## Leçons d'orchestration

- Les agents experts en mission large épuisent leur budget avant de rendre leur rapport : missions bornées (≤5 questions, fichiers indiqués, "écris ton résultat avant le message final").
- Les sous-agents et l'EM n'ont pas la permission d'écrire hors de `docs/` dans ce mode — archiver les résultats dans le dossier d'audit.
