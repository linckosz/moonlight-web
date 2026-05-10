# Moonlight-Qt — Architecture Applicative

Analyse du code source de `D:\Code\moonlight-qt\app` — référence pour le projet Moonlight-Web.

---

## 1. Structure des dossiers

```
moonlight-qt/
├── moonlight-common-c/          # Bibliothèque C cœur du protocole GameStream
│   └── src/
│       ├── Limelight.h          # API publique du client streaming (STREAM_CONFIGURATION, callbacks)
│       ├── Connection.c         # RTSP handshake + contrôle de session
│       ├── ControlStream.c      # Canal de contrôle (ENet) — input, messages
│       ├── InputStream.c        # Canal d'input (contrôle manette/clavier réseau)
│       ├── AudioStream.c        # Réception flux audio RTP
│       ├── RtpVideoQueue.c      # File de réception vidéo RTP (réassemblage, FEC)
│       ├── RtpAudioQueue.c      # File de réception audio RTP
│       ├── PlatformSockets.c    # Abstraction sockets (TCP/UDP)
│       ├── PlatformCrypto.c     # Abstraction chiffrement (AES)
│       └── Platform.c           # Abstraction plateforme (threads, temps)
│
└── app/
    ├── main.cpp                 # Point d'entrée — QGuiApplication + QQmlApplicationEngine
    ├── app.pro                  # Projet Qt (qmake)
    │
    ├── backend/                 # Communication API avec l'hôte (Sunshine/GameStream)
    │   ├── nvhttp.h/.cpp        # Client HTTP/HTTPS — /serverinfo, /launch, /resume, /applist, /pair
    │   ├── nvcomputer.h/.cpp    # Modèle hôte (adresses, apps, état, pairing)
    │   ├── nvapp.h/.cpp         # Modèle application distante (ID, nom, HDR support)
    │   ├── nvaddress.h/.cpp     # Adresse réseau (IPv4/IPv6, ports)
    │   ├── computermanager.h/.cpp    # Gestionnaire de la liste d'hôtes (découverte, persistance)
    │   ├── computerseeker.h/.cpp     # Découverte réseau (mDNS/SSDP)
    │   ├── nvpairingmanager.h/.cpp   # Appairage PIN avec l'hôte
    │   ├── identitymanager.h/.cpp    # Identité client (certificat SSL, UUID)
    │   ├── boxartmanager.h/.cpp      # Téléchargement/cache des jaquettes de jeux
    │   ├── autoupdatechecker.h/.cpp  # Vérification de mise à jour
    │   ├── richpresencemanager.h/.cpp # Intégration Discord Rich Presence
    │   └── systemproperties.h/.cpp   # Propriétés système (GPU, OS)
    │
    ├── streaming/               # Moteur de streaming cœur
    │   ├── session.h/.cpp       # Orchestrateur de session (connexion → decode → render)
    │   ├── streamutils.h/.cpp   # Utilitaires (URL, logging, statistiques)
    │   ├── bandwidth.h/.cpp     # Suivi bande passante
    │   │
    │   ├── video/               # Décodage et rendu vidéo
    │   │   ├── decoder.h        # Interface abstraite IVideoDecoder
    │   │   ├── ffmpeg.h/.cpp    # Décodeur FFmpeg (implémente IVideoDecoder)
    │   │   ├── ffmpeg_videosamples.cpp  # Frames de test pour validation décodeur
    │   │   ├── slvid.h/.cpp     # Décodeur alternatif SDL (SLVideo)
    │   │   ├── overlaymanager.h/.cpp    # Overlay statistiques perfs
    │   │   └── ffmpeg-renderers/
    │   │       ├── renderer.h           # Interface abstraite IFFmpegRenderer
    │   │       ├── cuda.h/.cpp          # Rendu accéléré CUDA (NVIDIA)
    │   │       ├── d3d11va.h/.cpp       # Rendu Direct3D 11 Video Acceleration
    │   │       ├── dxva2.h/.cpp         # Rendu DirectX Video Acceleration 2 (D3D9)
    │   │       ├── plvk.h/.cpp          # Rendu Vulkan via libplacebo
    │   │       ├── drm.h/.cpp           # Rendu DRM (Linux — KMS/GBM)
    │   │       ├── eglvid.h/.cpp        # Rendu EGL/OpenGL ES
    │   │       ├── mmal.h/.cpp          # Rendu MMAL (Raspberry Pi)
    │   │       ├── sdlvid.h/.cpp        # Rendu SDL (fallback software)
    │   │       ├── vaapi.h/.cpp         # Rendu VA-API (Intel/AMD Linux)
    │   │       ├── vdpau.h/.cpp         # Rendu VDPAU (NVIDIA Linux legacy)
    │   │       ├── vt_metal.mm          # Rendu VideoToolbox + Metal (Apple Silicon)
    │   │       ├── vt_avsamplelayer.mm  # Rendu VideoToolbox AVSampleBufferDisplayLayer (Apple)
    │   │       ├── swframemapper.h/.cpp # Mapper CPU pour textures software
    │   │       ├── genhwaccel.h/.cpp    # Accélération matérielle générique FFmpeg
    │   │       └── pacer/               # Pacement de frames (VSync)
    │   │           ├── pacer.h/.cpp
    │   │           ├── dxvsyncsource.h/.cpp
    │   │           └── waylandvsyncsource.h/.cpp
    │   │
    │   ├── audio/               # Décodage et rendu audio
    │   │   ├── audio.cpp        # Décodage Opus multistream
    │   │   └── renderers/
    │   │       ├── renderer.h   # Interface abstraite IAudioRenderer
    │   │       ├── sdlaud.cpp   # Rendu audio SDL (fallback)
    │   │       └── slaud.h/.cpp # Rendu audio SL (alternatif)
    │   │
    │   └── input/               # Gestion des périphériques d'entrée
    │       ├── input.h/.cpp     # SdlInputHandler — dispatch événements SDL vers réseau
    │       ├── gamepad.cpp      # Mapping manettes → protocole GameStream
    │       ├── keyboard.cpp     # Mapping clavier → protocole GameStream
    │       ├── mouse.cpp        # Mapping souris → protocole GameStream
    │       ├── abstouch.cpp     # Événements tactiles absolus
    │       └── reltouch.cpp     # Événements tactiles relatifs
    │
    ├── gui/                     # Interface utilisateur Qt/QML
    │   ├── main.qml             # Fenêtre principale QML
    │   ├── PcView.qml           # Vue liste des hôtes
    │   ├── AppView.qml          # Vue liste des applications d'un hôte
    │   ├── StreamSegue.qml      # Transition vers le streaming
    │   ├── SettingsView.qml     # Vue paramètres
    │   ├── computermodel.cpp    # Modèle Qt pour la liste d'hôtes
    │   ├── appmodel.cpp         # Modèle Qt pour la liste d'apps
    │   └── sdlgamepadkeynavigation.cpp  # Navigation clavier/manette dans l'UI
    │
    ├── cli/                     # Interface ligne de commande
    │   ├── startstream.cpp      # Lancer un stream via CLI
    │   ├── quitstream.cpp       # Arrêter un stream via CLI
    │   ├── listapps.cpp         # Lister les apps d'un hôte
    │   ├── pair.cpp             # Appairage via CLI
    │   └── commandlineparser.cpp # Parsing arguments CLI
    │
    ├── settings/                # Préférences utilisateur persistantes
    │   ├── streamingpreferences.h/.cpp  # Toutes les préférences de streaming
    │   └── mappingmanager.h/.cpp        # Gestion mapping manettes
    │
    ├── shaders/                 # Shaders GPU pour rendu vidéo
    └── SDL_GameControllerDB/    # Base de données mappings manettes
```

---

## 2. Composants principaux

### 2.1 Couche protocole — Limelight C Library

Bibliothèque C pure (`moonlight-common-c/`) qui implémente le protocole GameStream (NVIDIA) / Sunshine.

**Fonctions clés exposées dans `Limelight.h` :**
- `LiStartConnection()` — établit la connexion streaming (RTSP handshake)
- `LiStopConnection()` — termine la connexion
- `LiSendMouseMoveEvent()`, `LiSendKeyboardEvent()`, etc. — envoi d'événements input

**Structure clé `STREAM_CONFIGURATION` :**
```c
typedef struct _STREAM_CONFIGURATION {
    int width, height, fps, bitrate;
    int packetSize;
    int streamingRemotely;     // LOCAL, REMOTE, AUTO
    int audioConfiguration;    // stéréo, 5.1, 7.1
    int supportedVideoFormats; // masque H.264, HEVC, AV1
    int clientRefreshRateX100;
    int colorSpace, colorRange;
    int encryptionFlags;
    char remoteInputAesKey[16], remoteInputAesIv[16];
} STREAM_CONFIGURATION;
```

**Flux réseau :**
- **RTSP** — handshake initial, négociation codecs, échange clés AES
- **RTP Vidéo** — flux UDP/TCP, réassemblage, correction FEC (Video Queue)
- **RTP Audio** — flux UDP, décodage Opus multistream
- **Control Stream (ENet)** — canal fiable pour input et messages de contrôle

### 2.2 Session (Orchestrateur)

`Session` ([session.h](D:\Code\moonlight-qt\app\streaming\session.h)) est le cœur applicatif :

```
Session::exec()
  ├── startConnectionAsync()    → Thread async pour LiStartConnection()
  ├── initializeAudioRenderer() → Création IAudioRenderer + OpusDecoder
  ├── chooseDecoder()           → Sélection IVideoDecoder (HW/SW)
  └── Boucle événements SDL     → Input → ControlStream, Video → Decode → Render
```

**Cycle de vie :**
1. Construction : reçoit `NvComputer`, `NvApp`, `StreamingPreferences`
2. `initialize()` : crée la fenêtre SDL, sélectionne le décodeur vidéo
3. `start()` : lance la connexion réseau + boucle de streaming
4. Callbacks Limelight → signaux Qt (stageStarting, stageFailed, sessionFinished)

### 2.3 Décodeurs vidéo

**Interface `IVideoDecoder`** ([decoder.h](D:\Code\moonlight-qt\app\streaming\video\decoder.h)) :

```cpp
class IVideoDecoder {
    virtual bool initialize(PDECODER_PARAMETERS params) = 0;
    virtual bool isHardwareAccelerated() = 0;
    virtual int getDecoderCapabilities() = 0;
    virtual int submitDecodeUnit(PDECODE_UNIT du) = 0;  // Données réseau → décodeur
    virtual void renderFrameOnMainThread() = 0;           // Appelé au prochain tick SDL
    virtual void setHdrMode(bool enabled) = 0;
};
```

**Implémentations :**
| Classe | Fichier | Description |
|--------|---------|-------------|
| `FFmpegVideoDecoder` | `ffmpeg.h/.cpp` | Décodeur FFmpeg avec pipeline complet (choisit backend + frontend renderer) |
| `SLVideoDecoder` | `slvid.h/.cpp` | Décodeur alternatif SDL |

**Sélection du décodeur** (`Session::chooseDecoder()`) :
1. Essaie les décodeurs hardware dans l'ordre de priorité
2. Teste avec une frame de test (H.264/HEVC/AV1 selon le format)
3. Fallback vers décodeur logiciel si le hardware échoue

### 2.4 Renderers vidéo (backends graphiques)

**Interface `IFFmpegRenderer`** ([renderer.h](D:\Code\moonlight-qt\app\streaming\video\ffmpeg-renderers\renderer.h)) :

```cpp
class IFFmpegRenderer : public Overlay::IOverlayRenderer {
    virtual bool initialize(PDECODER_PARAMETERS params) = 0;
    virtual bool prepareDecoderContext(AVCodecContext*, AVDictionary**) = 0;
    virtual void renderFrame(AVFrame* frame) = 0;
    virtual AVPixelFormat getPreferredPixelFormat(int videoFormat);
    virtual int getRendererAttributes();  // FULLSCREEN_ONLY, 1080P_MAX, HDR_SUPPORT, etc.
};
```

**Architecture deux-renderers :**
- **Backend renderer** — effectue le décodage accéléré (ex: D3D11VA, CUDA, VAAPI)
- **Frontend renderer** — effectue le rendu à l'écran (conversion CSC, scaling, HDR)

**Renderers disponibles (RendererType enum) :**

| Renderer | Plateforme | Type |
|----------|-----------|------|
| **Vulkan (libplacebo)** | Cross-platform | Moderne, HDR, framepacing |
| **CUDA** | NVIDIA Windows/Linux | Décodage NVDEC + rendu CUDA |
| **D3D11VA** | Windows | Direct3D 11 Video Acceleration |
| **DXVA2 (D3D9)** | Windows legacy | DirectX Video Acceleration 2 |
| **VAAPI** | Linux (Intel/AMD) | Video Acceleration API |
| **VDPAU** | Linux (NVIDIA legacy) | Video Decode and Presentation |
| **DRM** | Linux (KMS/GBM) | Direct Rendering Manager |
| **EGL/GLES** | Linux embedded | OpenGL ES via EGL |
| **MMAL** | Raspberry Pi | Multi-Media Abstraction Layer |
| **VideoToolbox** | macOS/iOS | Hardware decoding Apple |
| **SDL** | Cross-platform | Fallback software |

**Attributs renderer** (masque de bits) :
- `RENDERER_ATTRIBUTE_FULLSCREEN_ONLY` — ne fonctionne qu'en plein écran
- `RENDERER_ATTRIBUTE_1080P_MAX` — résolution max 1080p
- `RENDERER_ATTRIBUTE_HDR_SUPPORT` — support HDR
- `RENDERER_ATTRIBUTE_NO_BUFFERING` — pas de buffering (latence minimale)
- `RENDERER_ATTRIBUTE_FORCE_PACING` — force le pacement de frames

### 2.5 Audio

- **Décodage** : Opus multistream (`OpusMSDecoder`)
- **Interface `IAudioRenderer`** ([renderer.h](D:\Code\moonlight-qt\app\streaming\audio\renderers\renderer.h)) :
  ```cpp
  class IAudioRenderer {
      virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION*) = 0;
      virtual void* getAudioBuffer(int* size) = 0;
      virtual bool submitAudio(int bytesWritten) = 0;
  };
  ```
- **Implémentations** : SDL (fallback), SL (alternatif)

### 2.6 Input

**`SdlInputHandler`** ([input.h](D:\Code\moonlight-qt\app\streaming\input\input.h)) :
- Capture les événements SDL (clavier, souris, manette, touch)
- Convertit en appels `LiSend*()` du protocole GameStream
- Gère les combos de touches spéciales (quitter, plein écran, stats overlay)
- Support multi-manettes (jusqu'à 16), DualSense (triggers adaptatifs, LED)
- Modes souris absolu/relatif, capture de curseur, toucher absolu/relatif

### 2.7 Communication hôte — Backend API

**`NvHTTP`** ([nvhttp.h](D:\Code\moonlight-qt\app\backend\nvhttp.h)) — client HTTP/HTTPS :
- `getServerInfo()` → `/serverinfo` — capacités du serveur, modes affichage, codecs
- `getAppList()` → `/applist` — liste des applications streamables
- `startApp()` → `/launch` — lance une app (retourne l'URL RTSP)
- `quitApp()` → `/quit` — ferme l'app en cours
- `getBoxArt()` → `/boxart` — jaquette du jeu

**`NvComputer`** ([nvcomputer.h](D:\Code\moonlight-qt\app\backend\nvcomputer.h)) :
- État de l'hôte : online/offline, paired/not_paired
- Adresses réseau (locale, distante, IPv6, manuelle)
- Liste des apps, modes d'affichage, capacités du serveur
- Persistance via `QSettings`

---

## 3. Flux de données

```
┌─────────────────────────────────────────────────────────────────────┐
│                         HÔTE SUNSHINE                                │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ HTTPS API │  │ RTSP     │  │ RTP Video│  │ RTP Audio+Input  │   │
│  │ :47984    │  │ :48010   │  │ :47998   │  │ :48000/:48010    │   │
│  └─────┬─────┘  └────┬─────┘  └────┬─────┘  └────────┬─────────┘   │
└────────┼──────────────┼─────────────┼─────────────────┼─────────────┘
         │              │             │                  │
         ▼              ▼             ▼                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     MOONLIGHT-QT CLIENT                             │
│                                                                     │
│  ┌──────────────┐   ┌──────────────────────────────────────────┐   │
│  │   NvHTTP     │   │        Limelight C Library                │   │
│  │  (backend/)  │   │  ┌──────────┐ ┌───────────┐ ┌─────────┐ │   │
│  │              │   │  │Connection│ │RtpVideoQ  │ │RtpAudioQ│ │   │
│  │ • /serverinfo│   │  │(RTSP)    │ │(réass.)   │ │(réass.) │ │   │
│  │ • /applist   │   │  └──────────┘ └─────┬─────┘ └────┬────┘ │   │
│  │ • /launch    │   │                     │            │       │   │
│  │ • /pair      │   │                     ▼            ▼       │   │
│  └──────┬───────┘   │  ┌──────────────────────────────────┐   │   │
│         │           │  │         Session                   │   │   │
│         ▼           │  │  ┌────────────┐ ┌──────────────┐ │   │   │
│  ┌──────────────┐   │  │  │IVideoDecoder│ │IAudioRenderer│ │   │   │
│  │ NvComputer   │   │  │  │(FFmpeg)     │ │(SDL)         │ │   │   │
│  │ NvApp        │   │  │  └──────┬─────┘ └──────┬───────┘ │   │   │
│  │ (modèles)    │   │  │         │              │         │   │   │
│  └──────┬───────┘   │  │         ▼              ▼         │   │   │
│         │           │  │  ┌──────────────┐ ┌──────────┐  │   │   │
│         ▼           │  │  │IFFmpegRenderer│ │SDL Audio │  │   │   │
│  ┌──────────────┐   │  │  │(HW Accel)    │ │Device    │  │   │   │
│  │  GUI (QML)   │   │  │  └──────┬───────┘ └──────────┘  │   │   │
│  │  main.qml    │   │  └─────────┼────────────────────────┘   │   │
│  │  PcView.qml  │   │            │                             │   │
│  │  AppView.qml │   │            ▼                             │   │
│  └──────────────┘   │  ┌────────────────┐                      │   │
│                     │  │  SDL Window    │                      │   │
│  ┌──────────────┐   │  │  (affichage)   │                      │   │
│  │ SdlInputHdlr │───┼──│> ControlStream │                      │   │
│  │(input/)      │   │  └────────────────┘                      │   │
│  └──────────────┘   └──────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

**Séquence type d'un stream :**
1. **Découverte** : `ComputerSeeker` (mDNS/SSDP) → `ComputerManager` → UI
2. **Sélection** : UI → `NvHTTP::getAppList()` → `NvComputer.appList`
3. **Lancement** : `NvHTTP::startApp(STREAM_CONFIGURATION)` → URL RTSP
4. **Handshake RTSP** : `LiStartConnection(streamConfig)` → négociation codecs + clés
5. **Streaming** : Boucle RTP → `IVideoDecoder::submitDecodeUnit(PDECODE_UNIT)` → `IFFmpegRenderer::renderFrame(AVFrame*)` → SDL Window + `IAudioRenderer::submitAudio()`
6. **Input** : `SdlInputHandler` → `LiSend*Event()` → ControlStream (ENet)

---

## 4. Patterns architecturaux

### 4.1 Strategy Pattern — Renderers
`IFFmpegRenderer` définit l'interface, chaque backend (D3D11VA, CUDA, Vulkan, etc.) est une stratégie concrète. La sélection est faite dynamiquement dans `FFmpegVideoDecoder` selon la plateforme, le GPU, et les préférences utilisateur.

### 4.2 Pipeline Décodage/Rendu en deux étapes
- **Backend renderer** : intégré à FFmpeg via `AVCodecContext::get_format` + `hw_frames_ctx` pour le décodage accéléré
- **Frontend renderer** : reçoit les `AVFrame*` décodés et les affiche (conversion CSC, scaling, HDR tone-mapping si nécessaire)
- Les deux peuvent être le même objet (ex: CUDA fait les deux) ou séparés (ex: VAAPI backend + Vulkan frontend)

### 4.3 Observer/Delegate — Callbacks Limelight → Qt
Les callbacks C de Limelight (`CONNECTION_LISTENER_CALLBACKS`) sont pontées vers des signaux Qt dans `Session` :
```cpp
Session::clStageFailed() → emit s_ActiveSession->stageFailed(...)
Session::clConnectionTerminated() → emit s_ActiveSession->sessionFinished(...)
```

### 4.4 Abstraction de plateforme
- **Limelight** : abstraction sockets/crypto/threads (`Platform.h`)
- **Renderers** : chaque backend encapsule les appels API graphiques natives
- **Compilation conditionnelle** : `#ifdef Q_OS_WIN32`, `#ifdef HAVE_FFMPEG`, etc.

### 4.5 Preferences-driven configuration
`StreamingPreferences` est un singleton QObject qui persiste via `QSettings`. Toutes les décisions (décodeur, résolution, FPS, bitrate) sont dérivées des préférences.

---

## 5. Intégration Moonlight-Web — Décisions réelles

### 5.1 Architecture d'intégration

Moonlight-Web intègre directement moonlight-common-c dans son backend C++/Qt.
Contrairement à moonlight-qt qui lie LiStartConnection à une boucle SDL + décodeur
FFmpeg + renderer GPU, moonlight-Web remplace toute la partie décodage/rendu par
un relay WebSocket vers le navigateur.

```
┌────────────────────────────────────────────────────────────────────────┐
│                         MOONLIGHT-WEB (Backend C++/Qt)                  │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────┐          │
│  │                Limelight C Library                        │          │
│  │  ┌──────────┐ ┌───────────┐ ┌─────────┐ ┌────────────┐ │          │
│  │  │Connection│ │RtpVideoQ  │ │RtpAudioQ│ │ControlStrm │ │          │
│  │  │(RTSP)    │ │(réass.)   │ │(réass.) │ │(ENet)      │ │          │
│  │  └──────────┘ └─────┬─────┘ └────┬────┘ └─────┬──────┘ │          │
│  │                     │            │             │        │          │
│  │                     ▼            ▼             ▼        │          │
│  │              ┌─────────────┐ ┌────────┐ ┌──────────┐   │          │
│  │              │MoonlightShim│ │(Opus→  │ │InputCryp │   │          │
│  │              │(callbacks C │ │ PCM)   │ │to+AES-   │   │          │
│  │              │ → Qt sigs) │ │        │ │128-GCM   │   │          │
│  │              └──────┬──────┘ └────────┘ └──────────┘   │          │
│  └─────────────────────┼──────────────────────────────────┘          │
│                        │                                            │
│                        ▼                                            │
│  ┌──────────────────────────────────────────────┐                   │
│  │              StreamRelay                      │                   │
│  │  WebSocket → [channel:1][flags:1][payload:N]  │                   │
│  │  • Video (ch 0x01) — NAL units H.264 Annex B  │                   │
│  │  • Audio (ch 0x02) — PCM16 LE                 │                   │
│  │  • Input (JSON)  — key/mouse events           │                   │
│  └──────────────────────┬───────────────────────┘                   │
│                         │                                            │
│                         ▼                                            │
│  ┌──────────────────────────────────────────────┐                   │
│  │              EnetControlStream                │                   │
│  │  ENet reliable UDP → START_A/B + input        │                   │
│  │  (port 47999, moonlight-common-c ENet)        │                   │
│  └──────────────────────────────────────────────┘                   │
└─────────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    NAVIGATEUR (Frontend Vanilla JS)                   │
│                                                                      │
│  ┌────────────┐  ┌───────────────────────────────────────────────┐  │
│  │ BackendCli │  │               StreamView                       │  │
│  │ ent (fetch)│  │  ┌──────────────┐  ┌───────────────────────┐ │  │
│  │            │  │  │ WebSocketCli │  │  MSE + <video> tag    │ │  │
│  │ /api/hosts │  │  │  ent         │  │  MediaSource           │ │  │
│  │ /api/start │  │  │              │  │  SourceBuffer          │ │  │
│  │ /api/quit  │  │  │  onBinary →  │  │  Mp4Muxer (fMP4)      │ │  │
│  └────────────┘  │  │  onText   →  │  └───────────────────────┘ │  │
│                  │  └──────┬───────┘                             │  │
│                  │         │ send(JSON input)                     │  │
│                  └─────────┼─────────────────────────────────────┘  │
│                            │                                        │
│                    KeyboardEvent, MouseEvent,                        │
│                    PointerLock API                                   │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.2 Décisions architecturales clés

| Décision | Description |
|----------|-------------|
| **LiStartConnection** intégré dans le backend | Pas de réimplémentation RTSP en JS. moonlight-common-c gère tout le handshake (OPTIONS→DESCRIBE→SETUP×3→ANNOUNCE→PLAY) directement dans le backend C++. |
| **MoonlightShim** pont C→Qt | Wrapper QObject qui expose les callbacks C de Limelight (`drSubmitDecodeUnit`, `arDecodeAndPlaySample`, `clConnectionStarted`) comme signaux Qt. Tourne sur un QThread dédié. |
| **ENet natif** pour le contrôle | Utilisation de l'ENet embarqué dans moonlight-common-c pour le canal de contrôle (START_A/B handshake, input). Pas de pont WebSocket. |
| **StreamRelay** (remplace Video/Audio/InputBridge) | Un seul objet QObject relaye vidéo + audio + input entre MoonlightShim et le navigateur via un WebSocket dédié (port 48001). |
| **MSE + fMP4** plutôt que WebCodecs | Le frontend utilise MediaSource Extensions avec un muxeur fMP4 minimal (Mp4Muxer.js) qui encapsule les NAL units H.264 Annex B en segments fMP4. Décodé dans un élément `<video>` standard. |
| **Pas de pipeline audio encore** | L'audio PCM est forwardé via WebSocket (channel 0x02) mais n'est pas encore joué. Phase 6 à venir. |
| **Pas de VideoPipeline/AudioPipeline séparés** | Tout le streaming frontend est dans `StreamView.js`. Pas de modules `VideoPipeline.js` ou `AudioPipeline.js`. |
| **Input relayé par StreamRelay** | Les events JSON du navigateur sont parsés directement par `StreamRelay::onWsTextMessage()` qui appelle `InputEncoder` → `EnetControlStream::sendInput()`. Pas d'InputBridge séparé. |

### 5.3 Protocole WebSocket réel

**Messages binaires (Backend → Frontend) :**

```
[channel:1][flags:1][payload:N]
```

| Channel | Nom | Payload |
|---------|-----|---------|
| 0x01 | VIDEO | NAL units H.264 en format Annex B (start codes 00 00 00 01 préfixés). flags bit0 = 1 si IDR keyframe. |
| 0x02 | AUDIO | PCM16 little-endian entrelacé. flags inutilisés. |

**Messages texte (Frontend → Backend) — input :**

```json
{"type":"keydown","keyCode":65,"ctrlKey":false,"shiftKey":false}
{"type":"mousemove","dx":10,"dy":-5}
{"type":"mousedown","button":1}
{"type":"mousewheel","delta":-120}
```

Pas de messages texte backend→frontend pour l'instant (state/stats/error viendront en Phase 8).

### 5.4 Flux réel d'un stream

1. **POST /api/hosts/:id/start** (async) → crée `StreamSession`
2. `StreamSession::start()` → `NvHTTP::launchAppAsync()` (HTTPS /launch)
3. Parse sessionUrl du XML de réponse → crée `MoonlightShim` + `StreamRelay` (WebSocket)
4. `MoonlightShim::startConnection()` → appelle `LiStartConnection()` sur QThread dédié
5. moonlight-common-c : handshake RTSP → bind UDP → RTP video+audio → callbacks
6. `drSubmitDecodeUnit()` → `MoonlightShim::videoFrameReady` signal → `StreamRelay` forwarde via WebSocket
7. `arDecodeAndPlaySample()` → `MoonlightShim::audioSampleReady` signal → `StreamRelay` forwarde via WebSocket
8. `clConnectionStarted()` → `m_StreamStarted = true`, client WebSocket peut se connecter
9. Frontend : WebSocket connect → reçoit frames vidéo → `Mp4Muxer` → fMP4 → `SourceBuffer` → `<video>`
10. Input : events navigateur → WebSocket JSON → `StreamRelay::onWsTextMessage()` → `InputEncoder` → AES-128-GCM → `EnetControlStream` → ENet → Sunshine

### 5.5 Structures de données clés

Identiques à moonlight-qt (mêmes headers Limelight.h) :
- **`STREAM_CONFIGURATION`** ([Limelight.h](D:\Code\moonlight-qt\moonlight-common-c\moonlight-common-c\src\Limelight.h))
- **`DECODE_UNIT`** — unité de décodage (frames réseau réassemblées)
- **`OPUS_MULTISTREAM_CONFIGURATION`** — configuration audio
- **`SERVER_INFORMATION`** — infos serveur (adresse, versions, codec support)

### 5.6 Limites connues du MVP actuel

| Point | Statut | Description |
|-------|--------|-------------|
| Audio | Non joué | Forwardé via WebSocket mais pas décodé/joué côté navigateur |
| Gamepad | Non implémenté | Phase 7 |
| Overlay stats | Non implémenté | Phase 8 |
| HEVC/AV1 | Non testé | Seul H.264 est utilisé (codec fixe) |
| Rumble | Non implémenté | Callback `clRumble` vide |
| HDR | Non implémenté | Callback `clSetHdrMode` vide |
