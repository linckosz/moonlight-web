Tu es expert-moonlight-web-stream. Analyse comment moonlight-web-stream (Rust, WebRTC) gère la fiabilité du transport vidéo DataChannel.

Lis les fichiers pertinents dans D:\Code\moonlight-web-stream.

Réponds à ces questions :
1. Moonlight-web-stream utilise-t-il `ordered` ou `unordered` pour le DataChannel vidéo ?
2. Utilise-t-il des `maxRetransmits` ou `maxPacketLifeTime` ?
3. Comment gère-t-il les DataChannels SCTP pour éviter la perte de chunks vidéo ?
4. Y a-t-il un mécanisme de pacing ou backpressure ?
5. Quelle est la taille typique des chunks vidéo envoyés sur le SCTP DataChannel ?
6. Comment moonlight-web-stream gère-t-il les erreurs de décodage (frames corrompues, decoder reset) ?

Retourne uniquement les faits pertinents — pas d'opinion, pas de suggestion de fix.
