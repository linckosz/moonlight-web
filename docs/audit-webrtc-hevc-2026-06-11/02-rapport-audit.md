# Audit WebRTC HEVC — Livrable 2 : Rapport d'audit par composant

> **Session :** 2026-06-11-audit-webrtc-hevc
> **Problèmes cibles :** (A) accumulation de latence / buffer bloat sur Android — (B) pixélisation blocky persistante après perte de référence.
> Chaque finding : localisation, condition de déclenchement, impact, lien A/B, sévérité.

---

## 1. Lecture d'ensemble — les causes racines

### Problème A — Latence accumulée (buffer bloat)

Trois buffers non bornés ou mal bornés s'additionnent le long du pipeline, chacun invisible des autres :

1. **File d'événements Qt** (worker → main, backend) — non bornée, en amont du watermark SCTP (F1).
2. **Buffer SCTP** — gonflé par `maxRetransmits=3` qui retransmet des données périmées sous perte (F6/F1.1), watermark 128 KB trop permissif (~52 ms à 20 Mbps) (F7).
3. **File interne du VideoDecoder** (frontend) — `decodeQueueSize` jamais consulté, `decode()` empile sans limite sur le décodeur HW Android lent (F2.1). **C'est le mécanisme dominant du problème A.**

Le seul drop "keep-newest" existant (frameQueue cap 3) est situé APRÈS le décodage : il borne le rendu mais pas la latence de décodage.

### Problème B — Pixélisation persistante

La chaîne de récupération après perte de référence est trouée aux deux extrémités :

1. **Backend** : le drop de delta sous backpressure n'est pas sticky — la delta suivante repart dès que le buffer redescend, alors que sa référence n'a jamais été envoyée (F5).
2. **Frontend** : toute frame incomplète (I ou P) est **droppée silencieusement** dans `_assembleFrame` (l.864-879) sans signalement au consommateur ni requête IDR immédiate — NB : le commentaire l.47 décrit un zero-fill des P-frames qui n'existe pas dans le code (validation code-reviewer). Aucune détection de gap de frameId ; aucun état "référence perdue → drop des deltas jusqu'à l'IDR" (F2.3, F1.3) : les deltas suivantes, qui référencent la frame jamais reçue, sont soumises au décodeur. Le décodeur HW Android les décode **sans émettre d'erreur**, donc le recovery existant (`_handleDecoderError`) ne se déclenche jamais (F2.10).
3. **Récupération lente et fragile** : pas d'IDR immédiat à la perte (timers 200/500 ms, F1.2), pas de relance si l'IDR n'arrive pas (F9).

Toutes les implémentations de référence (moonlight-qt `waitingForIdrFrame`, moonlightweb-stream `needsKeyFrame`) droppent les deltas AVANT le décodeur tant que la récupération n'est pas arrivée. C'est le mécanisme manquant central.

---

## 2. Backend — findings détaillés

### 2.1 MoonlightShim.h/.cpp

| ID | Sév. | Lien | Finding |
|---|---|---|---|
| F1 | **P0** | A | `drSubmitDecodeUnit` (~l.384) émet `videoFrameReady` en QueuedConnection vers le main thread (connexion DataChannelRelay.cpp:~512). La file d'événements Qt est **non bornée** et invisible du watermark SCTP. Main thread bloqué 100 ms → ~6 frames empilées puis toutes envoyées → latence permanente. Aucun compteur de frames en vol. |
| F3 | P1 | A | `fflush(stderr)` dans le chemin chaud de `drSubmitDecodeUnit` (~l.348-374) — appel bloquant sur le thread réseau DIRECT_SUBMIT de moonlight-common-c. |
| F2 | P2 | — | Copie n°1 : concaténation des LENTRY en QByteArray. Inévitable pour traverser la boundary de thread ; acceptable. |

### 2.2 DataChannelRelay.h/.cpp

| ID | Sév. | Lien | Finding |
|---|---|---|---|
| F5 | **P0** | **B** | (~l.1234-1252) Drop de delta sous backpressure **non sticky** : on droppe la delta courante, mais dès que `bufferedAmount` redescend la suivante est envoyée alors que sa référence n'a jamais été transmise → P-frames orphelines décodées par le navigateur → macro-blocks persistants. Aucun chemin de drop (file Qt, DC pas ouvert ~l.891-899, backpressure) ne pose de flag "awaiting IDR". |
| F6 | **P0** | **A** | (~l.697-701) DC vidéo `unordered=true` mais **`maxRetransmits=3`** alors que le commentaire dit "no retransmits". Sous perte : retransmissions de données périmées, gonflement du buffer SCTP, head-of-line de fait. Correction d'une ligne (les deux côtés du canal negotiated doivent correspondre — voir F1.1 frontend). |
| F9 | P1 | B | (~l.1240-1244) IDR demandé uniquement au 1er drop d'un épisode (`m_BackpressureDropCount==0`, reset à chaque keyframe). Si la keyframe n'arrive jamais (perdue, backpressure), **plus aucune demande** → blocky indéfini. Aucun throttle temporel. |
| F7 | P1 | A | `kHighWatermark = 128 KB` (h:102) ≈ 52 ms de vidéo à 20 Mbps — trop permissif. Devrait être 48-64 KB, idéalement dérivé du bitrate effectif (F19). |
| F13 | P1 | A+B | `kMaxPayloadSize = 64000` (h:97) contredit son commentaire "under SCTP 16KB limit". En unreliable, la perte d'un seul fragment SCTP invalide le message de 64 Ko entier (≈ frame complète). Revenir vers ~16000. ⚠ Historique : un fix freeze Android avait monté 14000→32000 — à retester. |
| F10 | P1 | A | (~l.719-725) DC audio ordered+reliable, partage l'association SCTP avec la vidéo → couplage de congestion. Candidat unordered + `maxPacketLifeTime` court. |
| F11 | P1 | A | (~l.1350) `dc->send()` des keyframes même buffer plein — peut bloquer le main thread Qt. Évolution propre : thread relay dédié, ou `onBufferedAmountLow`. |
| F15 | P1 | A | (~l.908-918) `scanNals` exécuté sur CHAQUE frame pour calculer `isHevcFrame` qui ne sert qu'en mode test → ~60 scans NAL/s gaspillés sur le main thread. |
| F17 | P2 | — | `kFallbackMaxPayloadSize=14000` (WS) incohérent avec 64000 (DC). |
| F18 | P2 | — | Compteurs `static` jamais reset entre sessions (`wsFrameId` croissant) — vérifier que le frontend ne suppose pas un reset. |
| F8 | P2 | — | Copie n°2 : memcpy fragmentation → `rtc::binary`. Acceptable. |

### 2.3 Session / StreamConfig / SignalingServer / RelayBase

| ID | Sév. | Lien | Finding |
|---|---|---|---|
| F20 | P2 | B | StreamConfig : `kMaxRefFrames=1` déclaré mais jamais transmis dans `STREAM_CONFIGURATION` ; pas d'intra-refresh ni de RFI exploités. Pistes pour atténuer B à la source (récupération moins coûteuse qu'un IDR complet). |
| F16 | P2 | A | SignalingServer.cpp (~l.488-570) : fallback WS sans aucune backpressure ni drop — TCP bufferise sans borne. Surveiller `bytesToWrite()` + sticky drop. |
| F14 | P2 | — | Patch HEVC one-shot au démarrage (Session) — sans impact runtime. |

---

## 3. Frontend — findings détaillés

### 3.1 WebRtcDataChannel.js

| ID | Sév. | Lien | Finding |
|---|---|---|---|
| F1.1 | **P0** | **A** | (l.574-579) `maxRetransmits: 3` sur le DC vidéo (negotiated, id=0, ordered:false) alors que le commentaire dit 0 (l.566/570, commentaire dupliqué par copier-coller) — miroir exact du F6 backend, configs cohérentes entre elles. Canal negotiated : les deux côtés doivent être corrigés ensemble. Vérifié : `maxRetransmits: 0` signifie bien "aucune retransmission" (PR-SCTP limite 0), distinct du champ absent (= reliable). |
| F1.3 | **P0** | **B** | `_assembleFrame` (l.864-879) : **toute frame incomplète (I ou P) est droppée silencieusement** (`stats.framesDropped++` + warn), sans signalement de gap au consommateur ni requête IDR. NB validation : le commentaire l.47 décrit un zero-fill des P-frames qui n'existe PAS dans le code (doc obsolète, à corriger). Les frameId sont émis (~l.981) mais StreamView n'exploite pas les discontinuités → les deltas suivantes (références orphelines) partent au décodeur sans que personne ne le sache. L'entrée étant supprimée immédiatement (l.860), une frame jamais complétée n'est vue que par le timer 500 ms. |
| F1.2 | P1→P0 | **B** | Aucune requête IDR immédiate au drop définitif d'une frame — récupération uniquement via timers (`CLEANUP_INTERVAL_MS=500`, `STARVATION_TIMEOUT_MS=200`) → jusqu'à 500 ms + RTT avant le début de récupération. |
| F1.6 | P1 | A | Code debug TEST-B/TEST-H actif : 3 hashes FNV-1a full-frame + scan octet par octet sur les 20 premières frames → blocage du main thread au démarrage. |
| F1.4 | P2 | B | `_starvationRequested` réarmé sur n'importe quel chunk (~l.808) au lieu de sur frame assemblée → risque de flood IDR sous perte partielle continue. |
| F1.7 | P2 | A | Map de réassemblage non bornée entre les ticks de cleanup (500 ms). |

### 3.2 StreamView.js

| ID | Sév. | Lien | Finding |
|---|---|---|---|
| F2.1 | **P0** | **A** | `decodeFrame` (~l.1219-1330) : **`decodeQueueSize` n'est consulté nulle part**. `decode()` appelé inconditionnellement → la file interne du VideoDecoder s'empile sans limite sur le décodeur HW Android lent. Le frameQueue cap 3 existant agit après décodage — bon pattern, mauvais étage. Levier n°1 du problème A. |
| F2.3 | **P0** | **B** | Aucun état "référence perdue → drop des deltas jusqu'au prochain IDR". Les P-frames post-perte (gap frontend, drop backend) sont soumises au décodeur ; le HW Android les décode **sans erreur** et affiche les macro-blocks. Levier n°1 du problème B. |
| F2.10 | P0 (cause) | B | `_handleDecoderError` (~l.734-791) est le seul mécanisme de récupération, et il dépend d'une erreur que le décodeur Android n'émet jamais sur frame corrompue → détection alternative obligatoire (= F2.3 + F1.3). |
| F2.2 | P1 | A | (~l.1252) Timestamps de chunk synthétiques (`frameCount × 16667`, 60 FPS fixe) au lieu du `backendTs` transmis et disponible → perturbe le scheduling interne du décodeur. |
| F2.11 | P1 | A | `optimizeForLatency` ABSENT des configs HEVC Annex B (~l.996-1002) — précisément le chemin Android. |
| F2.6 | P1 | A | (~l.1666) `getImageData(0,0,1,1)` : readback GPU synchrone sur CHAQUE frame HEVC (workaround green-image) → borner aux N premières frames. |
| F2.7 | P1 | A | console.log per-frame sur 3 chemins (output ~l.702-705, decodeFrame ~l.1313-1315, H.264 ~l.1696) → centaines de logs/s. |
| F2.12 | P1 | — | Aucune détection ni chemin spécifique Android (voir F4.1). |
| F2.5 | OK | A | frameQueue cap 3 au rendu + catch-up : bon pattern (confirme l'approche Xbox), à conserver. |
| F2.4 | P2 | A | Drop stale par `backendTs` peut créer des gaps sous jitter (interaction avec F2.3 à soigner). |
| F2.9 | P2 | A | `flushPendingFrames` peut soumettre jusqu'à 120 `decode()` en rafale à la configuration. |

### 3.3 Mp4Muxer.js

| ID | Sév. | Lien | Finding |
|---|---|---|---|
| F3.1/F2.8 | P1 | A | `toAvcc` (~l.442-490) : reconstruction via Array JS + push octet par octet puis `new Uint8Array(parts)` — l'opération la plus chère par frame. `splitNals` rappelé plusieurs fois par frame. → pré-allocation Uint8Array + `set()`, memoization du split. 2 copies full-frame éliminables sur ~4. |

### 3.4 BrowserDetect.js

| ID | Sév. | Lien | Finding |
|---|---|---|---|
| F4.1 | P1 | — | Aucune détection Android (seulement mobile/tablet/touch). Nécessaire pour gater d'éventuels comportements spécifiques sans risquer de régression PC/Mac/iOS. NB : les fixes P0 sont des corrections universelles, à appliquer partout. |

---

## 4. Copies mémoire par frame (régime établi)

| Étape | Copie | Éliminable ? |
|---|---|---|
| Backend : LENTRY → QByteArray | full-frame | Non (boundary thread) |
| Backend : fragmentation → rtc::binary | full-frame | Difficilement (API libdatachannel) |
| Frontend : chunks → frame assemblée | full-frame | Non (réassemblage) |
| Frontend : slices splitNals | partielle, répétée | **Oui** (memoization) |
| Frontend : toAvcc (Array JS octet/octet) | full-frame, la plus chère | **Oui** (préallocation) |
| Frontend : EncodedVideoChunk | full-frame | Non (API WebCodecs) |

Aucune fuite de VideoFrame détectée : tous les chemins de rendu font `close()`.

---

## 5. Résilience réseau — comportement attendu en conditions dégradées (état actuel)

| Scénario | Comportement actuel |
|---|---|
| Perte 1-5 % | maxRetransmits=3 retransmet du périmé (A) ; messages 64 Ko invalidés par 1 fragment perdu ; frames incomplètes droppées silencieusement puis deltas orphelines décodées (B) ; récupération en ≥ 500 ms + RTT, sans relance (B persistant) |
| Jitter > 50 ms | Drop stale par backendTs peut créer des gaps non signalés (B) ; aucune hystérésis de drop |
| < 10 Mbps / congestion | Watermark 128 KB ≈ >50 ms de file ; deltas droppées non-sticky → corruption (B) ; audio reliable en concurrence avec la vidéo |
| Rafale de pertes puis rattrapage | File Qt + buffer SCTP + queue décodeur se vident en rafale → pic de latence prolongé (A) |

---

## 6. Parallélisme et isolation — verdict

- **Backend** : la fragmentation + `send()` sur le main thread Qt est un goulot réel mais secondaire ; le thread relay dédié est une évolution propre (P2), pas un préalable.
- **Frontend** : déporter réassemblage/parsing dans un Web Worker est un chantier lourd dont le bénéfice n'apparaît qu'APRÈS la backpressure décodeur (sinon on remplit plus vite une file illimitée). OffscreenCanvas : gain modeste. **Priorité aux corrections algorithmiques (F2.1, F2.3) avant tout chantier de parallélisme.**
- **Isolation de crash** : la valeur principale viendrait du Worker de réassemblage (protège des frames malformées), mais le risque réel observé est faible — P2.
