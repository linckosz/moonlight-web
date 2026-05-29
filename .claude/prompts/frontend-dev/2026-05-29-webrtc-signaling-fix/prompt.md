Tu dois appliquer 3 modifications dans le fichier WebRtcDataChannel.js du projet.

1. D'abord, trouve le fichier WebRtcDataChannel.js dans l'arborescence `frontend/js/streaming/`. S'il n'existe pas sous ce nom, cherche un fichier similaire (peut-être juste `DataChannel.js` ou un autre nom).

2. Applique ces 3 modifications exactement :

### Modification 1 - Dans `onopen` (retirer `_createPeerConnection()`)

Avant :
```javascript
this.signalingWs.onopen = () => {
    this._wsHadOpen = true;
    console.log('[WebRTC] Signaling WS connected, waiting for SDP offer...');
    this._createPeerConnection();
};
```

Après :
```javascript
this.signalingWs.onopen = () => {
    this._wsHadOpen = true;
    console.log('[WebRTC] Signaling WS connected, waiting for ICE config...');
};
```

### Modification 2 - Dans `_handleSignalingMessage`, bloc `ice-config` (créer le PC ici)

Avant :
```javascript
} else if (msg.type === 'ice-config') {
    console.log('[WebRTC] Received ice-config:', JSON.stringify(msg.iceServers));
    this._dynamicIceServers = msg.iceServers;
    if (this.pc) {
        try {
            this.pc.setConfiguration({ iceServers: this._dynamicIceServers });
            console.log('[WebRTC] Updated PC ICE servers via setConfiguration');
        } catch (e) {
            console.warn('[WebRTC] Failed to update PC ICE config:', e.message);
        }
    }
}
```

Après :
```javascript
} else if (msg.type === 'ice-config') {
    console.log('[WebRTC] Received ice-config:', JSON.stringify(msg.iceServers));
    this._dynamicIceServers = msg.iceServers;
    if (!this.pc) {
        this._createPeerConnection();
    }
}
```

### Modification 3 - Dans `_handleSdpOffer` (fallback si ice-config jamais arrivé)

Avant :
```javascript
async _handleSdpOffer(sdp) {
    console.log('[WebRTC] Received SDP offer, length=' + sdp.length);
```
Après :
```javascript
async _handleSdpOffer(sdp) {
    console.log('[WebRTC] Received SDP offer, length=' + sdp.length);

    if (!this.pc) {
        console.log('[WebRTC] Creating PC in SDP handler (ice-config not received)');
        this._createPeerConnection();
    }
```

3. En fin de travail, écris ton résumé dans `.claude/results/frontend-dev/2026-05-29-webrtc-signaling-fix/Resume-2026-05-29.md`.
   Inclus le chemin exact du fichier modifié, les 3 modifications appliquées, et le statut final (succès/échec).
