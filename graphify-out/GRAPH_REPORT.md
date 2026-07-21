# Graph Report - moonlight-web  (2026-07-21)

## Corpus Check
- 215 files · ~636,267 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 3123 nodes · 6545 edges · 157 communities (124 shown, 33 thin omitted)
- Extraction: 92% EXTRACTED · 8% INFERRED · 0% AMBIGUOUS · INFERRED: 526 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `e20899b4`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- AppSettings.cpp
- StreamSession
- AuthManager
- DataChannelRelay
- StreamRelay
- AdminView
- HttpServer
- WebGpuRenderer
- NvComputer
- MediaTrackRelay
- ComputerManager
- InternetAccessManager
- SignalingServer
- Self-hosted DNS stack for MoonlightWeb (Docker)
- MoonlightShim.cpp
- BackendClient
- .info
- app.js
- StreamView.js
- NvPairingManager
- AcmeClient
- QJsonObject
- EnetControlStream
- ComputerManager.cpp
- scripts
- t
- TrayManager
- PdnsClient
- RestRouter
- InstallerPane
- NvHTTP
- WebRtcDataChannel
- UpdateChecker
- UPNPClient
- QFile
- IdentityManager
- InternetAccessManager.cpp
- StreamConfig
- AudioPipeline
- ControlChannel
- AcmeClient.cpp
- MediaTrackRelay.cpp
- MoonlightShim.h
- HttpResponse
- ConnectionGuard
- HttpServer.cpp
- FrameSender
- StreamWorkerHost
- .constructor
- compilerOptions
- StreamWorkerMain.cpp
- Logger
- StreamView
- src/main.cpp
- VideoDecodeWorker.js
- DataChannelRelay.h
- .warning
- GeoIpService
- DataChannelRelay.cpp
- SetupView
- WebRtcMedia
- GamepadManager
- LoginView
- Av1Utils.js
- ClipboardBridge
- RelayBase
- Host
- SettingsView
- StunClient
- SignalingServer.cpp
- test_framework.h
- NvAddress
- StreamViewKeyboard
- onLaunchReplyFinished
- SessionInfo
- QByteArray
- MoonlightShim
- BrowserDetect.js
- e
- StreamViewFullscreen
- SslServer
- Home.md
- QByteArray
- StaticFileHandler
- iosAudioUnlock.js
- NvApp
- UPNP NAT Traversal — Plan de Test (Phase 7)
- install.sh
- AudioProcessor
- PairDialog
- Changements demandés
- check-i18n.cjs
- InputCrypto
- encodeFromJson
- WebRtcDataChannel.js
- 7.2 Key reference
- StreamViewTouch
- test_upnpclient.cpp
- 1.1 The User's journey
- 5. Streaming & Transports
- JitterController
- r
- .prettierrc.json
- .tolgeerc.json
- 2. Architecture
- 6. Security
- 10. PowerDNS Stack (`deploy/powerdns/`)
- 11. Build, CI & Testing
- SystemRoutes.cpp
- 4. Frontend (Vanilla JS)
- 12. Agentic Coding — AI-assisted development on this repo
- SlidingStats
- mapHttpsPortParity
- StreamSession::StreamSession
- 3. Backend (C++ / Qt)
- 8. REST API & WebSocket surfaces
- onMdnsResolved
- .~AuthManager
- SystemRoutes.h
- QString
- run_input_encoder_tests
- 9. Installers & Packaging
- postinstall
- registerAuthRoutes
- NalLocation
- ComPtr
- StreamRelay::StreamRelay
- test_static_files.cpp
- renew-certs.sh
- MoonlightWeb — Technical Wiki
- build_stream_image.py
- InternetAccessManager::InternetAccessManager
- SignalingServer::SignalingServer
- test_http_parser.cpp
- 13. Roadmap, Constraints & Improvement Leads
- NvHTTP::NvHTTP
- AcmeClient::AcmeClient
- .peerConnection
- onLocalIceCandidate
- quint16
- init.sh
- build.sh
- build-pkg.sh
- make-packages.sh
- run_clang_tidy.sh
- statusJson
- cleanupUPnP
- entrypoint.sh
- run-tests.sh

## God Nodes (most connected - your core abstractions)
1. `StreamView` - 124 edges
2. `DataChannelRelay` - 91 edges
3. `t()` - 88 edges
4. `StreamSession` - 87 edges
5. `ComputerManager` - 86 edges
6. `InternetAccessManager` - 84 edges
7. `AppSettings` - 80 edges
8. `SignalingServer` - 77 edges
9. `readAll` - 74 edges
10. `HttpServer` - 74 edges

## Surprising Connections (you probably didn't know these)
- `main()` --calls--> `init`  [INFERRED]
  backend/src/main.cpp → backend/src/TrayManager.h
- `main()` --calls--> `refreshTooltip`  [INFERRED]
  backend/src/main.cpp → backend/src/TrayManager.h
- `handleGetAppList` --calls--> `getCertificate`  [INFERRED]
  backend/src/backend/ComputerManager.h → backend/src/backend/IdentityManager.h
- `handleGetAppList` --calls--> `getPrivateKey`  [INFERRED]
  backend/src/backend/ComputerManager.h → backend/src/backend/IdentityManager.h
- `resolveMacFromArp` --references--> `NvAddress`  [INFERRED]
  backend/src/backend/ComputerManager.h → backend/src/backend/NvAddress.h

## Import Cycles
- None detected.

## Communities (157 total, 33 thin omitted)

### Community 0 - "AppSettings.cpp"
Cohesion: 0.08
Nodes (85): AppSettings, audioTimeStretch, autoIpDetection, certAuthEnabled, certificateToken, certKey, certPem, chroma444Enabled (+77 more)

### Community 1 - "StreamSession"
Cohesion: 0.03
Nodes (63): DataChannelRelay, Q_OBJECT, QNetworkReply, QObject, QSet, QString, QStringList, quint16 (+55 more)

### Community 2 - "AuthManager"
Cohesion: 0.08
Nodes (63): AppSettings, AuthManager, autoRegeneratePin, certAuthEnabled, certificateToken, cleanClientAddress, cleanupExpired, clearPin (+55 more)

### Community 3 - "DataChannelRelay"
Cohesion: 0.03
Nodes (58): DataChannel, DataChannelRelay, handleKeyEvent, handleMouseButton, handleMouseMove, handleMouseScroll, kFragHeaderSize, kHighWatermark (+50 more)

### Community 4 - "StreamRelay"
Cohesion: 0.05
Nodes (50): QByteArray, QString, Q_OBJECT, QByteArray, QElapsedTimer, QList, QObject, QString (+42 more)

### Community 6 - "HttpServer"
Cohesion: 0.05
Nodes (40): QList, AuthManager, Q_OBJECT, QByteArray, QMap, QObject, QSet, QSslConfiguration (+32 more)

### Community 7 - "WebGpuRenderer"
Cohesion: 0.06
Nodes (6): Canvas2DRenderer, createVideoRenderer(), VideoElementRenderer, VideoRenderer, WebGpuRenderer, canvas

### Community 8 - "NvComputer"
Cohesion: 0.05
Nodes (50): ComputerState, NvComputer, PairState, QJsonObject, QSettings, QString, QVector, ComputerState (+42 more)

### Community 9 - "MediaTrackRelay"
Cohesion: 0.04
Nodes (47): DataChannel, atomic, mutex, Q_OBJECT, QByteArray, QTimer, RelayBase, shared_ptr (+39 more)

### Community 10 - "ComputerManager"
Cohesion: 0.05
Nodes (45): Browser, ComputerManager, getHostsJson, hostAddCompleted, hostsChanged, init, loadHosts, m_ActiveBoxArtFetches (+37 more)

### Community 11 - "InternetAccessManager"
Cohesion: 0.05
Nodes (39): AppSettings, function, Q_OBJECT, QDateTime, QObject, QString, QTimer, quint16 (+31 more)

### Community 12 - "SignalingServer"
Cohesion: 0.04
Nodes (41): atomic, Q_OBJECT, QObject, QTimer, MoonlightShim, RelayBase, SignalingServer, clientConnected (+33 more)

### Community 13 - "Self-hosted DNS stack for MoonlightWeb (Docker)"
Cohesion: 0.04
Nodes (44): 1. Install the toolchain, 2. Clone & build, 3. Build in Qt Creator (optional), 4. Frontend tooling & tests, 5. Open a pull request, Code style, Contributing to Moonlight‑Web, DNS stack (Internet access) (+36 more)

### Community 14 - "MoonlightShim.cpp"
Cohesion: 0.05
Nodes (10): QObject, QString, MoonlightShim::arInit(), MoonlightShim::drSubmitDecodeUnit(), MoonlightShim::MoonlightShim(), MoonlightShim::sendUtf8Text(), MoonlightShim::startConnection(), InitParams (+2 more)

### Community 16 - ".info"
Cohesion: 0.14
Nodes (31): QString, CertManager, ensureLocalSslConfig, extractCertCN, findCertByDomain, findCertDir, generateSelfSignedCert, loadCert (+23 more)

### Community 17 - "app.js"
Cohesion: 0.12
Nodes (17): MoonlightApp, applyDOM(), AVAILABLE_LANGUAGES, detectLanguage(), fetchLocale(), getLanguage(), init(), interpolate() (+9 more)

### Community 18 - "StreamView.js"
Cohesion: 0.10
Nodes (26): IMPORTANT: The VK codes here do NOT include the 0x8000 modifier bit, buildAvccDescription(), buildDescription(), buildHvcCDescription(), detectCodec(), getCodecString(), getH264CodecString(), getHevcCodecString() (+18 more)

### Community 19 - "NvPairingManager"
Cohesion: 0.11
Nodes (36): function, QByteArray, QString, quint16, X509, EVP_PKEY, QByteArray, QNetworkAccessManager (+28 more)

### Community 20 - "AcmeClient"
Cohesion: 0.06
Nodes (35): AcmeClient, errorOccurred, finished, m_AccountKeyPath, m_AccountUrl, m_AuthorizationUrl, m_BaseDomain, m_Cancelled (+27 more)

### Community 21 - "QJsonObject"
Cohesion: 0.14
Nodes (13): QDateTime, QHash, QMap, QNetworkAccessManager, QNetworkReply, QString, QTimer, QHostAddress (+5 more)

### Community 22 - "EnetControlStream"
Cohesion: 0.07
Nodes (34): QByteArray, QObject, QString, quint16, EnetControlStream, connected, connectionFailed, disconnected (+26 more)

### Community 23 - "ComputerManager.cpp"
Cohesion: 0.15
Nodes (33): ComputerManager::ComputerManager(), enqueueBoxArtFetch, fetchNextBoxArtInBackground, findHostByUuid, getHost, handleDeleteHost, handleGetAppList, handleGetBoxArt (+25 more)

### Community 24 - "scripts"
Cohesion: 0.06
Nodes (32): eslint, @eslint/js, description, devDependencies, eslint, @eslint/js, globals, jsdom (+24 more)

### Community 25 - "t"
Cohesion: 0.16
Nodes (3): resolve(), t(), HostListView

### Community 27 - "TrayManager"
Cohesion: 0.11
Nodes (29): ActivationReason, HttpServer, QObject, QString, QUrl, function, Q_OBJECT, HttpServer (+21 more)

### Community 28 - "PdnsClient"
Cohesion: 0.17
Nodes (28): QByteArray, QNetworkReply, QNetworkRequest, QObject, QString, QUrl, Q_OBJECT, QNetworkAccessManager (+20 more)

### Community 29 - "RestRouter"
Cohesion: 0.12
Nodes (31): AsyncRouteHandler, ParamRoute, QMap, QObject, QString, QStringList, ResponseCallback, AsyncRouteHandler (+23 more)

### Community 30 - "InstallerPane"
Cohesion: 0.07
Nodes (27): InstallerPane, -contentView, -didEnterPane, -initWithSection, -section, -setContentView, -setNextEnabled, -shouldExitPane (+19 more)

### Community 31 - "NvHTTP"
Cohesion: 0.17
Nodes (30): get, QByteArray, QNetworkReply, QString, quint16, QUrl, QVector, Q_OBJECT (+22 more)

### Community 33 - "UpdateChecker"
Cohesion: 0.11
Nodes (28): QJsonArray, QJsonObject, QObject, QString, Q_OBJECT, QDateTime, QJsonObject, QObject (+20 more)

### Community 34 - "UPNPClient"
Cohesion: 0.10
Nodes (25): QObject, string, Q_OBJECT, QHostAddress, QObject, IGDdatas, UPNPClient, addPortMapping (+17 more)

### Community 35 - "QFile"
Cohesion: 0.10
Nodes (21): QString, entryPath(), installLoginItem(), isLoginItemInstalled(), plistPath(), xmlEscape(), QString, QStringList (+13 more)

### Community 36 - "IdentityManager"
Cohesion: 0.11
Nodes (27): handleScanRequest, m_StreamActivePredicate, onPollTick, EVP_PKEY, QByteArray, QString, X509, EVP_PKEY (+19 more)

### Community 37 - "InternetAccessManager.cpp"
Cohesion: 0.20
Nodes (27): baseDomain(), QString, buildDomain, checkCertificate, claimOrVerifyOwnership, createOrUpdateARecord, ensureIdentifiers, forceRefresh (+19 more)

### Community 38 - "StreamConfig"
Cohesion: 0.08
Nodes (26): QByteArray, VideoCodec, StreamConfig, chroma, codec, computeColorSpace, computeVideoFormats, generateKeys (+18 more)

### Community 39 - "AudioPipeline"
Cohesion: 0.14
Nodes (7): decodeOpus(), decodeWasm(), onDecoded(), onError(), postPCM(), setupDecoder(), AudioPipeline

### Community 40 - "ControlChannel"
Cohesion: 0.11
Nodes (21): QSet, ControlChannel, broadcastFocusAdmin, ControlChannel::ControlChannel(), m_Clients, m_Port, m_Server, onDisconnected (+13 more)

### Community 41 - "AcmeClient.cpp"
Cohesion: 0.19
Nodes (26): acmePost, acmePostAsGet, createChallengeTxtRecord, deleteChallengeTxtRecord, fetchNonce, generateRsaKey, setAccountKeyPath, setBaseDomain (+18 more)

### Community 42 - "MediaTrackRelay.cpp"
Cohesion: 0.11
Nodes (26): Configuration, MoonlightShim, QByteArray, QObject, string, addRemoteCandidate, computeRtpTimestamp, createTracksAndChannels (+18 more)

### Community 43 - "MoonlightShim.h"
Cohesion: 0.08
Nodes (8): MoonlightShim, HostAudioSink::ensureFullVolume(), _DECODE_UNIT, _OPUS_MULTISTREAM_CONFIGURATION, _SERVER_INFORMATION, _STREAM_CONFIGURATION, spawnRelayThread(), QThread

### Community 44 - "HttpResponse"
Cohesion: 0.10
Nodes (24): QByteArray, QMap, QString, HttpRequest, body, clientAddress, headers, isLocal (+16 more)

### Community 45 - "ConnectionGuard"
Cohesion: 0.14
Nodes (22): ConnectionGuard, allowConnection, AUTHFAIL_MAX, AUTHFAIL_WINDOW_MS, BAN_MS, banSecondsRemaining, CONN_MAX_PER_WINDOW, CONN_WINDOW_MS (+14 more)

### Community 46 - "HttpServer.cpp"
Cohesion: 0.20
Nodes (24): QByteArray, QObject, QString, QTcpSocket, quint16, addSecondaryHttpsListener, changeHttpsPort, createHttpsServer (+16 more)

### Community 47 - "FrameSender"
Cohesion: 0.10
Nodes (23): DataChannel, Job, QByteArray, shared_ptr, FrameSender, enqueue, FrameSender::FrameSender(), kFragHeaderSize (+15 more)

### Community 48 - "StreamWorkerHost"
Cohesion: 0.13
Nodes (24): QJsonObject, QObject, Q_OBJECT, QByteArray, QObject, signals, StreamWorkerHost, ended (+16 more)

### Community 50 - "compilerOptions"
Cohesion: 0.08
Nodes (24): compilerOptions, allowJs, checkJs, forceConsistentCasingInFileNames, lib, module, moduleResolution, noEmit (+16 more)

### Community 51 - "StreamWorkerMain.cpp"
Cohesion: 0.11
Nodes (20): evp_pkey_st, x509_st, ClipboardBridge::ClipboardBridge(), QObject, QJsonObject, emitEvent(), runStreamWorker(), teardownAndExit() (+12 more)

### Community 52 - "Logger"
Cohesion: 0.12
Nodes (21): QObject, QString, Q_OBJECT, QObject, Logger, instance, levelString, log (+13 more)

### Community 54 - "src/main.cpp"
Cohesion: 0.14
Nodes (16): applyEmbeddedEnvDefaults(), QString, hasGuiSession(), loadEnvFile(), main(), openInBrowser(), requestFocusAdmin(), writeAdminShortcut() (+8 more)

### Community 55 - "VideoDecodeWorker.js"
Cohesion: 0.23
Nodes (22): addClientSample(), clientLatencyAvg(), configureAv1Decoder(), configureDecoder(), decodeAv1Frame(), decodeFrame(), drawFrame(), flushPendingFrames() (+14 more)

### Community 56 - "DataChannelRelay.h"
Cohesion: 0.13
Nodes (12): QByteArray, evp_pkey_st, x509_st, string, atomic, mutex, DataChannel, deque (+4 more)

### Community 57 - ".warning"
Cohesion: 0.13
Nodes (21): QString, quint16, Q_OBJECT, QObject, QNetworkAccessManager, SunshineRestClient, ensureMinChannels, m_Nam (+13 more)

### Community 58 - "GeoIpService"
Cohesion: 0.10
Nodes (21): GeoCallback, QObject, QPair, QString, GeoIpService, CACHE_TTL_MS, cachedLocation, clearCache (+13 more)

### Community 59 - "DataChannelRelay.cpp"
Cohesion: 0.12
Nodes (21): Configuration, MoonlightShim, QObject, string, addRemoteCandidate, DataChannelRelay::DataChannelRelay(), notifyClientRevoked, notifyClientTakenOver (+13 more)

### Community 62 - "GamepadManager"
Cohesion: 0.16
Nodes (6): axisToShort(), BTN, BUTTON_MAP, CTYPE, detectType(), GamepadManager

### Community 64 - "Av1Utils.js"
Cohesion: 0.17
Nodes (13): AV1_FALLBACK_CODEC_STRINGS, BitReader, buildAv1DecoderConfigs(), codecStringFromSeqInfo(), findSequenceHeader(), getObuType(), isAv1Buffer(), isAv1HdrFromSeq() (+5 more)

### Community 65 - "ClipboardBridge"
Cohesion: 0.14
Nodes (20): ClipboardBridge, instance, isSelfAddress, m_LastText, onClipboardChanged, pasteFromClient, public, requestAnnounce (+12 more)

### Community 66 - "RelayBase"
Cohesion: 0.10
Nodes (18): Q_OBJECT, QObject, signals, RelayBase, addRemoteCandidate, dataChannelsOpen, isConnected, moonlightShim (+10 more)

### Community 69 - "StunClient"
Cohesion: 0.19
Nodes (17): detectPublicIp, detectPublicIpViaHttp, QByteArray, QList, QObject, QString, Q_OBJECT, QObject (+9 more)

### Community 70 - "SignalingServer.cpp"
Cohesion: 0.19
Nodes (18): Configuration, QByteArray, QString, buildIceConfig, forwardAudioViaWs, forwardVideoViaWs, handleWsFallbackInput, isPrivateAddress (+10 more)

### Community 71 - "test_framework.h"
Cohesion: 0.14
Nodes (12): A, B, main(), main(), run_app_settings_tests(), run_auth_manager_tests(), run_connection_guard_tests(), mw_check() (+4 more)

### Community 72 - "NvAddress"
Cohesion: 0.21
Nodes (10): addOrUpdateHost, clientUniqueId, handleAddManualHost, onBackupPollTick, tryAddHostFromAddress, QString, quint16, NvAddress (+2 more)

### Community 74 - "onLaunchReplyFinished"
Cohesion: 0.24
Nodes (16): error, json, ComputerManager, HttpServer, registerHostRoutes(), QByteArray, QString, doLaunchApp (+8 more)

### Community 75 - "SessionInfo"
Cohesion: 0.12
Nodes (15): sessions, QList, qint64, QJsonObject, QString, SessionInfo, city, country (+7 more)

### Community 76 - "QByteArray"
Cohesion: 0.22
Nodes (17): addHvcEp(), DataChannel, QByteArray, qint64, QString, QVector, shared_ptr, onVideoFrame (+9 more)

### Community 77 - "MoonlightShim"
Cohesion: 0.12
Nodes (17): Q_OBJECT, QByteArray, QObject, MoonlightShim, aesKey, audioConfiguration, bitrateKbps, colorRange (+9 more)

### Community 78 - "BrowserDetect.js"
Cohesion: 0.17
Nodes (12): TODO: implement resampling later (WSOLA or offline converter), IMPORTANT: moonlight-common-c delivers ENCODED Opus packets (see Limelight.h, NOTE: `sample` is typically a sub-view of a larger transport buffer; we, NOTE: the iOS playback-session hold (iosAudioUnlock) is intentionally, IMPORTANT: never use webkitEnterFullscreen() (iOS native video player)., detectPlatform(), IS_IOS, isIphone() (+4 more)

### Community 81 - "SslServer"
Cohesion: 0.17
Nodes (11): QSslConfiguration, QTcpServer, applyPublicSslConfig, reloadTls, SslServer, m_Guard, m_LocalSslConfig, m_OnSslReady (+3 more)

### Community 83 - "QByteArray"
Cohesion: 0.25
Nodes (14): accountKeyJwk, accountKeyThumbprint, b64urlDecode, b64urlEncode, buildEabJws, buildJws, generateCsr, parseRsaExponent (+6 more)

### Community 84 - "StaticFileHandler"
Cohesion: 0.19
Nodes (13): QObject, QString, Q_OBJECT, QMap, QObject, QString, StaticFileHandler, m_RootDir (+5 more)

### Community 85 - "iosAudioUnlock.js"
Cohesion: 0.25
Nodes (10): armOutputRetry(), ensureEl(), makeSilentWavUrl(), makeUnlockedCtx(), playEl(), playStream(), prepareForLaunch(), prime() (+2 more)

### Community 86 - "NvApp"
Cohesion: 0.18
Nodes (5): QString, NvApp, m_HdrSupported, m_Id, m_Name

### Community 87 - "UPNP NAT Traversal — Plan de Test (Phase 7)"
Cohesion: 0.15
Nodes (12): 1. Tests unitaires (backend), 1a. Fallback sans miniupnpc, 1b. E2E avec miniupnpc (necessite routeur UPnP sur le LAN), 2. Tests API REST, 2a. Settings streaming (upnp_enabled), 2b. Reponse /start, 2c. Frontend verification, 3. Test de regression LAN (+4 more)

### Community 89 - "install.sh"
Cohesion: 0.30
Nodes (9): build_caddy_with_progress(), die(), ensure_env(), ok(), pm_install(), pm_refresh(), install.sh script, step() (+1 more)

### Community 93 - "Changements demandés"
Cohesion: 0.17
Nodes (11): 1) Header simplifié — supprimer le codec badge ET le status dot, 2) Stats overlay — déplacer en haut-centre, avec responsive mobile, 3) Stats content — utiliser innerHTML avec structure riche, 4) CSS — style élégant pour la stats card, 5) Ajustement : premier affichage de l'overlay, 6) Ne pas casser le reste, Changements demandés, Contexte (+3 more)

### Community 94 - "check-i18n.cjs"
Cohesion: 0.17
Nodes (8): catalogs, enKeys, fs, JS, localeFiles, LOCALES, path, ROOT

### Community 95 - "InputCrypto"
Cohesion: 0.29
Nodes (8): QByteArray, InputCrypto, encrypt, InputCrypto::InputCrypto(), m_Iv, m_Key, wrapAndEncrypt, run_input_crypto_tests()

### Community 96 - "encodeFromJson"
Cohesion: 0.58
Nodes (9): QByteArray, QJsonObject, InputEncoder, encodeFromJson, encodeKeyEvent, encodeMouseButton, encodeMouseMove, encodeMouseScroll (+1 more)

### Community 97 - "WebRtcDataChannel.js"
Cohesion: 0.24
Nodes (7): RFC-7587, NOTE: no audio DataChannel — audio is a native RTP Opus track now (id=1, wsCloseDescription(), NOTE: no audio DataChannel — audio is a native RTP Opus track now., wsCloseDescription(), forceOpusStereo(), CHROME_ANSWER

### Community 98 - "7.2 Key reference"
Cohesion: 0.18
Nodes (11): 7.1 `settings.json` location, 7.2 Key reference, 7.3 `.env` — environment configuration, 7.4 Browser-side preferences (`localStorage`), 7. Settings Reference, Build-time embedded fallbacks, Internet Access, Lifecycle (+3 more)

### Community 100 - "test_upnpclient.cpp"
Cohesion: 0.38
Nodes (9): main(), test_add_mapping_fallback(), test_construction(), test_discover_fallback(), test_discover_with_upnp(), test_double_cleanup(), test_external_ip_fallback(), test_remove_mapping_fallback() (+1 more)

### Community 101 - "1.1 The User's journey"
Cohesion: 0.20
Nodes (10): 1.1 The User's journey, 1.2 The Administrator's journey, 1.3 What runs where, 1. Overview — MoonlightWeb from the outside, Day-2 operations, Discover, pair, stream, First-run setup, Internet access (+2 more)

### Community 102 - "5. Streaming & Transports"
Cohesion: 0.20
Nodes (10): 5.1 The five transport modes, 5.2 Video path, 5.3 HDR — support and limitations, 5.4 Audio path, 5.5 Input path, 5.6 Session lifecycle & teardown discipline, 5.7 Notable workarounds catalog, 5. Streaming & Transports (+2 more)

### Community 105 - ".prettierrc.json"
Cohesion: 0.20
Nodes (9): arrowParens, bracketSpacing, endOfLine, printWidth, semi, singleQuote, tabWidth, trailingComma (+1 more)

### Community 106 - ".tolgeerc.json"
Cohesion: 0.20
Nodes (9): apiUrl, format, projectId, pull, path, push, files, forceMode (+1 more)

### Community 107 - "2. Architecture"
Cohesion: 0.22
Nodes (9): 2.1 System diagram, 2.2 The three-party exchange in detail, 2.3 Technology stack & rationale, 2.4 Repository layout, 2.5 Code architecture principles, 2. Architecture, Browser ↔ MoonlightWeb, End-to-end launch sequence (+1 more)

### Community 108 - "6. Security"
Cohesion: 0.22
Nodes (9): 6.1 Threat model in one paragraph, 6.2 Authentication, 6.3 Sessions, 6.4 Brute-force & flood mitigation, 6.5 TLS, 6.6 DNS subdomain ownership, 6.7 Internet-access consent & audit, 6.8 Other hardening (+1 more)

### Community 109 - "10. PowerDNS Stack (`deploy/powerdns/`)"
Cohesion: 0.22
Nodes (9): 10.1 Topology, 10.2 PowerDNS configuration, 10.3 dnsdist configuration (`dnsdist/dnsdist.conf`), 10.4 Caddy (`caddy/`), 10.5 The installer (`install.sh`), 10.6 Manual steps (VM / cloud / registrar), 10.7 Hardening & limits, 10.8 Operations cheat-sheet (+1 more)

### Community 110 - "11. Build, CI & Testing"
Cohesion: 0.22
Nodes (9): 11.1 Building from source, 11.2 CI (`.github/workflows/ci.yml`), 11.3 Release (`.github/workflows/release.yml`), 11.4 Testing, 11.5 Code quality conventions, 11. Build, CI & Testing, Backend — Qt Test (`backend/tests/`), Frontend — Vitest (`frontend/test/`, jsdom) (+1 more)

### Community 111 - "SystemRoutes.cpp"
Cohesion: 0.43
Nodes (7): AppSettings, AuthManager, ComputerManager, function, HttpServer, registerSystemRoutes(), InternetAccessManager

### Community 112 - "4. Frontend (Vanilla JS)"
Cohesion: 0.25
Nodes (8): 4.1 File architecture, 4.2 Navigation model, 4.3 StreamView — the streaming overlay, 4.4 Renderers — why canvas *and* video, 4.5 Audio pipeline, 4.6 i18n, 4.7 Quality tooling, 4. Frontend (Vanilla JS)

### Community 113 - "12. Agentic Coding — AI-assisted development on this repo"
Cohesion: 0.25
Nodes (8): 12.1 Ground rules for any agent, 12.2 Recommended architecture: one master agent + on-demand skills + scoped sub-agents, 12.3 Claude Code configuration, 12.4 GitHub Copilot configuration, 12.5 Formatting this repo for AI ingestion (why the wiki looks like this), 12. Agentic Coding — AI-assisted development on this repo, Suggested skills (playbooks the master loads on demand), When the master should spawn a sub-agent

### Community 115 - "mapHttpsPortParity"
Cohesion: 0.43
Nodes (7): quint16, fallbackExternalPort, isLocalPortBindable, m_HttpsRebindCallback, mapHttpsPortParity, mapPortWithFallback, setPorts

### Community 116 - "StreamSession::StreamSession"
Cohesion: 0.29
Nodes (7): NvComputer, NvHTTP, QObject, quint16, ResponseCallback, VideoCodec, StreamSession::StreamSession()

### Community 117 - "3. Backend (C++ / Qt)"
Cohesion: 0.29
Nodes (7): 3.1 Module map, 3.2 HTTP server, 3.3 Streaming layer, 3.4 Internet access, 3.5 Startup sequence (`main.cpp`), 3.6 Data on disk, 3. Backend (C++ / Qt)

### Community 118 - "8. REST API & WebSocket surfaces"
Cohesion: 0.29
Nodes (7): 8.1 Health & server info, 8.2 Authentication (`AuthRoutes.cpp`), 8.3 Hosts & streaming (`HostRoutes.cpp` + `main.cpp`), 8.4 Admin, settings, internet, setup, system (`SystemRoutes.cpp`), 8.5 WebSocket surfaces, 8.6 Static file serving, 8. REST API & WebSocket surfaces

### Community 119 - "onMdnsResolved"
Cohesion: 0.47
Nodes (6): chooseBestMdnsAddress(), onMdnsResolved, QHostAddress, QVector, handleResolvedAddress(), MdnsPendingComputer

### Community 120 - ".~AuthManager"
Cohesion: 0.33
Nodes (5): generateRandomKey, loadSessions, AppSettings, QByteArray, QObject

### Community 121 - "SystemRoutes.h"
Cohesion: 0.33
Nodes (5): AppSettings, AuthManager, ComputerManager, HttpServer, InternetAccessManager

### Community 123 - "run_input_encoder_tests"
Cohesion: 0.60
Nodes (5): beU32(), QByteArray, leU32(), run_input_encoder_tests(), quint32

### Community 124 - "9. Installers & Packaging"
Cohesion: 0.33
Nodes (6): 9.1 Windows — Inno Setup (`backend/installer/moonlightweb.iss`), 9.2 macOS — interactive `.pkg` (`backend/installer/macos/`), 9.3 Linux — `.deb` / `.rpm` / AppImage (`backend/packaging/linux/make-packages.sh`), 9.4 Shared runtime behaviors, 9.5 Workarounds catalog (installers), 9. Installers & Packaging

### Community 126 - "registerAuthRoutes"
Cohesion: 0.70
Nodes (4): AuthManager, HttpServer, registerAuthRoutes(), GeoIpService

### Community 127 - "NalLocation"
Cohesion: 0.40
Nodes (5): NalLocation, nalLen, nalOffset, startLen, startOffset

### Community 128 - "ComPtr"
Cohesion: 0.50
Nodes (3): ComPtr, p, T

### Community 129 - "StreamRelay::StreamRelay"
Cohesion: 0.40
Nodes (5): MoonlightShim, QObject, QSslConfiguration, quint16, StreamRelay::StreamRelay()

### Community 130 - "test_static_files.cpp"
Cohesion: 0.50
Nodes (4): QByteArray, QString, run_static_files_tests(), writeFile()

### Community 131 - "renew-certs.sh"
Cohesion: 0.70
Nodes (4): die(), ok(), renew-certs.sh script, warn()

### Community 132 - "MoonlightWeb — Technical Wiki"
Cohesion: 0.40
Nodes (5): Conventions used throughout, How to read this wiki, MoonlightWeb — Technical Wiki, Project at a glance, Table of contents

### Community 133 - "build_stream_image.py"
Cohesion: 0.40
Nodes (3): find_coeffs(), Build the marketing 'stream' illustration from blf.ai source.  - remove white/c, coeffs mapping OUTPUT coords (pa) -> INPUT coords (pb) for Image.PERSPECTIVE.

### Community 134 - "InternetAccessManager::InternetAccessManager"
Cohesion: 0.50
Nodes (4): AppSettings, QObject, InternetAccessManager::InternetAccessManager(), stop

### Community 135 - "SignalingServer::SignalingServer"
Cohesion: 0.50
Nodes (4): QObject, quint16, RelayBase, SignalingServer::SignalingServer()

### Community 136 - "test_http_parser.cpp"
Cohesion: 0.67
Nodes (3): QByteArray, raw(), run_http_parser_tests()

### Community 137 - "13. Roadmap, Constraints & Improvement Leads"
Cohesion: 0.50
Nodes (4): 13.1 Known remaining work, 13.2 Structural constraints (accept, don't fight), 13.3 Improvement leads, 13. Roadmap, Constraints & Improvement Leads

### Community 138 - "NvHTTP::NvHTTP"
Cohesion: 0.67
Nodes (3): QNetworkAccessManager, QObject, NvHTTP::NvHTTP()

### Community 139 - "AcmeClient::AcmeClient"
Cohesion: 0.67
Nodes (3): AcmeClient::AcmeClient(), cancel, QObject

### Community 141 - "onLocalIceCandidate"
Cohesion: 0.67
Nodes (3): string, onLocalIceCandidate, onLocalSdp

## Knowledge Gaps
- **816 isolated node(s):** `build.sh script`, `build-pkg.sh script`, `-initWithSection`, `-section`, `-contentView` (+811 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **33 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `ComputerManager` connect `ComputerManager` to `IdentityManager`, `HttpServer`, `NvAddress`, `ControlChannel`, `ComputerManager.cpp`, `QJsonObject`, `onMdnsResolved`, `DataChannelRelay.h`?**
  _High betweenness centrality (0.033) - this node is a cross-community bridge._
- **Why does `MediaTrackRelay` connect `MediaTrackRelay` to `DataChannelRelay.h`, `ClipboardBridge`, `MediaTrackRelay.cpp`?**
  _High betweenness centrality (0.033) - this node is a cross-community bridge._
- **Why does `HttpServer` connect `HttpServer` to `.info`, `SslServer`, `ConnectionGuard`, `HttpServer.cpp`?**
  _High betweenness centrality (0.029) - this node is a cross-community bridge._
- **What connects `build.sh script`, `build-pkg.sh script`, `-initWithSection` to the rest of the system?**
  _816 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `AppSettings.cpp` be split into smaller, more focused modules?**
  _Cohesion score 0.08333333333333333 - nodes in this community are weakly interconnected._
- **Should `StreamSession` be split into smaller, more focused modules?**
  _Cohesion score 0.030969030969030968 - nodes in this community are weakly interconnected._
- **Should `AuthManager` be split into smaller, more focused modules?**
  _Cohesion score 0.075990675990676 - nodes in this community are weakly interconnected._