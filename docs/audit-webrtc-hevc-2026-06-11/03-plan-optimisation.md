# Audit WebRTC HEVC — Livrable 3 : Plan d'optimisation priorisé

> **Session :** 2026-06-11-audit-webrtc-hevc
> **Statut :** Validé par code-reviewer (verdict : approuvé avec amendements, intégrés ci-dessous).
> **Règles :** drop accepté plutôt qu'accumulation ; artefact < 300 ms accepté ; AUCUNE régression PC/Mac/iOS ; pas de sur-ingénierie.
> **Invariant global (amendement reviewer) : une keyframe n'est JAMAIS droppée, par aucun mécanisme, à aucun étage.**

---

## OPT-0 — Prérequis (avant tout autre changement)

### OPT-0a — Corriger la documentation mensongère du code
- **Quoi :** Le commentaire WebRtcDataChannel.js l.47 décrit un zero-fill des P-frames qui n'existe pas ; les commentaires "no retransmits" (DataChannelRelay.cpp l.695, WebRtcDataChannel.js l.566/570 — dupliqué) contredisent `maxRetransmits=3`.
- **Pourquoi d'abord :** éviter de concevoir les fixes contre une doc fausse (le reviewer a montré que l'audit initial lui-même avait été induit en erreur par l.47).
- **Risque :** nul. **Gain :** fiabilité du travail suivant.

### OPT-0b — Coalescing IDR unique côté backend (cooldown)
- **Problème :** après les fixes P0, jusqu'à 6 sources demanderont des IDR (backend : backpressure, sticky awaiting-IDR, file Qt pleine ; frontend : stale, starvation, referenceValid). Sans convergence → flood de `LiRequestIdrFrame` vers Sunshine (régression déjà vécue : épisode "reorder buffer → IDR flood").
- **Quoi :** un point de convergence unique dans `DataChannelRelay` (toutes les demandes, internes et venues du frontend, passent par `requestIdrFrame()`), avec cooldown temporel de **250-500 ms** + relance automatique tant que l'état "awaiting IDR" est actif (généralisation du coalescing par épisode existant `m_BackpressureDropCount`).
- **Fichiers :** `backend/src/streaming/DataChannelRelay.h/.cpp` (état + timer), `MoonlightShim` inchangé.
- **Référence :** moonlight-common-c `idrFrameRequiredEvent` (coalescing idempotent) ; moonlightweb-stream `requestedIdr` (flag par état).
- **Tests :** compteur de requêtes IDR émises sous perte continue → ≤ 2-4/s. **Gain :** prérequis de B. **Risque :** faible.

---

## P0 — Critiques (causes racines des problèmes A et B)

### OPT-1 [P0, A] `maxRetransmits = 0` sur le DC vidéo, des deux côtés
- **Fichiers :** `DataChannelRelay.cpp:697-701` (backend) + `WebRtcDataChannel.js:574-579` (frontend). Canal negotiated id=0 : les deux configs doivent rester identiques.
- **Approche :** remplacer `maxRetransmits=3` par `0` (validé : 0 = aucune retransmission en libdatachannel et Chrome, distinct du champ absent = reliable). Conserver `unordered`.
- **Pourquoi :** sous perte, SCTP retransmet jusqu'à 3× des données périmées → gonflement de `bufferedAmount`, head-of-line de fait, frames fraîches retardées. Aucune référence (qt/xbox/web-stream) ne retransmet de la vidéo temps réel.
- **Impact :** élimine le buffer bloat induit par les retransmissions (A) ; davantage de frames perdues sous perte → compensé par la chaîne de récupération (OPT-3/4). **Risque :** faible ; valider sur réseau lossy (les pertes ne seront plus masquées par SCTP). **Effort :** 2 lignes + commentaires.

### OPT-2 [P0, A] Backpressure décodeur : consulter `decodeQueueSize` avant `decode()`
- **Fichier :** `frontend/js/ui/StreamView.js`, `decodeFrame()` (~l.1219-1330).
- **Approche :**
  1. Avant `decode()` : si `decoder.decodeQueueSize > seuil` ET frame = delta → drop + armer l'état "awaiting keyframe" (OPT-4) + demander IDR (via OPT-0b). **Jamais de drop de keyframe** sur ce critère.
  2. Seuil : **3-4** (amendement reviewer : 2-3 trop agressif pour la latence de pipeline du décodeur HW iOS ; éventuellement adaptatif).
  3. Critère de reset (pattern moonlightweb-stream) : si `decodeQueueSize × (1000/fps) > 200 ms` ET `decodeQueueSize > 2` → `decoder.reset()` + reconfiguration + demande d'IDR (obligatoire : un reset sans IDR laisserait le décodeur sans référence).
- **Pourquoi :** levier n°1 du problème A — la file interne du VideoDecoder est le buffer non borné dominant ; le frameQueue cap 3 existant agit après décodage, trop tard.
- **Références :** decodeUnitQueue=15 + overflow→flush+IDR (moonlight-qt) ; FrameQueue=5/HWM=3 drop à l'enqueue (xbox) ; reset si backlog>200 ms (web-stream).
- **Impact :** borne dure sur la latence de décodage — le cœur du fix Android. **Risque :** moyen — valider l'absence de micro-drops parasites sur iOS/Safari et l'absence de ghosting sur Chrome Windows HEVC. **Effort :** ~30 lignes.

### OPT-3 [P0, B] Sticky "awaiting IDR" côté backend
- **Fichier :** `backend/src/streaming/DataChannelRelay.h/.cpp` (~l.891-899 et ~l.1234-1252).
- **Approche :** flag `m_AwaitingIdr` armé sur **tous** les chemins de drop de delta : (1) backpressure (l.1234), (2) DC vidéo pas encore ouvert (l.891-899), (3) file Qt pleine (OPT-5). Tant que `m_AwaitingIdr` : toutes les deltas sont droppées, les keyframes passent (logique existante l.1266 réutilisée pour le reset du flag), la demande d'IDR est relancée par le cooldown OPT-0b.
- **Pourquoi :** actuellement le drop n'est pas sticky — la delta suivante repart dès que `bufferedAmount` redescend alors que sa référence n'a jamais été envoyée → P-frames orphelines → macro-blocks persistants. Cause backend directe du problème B.
- **Référence :** `waitingForIdrFrame` de moonlight-common-c (les P-frames sont jetées AVANT `reassembleFrame`).
- **Impact :** plus jamais de delta orpheline émise par le backend. **Risque :** faible — pendant l'attente d'IDR l'image gèle brièvement (< 300 ms avec OPT-0b), accepté par les règles. **Effort :** ~20 lignes.

### OPT-4 [P0, B] État `referenceValid` + détection de gap côté frontend
- **Fichiers :** `frontend/js/api/WebRtcDataChannel.js` (`_assembleFrame` l.864-879, callback onVideo) + `frontend/js/ui/StreamView.js`.
- **Approche :**
  1. `WebRtcDataChannel` signale au consommateur tout drop définitif de frame (frame incomplète, stale) — ex. callback `onFrameLoss(frameId, wasKeyframe)` — et demande un IDR immédiatement (routé backend, throttlé par OPT-0b) au lieu d'attendre les timers.
  2. `StreamView` maintient `_referenceValid` : passe à `false` sur (a) signal de perte, (b) discontinuité de `frameId` (les IDs sont déjà transmis ~l.981, inexploités), (c) drop local OPT-2, (d) erreur décodeur. Tant que `false` : les deltas sont droppées avant `decode()`, seule une keyframe restaure `_referenceValid=true`.
- **Pourquoi :** levier n°1 du problème B — le décodeur HW Android affiche les deltas orphelines **sans émettre d'erreur**, donc `_handleDecoderError` ne se déclenche jamais. Défense en profondeur volontairement redondante avec OPT-3 (le backend ne couvre pas les pertes SCTP pures post-backend) — redondance par conception, à documenter.
- **Impact :** suppression de la pixélisation persistante ; récupération bornée par le RTT + cooldown au lieu de "jusqu'au prochain changement visuel fort". **Risque :** faible-moyen — interaction avec le drop stale par `backendTs` (F2.4) à soigner ; valider sur Chrome Windows HEVC (historique ghosting). **Effort :** ~40 lignes réparties sur 2 fichiers.

### OPT-5 [P0, A] Borner la file de signaux Qt worker → main
- **Fichiers :** `backend/src/streaming/MoonlightShim.h/.cpp` (~l.384, `drSubmitDecodeUnit`) + `DataChannelRelay.cpp` (`onVideoFrame`, ~l.512).
- **Approche :** `std::atomic<int> m_PendingFrames` — incrément avant `emit` (worker), décrément en tête de `onVideoFrame` (main). Si `m_PendingFrames > 2-3` à l'emit : dropper la **delta** côté worker (avant la file Qt) + armer `m_AwaitingIdr` (OPT-3). **Les keyframes ne sont jamais droppées ici** (perte de SPS/PPS → decoder null, régression connue) ; borne 2-3, pas 1 (tolérer le jitter normal du main thread).
- **Pourquoi :** la file d'événements Qt est non bornée et invisible du watermark SCTP — main thread bloqué 100 ms = ~6 frames empilées puis toutes envoyées.
- **Référence :** decodeUnitQueue=15 (moonlight-common-c), FrameQueue=5 (xbox).
- **Impact :** supprime le 3e buffer caché du problème A. **Risque :** faible (drop cohérent avec OPT-3). **Effort :** ~15 lignes.

---

## P1 — Importants

### OPT-6 [P1, B] Relance et throttle IDR backend — fusionné dans OPT-0b
Couvre F9 : plus de "demande unique par épisode" — relance toutes les 250-500 ms tant que `m_AwaitingIdr`.

### OPT-7 [P1, A] Timestamps réels sur les `EncodedVideoChunk`
- **Fichier :** `StreamView.js` ~l.1252. Remplacer `frameCount × 16667` (60 FPS fixe) par une base dérivée de `backendTs` (déjà transmis, inutilisé). Timestamps strictement croissants à garantir. **Impact :** scheduling interne du décodeur correct sous FPS variable. **Risque :** faible. **Effort :** ~5 lignes.

### OPT-8 [P1, A] `optimizeForLatency: true` sur les configs HEVC Annex B
- **Fichier :** `StreamView.js` ~l.996-1002 (chemin Android). Déjà présent sur d'autres chemins de config. **Risque :** faible — vérifier que Safari iOS ne rejette pas la clé (précédent : `colorSpace` rejeté par Safari → l'ajouter de manière tolérante, comme le fix iOS colorSpace). **Effort :** quelques lignes + fallback.

### OPT-9 [P1, A] Éliminer les coûts fixes par frame (frontend)
- `getImageData(0,0,1,1)` readback GPU synchrone par frame HEVC (~l.1666) → borner aux N premières frames post-configure (le diagnostic green-image n'a de valeur qu'au démarrage).
- console.log per-frame ×3 chemins (~l.702-705, ~l.1313-1315, ~l.1696) → supprimer ou gater derrière un flag debug.
- Code debug TEST-B/TEST-H (3 hashes FNV-1a full-frame, 20 premières frames, WebRtcDataChannel.js) → supprimer.
- **Impact :** réduction directe de la charge main thread JS, particulièrement sensible sur Android. **Risque :** quasi nul.

### OPT-10 [P1, A] Tuning backend
- `kHighWatermark` 128 KB → **48-64 KB** (DataChannelRelay.h:102), idéalement dérivé du bitrate effectif des settings (F19) : `watermark ≈ bitrate × 25 ms`.
- `kMaxPayloadSize` 64000 → **~16000** (h:97) : en unreliable, 1 fragment SCTP perdu invalide le message entier. ⚠ Historique : 14000→32000 avait corrigé un freeze Android — retester ce scénario spécifiquement après le changement (le contexte a changé : le freeze était lié à un autre étage).
- `scanNals` par frame pour flag de test (~l.908-918) → exécuter uniquement en mode test.
- `fflush(stderr)` dans `drSubmitDecodeUnit` (~l.348-374) → retirer du chemin chaud.
- **Risque :** moyen sur kMaxPayloadSize (régression freeze à surveiller), faible sur le reste.

### OPT-11 [P1, A] DC audio : `unordered` + `maxPacketLifeTime` court
- **Fichiers :** `DataChannelRelay.cpp:719-725` + miroir frontend. L'audio ordered+reliable partage l'association SCTP et entre en concurrence avec la vidéo sous congestion. PCM brut : la perte d'un paquet = clic bref, préférable au retard. **Risque :** moyen (qualité audio sous perte) — à tester séparément ; rollback simple.

### OPT-12 [P1] `isAndroid()` dans BrowserDetect.js
- Détection Android explicite, pour le diagnostic (stats par plateforme) et d'éventuels gates futurs. **Les fixes P0 restent universels** (corrections de justesse, pas des workarounds Android). **Effort :** trivial.

---

## P2 — Nice-to-have

| ID | Description | Fichiers | Note |
|---|---|---|---|
| OPT-13 | `toAvcc` préallocation Uint8Array + `set()` ; memoization de `splitNals` (1 seul split par frame) | Mp4Muxer.js:442-490, StreamView.js | 2 copies full-frame éliminées sur ~4 |
| OPT-14 | Purge des deltas en attente à l'arrivée d'une keyframe (pattern web-stream `clear_queue`) | DataChannelRelay.cpp | Complément d'OPT-3 |
| OPT-15 | Garde-fous : N drops consécutifs → IDR forcé (~120 chez moonlight-qt) ; reset VideoDecoder après N erreurs | StreamView.js | Filets de sécurité |
| OPT-16 | Backpressure du fallback WS (`bytesToWrite()` + sticky drop) | SignalingServer.cpp:488-570 | Mode legacy |
| OPT-17 | Thread relay dédié backend (fragmentation + send hors main thread Qt) | DataChannelRelay | Évolution propre, pas un préalable — OPT-5 réduit déjà la pression |
| OPT-18 | Exploration RFI / intra-refresh (`kMaxRefFrames` jamais transmis ; `CAPABILITY_INTRA_REFRESH`) | StreamConfig, MoonlightShim | Récupération moins coûteuse qu'un IDR complet — étude préalable |
| OPT-19 | Web Worker réassemblage / OffscreenCanvas | frontend | REPORTÉ : bénéfice réel seulement après OPT-2 ; chantier lourd vs gain incertain |
| OPT-20 | Divers : `_starvationRequested` réarmé par frame assemblée (pas par chunk) ; borne sur la Map de réassemblage ; `flushPendingFrames` borné ; reset des compteurs static entre sessions ; `CLEANUP_INTERVAL_MS` 500→100 ms | WebRtcDataChannel.js, DataChannelRelay.cpp | Hygiène |

---

## Matrice gain / risque

| Optimisation | Problème | Gain | Risque régression | Effort |
|---|---|---|---|---|
| OPT-0b coalescing IDR | B (prérequis) | Élevé | Faible | XS |
| OPT-1 maxRetransmits=0 | A | Élevé (réseau réel) | Faible | XS |
| OPT-2 decodeQueueSize | **A (n°1)** | Très élevé | Moyen (iOS, HEVC Win) | S |
| OPT-3 sticky awaiting-IDR | **B (n°1 backend)** | Très élevé | Faible | S |
| OPT-4 referenceValid | **B (n°1 frontend)** | Très élevé | Faible-moyen | M |
| OPT-5 borne file Qt | A | Élevé | Faible | S |
| OPT-7/8 timestamps + latency | A | Moyen | Faible | XS |
| OPT-9 coûts par frame | A | Moyen (Android) | Quasi nul | S |
| OPT-10 tuning | A | Moyen | Moyen (kMaxPayloadSize) | XS |
| OPT-11 audio unordered | A | Faible-moyen | Moyen (audio) | XS |
