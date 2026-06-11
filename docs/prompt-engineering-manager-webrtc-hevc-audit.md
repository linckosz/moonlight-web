# Prompt Engineering Manager — Audit Pipeline WebRTC DataChannel UDP HEVC

> **Session :** Audit complet du pipeline vidéo HEVC sur WebRTC DataChannel (UDP)
> **Date :** 2026-06-11
> **Contexte :** Le stream fonctionne, mais des problèmes de latence et de pixélisation subsistent sur Android.
> **Cible :** Latence optimale + image propre, en acceptant du drop plutôt que de l'accumulation.

---

## 1. État actuel du projet

- **Phase 1–6** : ✅ Terminées (HTTP, découverte, pairing, apps, RTSP, vidéo, audio)
- **Phase 7 (Input)** : ⏳ En cours
- **Phase 8 (Polish)** : ⏳
- **Transport vidéo** : WebRTC DataChannel (SCTP over UDP) — canal `video` pour les NAL units HEVC/H.264
- **Encodage** : Sunshine encode en HEVC (ou H.264 en fallback), envoie les NAL units via WebRTC DataChannel
- **Backend** : C++17, Qt 6.11, libdatachannel (libdatachannel.dll)
- **Frontend** : Vanilla JS, WebCodecs (VideoDecoder), Canvas 2D, AudioWorklet
- **Plateformes testées** : PC (Windows Chrome), Mac (Chrome/Safari), iOS (Chrome/Safari), Android (Chrome)
- **Fonctionnement** : Le stream fonctionne sur **toutes les plateformes**

### Comportement observé par plateforme

| Plateforme | Status | Observations |
|---|---|---|
| **PC (Windows)** | ✅ Fonctionnel | Pas de problème particulier détecté en test |
| **Mac (Chrome)** | ✅ Fonctionnel | Pas de problème particulier détecté en test |
| **iOS (Safari)** | ✅ Fonctionnel | Pas de problème particulier détecté en test |
| **Android (Chrome)** | ⚠️ Dégradé | **Deux problèmes identifiés** (voir section 2) |

---

## 2. Problèmes identifiés sur Android

### Problème A — Accumulation de latence (buffer bloat visuel)

**Symptôme :** À certains moments, on perçoit une augmentation de la latence, comme si l'affichage était plus lent que le nombre d'images reçues, et que le pipeline se forçait à traiter **toutes** les images dans la limite d'un buffer, accumulant du retard.

**Comportement attendu :** Si une frame arrive et qu'on n'a pas assez de temps pour la traiter (rendu de la frame précédente toujours en cours, decodeur saturé, etc.), on doit **dropper** cette frame plutôt que de l'accumuler dans une file d'attente qui ne fait qu'augmenter la latence.

**Hypothèses possibles :**
- Buffer/queue quelque part dans le pipeline qui accumule sans backpressure
- Le VideoDecoder ou le canvas n'impose pas de signal "occupé" en amont
- Absence de mécanisme de détection de retard et de drop proactif
- Différence de performance CPU/GPU Android vs desktop qui expose le problème

### Problème B — Pixélisation blocky (type blocs de compression)

**Symptôme :** À certains moments, pixélisation sous forme de blocs de compression vidéo (macro-blocks), comme si l'image de référence (keyframe / I-frame) est perdue ou corrompue. Les blocs persistent jusqu'à ce qu'un changement fort de couleur dans la zone force un reset des pixels (ex: ouverture/fermeture d'une application).

**Comportement attendu :** L'image doit rester propre en permanence. Si une image de référence est perdue, le décodeur doit récupérer rapidement (soit via un IDR automatique, soit via une détection + demande d'IDR).

**Hypothèses possibles :**
- Perte de paquets sur le DataChannel SCTP qui corrompt des NAL units
- Le décodeur HEVC Android gère mal certaines corruptions sans le signaler
- Pas de détection de corruption suivie d'une demande d'I-frame
- Problème spécifique au décodeur HEVC hardware Android

### ⚠️ Tests réseau instable — non faits

**Les tests actuels ont été réalisés en conditions réseau favorables (LAN ou WiFi stable).** Aucun test poussé n'a encore été fait avec un réseau instable, à faible bande passante, ou avec une latence élevée.

Il est **nécessaire d'optimiser le pipeline pour les réseaux peu performants**, en anticipant les scénarios suivants :

| Scénario | Impact potentiel |
|---|---|
| **Bande passante réduite** (< 10 Mbps) | Congestion, paquets drops, frames incomplètes |
| **Latence variable** (jitter > 50ms) | Arrivée désordonnée ou tardive des NAL units, accumulation |
| **Perte de paquets** (1-5%) | Corruption de NAL units, perte d'I-frame, pixélisation blocky |
| **Réseau mobile** (4G/5G instable) | Combinaison de tous les facteurs ci-dessus, particulièrement pertinent pour Android |
| **Congestion temporaire** | Rafales de pertes suivies de rattrapage → risque de buffer bloat |

**L'audit doit donc considérer la résilience du pipeline face à ces conditions dégradées**, et pas seulement le fonctionnement en conditions idéales. Les mécanismes de drop, de backpressure, et de récupération après corruption sont d'autant plus critiques.

---

## 3. Mission — Audit complet du pipeline

### 3.1 Pipeline AsIs — Cartographie actuelle du chemin WebRTC DataChannel HEVC

> **Ceci est l'état actuel du pipeline, documenté avant l'audit. L'audit n'a pas à le redécouvrir — il doit l'utiliser comme base pour identifier les problèmes.**

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Sunshine (host distant)                                                │
│    Encode HEVC → NAL units (Annex B) → RTP/UDP → Internet              │
└──────────────────────────────┬──────────────────────────────────────────┘
                               ↓
┌─────────────────────────────────────────────────────────────────────────┐
│  Étape 1 — MoonlightShim (backend/src/streaming/MoonlightShim.cpp)     │
│                                                                         │
│  Thread    : QThread worker dédié (pas le main thread Qt)              │
│  Rôle      : Wrapper C++ autour de moonlight-common-c (LiStartConnection)
│              Callback drSubmitDecodeUnit() reçoit les DECODE_UNIT       │
│  Buffer    : Aucun — le callback est appelé directement par m-common-c │
│  Sortie    : Signal Qt videoFrameReady(QByteArray, frameType, frameNum)│
│              → marshalé vers le main thread Qt (AutoConnection)        │
│  Coût      : Copie QByteArray (deep copy) pour traverser la boundary   │
│              de thread. Le DECODE_UNIT est libéré après le callback.   │
│                                                                         │
│  Note      : Le worker thread exécute LiStartConnection + boucle       │
│              d'events moonlight-common-c. S'il crash, le main thread   │
│              reçoit connectionTerminated() et peut nettoyer.           │
└──────────────────────────────┬──────────────────────────────────────────┘
                               ↓ (main thread Qt)
┌─────────────────────────────────────────────────────────────────────────┐
│  Étape 2 — DataChannelRelay (backend/src/streaming/DataChannelRelay.cpp)│
│                                                                         │
│  Thread    : Main thread Qt (où vivent tous les QObjects)              │
│  Rôle      : Reçoit videoFrameReady → fragmente → envoie sur DataChannel│
│  Frag      : Header 17 octets [frame_id:4][chunk:2][total:2]           │
│              [is_keyframe:1][payload:4][backend_ts:4]                   │
│              Max payload/chunk = 64000 octets (sous limite SCTP 16KB?) │
│  Buffer    : m_BufferedKeyframe — 1 keyframe retenue si le DC vidéo    │
│              n'est pas encore ouvert (évite écran noir au démarrage)   │
│  Backpressure: kHighWatermark = 128 KB (SCTP buffer pending).          │
│              Si dépassé → drop des delta frames. Keyframes passent     │
│              toujours. IDR request après ~500ms de backpressure.       │
│  Sortie    : libdatachannel → SCTP → DataChannel "video" → navigateur  │
│  Coût      : Fragmentation CPU (découpage + header), send() bloquant   │
│              si buffer SCTP plein (atténué par le watermark)           │
└──────────────────────────────┬──────────────────────────────────────────┘
                               ↓ (réseau — SCTP over UDP)
┌─────────────────────────────────────────────────────────────────────────┐
│  Étape 3 — WebRtcDataChannel (frontend/js/api/WebRtcDataChannel.js)    │
│                                                                         │
│  Thread    : Main thread JS (event loop unique du navigateur)          │
│  Rôle      : RTCPeerConnection + 3 DataChannels (video, audio, input)  │
│  Réception : dc.onmessage → buffer binaire (1 chunk)                   │
│  Réass     : Map frame_id → chunks[]. Si total_chunks atteint → frame  │
│              complète. I-frame incomplète → DROP. P-frame incomplète   │
│              → trous remplis de 0x00.                                  │
│  Cleanup   : Timer 500ms — drop les frames trop vieilles (stale)       │
│  Sortie    : Callback onVideo(frame: Uint8Array, isKeyframe: bool,    │
│              frameId: number, backendTs: number)                       │
│  Coût      : Plusieurs copies mémoire par chunk (Buffer→Uint8Array→   │
│              concaténation). Pas de ring buffer.                       │
│                                                                         │
│  Note      : Tout s'exécute sur le main thread JS. Une frame lourde    │
│              bloque le traitement des frames suivantes.                │
└──────────────────────────────┬──────────────────────────────────────────┘
                               ↓ (main thread JS)
┌─────────────────────────────────────────────────────────────────────────┐
│  Étape 4 — StreamView (frontend/js/ui/StreamView.js)                    │
│                                                                         │
│  Thread    : Main thread JS                                            │
│  Rôle      : Point d'entrée du pipeline vidéo. Reçoit onVideo().       │
│  Parsing   : splitNals() → extrait les NAL units individuelles         │
│              (Annex B start codes 0x00 0x00 0x00 0x01).               │
│              findSequenceHeader() → extrait SPS/PPS/VPS pour config.   │
│  Décodage  : VideoDecoder (WebCodecs). configure() avec description    │
│              (hvcC pour HEVC, avcC pour H.264).                        │
│              decode(chunk) → VideoFrame (texture GPU).                 │
│  Queue     : File d'attente implicite dans VideoDecoder (interne       │
│              navigateur). Pas de contrôle explicite sur la profondeur. │
│  Rendu     : VideoFrame → createImageBitmap() → drawImage() sur Canvas │
│              Chemins alternatifs selon plateforme :                     │
│              - iOS < 17: drawImage + copyTo RGBA (fallback)            │
│              - HEVC Chrome Windows: createImageBitmap(ImageData) pour  │
│                flush compositor (évite écran vert)                     │
│  Stats     : SlidingStats (fenêtre 5s) pour FPS, latence, drops       │
│  Sortie    : Pixels affichés sur le Canvas 2D                          │
│  Coût      : splitNals (CPU), VideoDecoder (GPU), createImageBitmap   │
│              (GPU→CPU→GPU round-trip sur certaines plateformes)        │
└──────────────────────────────┬──────────────────────────────────────────┘
                               ↓
┌─────────────────────────────────────────────────────────────────────────┐
│  Étape 5 — Affichage Canvas                                             │
│                                                                         │
│  Thread    : Main thread JS + compositor GPU (thread séparé navigateur)│
│  Rôle      : drawImage() sur Canvas 2D → compositor → écran            │
│  Fullscreen: canvas.requestFullscreen() (pas documentElement) pour     │
│              éviter distortion HiDPI                                    │
│  Coût      : Composition GPU (variable selon plateforme/driver)        │
└─────────────────────────────────────────────────────────────────────────┘
```

**Chemin audio (parallèle au chemin vidéo) :**

```
MoonlightShim::arDecodeAndPlaySample (worker thread)
  → signal audioSampleReady(QByteArray) → main thread
  → DataChannelRelay::onAudioSample → fragmentation → SCTP → DC "audio"
  → WebRtcDataChannel.onAudio → callback
  → StreamView → AudioPipeline.write(pcm16Buffer)
  → AudioWorklet (thread séparé): PCM16→Float32 → AudioBuffer → sortie
```

**Modes de transport alternatifs :**

| Mode | Backend | Frontend | Usage |
|---|---|---|---|
| `webrtc` (défaut) | DataChannelRelay (SCTP) | WebRtcDataChannel | HEVC/H.264 sur DataChannels |
| `webrtc-media` | MediaTrackRelay (RTP track) | WebRtcMedia + `<video>` | H.264 sur RTP media track |
| `wss` (legacy) | StreamRelay (WebSocket) | WsStreamChannel | Fallback TCP sans WebRTC |

### 3.2 Fichiers à auditer — Pipeline WebRTC DataChannel HEVC

> Seuls les fichiers directement impliqués dans le pipeline vidéo HEVC sur WebRTC DataChannel sont listés. Les fichiers audio, input, settings, admin, auth, réseau et fallback legacy sont exclus.

**Backend C++ (`backend/src/`) :**

| Fichier | Rôle dans le pipeline |
|---|---|
| `streaming/MoonlightShim.h/.cpp` | Wrapper moonlight-common-c, worker thread, callback `drSubmitDecodeUnit()` → signal `videoFrameReady()`, metrics (RTT, decode latency) |
| `streaming/DataChannelRelay.h/.cpp` | **Cœur du relay** : fragmentation frames (header 17 octets), backpressure 128KB watermark, drop delta frames, buffer keyframe, envoi SCTP sur DC "video", IDR request |
| `streaming/RelayBase.h` | Interface abstraite commune (prepare, setRemoteDescription, stop, requestIdrFrame) |
| `streaming/SignalingServer.h/.cpp` | Serveur WebSocket signaling SDP/ICE, WS fallback (TCP) si ICE timeout — impacte le setup et la résilience du pipeline |
| `streaming/Session.h/.cpp` | Orchestrateur : launch app, crée DataChannelRelay + SignalingServer, négocie codec HEVC/H.264, répond JSON au navigateur |
| `streaming/StreamConfig.h/.cpp` | Configuration codec (HEVC/H.264/AV1), résolution, FPS, bitrate, computeVideoFormats() |

**Frontend JS (`frontend/js/`) :**

| Fichier | Rôle dans le pipeline |
|---|---|
| `api/WebRtcDataChannel.js` | Client WebRTC DataChannel : RTCPeerConnection + 3 DCs, réassemblage fragments, drop I-frame incomplètes, cleanup stale frames (>500ms), starvation detection + IDR request |
| `ui/StreamView.js` | **Cœur du rendu** (3581 lignes) : parsing NAL (splitNals), VideoDecoder WebCodecs avec fallback chain (HEVC Annex B → HEVC AVCC → H.264), rendu Canvas 2D (createImageBitmap + drawImage), HEVC workarounds par plateforme, stats, backpressure/starvation handling |
| `util/Mp4Muxer.js` | Utilitaires NAL : splitNals, NalParser (VPS/SPS/PPS), buildDescription (hvcC/avcC), getCodecString, toAvcc, removeEmulationPrevention, codec strings |
| `util/BrowserDetect.js` | Détection plateforme (iOS, Android, mobile, touch) — utilisé pour les workarounds HEVC spécifiques par plateforme |

### 3.3 Axes d'audit

Pour chaque composant du pipeline, évalue :

| Axe | Question |
|---|---|
| **Bottlenecks** | Où est-ce que le débit est limité ? Thread unique ? Attente synchrone ? |
| **Copies inutiles** | Y a-t-il des copies mémoire CPU↔CPU ou CPU↔GPU évitables ? |
| **Buffers/Queues** | Taille des buffers ? Comportement quand plein (block ou drop) ? |
| **Backpressure** | Le pipeline a-t-il un mécanisme qui propage la congestion vers l'amont ? |
| **Drops** | Y a-t-il un mécanisme de drop ? Est-il assez agressif ? Se déclenche-t-il au bon endroit ? |
| **Synchronisation** | Y a-t-il des attentes inutiles (locks, barriers) entre étapes ? |
| **Paramètres** | Les paramètres SCTP, WebRTC, décodeur, canvas sont-ils optimaux ? |
| **Gestion d'erreur** | Que se passe-t-il si une NAL unit est corrompue ? Le décodeur récupère-t-il ? |
| **Résilience réseau** | Comment le pipeline se comporte-t-il en cas de perte de paquets, jitter élevé, bande passante réduite ? Les mécanismes de récupération sont-ils suffisants ? |
| **Plateforme** | Y a-t-il des différences de comportement par plateforme (Android spécifiquement) ? |
| **Parallélisme & isolation de crash** | Le pipeline exploite-t-il assez le parallélisme ? **Backend :** MoonlightShim tourne déjà sur un thread worker séparé — est-ce suffisant ? Pourrait-on déplacer la fragmentation/relay sur un thread dédié pour éviter de bloquer le main thread Qt ? **Frontend :** Tout le pipeline JS (réception, parsing NAL, décodage, rendu) s'exécute sur le main thread unique du navigateur — peut-on délester des étapes vers des Web Workers (réassemblage fragments, parsing NAL) ou des APIs asynchrones (OffscreenCanvas) ? L'objectif est double : **(a)** éviter qu'un crash dans une étape non-critique (ex: parsing NAL) ne bloque tout le pipeline, et **(b)** réduire la latence en parallélisant les étapes indépendantes. |

### 3.4 Identifier les optimisations

Classer les optimisations par catégorie :

1. **Optimisations de code** — changements dans la logique (ex: détection de retard, drop proactif)
2. **Optimisations de paramètres** — tuning de constantes (ex: buffer size, timeout, watermark SCTP)
3. **Optimisations de workflow** — réduction de copies, changement d'architecture de données (ex: éviter une copie CPU→GPU, transfert zero-copy, ring buffer, etc.)
4. **Optimisations d'architecture** — réorganisation des fichiers, découpage en modules, refactoring structurel si l'organisation actuelle limite la maintenabilité ou la performance. **Si l'architecture des fichiers doit changer pour être plus efficace, cela doit faire partie du plan.**
5. **Optimisations de parallélisme** — déplacement d'étapes du pipeline sur des threads/workers séparés (backend: QThread, frontend: Web Workers, OffscreenCanvas, AudioWorklet déjà utilisé). Évaluer pour chaque étape si l'isolation sur un thread dédié permettrait de **(a)** survivre à un crash isolé sans tuer l'application entière, et **(b)** réduire la latence bout-en-bout en découplant les étapes.

Pour chaque optimisation, précise :
- Le gain estimé (latence, fluidité, qualité)
- Le risque (régression, complexité)
- La priorité (P0 = critique, P1 = important, P2 = nice-to-have)

### 3.5 Identifier les bugs

Pour chaque bug suspecté :
- Localisation précise (fichier, fonction)
- Condition de déclenchement
- Impact (latence, corruption visuelle, crash)
- Lien avec les problèmes observés sur Android

---

## 4. Règles et contraintes

### Ce que j'accepte

- ✅ **Drop d'image** s'il y a un bottleneck (plus de frames reçues que traitées) — je préfère un drop qu'une accumulation de latence
- ✅ **Léger artefact visuel** durant une frame ou très peu de frames (< 300ms) si ça permet de garder une latence optimale
- ✅ **Modifications agressives** du pipeline si nécessaire (changer l'architecture de buffer, ajouter un mécanisme de drop, etc.)

### Ce que je n'accepte pas

- ❌ Augmentation de la latence par accumulation (buffer bloat)
- ❌ Pixélisation persistante qui nécessite une action utilisateur pour se résorber
- ❌ Régression sur les plateformes qui fonctionnent déjà bien (PC, Mac, iOS), ou partiellement (Android)

### Règles de développement

- Pas de sur-ingénierie — solutions simples, robustes, performantes
- Commentaires en anglais, concis
- Testable sur toutes les plateformes (PC, Mac, iOS, Android)
- Le code doit rester lisible et maintenable

---

## 5. Livrables attendus

### Livrable 1 — Cartographie du pipeline

Document markdown détaillant le pipeline complet frame-par-frame, avec pour chaque étape :
- Fichier, classe, fonction
- Thread d'exécution
- Buffers/queues utilisés
- Transformation effectuée
- Coût estimé

### Livrable 2 — Rapport d'audit

Pour chaque composant audité :
- Bottlenecks identifiés
- Copies inutiles
- Absence de backpressure
- Absence de mécanisme de drop
- Bugs suspectés

### Livrable 3 — Plan d'optimisation

Liste ordonnée et priorisée des optimisations à implémenter :
- Description technique détaillée
- Fichiers et fonctions à modifier
- Approche d'implémentation
- Tests de validation
- Estimation de l'impact

### Livrable 4 — Plan d'implémentation

Roadmap séquencée avec dépendances entre les optimisations, regroupées en étapes logiques pour éviter les conflits et permettre des tests incrémentaux.

---

## 6. Modèle

Je veux la meilleure qualité d'analyse possible pour cet audit.

- Engineering Manager : `fable`
- Agents backend-dev / frontend-dev : `opus` également — pour la phase d'audit, la précision prime sur la rapidité
- Agents experts (moonlight-qt, moonlight-xbox, moonlight-web-stream) : `opus`
- Code-reviewer : `opus`

Pas de contrainte de tokens — allez en profondeur.

---

## 7. Instructions pour l'Engineering Manager

1. **Commence par l'audit** — lis et comprends TOUS les fichiers du pipeline avant de proposer des changements
2. **Utilise les agents experts** (`expert-moonlight-qt`, `expert-moonlight-xbox`, `expert-moonlight-web-stream`) pour comprendre comment les implémentations de référence gèrent ces problèmes
3. **Utilise `backend-dev` et `frontend-dev`** pour les analyses détaillées de code
4. **Utilise `code-reviewer`** pour valider chaque proposition avant implémentation
5. **Présente tes conclusions en français**, de façon structurée
6. **Sois exhaustif** — je veux un audit complet, pas un survol
7. **Si tu as besoin de clarifications**, pose des questions avant de conclure
8. **Ne code pas** dans un premier temps — on veut l'audit et le plan d'abord
