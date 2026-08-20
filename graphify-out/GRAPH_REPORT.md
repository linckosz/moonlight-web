# Graph Report - moonlight-web  (2026-08-20)

## Corpus Check
- 295 files · ~807,999 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 4512 nodes · 9863 edges · 232 communities (183 shown, 49 thin omitted)
- Extraction: 92% EXTRACTED · 8% INFERRED · 0% AMBIGUOUS · INFERRED: 830 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `7436e7ad`
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
- compilerOptions
- StreamWorkerMain.cpp
- Logger
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
- .render
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
- StreamBackendRegistry
- GameStreamBackend
- ShareMenu
- HttpResponse
- MoonlightWeb in Docker
- RequestGuard.cpp
- Moonlight‑Web
- website/install.sh
- AspectProbe.js
- QFile
- BackendDialog
- ambient.d.ts
- Caller
- PlayerJoinView.js
- PipelineDiag
- MultiSeatSeat
- ShareManager.cpp
- Context
- activate
- StreamRelay.cpp
- GameStreamMedia
- pruneExpired
- LaunchRequest
- QString
- FramePacer
- .NvComputer
- EnhancerGovernor
- VideoRenderer
- Logger.h
- registerShareRoutes
- Contributing to Moonlight‑Web
- PeriodicStallDetector
- Canvas2DRenderer
- load
- VideoElementRenderer
- Policy
- Security Policy
- try-install.sh
- Decision
- ShareRoutesDeps
- createRenderer.js
- run
- BackendCapabilities
- NetClassify.h
- WolfBackend::WolfBackend
- startWsFallback
- NvDisplayMode
- Result
- MultiSeatBackend::MultiSeatBackend
- QString
- hdr
- docker/entrypoint.sh
- parseHevcSpsInfo
- panMomentum.test.js
- AuthRoutes.h
- MoonlightShim::startConnection
- quint16
- streamview-mixins.d.ts
- isEqualSerialized
- ControlChannel::ControlChannel
- status
- MoonlightShim::syncHeldInputs
- quantizeScroll
- BackendClient.test.js
- make-repo.sh
- MoonlightShim::MoonlightShim
- MoonlightShim::arInit
- MoonlightShim::drSubmitDecodeUnit
- VideoCodec
- moonlightweb/mw-proxy

## God Nodes (most connected - your core abstractions)
1. `StreamView` - 159 edges
2. `t()` - 126 edges
3. `StreamSession` - 100 edges
4. `ComputerManager` - 97 edges
5. `InternetAccessManager` - 92 edges
6. `AuthManager` - 90 edges
7. `HttpServer` - 87 edges
8. `BackendClient` - 85 edges
9. `AppSettings` - 83 edges
10. `DataChannelRelay` - 82 edges

## Surprising Connections (you probably didn't know these)
- `withSeatBackend` --calls--> `ready`  [INFERRED]
  backend/src/backend/streambackend/MultiSeatBackend.h → backend/src/network/InternetAccessManager.h
- `main()` --calls--> `init`  [INFERRED]
  backend/src/main.cpp → backend/src/TrayManager.h
- `runTrayClient()` --calls--> `init`  [INFERRED]
  backend/src/main.cpp → backend/src/TrayManager.h
- `main()` --calls--> `refreshTooltip`  [INFERRED]
  backend/src/main.cpp → backend/src/TrayManager.h
- `runTrayClient()` --calls--> `refreshTooltip`  [INFERRED]
  backend/src/main.cpp → backend/src/TrayManager.h

## Import Cycles
- None detected.

## Communities (232 total, 49 thin omitted)

### Community 0 - "AppSettings.cpp"
Cohesion: 0.08
Nodes (88): AppSettings, adminPasswordDigest, audioTimeStretch, autoIpDetection, certAuthEnabled, certificateToken, certKey, certPem (+80 more)

### Community 1 - "StreamSession"
Cohesion: 0.03
Nodes (66): DataChannelRelay, Q_OBJECT, QNetworkReply, QObject, QSet, QStringList, ResponseCallback, shared_ptr (+58 more)

### Community 2 - "AuthManager"
Cohesion: 0.13
Nodes (49): autoRegeneratePin, bucketLockout, bucketRemainingAttempts, certificateToken, cleanClientAddress, cleanupExpired, clearPin, constantTimeEquals (+41 more)

### Community 3 - "DataChannelRelay"
Cohesion: 0.04
Nodes (54): DataChannel, DataChannelRelay, handleKeyEvent, handleMouseButton, handleMouseMove, handleMouseScroll, kFragHeaderSize, kHighWatermark (+46 more)

### Community 4 - "StreamRelay"
Cohesion: 0.05
Nodes (35): Q_OBJECT, QByteArray, QElapsedTimer, QList, QObject, QString, QTimer, quint16 (+27 more)

### Community 6 - "HttpServer"
Cohesion: 0.04
Nodes (43): AuthManager, function, Q_OBJECT, QByteArray, QMap, QObject, QSet, QSslConfiguration (+35 more)

### Community 8 - "NvComputer"
Cohesion: 0.06
Nodes (34): ComputerState, PairState, QByteArray, QString, quint16, NvComputer, activeAddress, activeHttpsPort (+26 more)

### Community 9 - "MediaTrackRelay"
Cohesion: 0.04
Nodes (66): Configuration, MoonlightShim, QByteArray, QObject, string, DataChannel, atomic, mutex (+58 more)

### Community 10 - "ComputerManager"
Cohesion: 0.05
Nodes (45): Browser, ComputerManager, getHostsJson, hostAddCompleted, hostsChanged, init, loadHosts, m_ActiveBoxArtFetches (+37 more)

### Community 11 - "InternetAccessManager"
Cohesion: 0.04
Nodes (42): AppSettings, function, Q_OBJECT, QDateTime, QObject, QString, QTimer, quint16 (+34 more)

### Community 12 - "SignalingServer"
Cohesion: 0.04
Nodes (47): atomic, Q_OBJECT, QObject, QTimer, MoonlightShim, RelayBase, SignalingServer, cleanupUPnP (+39 more)

### Community 13 - "Self-hosted DNS stack for MoonlightWeb (Docker)"
Cohesion: 0.09
Nodes (23): 1. Create the VM, 2. Make the public IP static, 3. Disable the daily auto-shutdown, 4. Open the firewall (Network Security Group), 5. Run the installer, Attack-surface hardening (built into this stack), Bring your own certificate, Contents (+15 more)

### Community 16 - ".info"
Cohesion: 0.14
Nodes (31): CertManager, certFileMatchesDomain, certMatchesDomain, ensureLocalSslConfig, extractCertCN, findCertByDomain, findCertDir, generateSelfSignedCert (+23 more)

### Community 17 - "app.js"
Cohesion: 0.10
Nodes (21): MoonlightApp, applyDOM(), AVAILABLE_LANGUAGES, detectLanguage(), fetchLocale(), getLanguage(), init(), setLanguage() (+13 more)

### Community 18 - "StreamView.js"
Cohesion: 0.12
Nodes (24): IMPORTANT: The VK codes here do NOT include the 0x8000 modifier bit, buildAvccDescription(), buildDescription(), buildHvcCDescription(), detectCodec(), getCodecString(), getH264CodecString(), getHevcCodecString() (+16 more)

### Community 19 - "NvPairingManager"
Cohesion: 0.10
Nodes (39): function, QByteArray, QString, quint16, X509, EVP_PKEY, QByteArray, QNetworkAccessManager (+31 more)

### Community 20 - "AcmeClient"
Cohesion: 0.06
Nodes (36): AcmeClient, errorOccurred, finished, m_AccountKeyPath, m_AccountUrl, m_AuthorizationUrl, m_BaseDomain, m_Cancelled (+28 more)

### Community 21 - "QJsonObject"
Cohesion: 0.07
Nodes (32): QByteArray, QDateTime, QHash, QList, QMap, QNetworkAccessManager, QNetworkReply, QSet (+24 more)

### Community 22 - "EnetControlStream"
Cohesion: 0.07
Nodes (34): QByteArray, QObject, QString, quint16, EnetControlStream, connected, connectionFailed, disconnected (+26 more)

### Community 23 - "ComputerManager.cpp"
Cohesion: 0.19
Nodes (26): enqueueBoxArtFetch, fetchNextBoxArtInBackground, findHostByUuid, getHost, handleClearBackend, handleDeleteHost, handleGetBackend, handleGetBoxArt (+18 more)

### Community 24 - "scripts"
Cohesion: 0.06
Nodes (33): eslint, @eslint/js, description, devDependencies, eslint, @eslint/js, globals, jsdom (+25 more)

### Community 25 - "t"
Cohesion: 0.11
Nodes (4): interpolate(), resolve(), t(), HostListView

### Community 27 - "TrayManager"
Cohesion: 0.10
Nodes (32): ActivationReason, HttpServer, QObject, QString, QUrl, function, Q_OBJECT, HttpServer (+24 more)

### Community 28 - "PdnsClient"
Cohesion: 0.15
Nodes (29): QByteArray, QNetworkReply, QNetworkRequest, QObject, QString, QUrl, Q_OBJECT, QNetworkAccessManager (+21 more)

### Community 29 - "RestRouter"
Cohesion: 0.11
Nodes (34): AsyncRouteHandler, ParamRoute, QJsonObject, QMap, QObject, QString, QStringList, ResponseCallback (+26 more)

### Community 30 - "InstallerPane"
Cohesion: 0.07
Nodes (43): InstallerPane, -contentView, -didEnterPane, -gotoNextPane, -initWithSection, -section, -setContentView, -setNextEnabled (+35 more)

### Community 31 - "NvHTTP"
Cohesion: 0.17
Nodes (31): QByteArray, QNetworkReply, QString, quint16, QUrl, QVector, Q_OBJECT, QNetworkAccessManager (+23 more)

### Community 33 - "UpdateChecker"
Cohesion: 0.09
Nodes (36): buildRelayUrl(), qint64, QJsonArray, QJsonObject, QObject, QString, Q_OBJECT, QDateTime (+28 more)

### Community 34 - "UPNPClient"
Cohesion: 0.07
Nodes (38): QList, QObject, QString, QStringList, quint32, string, defaultRouteAddress(), Q_OBJECT (+30 more)

### Community 35 - "QFile"
Cohesion: 0.10
Nodes (25): QString, QStringList, DetectResult, exePath, installed, version, QString, run() (+17 more)

### Community 36 - "IdentityManager"
Cohesion: 0.08
Nodes (46): ClientIdentity, certPem, keyPem, EVP_PKEY, QByteArray, QString, X509, EVP_PKEY (+38 more)

### Community 37 - "InternetAccessManager.cpp"
Cohesion: 0.20
Nodes (24): AppSettings, QObject, quint16, checkCertificate, createOrUpdateARecord, fallbackExternalPort, forceRefresh, InternetAccessManager::InternetAccessManager() (+16 more)

### Community 38 - "StreamConfig"
Cohesion: 0.08
Nodes (26): QByteArray, VideoCodec, StreamConfig, chroma, codec, computeColorSpace, computeVideoFormats, generateKeys (+18 more)

### Community 39 - "AudioPipeline"
Cohesion: 0.10
Nodes (9): decodeOpus(), decodeWasm(), onDecoded(), onError(), postPCM(), setupDecoder(), AudioPipeline, r (+1 more)

### Community 40 - "ControlChannel"
Cohesion: 0.17
Nodes (16): ControlChannel, broadcastFocusAdmin, m_Clients, m_Port, m_Server, onDisconnected, onNewConnection, public (+8 more)

### Community 41 - "AcmeClient.cpp"
Cohesion: 0.19
Nodes (26): acmePost, acmePostAsGet, createChallengeTxtRecord, deleteChallengeTxtRecord, fetchNonce, generateRsaKey, setAccountKeyPath, setBaseDomain (+18 more)

### Community 42 - "MediaTrackRelay.cpp"
Cohesion: 0.33
Nodes (6): sendExitNotice, DataChannel, shared_ptr, string, sendAndFlush(), sendExitNotice

### Community 43 - "MoonlightShim.h"
Cohesion: 0.07
Nodes (18): filterHeldState(), QJsonObject, QString, quint32, QVector, modifierMask(), parseHeldKeys(), resolveHostKey() (+10 more)

### Community 44 - "HttpResponse"
Cohesion: 0.17
Nodes (12): HttpRequest, body, clientAddress, headers, hostTrusted, isHostMachine, isLocal, malformed (+4 more)

### Community 45 - "ConnectionGuard"
Cohesion: 0.14
Nodes (22): ConnectionGuard, allowConnection, AUTHFAIL_MAX, AUTHFAIL_WINDOW_MS, BAN_MS, banSecondsRemaining, CONN_MAX_PER_WINDOW, CONN_WINDOW_MS (+14 more)

### Community 46 - "HttpServer.cpp"
Cohesion: 0.15
Nodes (32): QByteArray, QObject, QString, QTcpServer, QTcpSocket, quint16, addSecondaryHttpsListener, adminKeyMatches (+24 more)

### Community 47 - "FrameSender"
Cohesion: 0.10
Nodes (23): DataChannel, Job, QByteArray, shared_ptr, FrameSender, enqueue, FrameSender::FrameSender(), kFragHeaderSize (+15 more)

### Community 48 - "StreamWorkerHost"
Cohesion: 0.13
Nodes (25): QJsonObject, QObject, Q_OBJECT, QByteArray, QObject, signals, StreamWorkerHost, ended (+17 more)

### Community 50 - "compilerOptions"
Cohesion: 0.08
Nodes (25): compilerOptions, allowJs, checkJs, forceConsistentCasingInFileNames, lib, module, moduleResolution, noEmit (+17 more)

### Community 51 - "StreamWorkerMain.cpp"
Cohesion: 0.07
Nodes (29): QTimer, string, atomic, mutex, DataChannel, Configuration, anythingHeld(), MoonlightShim::updateInputWatchdog() (+21 more)

### Community 52 - "Logger"
Cohesion: 0.12
Nodes (22): QObject, QString, Q_OBJECT, QObject, QString, Logger, instance, levelString (+14 more)

### Community 54 - "src/main.cpp"
Cohesion: 0.11
Nodes (44): applyEmbeddedEnvDefaults(), arecordRunning(), NvComputer, QByteArray, QJsonObject, QString, QStringList, quint16 (+36 more)

### Community 55 - "VideoDecodeWorker.js"
Cohesion: 0.18
Nodes (26): drawCapFor(), addStageSample(), applyEnhancerGovernor(), armPacingTimer(), clearPacingTimer(), configureAv1Decoder(), configureDecoder(), decodeAv1Frame() (+18 more)

### Community 56 - "DataChannelRelay.h"
Cohesion: 0.06
Nodes (64): buildMatchers(), clientIP(), envOr(), Client, Duration, Mutex, ResponseWriter, Time (+56 more)

### Community 57 - ".warning"
Cohesion: 0.22
Nodes (11): QString, quint16, Q_OBJECT, QObject, QNetworkAccessManager, SunshineRestClient, checkCredentials, m_Nam (+3 more)

### Community 58 - "GeoIpService"
Cohesion: 0.10
Nodes (21): GeoCallback, QObject, QPair, QString, GeoIpService, CACHE_TTL_MS, cachedLocation, clearCache (+13 more)

### Community 59 - "DataChannelRelay.cpp"
Cohesion: 0.32
Nodes (8): Candidate, candidateAddress(), QHostAddress, string, looksIpv6(), emitLocalCandidate, setPublicAddress, signalingIceCandidate

### Community 62 - "GamepadManager"
Cohesion: 0.15
Nodes (6): axisToShort(), BTN, BUTTON_MAP, CTYPE, detectType(), GamepadManager

### Community 64 - "Av1Utils.js"
Cohesion: 0.17
Nodes (13): AV1_FALLBACK_CODEC_STRINGS, BitReader, buildAv1DecoderConfigs(), codecStringFromSeqInfo(), findSequenceHeader(), getObuType(), isAv1Buffer(), isAv1HdrFromSeq() (+5 more)

### Community 65 - "ClipboardBridge"
Cohesion: 0.08
Nodes (33): ClipboardBridge, ClipboardBridge::ClipboardBridge(), instance, isSelfAddress, m_LastText, onClipboardChanged, pasteFromClient, public (+25 more)

### Community 66 - "RelayBase"
Cohesion: 0.07
Nodes (21): Q_OBJECT, QObject, string, RelayBase, addRemoteCandidate, dataChannelsOpen, isConnected, m_EmitLanCandidate (+13 more)

### Community 69 - "StunClient"
Cohesion: 0.21
Nodes (16): detectPublicIp, QByteArray, QList, QObject, QString, Q_OBJECT, QObject, StunClient (+8 more)

### Community 70 - "SignalingServer.cpp"
Cohesion: 0.18
Nodes (12): Configuration, QObject, QString, quint16, RelayBase, buildIceConfig, handleWsFallbackInput, onNewWsConnection (+4 more)

### Community 71 - "test_framework.h"
Cohesion: 0.10
Nodes (19): A, B, main(), main(), run_app_settings_tests(), run_auth_manager_tests(), run_connection_guard_tests(), mw_check() (+11 more)

### Community 72 - "NvAddress"
Cohesion: 0.20
Nodes (8): QString, quint16, NvAddress, m_Address, m_Port, QVector, exposureRank(), uniqueAddresses

### Community 74 - "onLaunchReplyFinished"
Cohesion: 0.33
Nodes (12): error, QString, buildLaunchRequest, doLaunchApp, doResumeApp, effectiveUniqueId, m_Respond, onLaunchResult (+4 more)

### Community 75 - "SessionInfo"
Cohesion: 0.12
Nodes (15): sessions, QList, qint64, QJsonObject, SessionInfo, city, country, createdAt (+7 more)

### Community 76 - "QByteArray"
Cohesion: 0.11
Nodes (31): addHvcEp(), DataChannel, QByteArray, qint64, QString, QVector, shared_ptr, notifyClientRevoked (+23 more)

### Community 77 - "MoonlightShim"
Cohesion: 0.12
Nodes (17): Q_OBJECT, QByteArray, QObject, MoonlightShim, aesKey, audioConfiguration, bitrateKbps, colorRange (+9 more)

### Community 78 - "BrowserDetect.js"
Cohesion: 0.16
Nodes (13): TODO: implement resampling later (WSOLA or offline converter), IMPORTANT: moonlight-common-c delivers ENCODED Opus packets (see Limelight.h, NOTE: `sample` is typically a sub-view of a larger transport buffer; we, NOTE: the iOS playback-session hold (iosAudioUnlock) is intentionally, IMPORTANT: never use webkitEnterFullscreen() (iOS native video player)., detectPlatform(), IS_ANDROID, IS_IOS (+5 more)

### Community 81 - "SslServer"
Cohesion: 0.19
Nodes (10): QSslConfiguration, applyPublicSslConfig, reloadTls, SslServer, m_Guard, m_LocalSslConfig, m_OnSslReady, m_PublicSslConfig (+2 more)

### Community 83 - "QByteArray"
Cohesion: 0.25
Nodes (14): accountKeyJwk, accountKeyThumbprint, b64urlDecode, b64urlEncode, buildEabJws, buildJws, generateCsr, parseRsaExponent (+6 more)

### Community 84 - "StaticFileHandler"
Cohesion: 0.16
Nodes (16): QDateTime, QObject, QString, Q_OBJECT, QMap, QObject, QString, httpDate() (+8 more)

### Community 85 - "iosAudioUnlock.js"
Cohesion: 0.28
Nodes (9): armOutputRetry(), ensureEl(), makeSilentWavUrl(), makeUnlockedCtx(), playEl(), playStream(), prepareForLaunch(), prime() (+1 more)

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
Cohesion: 0.15
Nodes (9): catalogs, enKeyList, enKeys, fs, JS, localeFiles, LOCALES, path (+1 more)

### Community 95 - "InputCrypto"
Cohesion: 0.29
Nodes (8): QByteArray, InputCrypto, encrypt, InputCrypto::InputCrypto(), m_Iv, m_Key, wrapAndEncrypt, run_input_crypto_tests()

### Community 96 - "encodeFromJson"
Cohesion: 0.58
Nodes (10): QByteArray, QJsonObject, InputEncoder, encodeFromJson, encodeKeyEvent, encodeMouseButton, encodeMouseHScroll, encodeMouseMove (+2 more)

### Community 97 - "WebRtcDataChannel.js"
Cohesion: 0.17
Nodes (10): RFC-7587, NOTE: no audio DataChannel — audio is a native RTP Opus track now (id=1, wsCloseDescription(), NOTE: no audio DataChannel — audio is a native RTP Opus track now., wsCloseDescription(), StreamTransport, forceOpusStereo(), CHROME_ANSWER (+2 more)

### Community 98 - "7.2 Key reference"
Cohesion: 0.17
Nodes (12): 7.1 `settings.json` location, 7.2 Key reference, 7.3 `.env` — environment configuration, 7.4 Browser-side preferences (`localStorage`), 7.5 Bring your own domain & certificate, 7. Settings Reference, Build-time embedded fallbacks, Internet Access (+4 more)

### Community 100 - "test_upnpclient.cpp"
Cohesion: 0.38
Nodes (9): main(), test_add_mapping_fallback(), test_construction(), test_discover_fallback(), test_discover_with_upnp(), test_double_cleanup(), test_external_ip_fallback(), test_remove_mapping_fallback() (+1 more)

### Community 101 - "1.1 The User's journey"
Cohesion: 0.20
Nodes (10): 1.1 The User's journey, 1.2 The Administrator's journey, 1.3 What runs where, 1. Overview — MoonlightWeb from the outside, Day-2 operations, Discover, pair, stream, First-run setup, Internet access (+2 more)

### Community 102 - "5. Streaming & Transports"
Cohesion: 0.17
Nodes (12): 5.1 The five transport modes, 5.2 Video path, 5.3 HDR — support and limitations, 5.4 Audio path, 5.5 Input path, 5.6 Session lifecycle & teardown discipline, 5.7 Notable workarounds catalog, 5. Streaming & Transports (+4 more)

### Community 104 - "r"
Cohesion: 0.06
Nodes (64): QJsonObject, QObject, QString, QStringList, Q_OBJECT, qint64, QObject, QString (+56 more)

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
Cohesion: 0.18
Nodes (11): 6.1 Threat model in one paragraph, 6.2.1 Remote admin password (LAN only), 6.2 Authentication, 6.3 Sessions, 6.4 Brute-force & flood mitigation, 6.4b Session sharing (invited players), 6.5 TLS, 6.6 DNS subdomain ownership (+3 more)

### Community 109 - "10. PowerDNS Stack (`deploy/powerdns/`)"
Cohesion: 0.18
Nodes (11): 10.10 Operations cheat-sheet, 10.1 Topology, 10.2 PowerDNS configuration, 10.3 dnsdist configuration (`dnsdist/dnsdist.conf`), 10.4 Caddy (`caddy/`), 10.5 The installer (`install.sh`), 10.6 Manual steps (VM / cloud / registrar), 10.7 Least-privilege API key (`mw-proxy`) (+3 more)

### Community 110 - "11. Build, CI & Testing"
Cohesion: 0.18
Nodes (11): 11.1 Building from source, 11.2 CI (`.github/workflows/ci.yml`), 11.3 Release (`.github/workflows/release.yml`), 11.3bis Container images (`.github/workflows/docker.yml`), 11.4 Testing, 11.5 Code quality conventions, 11. Build, CI & Testing, Backend — Qt Test (`backend/tests/`) (+3 more)

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
Cohesion: 0.08
Nodes (48): BackendAppListCallback, BackendJsonCallback, BackendMediaCallback, BackendSeatCallback, BackendSeatListCallback, BackendVoidCallback, QJsonObject, QSet (+40 more)

### Community 116 - "StreamSession::StreamSession"
Cohesion: 0.29
Nodes (7): NvComputer, NvHTTP, QObject, quint16, ResponseCallback, VideoCodec, StreamSession::StreamSession()

### Community 117 - "3. Backend (C++ / Qt)"
Cohesion: 0.25
Nodes (8): 3.1 Module map, 3.2 HTTP server, 3.3 Streaming layer, 3.4 Internet access, 3.5 Startup sequence (`main.cpp`), 3.6 Data on disk, 3.7 Headless operator CLI, 3. Backend (C++ / Qt)

### Community 118 - "8. REST API & WebSocket surfaces"
Cohesion: 0.25
Nodes (8): 8.1 Health & server info, 8.2 Authentication (`AuthRoutes.cpp`), 8.3 Hosts & streaming (`HostRoutes.cpp` + `main.cpp`), 8.3b Session sharing (`ShareRoutes.cpp`), 8.4 Admin, settings, internet, setup, system (`SystemRoutes.cpp`), 8.5 WebSocket surfaces, 8.6 Static file serving, 8. REST API & WebSocket surfaces

### Community 119 - "onMdnsResolved"
Cohesion: 0.16
Nodes (22): chooseBestMdnsAddress(), addOrUpdateHost, clientUniqueId, ComputerManager::ComputerManager(), handleAddManualHost, handleScanRequest, m_StreamActivePredicate, onBackupPollTick (+14 more)

### Community 120 - ".~AuthManager"
Cohesion: 0.05
Nodes (39): AppSettings, AuthManager, adminPasswordSet, certAuthEnabled, generateRandomKey, loadSessions, LOCKOUT_LONG_MS, LOCKOUT_MEDIUM_MS (+31 more)

### Community 121 - "SystemRoutes.h"
Cohesion: 0.33
Nodes (5): AppSettings, AuthManager, ComputerManager, HttpServer, InternetAccessManager

### Community 123 - "run_input_encoder_tests"
Cohesion: 0.60
Nodes (5): beU32(), QByteArray, quint32, leU32(), run_input_encoder_tests()

### Community 124 - "9. Installers & Packaging"
Cohesion: 0.15
Nodes (13): 9.1 Windows — Inno Setup (`backend/installer/moonlightweb.iss`), 9.2 macOS — interactive `.pkg` (`backend/installer/macos/`), 9.3 Linux — `.deb` / `.rpm` / AppImage (`backend/packaging/linux/make-packages.sh`), 9.3bis Docker — official images (`Dockerfile`, `docker/`), 9.4 Shared runtime behaviors, 9.5 Workarounds catalog (installers), 9. Installers & Packaging, AUR — `moonlightweb-bin` (+5 more)

### Community 126 - "registerAuthRoutes"
Cohesion: 0.29
Nodes (9): text, AuthManager, HttpServer, registerAuthRoutes(), call(), QString, run_rest_router_tests(), GeoIpService (+1 more)

### Community 127 - "NalLocation"
Cohesion: 0.07
Nodes (43): function, QJsonObject, QNetworkAccessManager, QNetworkReply, QObject, QString, Kind, Q_OBJECT (+35 more)

### Community 128 - "ComPtr"
Cohesion: 0.50
Nodes (3): ComPtr, p, T

### Community 129 - "StreamRelay::StreamRelay"
Cohesion: 0.25
Nodes (8): MoonlightShim, QObject, QSslConfiguration, QString, quint16, onShimConnectionFailed, StreamRelay::StreamRelay(), wsUrl

### Community 130 - "test_static_files.cpp"
Cohesion: 0.09
Nodes (39): QJsonObject, QNetworkAccessManager, QNetworkReply, QObject, QString, Kind, Q_OBJECT, QObject (+31 more)

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
Cohesion: 0.14
Nodes (33): QSettings, BackendAppListCallback, BackendMediaCallback, BackendSeatCallback, BackendSeatListCallback, BackendVoidCallback, NvComputer, QJsonObject (+25 more)

### Community 135 - "SignalingServer::SignalingServer"
Cohesion: 0.07
Nodes (29): BackendError, httpStatus, kind, message, BackendJsonCallback, BackendVoidCallback, Kind, QString (+21 more)

### Community 136 - "test_http_parser.cpp"
Cohesion: 0.12
Nodes (27): QString, quint16, quint16, QVector, Slot, T, SessionPool, acquire (+19 more)

### Community 137 - "13. Roadmap, Constraints & Improvement Leads"
Cohesion: 0.50
Nodes (4): 13.1 Known remaining work, 13.2 Structural constraints (accept, don't fight), 13.3 Improvement leads, 13. Roadmap, Constraints & Improvement Leads

### Community 138 - "NvHTTP::NvHTTP"
Cohesion: 0.67
Nodes (3): QNetworkAccessManager, QObject, NvHTTP::NvHTTP()

### Community 139 - "AcmeClient::AcmeClient"
Cohesion: 0.67
Nodes (3): AcmeClient::AcmeClient(), cancel, QObject

### Community 140 - ".peerConnection"
Cohesion: 0.06
Nodes (31): qint64, Q_OBJECT, QHash, qint64, QObject, QString, signals, Slot (+23 more)

### Community 141 - "onLocalIceCandidate"
Cohesion: 0.33
Nodes (6): string, addUpnpMapping, onLocalIceCandidate, onLocalSdp, setupUPnP, start

### Community 149 - "cleanupUPnP"
Cohesion: 0.08
Nodes (26): GameStreamBackend, HostResolver, NvComputer, Q_OBJECT, QMap, QObject, QString, unique_ptr (+18 more)

### Community 157 - "StreamBackendRegistry"
Cohesion: 0.09
Nodes (25): Factory, QJsonObject, QString, QStringList, unique_ptr, Factory, QMap, QString (+17 more)

### Community 158 - "GameStreamBackend"
Cohesion: 0.10
Nodes (22): BackendSeatCallback, BackendSeatListCallback, BackendVoidCallback, QJsonObject, GameStreamBackend, allocateSeat, ensurePaired, listSeats (+14 more)

### Community 160 - "HttpResponse"
Cohesion: 0.21
Nodes (23): backendFailure(), backendForHost, handleGetAppList, handleListLobbies, handleListProfiles, handleListSeats, handleProvisionSeat, handleReleaseSeatOwner (+15 more)

### Community 161 - "MoonlightWeb in Docker"
Cohesion: 0.09
Nodes (23): 1. Run it, 2. Open the ports — this is NOT automatic, 3. Set the two credentials, 4. Pair a Sunshine machine and stream, Building it yourself, Environment variables, First run — the PIN is not optional here, Hardware acceleration: none needed (+15 more)

### Community 162 - "RequestGuard.cpp"
Cohesion: 0.23
Nodes (21): adminTokenReply(), Authority, host, port, blockError(), blockStatus(), classifyInitiator(), Outcome (+13 more)

### Community 163 - "Moonlight‑Web"
Cohesion: 0.09
Nodes (22): About the author, Admin page, Advanced config — `settings.json`, Architecture, Docker — servers, NAS boxes and mini PCs, First launch (all platforms), Fork & build, How it works (+14 more)

### Community 164 - "website/install.sh"
Cohesion: 0.31
Nodes (21): ask_internet(), cli_hint(), die(), done_banner(), done_banner_headless(), enable_internet(), have(), in_container() (+13 more)

### Community 165 - "AspectProbe.js"
Cohesion: 0.21
Nodes (18): decideAspect(), frameAspect(), isBlankCanvas(), measureBars(), PROBE_FRACTIONS, sameBars(), scanBars(), startAspectProbe() (+10 more)

### Community 166 - "QFile"
Cohesion: 0.18
Nodes (18): QString, entryPath(), installLoginItem(), isLoginItemInstalled(), plistPath(), xmlEscape(), onAcmeFinished, readCertExpiry (+10 more)

### Community 168 - "ambient.d.ts"
Cohesion: 0.11
Nodes (12): AudioWorkletProcessor, GPUBufferUsage, GPUShaderStage, GPUTextureUsage, HdrVideoColorSpaceInit, HdrVideoDecoderConfig, HTMLVideoElement, MediaStreamTrackGenerator (+4 more)

### Community 169 - "Caller"
Cohesion: 0.15
Nodes (17): Caller, adminKeyOk, adminSession, hostSession, peerLocal, publicDomain, initializer_list, QByteArray (+9 more)

### Community 170 - "PlayerJoinView.js"
Cohesion: 0.22
Nodes (5): PlayerArt, HEIGHTS, loadPrefs(), PlayerJoinView, savePrefs()

### Community 171 - "PipelineDiag"
Cohesion: 0.15
Nodes (3): DiagWindow, formatDiag(), PipelineDiag

### Community 172 - "MultiSeatSeat"
Cohesion: 0.13
Nodes (13): quint16, MultiSeatSeat, accountName, apolloProcessId, errorMessage, fps, height, id (+5 more)

### Community 173 - "ShareManager.cpp"
Cohesion: 0.28
Nodes (15): QString, constantTimeEquals, hash, ShareManager::Permissions::accessLevel(), randomPin, randomToken, rateLockout, rateRecord (+7 more)

### Community 174 - "Context"
Cohesion: 0.13
Nodes (15): Context, adminKeyOk, adminSession, hostSession, peerLocal, publicDomain, describe(), QMap (+7 more)

### Community 175 - "activate"
Cohesion: 0.43
Nodes (14): activate, setStreaming, freshManager(), run_share_manager_tests(), test_a_dropped_stream_does_not_end_the_share(), test_activation_mints_distinct_secrets(), test_clear_secrets_are_memory_only(), test_permissions_freeze_when_the_popin_closes() (+6 more)

### Community 176 - "StreamRelay.cpp"
Cohesion: 0.16
Nodes (14): QByteArray, notifyClientRevoked, notifyClientSessionEnded, notifyClientTakenOver, onAudioSample, onShimConnectionStarted, onShimConnectionTerminated, onVideoFrame (+6 more)

### Community 177 - "GameStreamMedia"
Cohesion: 0.14
Nodes (14): GameStreamMedia, aesKey, appVersion, gfeVersion, hostAddress, rikeyid, rtspSessionUrl, serverCodecModeSupport (+6 more)

### Community 178 - "pruneExpired"
Cohesion: 0.29
Nodes (14): Slot, State, reasonName(), clearActivation, deactivate, deactivateAll, lockPermissions, pruneExpired (+6 more)

### Community 180 - "LaunchRequest"
Cohesion: 0.15
Nodes (13): QByteArray, LaunchRequest, appId, bitrateKbps, clientIdentitySeat, clientUniqueId, fps, hdrEnabled (+5 more)

### Community 181 - "QString"
Cohesion: 0.24
Nodes (13): baseDomain(), QString, buildDomain, claimOrVerifyOwnership, detectPublicIpViaHttp, ensureIdentifiers, ensureOwnerToken, generateUniqueId (+5 more)

### Community 183 - ".NvComputer"
Cohesion: 0.23
Nodes (11): ComputerState, PairState, QJsonObject, QSettings, QString, computerStateToString, isLocalMachine, pairStateFromString (+3 more)

### Community 184 - "EnhancerGovernor"
Cohesion: 0.23
Nodes (6): ENHANCER_LADDER, EnhancerGovernor, CONTENDED, feed(), IDLE, run()

### Community 186 - "Logger.h"
Cohesion: 0.22
Nodes (7): QString, install(), pruneOldDumps(), writeDump(), EXCEPTION_POINTERS, LONG, QMutex

### Community 187 - "registerShareRoutes"
Cohesion: 0.33
Nodes (10): HttpServer, QJsonObject, QString, SlotStatus, State, lanIPv4(), registerShareRoutes(), slotJson() (+2 more)

### Community 188 - "Contributing to Moonlight‑Web"
Cohesion: 0.20
Nodes (8): 1. Install the toolchain, 2. Clone & build, 3. Build in Qt Creator (optional), 4. Frontend tooling & tests, 5. Open a pull request, Code style, Contributing to Moonlight‑Web, DNS stack (Internet access)

### Community 189 - "PeriodicStallDetector"
Cohesion: 0.25
Nodes (3): DEFAULTS, PeriodicStallDetector, run()

### Community 191 - "load"
Cohesion: 0.20
Nodes (9): QJsonObject, QObject, load, permissions, ShareManager::Permissions::fromJson(), ShareManager::Permissions::toJson(), ratePurge, ShareManager::ShareManager() (+1 more)

### Community 193 - "Policy"
Cohesion: 0.22
Nodes (5): allowed(), QString, Policy, gamepad, keyboardMouse

### Community 194 - "Security Policy"
Cohesion: 0.22
Nodes (8): Dependency updates, In scope, Out of scope, Reporting a vulnerability, Security Policy, Supported versions, Third-party components, What this software does

### Community 195 - "try-install.sh"
Cohesion: 0.36
Nodes (6): cleanup(), die(), MSYS2_ARG_CONV_EXCL, MSYS_NO_PATHCONV, say(), try-install.sh script

### Community 196 - "Decision"
Cohesion: 0.25
Nodes (8): Decision, adminPrivilege, hostMachine, hostTrusted, hostUntrusted, localPrivilege, outcome, Outcome

### Community 197 - "ShareRoutesDeps"
Cohesion: 0.25
Nodes (8): function, QString, ShareRoutesDeps, machineName, ownerStreamAlive, publicOrigin, startPlayerStream, stopPlayerStream

### Community 199 - "run"
Cohesion: 0.33
Nodes (6): QString, generatePin(), run(), NvPairingManager, PinAnnouncer, ResultCallback

### Community 200 - "BackendCapabilities"
Cohesion: 0.29
Nodes (4): BackendCapabilities, lobbies, multiUser, provisioning

### Community 201 - "NetClassify.h"
Cohesion: 0.43
Nodes (6): classify(), Kind, QString, isPrivateOrLoopback(), isTrustedPeer(), toString()

### Community 202 - "WolfBackend::WolfBackend"
Cohesion: 0.33
Nodes (6): HostResolver, NvHTTP, PairingCommit, QNetworkAccessManager, QObject, WolfBackend::WolfBackend()

### Community 203 - "startWsFallback"
Cohesion: 0.33
Nodes (6): QByteArray, forwardAudioViaWs, forwardVideoViaWs, onDataChannelsOpen, onRelayIceTimedOut, startWsFallback

### Community 204 - "NvDisplayMode"
Cohesion: 0.40
Nodes (4): NvDisplayMode, height, refreshRate, width

### Community 205 - "Result"
Cohesion: 0.40
Nodes (5): Outcome, QByteArray, Result, outcome, serverCertPem

### Community 206 - "MultiSeatBackend::MultiSeatBackend"
Cohesion: 0.40
Nodes (5): HostResolver, NvHTTP, QNetworkAccessManager, QObject, MultiSeatBackend::MultiSeatBackend()

### Community 208 - "hdr"
Cohesion: 0.40
Nodes (4): initializer_list, pair, hdr(), Headers

### Community 209 - "docker/entrypoint.sh"
Cohesion: 0.60
Nodes (3): apply_setting(), row(), entrypoint.sh script

### Community 212 - "AuthRoutes.h"
Cohesion: 0.50
Nodes (3): AuthManager, GeoIpService, HttpServer

### Community 213 - "MoonlightShim::startConnection"
Cohesion: 0.50
Nodes (4): QString, MoonlightShim::sendUtf8Text(), MoonlightShim::startConnection(), InitParams

### Community 216 - "isEqualSerialized"
Cohesion: 0.67
Nodes (3): NvComputer, isEqualSerialized, update

### Community 217 - "ControlChannel::ControlChannel"
Cohesion: 0.67
Nodes (3): ControlChannel::ControlChannel(), QObject, quint16

### Community 218 - "status"
Cohesion: 0.67
Nodes (3): QList, SlotStatus, status

### Community 219 - "MoonlightShim::syncHeldInputs"
Cohesion: 0.67
Nodes (3): quint32, QVector, MoonlightShim::syncHeldInputs()

### Community 220 - "quantizeScroll"
Cohesion: 0.67
Nodes (3): MoonlightShim::sendMouseHScroll(), MoonlightShim::sendMouseScroll(), quantizeScroll()

## Knowledge Gaps
- **1116 isolated node(s):** `build.sh script`, `build-pkg.sh script`, `-initWithSection`, `-section`, `-contentView` (+1111 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **49 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `DataChannelRelay` connect `DataChannelRelay` to `ClipboardBridge`, `MediaTrackRelay.cpp`, `QByteArray`, `FrameSender`, `StreamWorkerMain.cpp`?**
  _High betweenness centrality (0.034) - this node is a cross-community bridge._
- **Why does `StreamSession` connect `StreamSession` to `Policy`, `VideoCodec`, `StreamConfig`, `SignalingServer::SignalingServer`, `onLaunchReplyFinished`, `QString`, `QJsonObject`, `quint16`?**
  _High betweenness centrality (0.031) - this node is a cross-community bridge._
- **Why does `HttpServer` connect `HttpServer` to `.info`, `SslServer`, `ConnectionGuard`, `HttpServer.cpp`?**
  _High betweenness centrality (0.030) - this node is a cross-community bridge._
- **What connects `build.sh script`, `build-pkg.sh script`, `-initWithSection` to the rest of the system?**
  _1116 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `AppSettings.cpp` be split into smaller, more focused modules?**
  _Cohesion score 0.08034188034188035 - nodes in this community are weakly interconnected._
- **Should `StreamSession` be split into smaller, more focused modules?**
  _Cohesion score 0.02738738738738739 - nodes in this community are weakly interconnected._
- **Should `AuthManager` be split into smaller, more focused modules?**
  _Cohesion score 0.12941176470588237 - nodes in this community are weakly interconnected._