# Audit latence — Transport WebRTC Media Track (`webrtc-media`)

> **Date :** 2026-06-13
> **Contexte :** Le transport `webrtc-media` (track RTP H.264 + `<video>` natif) fonctionne
> bien, mais la latence semble perfectible. Objectif : latence bout-en-bout minimale.
> **Périmètre :** `MediaTrackRelay.cpp/.h` (backend) + `WebRtcMedia.js` (frontend).

---

## 1. Pipeline AsIs — chemin vidéo `webrtc-media`

```
Sunshine ──RTP/UDP──> moonlight-common-c
   │
   ▼  (worker thread)
MoonlightShim::drSubmitDecodeUnit
   - concatène les PLENTRY en 1 QByteArray (deep copy)
   - m_PendingVideoFrames++ ; emit videoFrameReady(data, type, num)
   │
   ▼  (AutoConnection → marshalé sur le MAIN thread Qt : 1 hop d'event loop)
MediaTrackRelay::onVideoFrame
   - videoFrameDelivered() (m_PendingVideoFrames--) ; takeWorkerDroppedDelta()
   - bufferise la 1re keyframe si track pas ouvert
   - copie data → rtc::binary (2e deep copy)
   - FrameInfo(m_RtpTimestamp), isKeyFrame
   - m_VideoTrack->sendFrame()  →  H264RtpPacketizer (FU-A / STAP-A)
   - m_RtpTimestamp += 1500   ◄── compteur SYNTHÉTIQUE 60fps figé
   │
   ▼  H264RtpPacketizer → RtcpNackResponder(128) → PliHandler → SRTP/UDP
   │
   ▼  (réseau)
Navigateur : RTCPeerConnection.ontrack
   - getReceivers(): jitterBufferTarget=0 + setJitterBufferMinimumDelay(0)
   - videoElement.srcObject = MediaStream([track])
   - videoElement.playoutDelayHint = 0   ◄── NO-OP (mauvais objet)
   - décodage HW natif → rendu <video> → compositor → écran
```

**Audio (parallèle) :** PCM16 fragmenté sur **DataChannel SCTP fiable + ordonné** (id=0).
**Input :** JSON sur DataChannel id=1.

### Observation de fond

Le chemin `webrtc-media` est **déjà proche de l'optimal** côté navigateur : décodage
matériel natif, jitter buffer désactivé. Les marges restantes sont incrémentales et se
concentrent sur **(a)** le pacing/timestamp RTP, **(b)** un levier de rendu mal appliqué,
**(c)** un hop de thread et des copies, **(d)** la synchro A/V audio-sur-SCTP.

---

## 2. Constats priorisés

### 🔴 P1-A — `receiver.playoutDelayHint` n'est jamais réglé (levier appliqué au mauvais objet)

`WebRtcMedia.js:359` fait `this.videoElement.playoutDelayHint = 0`. **`playoutDelayHint`
n'existe pas sur `HTMLVideoElement`** — c'est une propriété de `RTCRtpReceiver` (Chrome).
L'affectation est silencieusement ignorée (no-op).

- **Impact :** Chrome conserve son délai de playout par défaut sur le rendu `<video>` d'un
  MediaStream temps réel. `jitterBufferTarget=0` réduit le buffer de jitter, mais
  `playoutDelayHint` pilote le **délai de présentation** du moteur de rendu — c'est un levier
  distinct. Le régler à 0 engage le chemin « low-delay » et peut retirer **plusieurs dizaines
  de ms** de bout-en-bout.
- **Fix :** dans la boucle `for (const receiver of this.pc.getReceivers())` de `ontrack`
  (`WebRtcMedia.js:340`), ajouter `if ('playoutDelayHint' in receiver) receiver.playoutDelayHint = 0;`
  et **supprimer** l'affectation sur `videoElement`.
- **Risque :** nul (no-op aujourd'hui, propriété standardisée côté receiver).

### 🔴 P1-B — Timestamp RTP synthétique figé à 60 fps

`MediaTrackRelay.cpp:356` : `m_RtpTimestamp += 1500` (= 90000 Hz / 60). Deux problèmes :

1. **FPS configurable** : le stream peut être 30/120 fps (`stream_fps`, défaut 60). À 30 fps
   le pas devrait être 3000, à 120 fps 750. Le pas figé fait croire au navigateur à une
   cadence 60 fps fausse → pacing/horloge média incorrects, dérive de synchro A/V.
2. **Cadence idéalisée** : même à 60 fps, le compteur suppose des frames parfaitement
   équidistantes. Le navigateur estime l'horloge de capture depuis les timestamps RTP ; un
   timestamp qui ignore le jitter réel d'arrivée empêche un pacing correct.

- **Impact :** jusqu'à 1–2 frames de latence/judder selon la plateforme et désync audio
  (l'audio sur DC n'a pas de timestamp RTP, voir P2-A). Limité par `jitterBufferTarget=0`
  mais réel sur le rendu `<video>`.
- **Fix recommandé (le meilleur) :** dériver le timestamp de l'horloge réelle d'arrivée :
  `m_RtpTimestamp = (arrivalUs * 90000) / 1000000` (avec un offset au 1er frame). Cela reflète
  le jitter réel et laisse le moteur cadencer correctement.
- **Fix minimal (à défaut) :** passer le FPS au relay et utiliser `90000 / fps` comme pas.
- **Risque :** faible. À valider sur les 3 plateformes (timestamp horloge réelle = standard WebRTC).

### 🟠 P2-A — Audio PCM16 sur DataChannel SCTP fiable+ordonné (HoL blocking + désync A/V)

L'audio passe en PCM16 sur DC `ordered:true` fiable (`MediaTrackRelay.cpp:236-248`,
`WebRtcMedia.js:399-405`). Trois conséquences :

- **Head-of-line blocking** : sous perte, SCTP retransmet **dans l'ordre** → un paquet perdu
  bloque tout l'audio suivant le temps de la retransmission → à-coups/latence audio.
- **Bande passante** : PCM16 (~1.5 Mbps stéréo 48 kHz) concurrence la vidéo sur la même
  association → congestion accrue sur réseau contraint → latence vidéo indirecte.
- **Désync A/V** : vidéo sur RTP temps réel vs audio sur SCTP fiable → les deux horloges
  divergent, surtout combiné à P1-B.
- **Pistes :** (i) court terme, DC audio `ordered:false` + `maxRetransmits:0` (drop plutôt
  que retransmettre — l'audio temps réel préfère un trou bref) ; (ii) cible, encoder en Opus et
  passer l'audio sur une track RTP (synchro A/V native, bande passante divisée par ~10) — coût :
  dépendance encodeur Opus.
- **Risque :** (i) faible et réversible ; (ii) chantier moyen.

### 🟠 P2-B — Hop de thread worker→main + double copie par frame

Chaque frame traverse `videoFrameReady` (worker) → `onVideoFrame` (main thread Qt) en
`QueuedConnection`, puis est recopiée `QByteArray` → `rtc::binary` avant `sendFrame`.

- **Impact :** 1 hop d'event loop (sub-ms en charge normale, davantage si le main thread Qt
  est occupé) + 2 deep copies par frame (concat dans le shim, puis copie dans le relay).
- **Pistes :** (i) packetiser/`sendFrame` directement depuis le worker thread (libdatachannel
  gère son propre threading) pour supprimer le hop ; (ii) éviter la 2e copie en transférant
  l'ownership du buffer (move/`std::shared_ptr<const>`), `sendFrame` accepte un `rtc::binary`
  par move. La copie subsiste car `QByteArray` (CoW) ≠ `rtc::binary`, mais on peut sérialiser
  une seule fois côté shim dans un buffer compatible.
- **Risque :** moyen (thread-safety, partagé avec DataChannelRelay/StreamRelay via le même
  signal). À ne tenter qu'après P1.

### 🟡 P3-A — Suppression `fmtp` trop large dans le SDP answer

`WebRtcMedia.js:576` : `answerSdp.replace(/^a=fmtp:\d+.*$/gim, '')` supprime **toutes** les
lignes `fmtp`, y compris `a=fmtp:96 ...packetization-mode=1;profile-level-id=...` du codec
vidéo, alors que l'intention (commentaire) ne vise que ULPFEC/RED.

- **Impact :** retire `packetization-mode=1` et `profile-level-id` de l'answer. Fonctionne en
  pratique (libdatachannel impose FU-A côté offerer), mais réponse asymétrique fragile : un
  décodeur conservateur peut supposer mode 0. Pas de gain latence — **risque de régression**.
- **Fix :** cibler uniquement les PT de FEC, p. ex. supprimer la ligne `fmtp` dont le PT
  correspond à un `rtpmap ulpfec|red` précédemment capturé, au lieu d'un `replace` global.
- **Risque :** la correction réduit le risque ; pas de gain latence direct.

### 🟡 P3-B — Strip NACK probablement inopérant (intention vs réalité)

`WebRtcMedia.js:581` : `replace(/^a=rtcp-fb:\*\s+nack\s*$/gim, '')` ne matche que la forme
**wildcard** `a=rtcp-fb:* nack`. libdatachannel émet généralement par-PT (`a=rtcp-fb:96 nack`),
non matché → **le NACK reste activé** (et le backend garde un `RtcpNackResponder(128)`).

- **Lecture :** ce n'est pas forcément un défaut — la retransmission NACK améliore la qualité
  sous perte légère. Mais l'intention affichée (désactiver NACK pour la latence) n'est pas
  réalisée : il faut **décider** explicitement.
- **Recommandation :** garder NACK activé (bon compromis qualité/latence sur track RTP) et
  **retirer le `replace` mort** pour lever l'ambiguïté, OU le rendre per-PT s'il faut vraiment
  désactiver. Documenter le choix.
- **Risque :** nul (clarification).

### 🟡 P3-C — `ptime:10` forcé sans track audio RTP

`WebRtcMedia.js:579` force `a=ptime:10`. L'audio étant sur DataChannel (pas de m=audio RTP),
cette réécriture ne s'applique à rien d'utile. Cosmétique — à retirer pour clarté.

---

## 3. Synthèse & ordre d'implémentation

| # | Constat | Gain latence estimé | Risque | Priorité |
|---|---|---|---|---|
| P1-A | `receiver.playoutDelayHint = 0` (vrai objet) | **Élevé** (dizaines de ms) | Nul | **1** |
| P1-B | Timestamp RTP horloge réelle (ou `90000/fps`) | Moyen + synchro A/V | Faible | **2** |
| P2-A | Audio DC unreliable (court terme) / Opus RTP (cible) | Moyen sous perte + synchro | Faible / Moyen | 3 |
| P2-B | Supprimer hop worker→main + 2e copie | Faible (sub-ms) | Moyen | 4 |
| P3-A | Strip `fmtp` ciblé FEC uniquement | Nul (anti-régression) | Réduit | 5 |
| P3-B | Clarifier/retirer strip NACK mort | Nul (clarté) | Nul | 5 |
| P3-C | Retirer `ptime:10` inutile | Nul (clarté) | Nul | 5 |

**Recommandation :** commencer par **P1-A** (gain le plus élevé, risque nul, ~3 lignes) puis
**P1-B**. Ces deux-là couvrent l'essentiel de la marge réellement contrôlable. P2/P3 sont des
durcissements (réseau dégradé, synchro A/V) et des clarifications, pas des gains LAN majeurs.

**Hors périmètre (non contrôlable ici) :** RTT réseau, latence d'encodage Sunshine, absence de
boucle de congestion REMB/TWCC vers Sunshine (la track est alimentée manuellement, le contrôle
de débit reste géré par Sunshine via la config RTSP).
