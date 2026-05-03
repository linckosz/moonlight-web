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

## 5. Points d'intégration pour Moonlight-Web

### 5.1 Protocoles à réimplémenter

| Protocole | Description | Approche Web |
|-----------|-------------|--------------|
| **HTTPS API** (`/serverinfo`, `/applist`, `/launch`, `/pair`, `/quit`) | REST API de Sunshine | `fetch()` / `XMLHttpRequest` — pas de contrainte particulière |
| **RTSP Handshake** | Négociation session, échange SDP, clés AES | Doit être réimplémenté en JS. Sunshine supporte aussi WebRTC (expérimental) |
| **RTP Video** | Flux UDP/TCP H.264/HEVC/AV1 | WebCodecs API (`VideoDecoder`) ou MSE via `MediaSource` |
| **RTP Audio** | Flux UDP Opus multistream | Web Audio API (`AudioContext`) + Opus decoder WASM/JS |
| **Control Stream** | Canal ENet pour input et messages | WebSocket (Sunshine a un pont WebSocket, ou implémenter ENet en WASM) |

### 5.2 APIs Sunshines existantes réutilisables

Sunshine expose ces endpoints (documentés dans le code de NvHTTP) :
- `GET /serverinfo` — XML avec capacités serveur, modes d'affichage, codecs supportés
- `GET /applist` — XML avec liste des applications
- `POST /launch` — lance une app, retourne XML avec URL RTSP
- `POST /quit` — ferme l'app en cours
- `POST /pair` — appairage PIN
- `GET /boxart` — jaquette d'un jeu (PNG)

### 5.3 Structures de données clés

- **`STREAM_CONFIGURATION`** ([Limelight.h:44-103](D:\Code\moonlight-qt\moonlight-common-c\moonlight-common-c\src\Limelight.h#L44-L103)) — configuration du stream
- **`DECODE_UNIT`** — unité de décodage (frames réseau réassemblées)
- **`VIDEO_STATS`** ([decoder.h:11-35](D:\Code\moonlight-qt\app\streaming\video\decoder.h#L11-L35)) — statistiques de performance
- **`NvDisplayMode`** ([nvhttp.h:15-28](D:\Code\moonlight-qt\app\backend\nvhttp.h#L15-L28)) — mode d'affichage (width, height, refreshRate)
- **`OPUS_MULTISTREAM_CONFIGURATION`** — configuration audio (canaux, sample rate)

### 5.4 Formats vidéo supportés (masques)

```cpp
#define VIDEO_FORMAT_H264             0x0001
#define VIDEO_FORMAT_H265             0x0100
#define VIDEO_FORMAT_H265_MAIN10      0x0200
#define VIDEO_FORMAT_H264_HIGH8_444   0x0400
#define VIDEO_FORMAT_H265_REXT8_444   0x0800
#define VIDEO_FORMAT_H265_REXT10_444  0x1000
#define VIDEO_FORMAT_AV1_MAIN8        0x2000
#define VIDEO_FORMAT_AV1_MAIN10       0x4000
#define VIDEO_FORMAT_AV1_HIGH8_444    0x8000
#define VIDEO_FORMAT_AV1_HIGH10_444   0x10000
```

### 5.5 Recommandations pour Moonlight-Web

1. **API Layer** : réimplémenter `NvHTTP` en JS (fetch + DOMParser pour XML)
2. **Streaming** : privilégier l'API WebCodecs pour le décodage H.264 hardware
3. **Rendu** : canvas 2D ou WebGL pour l'affichage vidéo (faible latence)
4. **Audio** : Web Audio API avec `AudioWorklet` pour la latence minimale
5. **Input** : Gamepad API, KeyboardEvent, PointerEvent (natifs navigateur)
6. **Réseau** : WebSocket pour le control stream (plutôt qu'ENet), RTSP en JS
