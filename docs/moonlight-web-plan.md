# Moonlight-Web — Plan d'Architecture & Développement

> **Note :** Ce document a été mis à jour pour refléter l'état réel du code après
> les phases 1-5a. L'architecture a évolué depuis la rédaction initiale — voir les
> annotations [REEL] pour les divergences.

## Vue d'ensemble

```
Browser (HTML/JS)              Backend (C++/Qt)               Sunshine Host
+-------------------+       +------------------------+       +----------------+
|                   |       |                        |       |                |
|  Web App          | REST  |  HTTP/HTTPS Server     | HTTPS |  Sunshine API  |
|  - Host List      |<----->|  - Static file serving |<----->|  /serverinfo   |
|  - App List       |       |  - REST endpoints      |       |  /applist      |
|  - Pair Dialog    |       |  - Proxy to Sunshine   |       |  /launch       |
|                   |       |                        |       |  /pair         |
|  Stream View      | WSS   |  WebSocket Relay       |       |                |
|  - MSE + <video>  |<----->|  - Video passthrough   |       |  RTSP/RTP/UDP  |
|  - Input Capture  |       |  - Audio PCM forward   |<----->|  Moonlight-    |
|                   |       |  - Input relay          |       |  Common-C      |
|                   |       |  - ENet control channel |       |  (embedded)    |
+-------------------+       +------------------------+       +----------------+
```

**Principe :** Le backend est un serveur HTTP (HTTP→HTTPS redirect) + WebSocket
qui sert le frontend, proxifie les appels REST vers Sunshine, et intègre
directement moonlight-common-c (sources compilées dans backend.pro) pour le
protocole RTSP/RTP et le canal de contrôle ENet. Le frontend reçoit les NAL units
H.264 brutes (passthrough) et les encapsule en fMP4 pour le décodage via MSE
(MediaSource Extensions) dans un élément `<video>`. L'audio est forwardé en PCM
brut mais pas encore joué. Pas de re-encodage dans le backend.

[REEL] La vidéo n'utilise PAS WebCodecs directement mais MSE + fMP4 (Mp4Muxer.js).
L'audio PCM est forwardé mais pas encore joué (Phase 6 à faire).
Le backend écoute à la fois HTTP (80→redirect) et HTTPS (443, self-signed cert).

---

## Structure du projet (état réel après Phase 5a)

```
moonlight-web-deepseek/
├── backend/
│   ├── backend.pro                  # Projet Qt (TEMPLATE=app, compile moonlight-common-c inline)
│   ├── src/
│   │   ├── main.cpp                 # Entry point — init serveurs + routes REST
│   │   ├── server/
│   │   │   ├── HttpServer.h/.cpp         # Serveur HTTP + HTTPS (QTcpServer + QSslServer)
│   │   │   ├── StaticFileHandler.h/.cpp  # Fichiers statiques frontend
│   │   │   └── RestRouter.h/.cpp         # Routage REST sync/async (ResponseCallback)
│   │   ├── backend/
│   │   │   ├── NvHTTP.h/.cpp             # Client HTTP/HTTPS Sunshine (async only)
│   │   │   ├── NvComputer.h/.cpp         # Modèle hôte (adresses, état, capacités, persistence)
│   │   │   ├── NvApp.h                   # Modèle application distante
│   │   │   ├── NvAddress.h              # Adresse réseau (IPv4, ports)
│   │   │   ├── ComputerManager.h/.cpp    # Gestion liste hôtes + polling + serialisation HTTPS
│   │   │   ├── ComputerSeeker.cpp        # (dans ComputerManager) découverte mDNS
│   │   │   ├── IdentityManager.h/.cpp    # RSA keypair, X.509 cert, UUID
│   │   │   └── NvPairingManager.h/.cpp   # Protocole challenge-response pairing
│   │   ├── streaming/
│   │   │   ├── StreamConfig.h/.cpp       # Configuration fixe (1080p/60/H.264)
│   │   │   ├── MoonlightShim.h/.cpp      # [REEL] Pont moonlight-common-c → Qt signals
│   │   │   ├── StreamRelay.h/.cpp        # [REEL] WebSocket relay (remplace Video/Audio/InputBridge)
│   │   │   ├── EnetControlStream.h/.cpp  # [REEL] ENet control channel (START_A/B + input)
│   │   │   ├── InputEncoder.h/.cpp       # [REEL] JSON input → GameStream wire format
│   │   │   ├── InputCrypto.h/.cpp        # [REEL] AES-128-GCM input encryption
│   │   │   ├── Session.h/.cpp            # Orchestrateur StreamSession (launch + LiStartConnection)
│   │   │   └── RtspClient.h/.cpp         # [DEAD CODE] Ancien client RTSP standalone (non compilé)
│   │   └── common/
│   │       ├── Logger.h/.cpp
│   │       └── Types.h                   # HttpRequest, HttpResponse, ResponseCallback
│   ├── third_party/
│   │   ├── moonlight-common-c/           # Submodule — sources compilées dans backend.pro
│   │   ├── enet/                         # Inclus dans moonlight-common-c
│   │   ├── qmdnsengine/                  # Submodule — build via .pro séparé, LIBS link
│   │   └── third_party.pro              # Subdirs build for qmdnsengine
│   ├── certs/                            # Généré au runtime (cert.pem + key.pem)
│   └── libs/windows/                     # OpenSSL DLLs et headers
├── frontend/
│   ├── index.html
│   ├── css/
│   │   ├── style.css
│   │   └── stream.css
│   ├── js/
│   │   ├── app.js                        # State machine (LOADING→HOST_LIST→APP_LIST→STREAMING)
│   │   ├── api/
│   │   │   ├── BackendClient.js          # Client REST (fetch)
│   │   │   └── WebSocketClient.js        # Client WebSocket (binaire + texte)
│   │   ├── models/
│   │   │   ├── Host.js
│   │   │   └── App.js
│   │   ├── util/
│   │   │   └── Mp4Muxer.js              # [REEL] Muxeur fMP4 minimal (H.264 → MSE)
│   │   └── ui/
│   │       ├── HostListView.js
│   │       ├── AppListView.js
│   │       ├── PairDialog.js
│   │       ├── StreamView.js            # [REEL] Contient tout le streaming frontend
│   │       └── Toast.js                 # [REEL] Notifications toast
│   └── assets/
│       └── favicon.ico
└── docs/
    ├── moonlight-qt-architecture.md
    └── moonlight-web-plan.md
```

[REEL] Fichiers du plan initial qui n'existent pas :
- `VideoBridge.h/.cpp` — remplacé par StreamRelay
- `AudioBridge.h/.cpp` — remplacé par StreamRelay
- `InputBridge.h/.cpp` — remplacé par StreamRelay + InputEncoder + InputCrypto + EnetControlStream
- `WebSocketServer.h/.cpp` — remplacé par StreamRelay (incorpore QWebSocketServer)
- `Protocol.js`, `ConnectionStatus.js` — jamais créés
- `StreamSession.js`, `VideoPipeline.js`, `AudioPipeline.js`, `audio-processor.js` — fusionnés dans StreamView.js
- `KeyboardHandler.js`, `MouseHandler.js`, `GamepadHandler.js` — input inline dans StreamView.js
- `Platform.h/.cpp` — jamais créé (inutile, moonlight-common-c a ses propres Platform*)

[REEL] Fichiers ajoutés depuis le plan :
- `MoonlightShim.h/.cpp` — pont C→Qt pour moonlight-common-c
- `StreamRelay.h/.cpp` — relay WebSocket central
- `EnetControlStream.h/.cpp` — canal ENet pour input
- `InputEncoder.h/.cpp` — encodage JSON→GameStream binary
- `InputCrypto.h/.cpp` — AES-128-GCM pour input chiffré
- `Mp4Muxer.js` — muxeur fMP4
- `Toast.js` — notifications

---

## Décisions architecturales clés

### 1. Passthrough vidéo (pas de décode dans le backend)

moonlight-common-c assemble les NAL units H.264 dans les callbacks
`drSubmitDecodeUnit`. On forwarde ces NAL units (format Annex B, start codes
00 00 00 01 préfixés) directement au navigateur via WebSocket.

[REEL] Le navigateur encapsule les NAL units en fMP4 segments via `Mp4Muxer.js`
et les passe à MSE (MediaSource Extensions + SourceBuffer) pour décodage GPU
natif dans un élément `<video>`. Ce n'est PAS WebCodecs+canvas comme planifié
initialement — MSE s'est avéré plus simple à intégrer (pas de gestion manuelle
des timestamps, pas de synchronisation AV à coder).

| Approche | Latence | Complexité | Statut |
|----------|---------|------------|--------|
| **fMP4 + MSE + `<video>`** | ~1 trame | Faible | Implementé |
| ~~WebCodecs + canvas~~ | Minimale | Moyenne | Abandonné (MSE suffit) |
| Décode backend + re-encode | +30ms+ | Très élevée | Jamais envisagé |

### 2. Protocole WebSocket binaire simple plutôt que WebRTC

moonlight-common-c utilise un format RTP propriétaire (FEC custom, séquencement).
Adapter vers WebRTC nécessiterait de modifier moonlight-common-c. Un protocole
binaire simple sur WebSocket suffit sur un LAN.

### 3. Connexion WebSocket unique

Une seule connexion WebSocket (StreamRelay) multiplexe vidéo, audio et input.
Pour un scénario LAN, le head-of-line blocking est négligeable.

### 4. Forward PCM audio plutôt que Opus

moonlight-common-c décode Opus multistream en interne et fournit du PCM dans
`arDecodeAndPlaySample`. Forwarder le PCM évite de shipper un décodeur Opus WASM
(~1MB+) et de réimplémenter le demuxing multistream en JS.

[REEL] L'audio PCM est forwardé via WebSocket (channel 0x02) mais n'est pas
encore joué côté navigateur. Phase 6 à implementer.

### 5. [REEL] ENet pour le canal de contrôle (pas WebSocket)

Le plan initial prévoyait un pont WebSocket pour les événements input. En réalité,
l'ENet embarqué dans moonlight-common-c est utilisé directement via
`EnetControlStream`. Les événements input arrivent du navigateur en JSON sur le
WebSocket, sont décodés par `InputEncoder`, chiffrés AES-128-GCM par
`InputCrypto`, puis envoyés à Sunshine via ENet (START_A/B handshake, canal
fiable).

### 6. [REEL] MoonlightShim comme pont C→Qt

`MoonlightShim` est un wrapper QObject qui :
- Exécute `LiStartConnection()` sur un QThread dédié
- Convertit les callbacks C de Limelight en signaux Qt
- Expose `videoFrameReady(QByteArray, frameType, frameNumber)` et
  `audioSampleReady(QByteArray)` pour StreamRelay
- Thread-safe : les callbacks arrivent du thread worker, les signaux sont émis
  et reçus par StreamRelay sur le main thread via Qt::AutoConnection

### 7. [REEL] Routes sync/async

Le RestRouter supporte deux modes :
- **Sync** (`get`, `post`, `del`) — retour immédiat HttpResponse
- **Async** (`getAsync`, `postAsync`) — réponse différée via ResponseCallback

Les routes qui impliquent HTTPS vers Sunshine (app list, launch, quit, box art)
sont async pour éviter les QEventLoop imbriqués qui causaient des
"Operation canceled" avec les sessions TLS concurrentes.

---

## Protocole WebSocket (état réel)

### Messages binaires (Backend → Frontend)

```
[channel:1][flags:1][payload:N]
```

| Channel | Nom | Payload |
|---------|-----|---------|
| 0x01 | VIDEO | NAL units H.264 format Annex B (start codes 00 00 00 01 préfixés par `MoonlightShim::drSubmitDecodeUnit`). flags bit0 = 1 si IDR keyframe. Timestamps implicites (ordre des frames). |
| 0x02 | AUDIO | PCM16 little-endian entrelacé. flags inutilisés. Pas encore joué côté navigateur. |

[REEL] Pas de message `VIDEO_CONFIG` (type 0x05) — les SPS/PPS sont inclus
directement dans le flux Annex B. Le `Mp4Muxer` les extrait des NAL units pour
construire le avcC de l'init segment fMP4.

### Messages texte (Frontend → Backend) — Input JSON

```json
{"type":"keydown","keyCode":65,"ctrlKey":false,"shiftKey":false,"altKey":false}
{"type":"keyup","keyCode":65,"ctrlKey":false,"shiftKey":false,"altKey":false}
{"type":"mousemove","dx":10,"dy":-5}
{"type":"mousedown","button":1}
{"type":"mouseup","button":1}
{"type":"mousewheel","delta":-120}
```

Pas de messages texte backend→frontend pour l'instant (state/stats/error
viendront en Phase 8).

[REEL] Différences avec le plan initial :
- Pas d'horodatage (timestamp) dans les messages binaires
- Pas de type 0x05 (VIDEO_CONFIG) — SPS/PPS in-band
- Les messages texte ne sont définis que pour l'input (frontend→backend)
- Pas d'implémentation des messages config/state/stats/error/rumble backend→frontend

---

## API REST (état réel)

| Méthode | Path | Description |
|---------|------|-------------|
| GET | `/` | Sert `index.html` (via HTTPS redirect) |
| GET | `/*` | Sert les fichiers statiques de `frontend/` |
| GET | `/api/health` | [REEL] Health check — retourne `{"status":"ok","version":"0.1.0"}` |
| GET | `/api/hosts` | Liste tous les hôtes connus avec états (JSON) |
| POST | `/api/hosts/scan` | Déclenche un scan LAN (mDNS) |
| POST | `/api/hosts/manual` | Ajoute un hôte par IP `{"address": "..."}` |
| DELETE | `/api/hosts/:id` | [REEL] Supprime un hôte de la liste persistée |
| GET | `/api/hosts/:id/pair` | Récupère le PIN/statut de pairing |
| POST | `/api/hosts/:id/pair` | Soumet le PIN pour pairing |
| GET | `/api/hosts/:id/apps` | [ASYNC] Liste les apps d'un hôte pairé (fetch HTTPS Sunshine) |
| GET | `/api/hosts/:id/appasset` | [REEL][ASYNC] Jaquette PNG d'une app (`?appid=N`), cache-first |
| POST | `/api/hosts/:id/start` | [REEL][ASYNC] Lance le stream — launchApp + LiStartConnection |
| POST | `/api/hosts/:id/quit` | [REEL][ASYNC] Quitte l'app en cours + stop StreamRelay |

[REEL] Routes marquées [ASYNC] utilisent `ResponseCallback` — le socket HTTP
reste ouvert jusqu'à ce que le handler appelle la callback. Timeout 30s → 504.
Les routes async évitent les QEventLoop imbriqués qui causaient des erreurs
"Operation canceled" avec les sessions TLS concurrentes vers Sunshine.

---

## Configuration fixe MVP

```cpp
width  = 1920;
height = 1080;
fps    = 60;
bitrate = 20000;  // 20 Mbps (bonne qualité pour 1080p60 H.264)
packetSize = 1024;
streamingRemotely = STREAM_CFG_LOCAL;
audioConfiguration = AUDIO_CONFIGURATION_STEREO;
supportedVideoFormats = VIDEO_FORMAT_H264;
clientRefreshRateX100 = 6000;
colorSpace = COLORSPACE_REC_709;
colorRange = COLOR_RANGE_LIMITED;
encryptionFlags = ENCFLG_AUDIO | ENCFLG_VIDEO;
```

---

## Budget latence estimé

```
Encode Sunshine (GPU) :            2-4ms
Réseau (LAN, <1ms RTT) :          1ms
moonlight-common-c reassembly :   1ms
Traitement backend + queue :      0.5ms
WebSocket send (localhost) :      0.5ms
MSE + SourceBuffer + decode GPU : 3-6ms  (via élément <video>)
Audio (AudioWorklet, Phase 6) :   5-10ms
Total :                          ~13-23ms
```

Bien en-dessous du seuil de 50ms pour un streaming "local-like".

---

## Threading model — Session (état réel)

```
Main Thread (Qt event loop)           Worker Thread (LiStartConnection via QThread::create)
   |                                         |
   |-- StreamSession::start()                 |
   |   -> NvHTTP::launchAppAsync()            |  [async HTTPS, callback sur main thread]
   |   -> MoonlightShim::startConnection()    |
   |      -> QThread::create([...])           |
   |                                         |-- LiStartConnection() bloque ici
   |                                         |   (RTSP handshake, bind UDP, RTP...)
   |                                         |
   |   callbacks (Qt::AutoConnection — main thread si QObject)
   |   <--------------------------------------  clStageStarting, clStageComplete,
   |                                             clStageFailed, clConnectionStarted,
   |                                             clConnectionTerminated
   |                                         |
   |   ===== STREAMING =====                  |
   |   drSubmitDecodeUnit() (direct call depuis moonlight-common-c)
   |   émet videoFrameReady via Qt signal     |
   |   <--------------------------------------  MoonlightShim (worker thread)
   |   |                                      |
   |   StreamRelay::onVideoFrame()            |
   |   -> m_WsClient->sendBinaryMessage()     |  [main thread via AutoConnection]
   |                                         |
   |   arDecodeAndPlaySample()                |
   |   émet audioSampleReady via Qt signal    |
   |   <--------------------------------------  MoonlightShim (worker thread)
   |   |                                      |
   |   StreamRelay::onAudioSample()           |
   |   -> m_WsClient->sendBinaryMessage()     |
   |                                         |
   |   EnetControlStream::service()           |
   |   (QTimer 20ms sur main thread)          |
   |   -> enet_host_service()                 |
   |                                         |
   |   Input : WS JSON →                     |
   |   StreamRelay::onWsTextMessage()        |
   |   -> InputEncoder → InputCrypto →        |
   |      EnetControlStream::sendInput()      |  [main thread]
   |                                         |
   |   connectionTerminated()                 |
   |   <--------------------------------------  Nettoyage
```

[REEL] Différences avec le plan initial :
- `QThread::create([lambda])` plutôt qu'un QThread sous-classé
- Pas de lock-free queue : les signaux Qt avec AutoConnection gèrent le thread
  handoff (émetteur = worker thread, récepteur StreamRelay = main thread)
- Le `EnetControlStream` tourne sur le main thread via QTimer 20ms
- `QWebSocket::sendBinaryMessage` est appelé depuis le main thread seulement
  (les callbacks arrivent via signaux Qt qui traversent les threads)

---

## Phases de développement

### Phase 1 : Squelette du projet et serveur HTTP

**Objectif :** Un projet Qt qui compile et sert une page web.

**Fichiers créés :**
- `backend/src/main.cpp`
- `backend/src/server/HttpServer.h/.cpp`, `StaticFileHandler.h/.cpp`, `RestRouter.h/.cpp`
- `backend/src/common/Logger.h/.cpp`, `common/Types.h`
- `frontend/index.html`, `frontend/css/style.css`, `frontend/js/app.js`

**Ce qui a été fait réellement :**
- HTTP sur port 80 + HTTPS sur port 443 (avec cert self-signed)
- Redirection HTTP→HTTPS automatique
- Routes sync ET async (ResponseCallback)
- Serveur statique pour frontend/

**Critères d'acceptation :**
- Le projet compile et s'exécute sur Windows 11 x64 avec Qt 6.x
- `http://localhost:48000/` redirige vers HTTPS puis affiche la page d'accueil
- La console affiche "Moonlight-Web server starting..."

---

### Phase 2 : Découverte d'hôtes et API backend

**Objectif :** Le backend découvre les hôtes Sunshine sur le LAN et les expose
via l'API REST. Le frontend affiche la liste des hôtes.

**Fichiers créés :**
- Backend : `NvHTTP.h/.cpp`, `NvComputer.h/.cpp`, `NvApp.h`, `NvAddress.h`
- Backend : `ComputerManager.h/.cpp` (incorpore ComputerSeeker)
- Frontend : `BackendClient.js`, `Host.js`, `App.js`, `HostListView.js`

**Ce qui a été fait réellement :**
- mDNS discovery via qmdnsengine
- `ComputerManager` avec polling périodique (3s), persistence QSettings
- `ComputerSeeker` intégré dans ComputerManager (pas de fichier séparé)
- Routes sync pour /hosts, /scan, /manual

**Critères d'acceptation :**
- Au démarrage, le backend découvre les hôtes Sunshine sur le LAN
- Le frontend affiche les hôtes en ligne avec badge "Live", hors ligne grisés
- Le bouton "Scan" (déclenché automatiquement au focus) fonctionne
- L'ajout manuel par IP fonctionne
- Les hôtes persistent entre les redémarrages

---

### Phase 3 : Pairing

**Objectif :** L'utilisateur peut pairer avec un hôte en ligne mais verrouillé.

**Fichiers créés :**
- Backend : `IdentityManager.h/.cpp`, `NvPairingManager.h/.cpp`
- Frontend : `PairDialog.js`

**Ce qui a été fait réellement :**
- IdentityManager : génération RSA 2048-bit + X.509 self-signed + UUID 64-bit
- NvPairingManager : challenge-response pairing complet (SHA-256 + AES-128-ECB)
- Routes /pair (GET pour PIN, POST pour soumission)
- PairDialog frontend : affiche PIN, instruction de saisie sur l'hôte

**Critères d'acceptation :**
- Clic sur un hôte verrouillé affiche le PairDialog avec PIN
- Saisie du PIN sur l'hôte Sunshine complète le pairing
- L'état de l'hôte passe de verrouillé à disponible dans l'UI
- Le pairing persiste entre les redémarrages

---

### Phase 4 : Liste des applications

**Objectif :** L'utilisateur peut voir et sélectionner les apps d'un hôte pairé.

**Fichiers créés :**
- Frontend : `AppListView.js`
- Backend : mise à jour NvHTTP pour getAppListAsync()

**Ce qui a été fait réellement :**
- `NvHTTP::getAppListAsync()` — fetch HTTPS /applist, parsing XML
- Route async GET /api/hosts/:id/apps (évite QEventLoop imbriqué)
- Route async GET /api/hosts/:id/appasset (cache-first box art)
- Frontend AppListView : grille d'applications avec navigation retour
- Ajout de Toast.js pour les notifications
- Sérialisation des requêtes HTTPS par host (ComputerManager) pour éviter
  "Operation canceled" avec les sessions TLS concurrentes

---

### Phase 5a : Streaming — RTSP Handshake (FAIT)

**Objectif :** Lancer une application sur Sunshine via HTTPS et établir la
connexion streaming avec moonlight-common-c (LiStartConnection → handshake RTSP).

**Fichiers créés :**
- Backend : `StreamConfig.h/.cpp`, `Session.h/.cpp` (StreamSession)
- Backend : `MoonlightShim.h/.cpp`, `StreamRelay.h/.cpp`
- Backend : `EnetControlStream.h/.cpp`
- Backend : `InputEncoder.h/.cpp`, `InputCrypto.h/.cpp`
- Frontend : `WebSocketClient.js`, `StreamView.js`, `Mp4Muxer.js`

**Ce qui a été fait réellement :**
- moonlight-common-c intégré en tant que sources compilées dans backend.pro
  (Connection.c, RtspConnection.c, VideoStream.c, AudioStream.c, RtpVideoQueue.c,
  RtpAudioQueue.c, ControlStream.c, InputStream.c, + ENet + nanors Reed-Solomon)
- `MoonlightShim` : pont C→Qt qui appelle LiStartConnection() sur QThread dédié
- `StreamConfig` : configuration fixe 1080p/60fps/H.264/20Mbps
- `StreamRelay` : WebSocket server (port 48001) qui relaye vidéo+audio+input
  entre MoonlightShim et le navigateur
- `EnetControlStream` : canal ENet pour START_A/B handshake + input
- `InputEncoder` : JSON input → GameStream wire format
- `InputCrypto` : AES-128-GCM pour input chiffré
- Frontend `StreamView` : MSE + `<video>` tag + Mp4Muxer fMP4
- Frontend `WebSocketClient` : connexion WS, dispatch binaire/texte
- Routes POST /api/hosts/:id/start et /quit (async)
- Navigation : app list → launch → StreamView avec overlay quit

**Architecture :**
```
POST /api/hosts/:id/start
  → StreamSession::start()
    → NvHTTP::launchAppAsync() [HTTPS → Sunshine /launch]
    → Parse sessionUrl du XML réponse
    → Crée MoonlightShim + StreamRelay
    → MoonlightShim::startConnection()
      → QThread::create([LiStartConnection])
        → RTSP handshake (OPTIONS,DESCRIBE,SETUP×3,ANNOUNCE,PLAY)
        → drSubmitDecodeUnit() → MoonlightShim → StreamRelay → WS → Browser
        → arDecodeAndPlaySample() → MoonlightShim → StreamRelay → WS → Browser
    → Retourne {status:"streaming", wsUrl:"ws://...", wsPort:48001}
```

**Critères d'acceptation (5a) :**
- Clic sur une app → POST /start → launch sur Sunshine → LiStartConnection réussit
- Le backend reçoit les callbacks drSubmitDecodeUnit (frames vidéo H.264)
- Le backend forwarde les frames H.264 via WebSocket
- Le frontend reçoit les frames et les affiche via MSE+video
- Le bouton "Stop Streaming" → POST /quit → LiStopConnection + Sunshine /quit

**Points d'attention :**
- `RtspClient.h/.cpp` est du dead code — c'est moonlight-common-c qui gère le RTSP
- Les frames IDR peuvent faire >100KB — QWebSocket supporte jusqu'à 256MB
- Les SPS/PPS sont envoyés in-band (format Annex B avec start codes)
- Le navigateur encapsule les NAL units en fMP4 via Mp4Muxer

### Phase 5b : Streaming — Pipeline Vidéo (EN COURS)

**Objectif :** Stabiliser le pipeline vidéo, gérer les cas limites, corriger
les bugs de rendu.

**Ce qu'il reste à faire :**
- Correction du buffering vidéo (frames en attente avant connexion WS)
- Gestion des keyframes et init segments fMP4
- Overlay de statut de connexion
- Détection de perte de connexion et reconnexion
- Gestion des erreurs de décodage (MSE SourceBuffer)
- Finalisation du quit propre (LiStopConnection + arrêt ENet)

**Critères d'acceptation (5b) :**
- La vidéo s'affiche dans les 2 secondes suivant le lancement
- Flux stable à 60fps sans freeze ni corruption
- Le "Stop Streaming" nettoie tout (processus Sunshine, ENet, WS, MSE)
- Gestion des cas d'erreur (déconnexion, timeout, échec launch)

---

### Phase 6 : Streaming — Pipeline Audio (A FAIRE)

**Objectif :** L'audio du jeu est joué par le navigateur.

**Ce qui est déjà fait :**
- Backend `MoonlightShim::arDecodeAndPlaySample()` → émet `audioSampleReady(QByteArray)`
- `StreamRelay::onAudioSample()` forwarde le PCM via WebSocket (channel 0x02)
- moonlight-common-c décode Opus multistream en PCM

**Ce qu'il reste à faire :**
1. Frontend `AudioPipeline` — AudioContext + AudioWorklet
2. Frontend `audio-processor.js` — AudioWorkletProcessor qui reçoit le PCM
3. Synchronisation audio/video (timestamps)

**Fichiers à créer :**
- Frontend : `js/streaming/AudioPipeline.js` (ou intégré dans StreamView)
- Frontend : `js/streaming/audio-processor.js` (AudioWorkletProcessor)

**Critères d'acceptation :**
- L'audio du jeu joue via les haut-parleurs du navigateur
- Audio sync avec la vidéo (dans ~1 frame à 60fps)
- Latence audio <20ms (AudioWorklet)
- Pas de craquements ou décrochages

---

### Phase 7 : Streaming — Input (PARTIELLEMENT FAIT)

**Objectif :** L'utilisateur peut interagir avec le stream (clavier, souris, manette).

**Ce qui est déjà fait :**
- Backend `InputEncoder` : JSON key events → GameStream wire format (NV_INPUT_HEADER)
- Backend `InputCrypto` : AES-128-GCM encryption (matching moonlight-common-c)
- Backend `EnetControlStream` : ENet reliable UDP → START_A/B handshake + input
- Backend `StreamRelay::onWsTextMessage()` : parse JSON input, appelle `InputEncoder`,
  puis `EnetControlStream::sendInput()`
- Frontend clavier : `keydown`/`keyup` events avec keyCode (Windows VK) + modifiers
- Frontend souris : Pointer Lock API → `mousemove` avec `movementX`/`movementY`,
  `mousedown`/`mouseup` avec boutons, `wheel` avec delta

**Ce qu'il reste à faire :**
1. Frontend `GamepadHandler` — Gamepad API polling
2. Mapping clavier complet (basé sur moonlight-qt `keyboard.cpp`)
3. Support des touches spéciales (combo quitter, plein écran)
4. Rumble feedback (Gamepad API hapticActuators)

**Fichiers à créer :**
- Frontend : `js/input/GamepadHandler.js`

**Critères d'acceptation :**
- Le clavier fonctionne dans le jeu sur l'hôte
- La souris avec pointer lock est responsive (FPS jouables)
- Les boutons et molette de souris fonctionnent
- La manette fonctionne (test Xbox/PS controller)

---

### Phase 8 : Polish et gestion d'erreurs (A FAIRE)

**Objectif :** Robustesse et UX fluide.

1. Overlay de statut de connexion (avertissement connexion faible)
2. Affichage stats FPS/bitrate (depuis `clConnectionStatusUpdate`)
3. Messages texte backend→frontend pour state/stats/error
4. Récupération sur erreur de session (reconnect)
5. Configuration release build (SUBSYSTEM:WINDOWS)
6. Logging fichier pour diagnostics

**Critères d'acceptation :**
- Perte de connexion → overlay d'erreur avec option "Reconnect"
- FPS et bitrate affichés dans l'overlay
- Quitter le stream depuis le navigateur quitte l'app sur Sunshine
- Logs écrits dans un fichier pour troubleshooting

---

## Défis et mitigations (mis à jour)

| Défi | Risque | Mitigation | Statut |
|------|--------|------------|--------|
| **Disponibilité WebCodecs** | Non supporté IE/Safari | Cibler Chrome/Edge. | [REEL] Utilise MSE sans fallback |
| **Taille messages WebSocket** | IDR frames ~150KB | QWebSocket supporte jusqu'à 256MB. | OK |
| **AudioContext autoplay** | Bloqué sans gesture utilisateur | Resume sur le clic de lancement d'app. | Phase 6 |
| **Pointer Lock** | Nécessite gesture utilisateur | Activer au premier clic sur le canvas. | [REEL] Implementé |
| **mDNS sur Windows** | Pas de répondeur mDNS natif | qmdnsengine. Fallback ajout IP manuel. | OK |
| **Thread safety QWebSocket** | Écriture depuis worker thread | [REEL] Callbacks via signaux Qt AutoConnection → main thread. | OK |
| **Build moonlight-common-c** | Ajustements MSVC | Sources dans backend.pro. | [REEL] Compile avec MSVC 2022 |
| **Dérive AV sync** | Callbacks non synchronisés | Accepter derive <50ms. | Phase 6 |
| **Gamepad API** | Nécessite HTTPS | Dev sur localhost. | Phase 7 |
| **Rumble** | Callback non forwardé | [REEL] clRumble vide pour l'instant. | Phase 7 |
| **Sérialisation HTTPS** | TLS concurrentes → "Operation canceled" | [REEL] File d'attente par host. Une requête à la fois. | Resolu |
| **ENet Windows** | Initialisation Winsock | [REEL] enet_initialize() + WS2_32 lié. | OK |
| **Input crypto AES-128-GCM** | Conformité moonlight-common-c | [REEL] InputCrypto avec OpenSSL EVP, IV rotation. | OK |

---

## Dépendances — Submodules Git (état réel)

```bash
# Phase 2: mDNS discovery
git submodule add https://github.com/cgutman/qmdnsengine.git \
    backend/third_party/qmdnsengine

# Phase 5: Streaming protocol
git submodule add https://github.com/moonlight-stream/moonlight-common-c.git \
    backend/third_party/moonlight-common-c
```

[REEL] Différences avec le plan initial :
- Pas de submodule h264bitstream — inutile (le parsing NAL est fait côté
  navigateur dans Mp4Muxer.js, et moonlight-common-c gère le depacketizing RTP)
- moonlight-common-c compile ses sources directement dans backend.pro :
  Connection.c, RtspConnection.c, VideoStream.c, AudioStream.c, RtpVideoQueue.c,
  RtpAudioQueue.c, ControlStream.c, InputStream.c, Platform.c, PlatformSockets.c,
  PlatformCrypto.c, Misc.c, ByteBuffer.c, LinkedBlockingQueue.c, FakeCallbacks.c,
  rswrapper.c + ENet (callbacks.c, compress.c, host.c, list.c, packet.c, peer.c,
  protocol.c, win32.c) + nanors (rs.c)
- qmdnsengine est buildé via son propre .pro en tant que bibliothèque statique
  séparée (LIBS link dans backend.pro)
- OpenSSL requis : headers dans `libs/windows/include`, libs dans
  `libs/windows/lib` (libssl.lib, libcrypto.lib)

---

## Configuration build Qt (état réel)

Le fichier `backend.pro` est un projet Qt `TEMPLATE = app` unique (pas de subdirs).
Toutes les sources, y compris moonlight-common-c, sont compilées directement.

Modules Qt requis : `core`, `network`, `websockets`
Compilateur : MSVC 2022 64-bit / Qt 6.x
Dépendances externes : OpenSSL 3.x (libssl.lib + libcrypto.lib)

```qmake
QT += core network websockets
CONFIG += c++17 console
TEMPLATE = app
TARGET = mw-server

INCLUDEPATH += src
INCLUDEPATH += $$PWD/third_party/moonlight-common-c/src
INCLUDEPATH += $$PWD/third_party/moonlight-common-c/enet/include
INCLUDEPATH += $$PWD/third_party/qmdnsengine/qmdnsengine/src/include
INCLUDEPATH += $$PWD/libs/windows/include/x64

# moonlight-common-c sources (compilés inline)
SOURCES += \
    third_party/moonlight-common-c/src/Connection.c \
    third_party/moonlight-common-c/src/RtspConnection.c \
    third_party/moonlight-common-c/src/VideoStream.c \
    third_party/moonlight-common-c/src/AudioStream.c \
    third_party/moonlight-common-c/src/ControlStream.c \
    third_party/moonlight-common-c/src/InputStream.c \
    third_party/moonlight-common-c/src/RtpVideoQueue.c \
    third_party/moonlight-common-c/src/RtpAudioQueue.c \
    third_party/moonlight-common-c/src/Platform.c \
    third_party/moonlight-common-c/src/PlatformSockets.c \
    third_party/moonlight-common-c/src/PlatformCrypto.c \
    # ... + ENet + nanors ...

win32 {
    LIBS += -lWS2_32 -lwinmm
    LIBS += -L$$PWD/libs/windows/lib/x64 -llibssl -llibcrypto
}
```

---

## Actions manuelles — Build et run

### Prérequis
1. Qt 6.x (modules : core, network, websockets) — kit MSVC2022 64-bit
2. OpenSSL 3.x (binaries + libs dans backend/libs/windows/)
3. Submodules initialisés : `git submodule update --init --recursive`

### Build
```bash
# Via Qt Creator : ouvrir backend/backend.pro, kit MSVC2022, Ctrl+B
# Via ligne de commande :
cmd //c d:/Code/moonlight-web-deepseek/backend/build_msvc.bat
```

### Run
```bash
cd backend/build/release && ./mw-server.exe
# HTTP :80 → HTTPS :443, WS relay :48001
# Ouvrir https://localhost/ dans Chrome/Edge
```

### Build manuel de qmdnsengine (si pas via le .pro subdirs)
```bash
cd backend/third_party/qmdnsengine
qmake qmdnsengine.pro
nmake
```

### Deploiement
```bash
windeployqt mw-server.exe
# Copier : libcrypto-3-x64.dll, libssl-3-x64.dll
```

---

## Effort estimé (mis à jour avec l'avancement réel)

| Phase | Description | Effort | Statut |
|-------|-------------|--------|--------|
| 1 | Squelette + HTTP | 2-3 jours | ✅ Fait |
| 2 | Découverte hôtes + API | 3-4 jours | ✅ Fait |
| 3 | Pairing | 2-3 jours | ✅ Fait |
| 4 | Liste apps | 1-2 jours | ✅ Fait |
| 5a | RTSP Handshake + intgration moonlight-common-c | 3-4 jours | ✅ Fait |
| 5b | Pipeline video (MSE + Mp4Muxer + StreamRelay) | 2-3 jours | 🔄 En cours |
| 6 | Pipeline audio (AudioWorklet PCM) | 2-3 jours | ⏳ |
| 7 | Input (Gamepad, polish mapping) | 2-3 jours | ⏳ |
| 8 | Polish, erreurs, overlay stats | 3-5 jours | ⏳ |
| **Total** | | **~20-30 jours** | **~60% fait** |
