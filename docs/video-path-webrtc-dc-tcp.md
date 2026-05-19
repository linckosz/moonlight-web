# Chemin du flux vidéo — WebRTC DataChannel sur TCP

Parcours complet d'une frame vidéo H.264, de Sunshine jusqu'au rendu canvas
dans le navigateur, en mode **Internet WebRTC DataChannel** lorsque la paire
ICE sélectionnée est TCP (RFC 6544, fallback quand UDP est bloqué).

## Résumé

```
Sunshine (RTSP/UDP) → moonlight-common-c → MoonlightShim → DataChannelRelay
→ libdatachannel (SCTP/DTLS/ICE-TCP) → TCP Socket → Browser RTCPeerConnection
→ WebRtcDataChannel.js → StreamView.js → WebCodecs VideoDecoder → Canvas
```

## Étapes détaillées

### 1. Sunshine → moonlight-common-c (UDP)

Sunshine encode le flux GPU en H.264 (Annex B). Les NAL units sont envoyées via
RTSP/RTP sur UDP (port 48010). Chaque `DECODE_UNIT` contient une frame vidéo
complète avec toutes ses NAL units déjà préfixées par un start code Annex B
(`00 00 00 01` ou `00 00 01`).

### 2. MoonlightShim — `drSubmitDecodeUnit()`

**Fichier :** `backend/src/streaming/MoonlightShim.cpp:253`

Callback C statique appelé par moonlight-common-c depuis un thread décodeur.
Concatène les `LENTRY` de la `bufferList` dans un `QByteArray` et émet le
signal `videoFrameReady(frameData, frameType, frameNumber)`.

### 3. DataChannelRelay — `onVideoFrame()`

**Fichier :** `backend/src/streaming/DataChannelRelay.cpp:272`

Reçoit le signal Qt de `MoonlightShim` sur le main thread :
- Si le DataChannel vidéo n'est pas encore ouvert, bufferise la première
  keyframe (contient SPS/PPS nécessaire au décodeur)
- Sinon, ignore les frames non-keyframe tant que le DC n'est pas prêt
- Applique le backpressure SCTP : si `bufferedAmount() > 128 KB`, drop les
  frames delta (une keyframe bloquée en SCTP est pire qu'une perdue)
- Appelle `sendFragmented(data, isKeyframe, m_VideoDc)`

### 4. DataChannelRelay — `sendFragmented()`

**Fichier :** `backend/src/streaming/DataChannelRelay.cpp:439`

Découpe la frame en chunks de ≤ 65535 bytes. Chaque chunk reçoit un header
de 17 bytes (big-endian) :

| Offset | Taille | Champ        |
|--------|--------|--------------|
| 0      | 4      | frame_id     |
| 4      | 2      | chunk_index  |
| 6      | 2      | total_chunks |
| 8      | 1      | is_keyframe  |
| 9      | 4      | payload_size |
| 13     | 4      | backend_ts   |

Le chunk est envoyé via `dc->send(bin)` (libdatachannel, mode binaire).

### 5–8. Pile réseau : SCTP → DTLS → ICE-TCP → TCP

**libdatachannel** (backend) et **libjuice** gèrent la pile :

- **SCTP (RFC 4960) :** multi-stream. Canal vidéo = `unordered` +
  `maxRetransmits=0` (pas de retransmission, une frame perdue est récupérée
  au prochain keyframe). Canaux audio et input = `ordered` + fiable.

- **DTLS (RFC 6347) :** chiffrement. Même handshake qu'en UDP, mais les
  records DTLS sont envoyés sur le socket TCP.

- **ICE-TCP (RFC 6544) :** libjuice (`enableIceTcp = true`, mode Internet
  uniquement). Le backend est en mode **actif** : c'est lui qui initie la
  connexion TCP sortante vers le candidat TCP passif du navigateur. Les
  candidats TCP ont une priorité ICE inférieure à l'UDP, ils ne sont
  sélectionnés que si toutes les paires UDP échouent.

- **TCP :** connexion TCP standard entre le port éphémère du backend et
  celui du navigateur.

Configuration : `SignalingServer::buildIceConfig()` (`backend/src/streaming/SignalingServer.cpp:712`)

### 9. Navigateur — RTCPeerConnection

Le navigateur génère automatiquement des candidats TCP passifs (RFC 6544)
quand `iceTransportPolicy = 'all'`. L'agent ICE reçoit la connexion TCP
entrante, désencapsule DTLS → SCTP, et délivre le message binaire au
DataChannel.

### 10–12. WebRtcDataChannel.js — Réception et réassemblage

**Fichier :** `frontend/js/api/WebRtcDataChannel.js`

- `dc.onmessage` (ligne 539) → route les messages binaires vers
  `_onVideoChunk()` (ligne 665)
- Parse le header 17 bytes, stocke le chunk dans un `Map<frameId, entry>`
- Quand `entry.received >= totalChunks` → `_assembleFrame()` (ligne 725)
- Vérifie l'intégrité (drop si chunks manquants), concatène dans un
  `Uint8Array` final
- Appelle `this.onVideo(assembled, entry.keyframe, entry.backendTs)`

### 13. StreamView.js — `handleVideoFrame()`

**Fichier :** `frontend/js/ui/StreamView.js:805`

- Si la première keyframe → extraction SPS/PPS via `NalParser`
- Configure le `VideoDecoder` WebCodecs avec la description extraite
- Si décodeur déjà configuré → `decoder.decode(chunk)` (file d'attente
  interne, régulée par `pendingFrames`)

### 14–15. WebCodecs VideoDecoder → Canvas

Le `VideoDecoder` WebCodecs décode le flux H.264 Annex B en `VideoFrame`.
Chaque frame est passée au canvas via `ctx.drawImage()` pour le rendu final.

## Pile TCP couche par couche

```
┌──────────────────────────┐
│  Fragmentation custom    │  Header 17 bytes + payload ≤ 65535 bytes
├──────────────────────────┤
│  SCTP (RFC 4960)         │  Multi-stream, unordered pour vidéo
├──────────────────────────┤
│  DTLS (RFC 6347)         │  Chiffrement (même handshake qu'en UDP)
├──────────────────────────┤
│  ICE-TCP (RFC 6544)      │  Actif (backend) ↔ Passif (navigateur)
├──────────────────────────┤
│  TCP                     │  Connexion socket standard
├──────────────────────────┤
│  IP                      │
└──────────────────────────┘
```

## Points importants

1. **Sunshine → Backend reste UDP.** Le TCP n'intervient qu'entre le backend
   et le navigateur. Le RTSP/RTP de Sunshine utilise toujours le port UDP 48010.

2. **TCP = fallback.** ICE-TCP n'est activé qu'en mode Internet
   (`enableIceTcp = true`). En LAN, il est désactivé car il causait des
   échecs de PeerConnection avec certaines versions de libdatachannel.

3. **Keyframe bufferisée.** La première keyframe arrive avant que le
   DataChannel soit ouvert (ICE pas encore négocié). Elle est bufferisée et
   envoyée dès que le DC vidéo passe à l'état `open`.

4. **Backpressure.** Même en TCP, le buffer SCTP peut saturer. Le backend
   drop les frames delta au-dessus de 128 KB (`kHighWatermark`) pour éviter
   de bloquer le main thread Qt.

5. **Starvation detector.** Côté frontend, si aucune frame n'est assemblée
   pendant 2 secondes, un message `requestidr` est envoyé via le DataChannel
   input pour demander un keyframe à Sunshine.

## Fichiers concernés

| Composant                          | Fichier                                              |
|------------------------------------|------------------------------------------------------|
| RTP → NAL units                    | `moonlight-common-c` (lib Limelight)                 |
| Signal videoFrameReady             | `backend/src/streaming/MoonlightShim.cpp:253`        |
| Fragmentation + envoi SCTP         | `backend/src/streaming/DataChannelRelay.cpp:272,439` |
| Configuration ICE-TCP              | `backend/src/streaming/SignalingServer.cpp:712`      |
| Réception + réassemblage           | `frontend/js/api/WebRtcDataChannel.js:539,665,725`   |
| Décodage WebCodecs + rendu Canvas  | `frontend/js/ui/StreamView.js:805`                   |
