# Audit WebRTC HEVC — Livrable 1 : Cartographie détaillée du pipeline

> **Session :** 2026-06-11-audit-webrtc-hevc
> **Périmètre :** Pipeline vidéo HEVC sur WebRTC DataChannel (SCTP/UDP), de Sunshine au Canvas.
> **Sources :** Audit backend (backend-dev), audit frontend (frontend-dev), références croisées moonlight-qt, moonlight-xbox, moonlightweb-stream.

---

## 1. Vue d'ensemble

```
Sunshine (encode HEVC) ──RTP/UDP──→ moonlight-common-c (FEC, depacketize)
  └─→ [Étape 1] MoonlightShim::drSubmitDecodeUnit (worker thread)
        └─→ signal Qt videoFrameReady (QueuedConnection, FILE NON BORNÉE ⚠)
              └─→ [Étape 2] DataChannelRelay::onVideoFrame (main thread Qt)
                    fragmentation (header 17 o, chunks ≤ 64000 o ⚠)
                    backpressure watermark 128 KB ⚠, drop deltas (non-sticky ⚠)
                    └─→ libdatachannel → SCTP (unordered, maxRetransmits=3 ⚠)
                          └─→ [Étape 3] WebRtcDataChannel.js (main thread JS)
                                réassemblage Map frame_id→chunks
                                frame incomplète (I ou P) = drop silencieux, non signalé ⚠
                                └─→ [Étape 4] StreamView.js
                                      splitNals / toAvcc (copies ⚠)
                                      VideoDecoder.decode() (queue interne NON consultée ⚠)
                                      └─→ [Étape 5] output() → frameQueue (cap 3)
                                            → rendu Canvas 2D (workarounds plateforme)
```

Les ⚠ marquent les points faibles détaillés dans le livrable 2.

---

## 2. Étape 1 — MoonlightShim (backend)

| Attribut | Valeur |
|---|---|
| Fichier | `backend/src/streaming/MoonlightShim.h/.cpp` |
| Fonction clé | `drSubmitDecodeUnit()` (callback moonlight-common-c) → émission `videoFrameReady(QByteArray, frameType, frameNum)` (~l.384) |
| Thread | Worker thread moonlight-common-c (mode DIRECT_SUBMIT : le callback s'exécute sur le thread réseau) |
| Buffers | Aucun buffer propre. En amont : `decodeUnitQueue` de moonlight-common-c (bornée à 15, overflow = flush + IDR automatique) |
| Transformation | Concaténation des `LENTRY` du DECODE_UNIT en un `QByteArray` contigu (copie n°1) |
| Sortie | Signal Qt cross-thread → **file d'événements Qt non bornée** vers le main thread |
| Coûts identifiés | Copie full-frame ; `fflush(stderr)` dans le chemin chaud (~l.348-374) ; la file Qt est invisible de tout mécanisme de backpressure |

**Point structurel :** entre l'émission du signal (worker) et `onVideoFrame` (main thread), les frames s'empilent dans la file d'événements Qt sans aucune borne ni drop. Si le main thread Qt est occupé 100 ms, ~6 frames à 60 FPS s'accumulent puis sont toutes traitées — aucun équivalent du `FrameQueue` borné à 5 de moonlight-xbox ni de la `decodeUnitQueue` à 15 de moonlight-common-c.

---

## 3. Étape 2 — DataChannelRelay (backend)

| Attribut | Valeur |
|---|---|
| Fichier | `backend/src/streaming/DataChannelRelay.h/.cpp` |
| Fonctions clés | `onVideoFrame()` (réception signal, ~l.512), fragmentation + envoi (~l.891-1350) |
| Thread | Main thread Qt (fragmentation, scanNals, `dc->send()`) |
| Header | 17 octets : `frame_id:4, chunk:2, total:2, is_keyframe:1, payload:4, backend_ts:4` |
| Constantes | `kMaxPayloadSize = 64000` (h:97, commentaire contradictoire "under SCTP 16KB limit") ; `kHighWatermark = 128 KB` (h:102) ; `kFallbackMaxPayloadSize = 14000` (WS) |
| Config DC vidéo | `unordered=true`, **`maxRetransmits=3`** (~l.697-701) — le commentaire dit "no retransmits", le code le contredit |
| Config DC audio | ordered + reliable (~l.719-725) — partage l'association SCTP avec la vidéo |
| Buffers | `m_BufferedKeyframe` (1 keyframe retenue tant que le DC n'est pas ouvert) ; buffer SCTP de libdatachannel (mesuré via `bufferedAmount`) |
| Backpressure | Si `bufferedAmount > 128 KB` : drop de la delta **courante uniquement** (non-sticky), keyframes passent toujours ; IDR request au 1er drop d'un épisode (`m_BackpressureDropCount==0`), aucun throttle temporel, aucune relance |
| Transformation | Découpage en chunks + memcpy vers `rtc::binary` (copie n°2) |
| Coûts identifiés | `scanNals` exécuté sur chaque frame pour un flag de test (~l.908-918) ; `dc->send()` potentiellement bloquant pour les keyframes (envoyées même buffer plein) |

**Bilan copies backend :** 2 copies physiques full-frame par frame en régime établi (concat LENTRY → QByteArray ; fragmentation → rtc::binary). Le reste est du COW QByteArray. Jugé acceptable.

---

## 4. Transport — SCTP over UDP (libdatachannel ↔ navigateur)

| Attribut | Valeur |
|---|---|
| Canal vidéo | negotiated `id=0`, `unordered=true`, `maxRetransmits=3` **des deux côtés** (backend DataChannelRelay.cpp:697-701 ; frontend WebRtcDataChannel.js:574-579) — configs cohérentes entre elles mais contredites par leurs propres commentaires "no retransmits" (backend l.695, frontend l.566/570 dupliqué) |
| Implication | Sous perte de paquets : SCTP retransmet jusqu'à 3× des données périmées → gonflement de `bufferedAmount`, head-of-line de fait, frames fraîches retardées (mécanisme principal du buffer bloat sur réseau mobile) |
| Taille message | jusqu'à 64 017 octets → fragmenté par SCTP en de nombreux paquets ; en unreliable, la perte d'UN fragment SCTP invalide le message entier |
| Référence | moonlightweb-stream utilise une media track RTP pour la vidéo (PlayoutDelay 0/0) et `max_retransmits: Some(0)` pour ses canaux temps-réel ; jamais `maxPacketLifeTime` |

---

## 5. Étape 3 — WebRtcDataChannel.js (frontend)

| Attribut | Valeur |
|---|---|
| Fichier | `frontend/js/api/WebRtcDataChannel.js` |
| Thread | Main thread JS |
| Rôle | RTCPeerConnection + 3 DC (video, audio, input), réassemblage des chunks |
| Réassemblage | `Map frame_id → chunks[]` (non bornée entre les ticks de cleanup) ; frame complète quand `total_chunks` atteint (copie n°1 frontend : concaténation) |
| Frames incomplètes | **Toute frame incomplète (I ou P) est droppée silencieusement** (`_assembleFrame`, l.864-879 : `stats.framesDropped++` + warn). ⚠ Le commentaire d'en-tête (l.47) décrit un zero-fill des P-frames qui **n'existe pas dans le code** — doc obsolète |
| Cleanup | Timer `CLEANUP_INTERVAL_MS = 500 ms` (stale frames, `FRAME_TIMEOUT_MS = 500 ms`) ; `STARVATION_TIMEOUT_MS = 200 ms` + IDR proactif |
| Signalement de perte | **Aucun** : les drops ne sont pas signalés au consommateur ; les frameId sont émis (~l.981) mais les discontinuités ne sont pas exploitées ; aucune requête IDR immédiate au moment du drop (timers uniquement) |
| IDR sur perte | **Aucune requête immédiate** — uniquement via les timers (latence de récupération jusqu'à 500 ms + RTT) |
| Divers | `_starvationRequested` réarmé sur n'importe quel chunk (~l.808) ; code debug TEST-B/TEST-H actif (3 hashes FNV-1a full-frame sur les 20 premières frames) |

---

## 6. Étape 4 — StreamView.js : décodage (frontend)

| Attribut | Valeur |
|---|---|
| Fichier | `frontend/js/ui/StreamView.js` (~3600 lignes) |
| Fonction clé | `decodeFrame()` (~l.1219-1330) |
| Thread | Main thread JS (parsing + submit) ; décodage effectif sur threads internes du navigateur |
| Parsing | `splitNals()` (rappelé plusieurs fois par frame), `toAvcc()` (Mp4Muxer.js:~442-490, reconstruction octet par octet via Array JS — l'opération la plus chère par frame) |
| Queue décodeur | File interne du `VideoDecoder` — **`decodeQueueSize` n'est consulté nulle part** ; `decode()` appelé inconditionnellement |
| Timestamps | Synthétiques `frameCount × 16667` (60 FPS fixe, ~l.1252) ; `backendTs` transmis dans le header mais inutilisé |
| Config | `optimizeForLatency` absent des configs HEVC Annex B (~l.996-1002) — le chemin utilisé sur Android |
| Récupération | `_handleDecoderError` (~l.734-791) : reset + IDR sur erreur du décodeur — **mais le décodeur HW Android n'émet pas d'erreur sur frame corrompue** → jamais déclenché |
| État référence | **Aucun état "référence perdue → drop des deltas jusqu'à l'IDR"** : les P-frames orphelines (dont la référence a été droppée en amont) sont décodées et affichées |
| Coûts | ~3-4 copies full-frame par frame (assemblage, slices splitNals, toAvcc, EncodedVideoChunk), dont 2 éliminables ; console.log per-frame (×3 chemins) ; `flushPendingFrames` peut soumettre jusqu'à 120 decode() en rafale |

---

## 7. Étape 5 — StreamView.js : rendu (frontend)

| Attribut | Valeur |
|---|---|
| Entrée | Callback `output(videoFrame)` du VideoDecoder (~l.702-705) |
| Queue rendu | `frameQueue` bornée à 3 (drop post-décodage — bon pattern, mais situé APRÈS le goulot du décodeur) |
| Rendu | Canvas 2D, chemins multiples par plateforme (18 workarounds HEVC hérités) : `drawImage(VideoFrame)` direct, `createImageBitmap`, `copyTo` RGBA (iOS<17), `createImageBitmap(ImageData)` (flush compositor Windows) |
| VideoFrames | Tous les chemins font bien `close()` — pas de fuite détectée |
| Coûts | `getImageData(0,0,1,1)` : readback GPU synchrone par frame HEVC (workaround green-image, ~l.1666) ; les chemins `copyTo`+`putImageData` sont des readbacks CPU coûteux sur mobile |
| Android | **Aucune détection Android** (`BrowserDetect.js` ne fournit que mobile/tablet/touch), aucun chemin spécifique Android |

---

## 8. Chemin de la demande IDR (récupération)

```
Frontend (starvation timer 200ms / cleanup 500ms / erreur décodeur)
  → message contrôle DC → DataChannelRelay::requestIdrFrame
  → MoonlightShim → LiRequestIdrFrame() → moonlight-common-c → ENet → Sunshine
  → IDR encodée → redescend tout le pipeline
```

Latence de récupération actuelle : détection (jusqu'à 500 ms) + RTT + encodage + transmission. Aucun des deux côtés ne relance la demande si l'IDR n'arrive pas, et le backend ne redemande pas tant que `m_BackpressureDropCount` n'est pas repassé par une keyframe.

---

## 9. Synthèse des écarts vs implémentations de référence

| Mécanisme | moonlight-qt | moonlight-xbox | moonlightweb-stream | **MoonlightWeb (actuel)** |
|---|---|---|---|---|
| Queue bornée pré-décodage | decodeUnitQueue=15, overflow=flush+IDR | FrameQueue=5, HWM=3, drop à l'enqueue | file envoi=3, drop deltas | **Aucune** (file Qt non bornée ; decodeQueueSize ignoré) |
| Keep-newest / catch-up | Pacer frameDropTarget 1-3 | renderModeImmediate saute à la plus récente | 1 seule VideoFrame vivante | frameQueue=3 post-décodage seulement |
| Drop deltas post-corruption | waitingForIdrFrame — drop avant décodeur | compensé par queue courte + IDR prioritaire | needsKeyFrame → drop deltas | **Absent des deux côtés** |
| Keyframes jamais droppées | oui | oui (évince l'oldest) | push_front + purge deltas | oui (mais send bloquant) |
| Throttle IDR | event coalescing + attente stabilité | DR_NEED_IDR sur toute erreur | flag requestedIdr par état | 1 requête par épisode, pas de relance |
| Retransmissions transport | RTP+FEC, pas de retransmission | idem | max_retransmits=0 / RTP | **maxRetransmits=3** |
| optimizeForLatency | n/a | n/a | systématique | absent du chemin HEVC Annex B |
| Décodeur saturé | overflow→flush+IDR ; reset après N échecs | backpressure naturel (pool surfaces) | reset() si backlog>200ms + IDR | **rien** |
