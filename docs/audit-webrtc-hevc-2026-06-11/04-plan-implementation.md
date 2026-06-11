# Audit WebRTC HEVC — Livrable 4 : Plan d'implémentation séquencé

> **Session :** 2026-06-11-audit-webrtc-hevc
> Roadmap en 6 étapes, chacune testable indépendamment. Les références OPT-x renvoient au livrable 3.
> **Trio de test à risque historique (à valider à CHAQUE étape, amendement reviewer) : Chrome Windows HEVC (bugs verts/ghosting), Safari iOS (latence pipeline décodeur), réseau lossy mobile.**

---

## Vue d'ensemble des dépendances

```
Étape 0 (prérequis doc + coalescing IDR)
   ├─→ Étape 1 (transport : maxRetransmits=0, chunk 16K)     [indépendante de 2-3]
   ├─→ Étape 2 (récupération — problème B : OPT-3 backend → OPT-4 frontend)
   └─→ Étape 3 (latence — problème A : OPT-2 frontend, OPT-5 backend)
              └─→ Étape 4 (tuning + nettoyage)
                        └─→ Étape 5 (P2 optionnels)
```

- Étape 2 avant étape 3 : les mécanismes de drop de l'étape 3 (OPT-2, OPT-5) arment les états "awaiting IDR/keyframe" créés à l'étape 2 — l'inverse produirait des drops sans récupération (pixélisation aggravée).
- Backend avant frontend dans l'étape 2 : OPT-4 (frontend) route ses demandes IDR via le cooldown backend d'OPT-0b.
- Étape 1 livrable seule et immédiatement (2 lignes, gain réseau réel).

---

## Étape 0 — Prérequis (OPT-0a, OPT-0b)

| Tâche | Agent | Fichiers |
|---|---|---|
| Corriger les commentaires obsolètes (zero-fill l.47 ; "no retransmits" l.695 backend, l.566/570 frontend + dédup) | frontend-dev + backend-dev | WebRtcDataChannel.js, DataChannelRelay.cpp |
| Coalescing IDR unique : cooldown 250-500 ms + relance tant qu'awaiting-IDR, point de convergence `DataChannelRelay::requestIdrFrame()` | backend-dev | DataChannelRelay.h/.cpp |

**Tests :** build OK ; stream nominal inchangé sur PC ; sous perte simulée, fréquence des `LiRequestIdrFrame` ≤ 2-4/s (log compteur).
**Critère de sortie :** aucun changement de comportement nominal ; throttle mesurable.

---

## Étape 1 — Transport (OPT-1, partie d'OPT-10)

| Tâche | Agent | Fichiers |
|---|---|---|
| `maxRetransmits` 3 → 0, DC vidéo, DEUX côtés simultanément (canal negotiated id=0) | backend-dev + frontend-dev | DataChannelRelay.cpp:697-701, WebRtcDataChannel.js:574-579 |
| `kMaxPayloadSize` 64000 → 16000 | backend-dev | DataChannelRelay.h:97 |

**Tests :** stream nominal PC/Mac/iOS/Android (LAN) ; **scénario freeze Android historique** (le fix 14000→32000 d'origine — vérifier l'absence de réapparition) ; réseau lossy simulé (clumsy/netem 2 % de perte) : la latence ne doit plus croître pendant les rafales de perte.
**Risque rollback :** trivial (2 constantes).
**Critère de sortie :** pas de freeze Android, latence stable sous perte. NB : la pixélisation peut transitoirement être PLUS visible (pertes non masquées par les retransmissions) — c'est attendu, l'étape 2 la traite.

---

## Étape 2 — Récupération : problème B (OPT-3, OPT-4, OPT-6)

### 2a — Backend (en premier)
| Tâche | Agent | Fichiers |
|---|---|---|
| `m_AwaitingIdr` sticky : armé sur drop backpressure (l.1234), drop DC-pas-ouvert (l.891-899) ; deltas droppées tant qu'actif ; reset sur keyframe envoyée (réutiliser l.1266) ; relance IDR via cooldown OPT-0b | backend-dev | DataChannelRelay.h/.cpp |

### 2b — Frontend (après 2a)
| Tâche | Agent | Fichiers |
|---|---|---|
| Signal de perte définitive depuis `_assembleFrame` (frame incomplète droppée) + demande IDR immédiate (routée backend, throttlée) | frontend-dev | WebRtcDataChannel.js:864-879 |
| État `_referenceValid` dans StreamView : false sur perte signalée / discontinuité frameId / erreur décodeur ; deltas droppées avant `decode()` tant que false ; keyframe restaure | frontend-dev | StreamView.js |

**Tests :**
- Perte simulée 1-5 % : la pixélisation doit disparaître en < 300-500 ms (1 cycle IDR), jamais persister.
- Vérifier l'absence de double-demande IDR (compteur backend).
- Trio à risque : Chrome Windows HEVC (pas de ghosting réintroduit), Safari iOS, Android.
- Interaction avec le drop stale `backendTs` sous jitter (F2.4) : forcer du jitter, vérifier que `_referenceValid` se réarme correctement.
**Critère de sortie :** problème B éradiqué en conditions de perte simulée sur Android.

---

## Étape 3 — Latence : problème A (OPT-2, OPT-5, OPT-7, OPT-8)

### 3a — Frontend
| Tâche | Agent | Fichiers |
|---|---|---|
| Backpressure `decodeQueueSize` : seuil 3-4, drop deltas uniquement, arme `_referenceValid=false` + IDR ; reset décodeur si backlog > 200 ms ET queue > 2, suivi d'IDR | frontend-dev | StreamView.js decodeFrame (~l.1219-1330) |
| Timestamps `EncodedVideoChunk` dérivés de `backendTs` (croissance stricte) | frontend-dev | StreamView.js ~l.1252 |
| `optimizeForLatency: true` sur configs HEVC Annex B, ajout tolérant (fallback sans la clé si rejet, pattern fix iOS colorSpace) | frontend-dev | StreamView.js ~l.996-1002 |

### 3b — Backend
| Tâche | Agent | Fichiers |
|---|---|---|
| `std::atomic<int> m_PendingFrames` : borne 2-3, drop deltas côté worker avant la file Qt (keyframes jamais droppées), arme `m_AwaitingIdr` | backend-dev | MoonlightShim.h/.cpp (~l.384), DataChannelRelay.cpp (~l.512) |

**Tests :**
- Android : charger le main thread (UI, stats) → la latence doit rester bornée, avec drops visibles dans les stats au lieu d'accumulation.
- Mesure A/B de latence bout-en-bout via `backendTs` (déjà transmis) : p50/p95 avant/après.
- iOS Safari : vérifier l'absence de micro-drops parasites au seuil 3-4 (sinon monter à 4-5).
- PC/Mac : aucune régression de fluidité.
**Critère de sortie :** problème A éradiqué — latence stable sur Android même sous charge, drops propres.

---

## Étape 4 — Tuning et nettoyage (OPT-9, OPT-10 reste, OPT-11, OPT-12)

| Tâche | Agent |
|---|---|
| `kHighWatermark` 128→48-64 KB (ou dérivé du bitrate) | backend-dev |
| `scanNals` gaté en mode test ; `fflush(stderr)` hors chemin chaud | backend-dev |
| `getImageData` borné aux N premières frames ; suppression console.log per-frame + TEST-B/TEST-H | frontend-dev |
| DC audio unordered + maxPacketLifeTime court (test audio séparé, rollback simple) | backend-dev + frontend-dev |
| `isAndroid()` dans BrowserDetect.js + stats par plateforme | frontend-dev |

**Tests :** profil CPU main thread JS avant/après sur Android ; qualité audio sous perte ; stream nominal toutes plateformes.

---

## Étape 5 — Optionnels P2 (au fil de l'eau)

Ordre suggéré : OPT-13 (copies toAvcc/splitNals) → OPT-15 (garde-fous) → OPT-14 (purge deltas sur keyframe) → OPT-20 (hygiène) → OPT-16 (fallback WS) → OPT-17 (thread relay) → OPT-18 (RFI/intra-refresh, étude) → OPT-19 (Web Worker, seulement si un goulot subsiste après mesures).

---

## Protocole de test transverse

1. **Simulation réseau** : clumsy (Windows, entre backend et navigateur) ou tc/netem — profils : perte 1 %, 5 %, jitter 50 ms, bande passante 8 Mbps, rafales de perte 500 ms.
2. **Mesures** : latence bout-en-bout via `backendTs` (p50/p95), FPS affiché, `framesDropped` par étage (nouveau compteur par mécanisme de drop), fréquence des IDR requests, `decodeQueueSize` max.
3. **Outillage existant** : `MW_FRAME_DUMP=1` pour capturer les frames brutes en cas d'anomalie ; overlay stats (show_performance_stats).
4. **Matrice plateformes** : chaque étape validée sur PC Windows Chrome (HEVC ET H.264), Mac Chrome, iOS Safari, Android Chrome — LAN d'abord, puis réseau dégradé.
5. **Revue** : passage de `code-reviewer` à la fin de chaque étape (2, 3 et 4 au minimum).

## Estimation globale

| Étape | Taille | Peut régresser |
|---|---|---|
| 0 | XS | Rien |
| 1 | XS | Freeze Android historique (surveillé) |
| 2 | S-M | Chrome Windows HEVC (ghosting), flood IDR (throttlé) |
| 3 | M | iOS (micro-drops), fluidité desktop |
| 4 | S | Audio sous perte |
| 5 | au fil de l'eau | — |
