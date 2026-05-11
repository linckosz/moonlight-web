J'ai besoin de comprendre en détail le protocole utilisé pour le transport des frames vidéo dans moonlight-common-c / moonlight-qt.

Contexte : Le projet Moonlight-Web est un client de streaming dans le navigateur (pas de WebRTC, pas de MSE — on utilise WebCodecs + Canvas côté frontend).

Questions précises :

1. Après le handshake RTSP, quel protocole transporte les données vidéo brutes ? Est-ce du RTP sur UDP ? Ou ENet ? Ou autre chose ?

2. Les NAL units H.264 arrivent-elles encapsulées dans RTP (comme le standard RTP/H.264 RFC 6184) ? Ou dans un format différent ?

3. Où se fait le réassemblage des paquets RTP et la reconstruction des NAL units complètes ? Dans quelle fonction/fichier de moonlight-common-c ?

4. Y a-t-il du FEC (Forward Error Correction) côté vidéo dans le protocole GameStream ? Si oui, comment ça fonctionne ?

5. Est-ce que le protocole utilise une forme de pacing / contrôle de congestion, ou est-ce que Sunshine envoie à fond et le client adapte (ou non) ?

6. Quelle est la latence typique ajoutée par le transport RTP/UDP vs un transport sur TCP comme WebSocket ?

7. Est-ce que moonlight-common-c utilise une socket UDP brute pour la vidéo, ou passe par ENet comme le canal de contrôle ?

Lis les fichiers pertinents dans D:\Code\moonlight-qt\moonlight-common-c\src\ (RtpVideoQueue.c, Connection.c, ControlStream.c, Limelight.h, PlatformSockets.c) pour répondre de manière précise.

Résumé attendu : une explication claire du protocole de transport vidéo de bout en bout (Sunshine → moonlight-common-c), avec les noms de fichiers et fonctions clés.
