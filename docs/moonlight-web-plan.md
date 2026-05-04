# Moonlight-Web — Plan d'Architecture & Développement

## Vue d'ensemble

```
Browser (HTML/JS)              Backend (C++/Qt)               Sunshine Host
+-------------------+       +------------------------+       +----------------+
|                   |       |                        |       |                |
|  Web App          | REST  |  HTTP Server           | HTTPS |  Sunshine API  |
|  - Host List      |<----->|  - Static file serving |<----->|  /serverinfo   |
|  - App List       |       |  - REST endpoints      |       |  /applist      |
|  - Pair Dialog    |       |  - Proxy to Sunshine   |       |  /launch       |
|                   |       |                        |       |  /pair         |
|  Stream View      | WSS   |  WebSocket Server       |       |                |
|  - WebCodecs      |<----->|  - Video passthrough   |       |  RTSP/RTP/UDP  |
|  - AudioWorklet   |       |  - Audio PCM bridge    |<----->|  Moonlight-    |
|  - Input Capture  |       |  - Input bridge        |       |  Common-C      |
+-------------------+       +------------------------+       +----------------+
```

**Principe :** Le backend est un serveur HTTP + WebSocket qui sert le frontend,
proxifie les appels REST vers Sunshine, et intègre moonlight-common-c pour le
protocole RTSP/RTP. Le frontend reçoit les access units H.264 brutes (passthrough)
et les décode via WebCodecs. L'audio est forwardé en PCM brut. Pas de
re-encodage dans le backend.

---

## Structure du projet

```
moonlight-web-deepseek/
├── mw-server.pro                  # Projet Qt (TEMPLATE = subdirs)
├── backend/
│   ├── backend.pro                # Sous-projet backend
│   ├── src/
│   │   ├── main.cpp               # Entry point
│   │   ├── server/
│   │   │   ├── HttpServer.h/.cpp       # Serveur HTTP embarqué (QTcpServer)
│   │   │   ├── WebSocketServer.h/.cpp  # Endpoint WebSocket
│   │   │   ├── StaticFileHandler.h/.cpp # Fichiers statiques frontend
│   │   │   └── RestRouter.h/.cpp       # Routage REST (méthode + path)
│   │   ├── backend/
│   │   │   ├── NvHTTP.h/.cpp           # Client HTTP/HTTPS Sunshine
│   │   │   ├── NvComputer.h/.cpp       # Modèle hôte (adresses, état, capacités)
│   │   │   ├── NvApp.h/.cpp            # Modèle application distante
│   │   │   ├── ComputerManager.h/.cpp   # Gestion liste hôtes + persistence + polling
│   │   │   ├── ComputerSeeker.h/.cpp    # Découverte mDNS (_nvstream._tcp.local.)
│   │   │   ├── PairingManager.h/.cpp    # Protocole challenge-response
│   │   │   └── IdentityManager.h/.cpp   # RSA keypair, X.509 cert, UUID
│   │   ├── streaming/
│   │   │   ├── Session.h/.cpp           # Orchestrateur LiStartConnection + callbacks
│   │   │   ├── StreamConfig.h/.cpp      # Configuration fixe (1080p/60/H.264)
│   │   │   ├── VideoBridge.h/.cpp       # DECODE_UNIT → WebSocket binaire
│   │   │   ├── AudioBridge.h/.cpp       # PCM → WebSocket binaire
│   │   │   └── InputBridge.h/.cpp       # WebSocket JSON → LiSend*Event()
│   │   └── common/
│   │       ├── Logger.h/.cpp
│   │       ├── Types.h
│   │       └── Platform.h/.cpp
│   ├── third_party/
│   │   ├── moonlight-common-c/    # Submodule Git
│   │   └── qmdnsengine/           # Submodule Git
│   └── certs/                     # Généré au runtime
├── frontend/
│   ├── index.html
│   ├── css/
│   │   ├── style.css
│   │   └── stream.css
│   ├── js/
│   │   ├── app.js                 # State machine
│   │   ├── api/
│   │   │   ├── BackendClient.js   # Client REST (fetch)
│   │   │   ├── WebSocketClient.js # Client WebSocket streaming
│   │   │   └── Protocol.js        # Helpers sérialisation
│   │   ├── models/
│   │   │   ├── Host.js
│   │   │   └── App.js
│   │   ├── streaming/
│   │   │   ├── StreamSession.js   # Orchestrateur streaming frontend
│   │   │   ├── VideoPipeline.js   # WebCodecs VideoDecoder + canvas
│   │   │   ├── AudioPipeline.js   # AudioContext + AudioWorklet
│   │   │   └── audio-processor.js # AudioWorkletProcessor
│   │   ├── input/
│   │   │   ├── KeyboardHandler.js
│   │   │   ├── MouseHandler.js
│   │   │   └── GamepadHandler.js
│   │   └── ui/
│   │       ├── HostListView.js
│   │       ├── AppListView.js
│   │       ├── PairDialog.js
│   │       ├── StreamView.js
│   │       └── ConnectionStatus.js
│   └── assets/
│       └── favicon.ico
└── docs/
    ├── moonlight-qt-architecture.md
    └── moonlight-web-plan.md
```

---

## Décisions architecturales clés

### 1. Passthrough vidéo (pas de décode dans le backend)

moonlight-common-c assemble les access units H.264 dans les callbacks
`drSubmitDecodeUnit`. On forwarde ces NAL units directement au navigateur
via WebSocket. Le navigateur les décode via WebCodecs (accéléré GPU).

| Approche | Latence | Complexité | Charge |
|----------|---------|------------|--------|
| **Passthrough NAL → WebCodecs** | Minimale | Faible | GPU navigateur |
| Décode backend + re-encode | +30ms+ | Très élevée | GPU backend |
| Décode backend + MJPEG | +20ms | Moyenne | CPU backend |

### 2. Protocole WebSocket custom plutôt que WebRTC

moonlight-common-c utilise un format RTP propriétaire (FEC custom, séquencement).
Adapter vers WebRTC nécessiterait de modifier moonlight-common-c (casser le contrat
de submodule) ou implémenter une gateway complexe. Un protocole binaire simple
sur WebSocket suffit sur un LAN.

### 3. Connexion WebSocket unique

Une seule connexion multiplexe vidéo, audio, contrôle et input. Pour un scénario
LAN avec perte de paquets négligeable, le head-of-line blocking est non significatif
et la gestion du cycle de vie est plus simple.

### 4. Forward PCM audio plutôt que Opus

moonlight-common-c décode Opus multistream en interne et fournit du PCM dans
`arDecodeAndPlaySample`. Forwarder le PCM évite de shipper un décodeur Opus WASM
(~1MB+) et de réimplémenter le demuxing multistream en JS.

---

## Protocole WebSocket

### Messages binaires

```
Offset  Taille  Champ         Description
------  ------  -----         -----------
0       1       type          Message type
1       4       timestamp     Microsecondes, uint32 big-endian
5       N       payload       Données spécifiques au type
```

| Type | Nom | Payload |
|------|-----|---------|
| 0x01 | VIDEO | Flux H.264 Annex B brut. Timestamp = temps de présentation en µs. |
| 0x02 | AUDIO | PCM16 little-endian entrelacé. Bytes 1-4 = sample rate, Byte 5 = channels, reste = PCM. |
| 0x05 | VIDEO_CONFIG | NAL units SPS/PPS pour initialisation WebCodecs. Envoyé avant les IDR frames. |

### Messages texte (JSON)

**Backend → Frontend :**
```json
{"type": "config", "data": {"width": 1920, "height": 1080, "fps": 60, "codec": "H264"}}
{"type": "state",  "data": {"state": "connecting|streaming|stopped", "stage": "..."}}
{"type": "stats",  "data": {"fps": 60, "bitrate": 20000, "rtt": 5}}
{"type": "error",  "data": {"code": -100, "message": "..."}}
{"type": "rumble", "data": {"controller": 0, "lowFreq": 128, "highFreq": 64}}
```

**Frontend → Backend :**
```json
{"type": "input", "data": {"event": "keyDown", "key": 4, "modifiers": 0}}
{"type": "input", "data": {"event": "mouseMove", "deltaX": 10, "deltaY": -5}}
{"type": "input", "data": {"event": "mouseButton", "button": 1, "down": true}}
{"type": "input", "data": {"event": "mouseWheel", "delta": 1}}
{"type": "input", "data": {"event": "gamepad", "id": 0, "buttons": [...], "axes": [...]}}
```

---

## API REST

| Méthode | Path | Description |
|---------|------|-------------|
| GET | `/` | Sert `index.html` |
| GET | `/*` | Sert les fichiers statiques de `frontend/` |
| GET | `/api/hosts` | Liste tous les hôtes connus avec états |
| POST | `/api/hosts/scan` | Déclenche un scan LAN manuel |
| POST | `/api/hosts/manual` | Ajoute un hôte par IP `{"address": "..."}` |
| GET | `/api/hosts/:id/pair` | Récupère le PIN/statut de pairing |
| POST | `/api/hosts/:id/pair` | Soumet le PIN pour pairing |
| GET | `/api/hosts/:id/apps` | Liste les apps d'un hôte pairé |
| POST | `/api/hosts/:id/start` | Lance le stream sur un hôte `{"appId": "..."}` |
| POST | `/api/hosts/:id/quit` | Quitte l'app en cours sur un hôte |

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
Encode Sunshine (GPU) :          2-4ms
Réseau (LAN, <1ms RTT) :        1ms
moonlight-common-c reassembly : 1ms
Traitement backend + queue :    0.5ms
WebSocket send (localhost) :    0.5ms
WebCodecs decode (GPU) :       2-4ms
Canvas render :                 1ms
Audio (AudioWorklet) :         5-10ms
Total :                        ~13-22ms
```

Bien en-dessous du seuil de 50ms pour un streaming "local-like".

---

## Threading model — Session

```
Main Thread (Qt event loop)           Worker Thread (LiStartConnection)
   |                                         |
   |-- Session::start()                       |
   |   -> QThread::start()                    |
   |                                         |
   |                                   LiStartConnection() bloque ici
   |                                         |
   |   callbacks stage (via QMetaObject::invoke si UI, sinon direct)
   |   <--------------------------------------
   |                                         |
   |   ===== STREAMING =====                  |
   |   drSubmitDecodeUnit() (direct call, lock-free queue)
   |   <--------------------------------------  VideoBridge → WebSocket send
   |                                         |
   |   arDecodeAndPlaySample()                |
   |   <--------------------------------------  AudioBridge → WebSocket send
   |                                         |
   |   connectionTerminated()                 |
   |   <--------------------------------------  Nettoyage
```

Les envois WebSocket vidéo/audio se font **directement depuis le worker thread**
pour éviter la latence de traversée de threads. `QWebSocket::sendBinaryMessage`
est thread-safe (écriture dans le buffer socket OS).

---

## Phases de développement

### Phase 1 : Squelette du projet et serveur HTTP

**Objectif :** Un projet Qt qui compile et sert une page web.

1. Créer `mw-server.pro` (qmake `TEMPLATE = subdirs`)
2. Créer `backend/backend.pro` avec modules Qt : `core`, `network`, `websockets`
3. Implémenter `main.cpp` — parser les arguments (port), démarrer les serveurs
4. Implémenter `HttpServer` — basé sur `QTcpServer`, parsing HTTP minimal
5. Implémenter `StaticFileHandler` — servir les fichiers de `frontend/`, MIME types
6. Créer `frontend/index.html` — page d'accueil avec layout de base

**Fichiers créés :**
- `mw-server.pro`, `backend/backend.pro`
- `backend/src/main.cpp`
- `backend/src/server/HttpServer.h/.cpp`, `StaticFileHandler.h/.cpp`, `RestRouter.h/.cpp`
- `backend/src/common/Logger.h/.cpp`
- `frontend/index.html`, `frontend/css/style.css`, `frontend/js/app.js`

**Critères d'acceptation :**
- Le projet compile et s'exécute sur Windows 11 x64 avec Qt 6.x
- `http://localhost:48000/` affiche la page d'accueil
- La console affiche "Moonlight-Web server starting on port 48000"

---

### Phase 2 : Découverte d'hôtes et API backend

**Objectif :** Le backend découvre les hôtes Sunshine sur le LAN et les expose
via l'API REST. Le frontend affiche la liste des hôtes.

1. Ajouter `qmdnsengine` comme submodule Git, intégrer dans le `.pro`
2. Implémenter `NvHTTP::getServerInfo()` — HTTP GET `/serverinfo`, parsing XML
3. Implémenter `NvComputer` et `NvApp` (classes modèles)
4. Implémenter `ComputerSeeker` — mDNS browse `_nvstream._tcp.local.`
5. Implémenter `ComputerManager` — liste d'hôtes, polling 3s, persistence `QSettings`
6. Implémenter les endpoints REST : `GET /api/hosts`, `POST /api/hosts/scan`, `POST /api/hosts/manual`
7. Frontend `HostListView` — cartes d'hôtes avec icônes d'état
8. Frontend `BackendClient` — client REST (fetch)

**Fichiers créés/modifiés :**
- Backend : `NvHTTP.h/.cpp`, `NvComputer.h/.cpp`, `NvApp.h/.cpp`
- Backend : `ComputerManager.h/.cpp`, `ComputerSeeker.h/.cpp`
- Backend : `Types.h`
- Backend : mise à jour `backend.pro` (ajout qmdnsengine)
- Frontend : `BackendClient.js`, `Host.js`, `App.js`, `HostListView.js`
- Frontend : mise à jour `app.js` (state machine), `index.html`

**Critères d'acceptation :**
- Au démarrage, le backend découvre les hôtes Sunshine sur le LAN
- Le frontend affiche les hôtes en ligne avec badge "Live", hors ligne grisés
- Le bouton "Scan" déclenche un nouveau scan mDNS
- L'ajout manuel par IP fonctionne
- Les hôtes persistent entre les redémarrages (offline par défaut)

---

### Phase 3 : Pairing

**Objectif :** L'utilisateur peut pairer avec un hôte en ligne mais verrouillé.

1. Implémenter `IdentityManager` — génération RSA 2048-bit + X.509 self-signed + UUID 64-bit
2. Implémenter les méthodes pairing de `NvHTTP` : `getPairState()`, étapes challenge-response
3. Implémenter `PairingManager` — protocole challenge-response (SHA-256 + AES-128-ECB)
4. Implémenter endpoints REST : `GET /api/hosts/:id/pair`, `POST /api/hosts/:id/pair`
5. Frontend `PairDialog` — affiche le PIN, instruction de saisie sur l'hôte
6. Frontend state machine : host list → pair dialog (hôtes verrouillés) → app list

**Fichiers créés/modifiés :**
- Backend : `IdentityManager.h/.cpp`, `PairingManager.h/.cpp`
- Backend : mise à jour `NvHTTP.h/.cpp` (méthodes pairing)
- Frontend : `PairDialog.js`
- Frontend : mise à jour `HostListView.js`, `app.js`

**Critères d'acceptation :**
- Clic sur un hôte verrouillé affiche le PairDialog avec PIN
- Saisie du PIN sur l'hôte Sunshine complète le pairing
- L'état de l'hôte passe de verrouillé à disponible dans l'UI
- Le pairing persiste entre les redémarrages

---

### Phase 4 : Liste des applications

**Objectif :** L'utilisateur peut voir et sélectionner les apps d'un hôte pairé.

1. Implémenter `NvHTTP::getAppList()` — GET `/applist`, parsing XML
2. Implémenter endpoint REST : `GET /api/hosts/:id/apps`
3. Frontend `AppListView` — grille d'apps
4. Frontend state machine : host list → app list → retour host list

**Fichiers créés/modifiés :**
- Backend : mise à jour `NvHTTP.h/.cpp` (getAppList)
- Frontend : `AppListView.js`
- Frontend : mise à jour `app.js`

**Critères d'acceptation :**
- Un hôte pairé affiche sa liste d'apps au clic
- Les apps sont affichées en grille avec leur nom
- L'utilisateur peut naviguer retour vers la liste d'hôtes

---

### Phase 5 : Streaming — Pipeline Vidéo

**Objectif :** L'utilisateur peut lancer un jeu et voir la vidéo dans le navigateur.

1. Ajouter `moonlight-common-c` comme submodule Git, intégrer dans le `.pro`
2. Configurer le build de moonlight-common-c (C99, CMake)
3. Implémenter `StreamConfig` — config fixe (1080p, 60fps, H.264)
4. Implémenter `NvHTTP::startApp()` — POST `/launch`, parser l'URL RTSP
5. Implémenter `WebSocketServer` — `QWebSocketServer`, single-client
6. Implémenter `Session` avec callbacks de connexion (stage tracking d'abord)
7. Implémenter `VideoBridge` — `drSubmitDecodeUnit` → extraction H.264 → WebSocket
8. Implémenter endpoint REST : `POST /api/hosts/:id/start`
9. Frontend `StreamSession` — connecte WebSocket, route les données vidéo
10. Frontend `VideoPipeline` — WebCodecs `VideoDecoder` + rendu `<canvas>`
11. Frontend `StreamView` — canvas plein écran avec overlay

**Fichiers créés/modifiés :**
- Backend : mise à jour `backend.pro` (moonlight-common-c)
- Backend : `StreamConfig.h/.cpp`, `Session.h/.cpp`, `VideoBridge.h/.cpp`
- Backend : `WebSocketServer.h/.cpp`
- Backend : mise à jour `NvHTTP.h/.cpp` (startApp, quitApp)
- Frontend : `StreamSession.js`, `VideoPipeline.js`, `WebSocketClient.js`, `Protocol.js`
- Frontend : `StreamView.js`, `ConnectionStatus.js`
- Frontend : mise à jour `app.js`, `index.html`
- Frontend : `css/stream.css`

**Critères d'acceptation :**
- Clic sur une app la lance sur Sunshine (le jeu démarre sur l'hôte)
- Le navigateur affiche la vidéo en streaming dans un canvas
- La vidéo est fluide à 60fps avec latence <50ms
- WebCodecs décode H.264 avec accélération matérielle
- Le bouton "Quit" arrête le stream et retourne à la liste d'apps

**Points d'attention :**
- Les frames IDR peuvent faire >100KB — `QWebSocket` supporte jusqu'à 256MB
- SPS/PPS doivent être capturés et forwardés avant la première frame (message `VIDEO_CONFIG`)
- `submitDecodeUnit` est appelé dans l'ordre des frames par moonlight-common-c

---

### Phase 6 : Streaming — Pipeline Audio

**Objectif :** L'audio joue en synchronisation avec la vidéo.

1. Implémenter `AudioBridge` — `arDecodeAndPlaySample` → forward PCM via WebSocket
2. Frontend `AudioPipeline` — AudioContext + AudioWorklet
3. Frontend `audio-processor.js` — AudioWorkletProcessor

**Fichiers créés/modifiés :**
- Backend : `AudioBridge.h/.cpp`
- Backend : mise à jour `Session.h/.cpp` (enregistrement AudioBridge)
- Frontend : `AudioPipeline.js`, `audio-processor.js`

**Critères d'acceptation :**
- L'audio du jeu joue via les haut-parleurs du navigateur
- Audio sync avec la vidéo (dans ~1 frame à 60fps)
- Latence audio <20ms (AudioWorklet)
- Pas de craquements ou décrochages

---

### Phase 7 : Streaming — Input

**Objectif :** L'utilisateur peut interagir avec le stream (clavier, souris, manette).

1. Implémenter `InputBridge` — parser JSON events, appeler `LiSend*Event()`
2. Frontend `KeyboardHandler` — capture KeyboardEvent, mapping HID key codes
3. Frontend `MouseHandler` — Pointer Lock API pour souris relative, boutons
4. Frontend `GamepadHandler` — polling Gamepad API à ~60Hz
5. Connecter signal `WebSocketServer::inputEventReceived` → `InputBridge::onInputEvent`
6. Implémenter le mapping HID key codes (basé sur moonlight-qt `keyboard.cpp`)

**Fichiers créés/modifiés :**
- Backend : `InputBridge.h/.cpp`
- Backend : mise à jour `Session.h/.cpp`, `WebSocketServer.h/.cpp`
- Frontend : `KeyboardHandler.js`, `MouseHandler.js`, `GamepadHandler.js`
- Frontend : mise à jour `StreamSession.js`

**Critères d'acceptation :**
- Le clavier fonctionne dans le jeu sur l'hôte
- La souris avec pointer lock est responsive (FPS jouables)
- Les boutons et molette de souris fonctionnent
- La manette fonctionne (test Xbox/PS controller)

---

### Phase 8 : Polish et gestion d'erreurs

**Objectif :** Robustesse et UX fluide.

1. Récupération sur erreur de session — gérer les déconnexions proprement
2. Overlay de statut de connexion (avertissement connexion faible)
3. Affichage stats FPS/bitrate (depuis `connectionStatusUpdate`)
4. Arrêt gracieux — quitter l'app sur l'hôte à la fermeture du stream
5. Configuration release build, sans console sur Windows (`SUBSYSTEM:WINDOWS`)
6. Logging fichier pour diagnostics

**Critères d'acceptation :**
- Perte de connexion → overlay d'erreur avec option "Reconnect"
- FPS et bitrate affichés dans l'overlay
- Quitter le stream depuis le navigateur quitte l'app sur Sunshine
- Logs écrits dans un fichier pour troubleshooting

---

## Défis et mitigations

| Défi | Risque | Mitigation |
|------|--------|------------|
| **Disponibilité WebCodecs** | Non supporté sur tous les navigateurs | Cibler Chrome/Edge Windows 11. Fallback MSE post-MVP. |
| **Taille messages WebSocket** | IDR frames ~150KB | `QWebSocket` supporte jusqu'à 256MB. Split si nécessaire. |
| **AudioContext autoplay** | Bloqué sans gesture utilisateur | Resume sur le clic de lancement d'app. |
| **Pointer Lock** | Nécessite gesture utilisateur | Activer au premier clic sur le canvas. |
| **mDNS sur Windows** | Pas de répondeur mDNS natif | `qmdnsengine` gère ça. Fallback : ajout IP manuel. |
| **Thread safety QWebSocket** | Écriture depuis worker thread | Utiliser `QMetaObject::invokeMethod` + `Qt::QueuedConnection` ou queue protégée par mutex drainée par timer sur main thread. |
| **Build moonlight-common-c Windows** | C99, peut nécessiter ajustements MSVC | CMake build. Définir `_CRT_SECURE_NO_WARNINGS` si nécessaire. |
| **Dérive AV sync** | Callbacks audio/vidéo non synchronisés | Les deux flux portent des timestamps. Pour le MVP, accepter une dérive mineure (<50ms imperceptible). |
| **Gamepad API** | Nécessite HTTPS ou localhost | Dev sur localhost. Prod avec certificat self-signed. |
| **Rumble/force feedback** | Callbacks à forwarder au navigateur | MVP : logger les requêtes rumble. Post-MVP : implémenter via Gamepad API `hapticActuators`. |

---

## Dépendances — Submodules Git

```bash
# Phase 2: mDNS discovery
git submodule add https://github.com/cgutman/qmdnsengine.git \
    backend/third_party/qmdnsengine

# Phase 5: Streaming protocol + H.264 parsing
git submodule add https://github.com/moonlight-stream/moonlight-common-c.git \
    backend/third_party/moonlight-common-c
git submodule add https://github.com/aizvorski/h264bitstream.git \
    backend/third_party/h264bitstream
```

### Fichiers wrapper nécessaires à la racine de chaque submodule

Moonlight-qt utilise une structure "wrapper" : un dossier externe avec un `.pro`
contient le submodule Git. Les fichiers suivants doivent être créés manuellement
car ils ne font pas partie des submodules upstream :

**qmdnsengine** — Fichier à créer à la racine du submodule :
- `qmdnsengine_export.h` — Macro `QMDNSENGINE_EXPORT` (vide pour build statique)

**moonlight-common-c** (Phase 5) :
- Pas de fichier wrapper nécessaire ; le `.pro` référence directement les sources

**h264bitstream** (Phase 5) :
- Pas de fichier wrapper nécessaire ; le `.pro` référence directement les sources

### Approche d'intégration

Pour éviter la complexité des subdirs Qt, les sources des bibliothèques tierces
sont compilées **directement dans le projet backend** (ajoutées dans `backend.pro`).
Ceci garantit la compatibilité ABI (même compilateur, mêmes flags) et simplifie
le build (pas de projet séparé à configurer).

## Build des dépendances (Windows)

Aucun build séparé n'est nécessaire. Toutes les sources tierces sont compilées
directement par `backend.pro`. Voir la section Configuration build Qt ci-dessous.

---

## Configuration build Qt

**`mw-server.pro` :**
```qmake
TEMPLATE = subdirs
CONFIG += ordered
SUBDIRS = backend
```

**`backend/backend.pro`** (modules et libs) :
```qmake
QT += core network websockets
CONFIG += c++17 console
TEMPLATE = app

INCLUDEPATH += third_party/moonlight-common-c
LIBS += -Lthird_party/moonlight-common-c/build -lmoonlight-common-c

INCLUDEPATH += third_party/qmdnsengine/src
LIBS += -Lthird_party/qmdnsengine/build -lqmdnsengine

win32 {
    LIBS += -lWS2_32 -lcrypt32 -ladvapi32
    CONFIG(release, debug|release) {
        QMAKE_LFLAGS += /SUBSYSTEM:WINDOWS
    }
}

macx {
    LIBS += -framework Security -framework CoreFoundation
}

unix:!macx {
    LIBS += -lpthread -ldl
}
```

---

## Actions manuelles dans Qt Creator

### Initialisation du projet

1. **Installer Qt 6.x** avec les modules : MSVC 2022 64-bit, Qt WebSockets, Qt Network
2. **Ouvrir Qt Creator** → File → Open File or Project → sélectionner `mw-server.pro`
3. **Configurer le kit** : choisir "Desktop Qt 6.x MSVC2022 64bit"
4. **Cloner les submodules** manuellement (ou via Git Bash) :
   ```bash
   cd d:\Code\moonlight-web-deepseek
   git init
   git submodule add https://github.com/moonlight-stream/moonlight-common-c backend/third_party/moonlight-common-c
   git submodule add https://github.com/moonlight-stream/qmdnsengine backend/third_party/qmdnsengine
   ```
5. **Builder les dépendances** (voir section Build des dépendances ci-dessus)
6. **Build** : Ctrl+B dans Qt Creator
7. **Run** : Ctrl+R — le serveur démarre sur `http://localhost:48000`

### Configuration additionnelle

- Ajouter OpenSSL au PATH Windows si les DLL ne sont pas trouvées
- Pour le déploiement : `windeployqt` pour empaqueter les DLL Qt avec l'exécutable

---

## Effort estimé

| Phase | Description | Effort estimé |
|-------|-------------|---------------|
| 1 | Squelette + HTTP | 2-3 jours |
| 2 | Découverte hôtes + API | 3-4 jours |
| 3 | Pairing | 2-3 jours |
| 4 | Liste apps | 1-2 jours |
| 5 | Pipeline vidéo | 4-5 jours |
| 6 | Pipeline audio | 2-3 jours |
| 7 | Input | 2-3 jours |
| 8 | Polish | 3-5 jours |
| **Total** | | **19-28 jours** |

Total estimé : **4-6 semaines** pour un développeur familier avec Qt/C++ et les APIs web modernes.
