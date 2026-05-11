J'ai besoin de comprendre le protocole de transport vidéo utilisé par moonlight-web-stream (D:\Code\moonlight-web-stream), et de comparer sa performance réseau avec l'approche "passthrough WebSocket" de moonlight-web.

Contexte : Moonlight-Web (D:\Code\moonlight-web-deepseek) est un client de streaming dans le navigateur qui n'utilise PAS WebRTC. Le backend C++/Qt compile moonlight-common-c pour lancer la session streaming (RTSP handshake + réception RTP/UDP), puis forwarde les NAL units H.264 brutes au navigateur via une WebSocket binaire. Le navigateur utilise WebCodecs pour décoder.

Moonlight-web-stream (D:\Code\moonlight-web-stream) de l'autre côté utilise une approche WebRTC avec un proxy Rust.

Questions précises :

1. Dans moonlight-web-stream, comment la vidéo est-elle transportée de Sunshine au navigateur ? Est-ce :
   - Sunshine → WebRTC (via un peer GStreamer/Rust qui fait office de relay)
   - Sunshine → proxy → WebRTC → navigateur
   - Ou autre ?

2. Si la vidéo passe par WebRTC, comment le flux RTP de Sunshine (protocole propriétaire GameStream) est-il converti en WebRTC ? Où se fait l'encapsulation ?

3. En termes de performance réseau, quels sont les avantages/inconvénients de chaque approche :
   - WebSocket TCP (approche MW) : WebSocket encapsule les NAL units brutes sur TCP. Quels problèmes de latence/retransmission/réseau ?
   - WebRTC (approche MWS) : utilise ICE/DTLS/SRTP sur UDP. Quels avantages concrets (contrôle de congestion, FEC, prioritisation) ?

4. Le protocole GameStream vidéo est-il "compatible" avec WebRTC nativement ? Les paquets RTP GameStream ressemblent-ils à du RTP standard ?

5. Dans ton analyse, est-ce que l'approche passthrough WebSocket (comme MW) peut atteindre une performance réseau comparable à WebRTC pour un usage local/réseau local ? Quels sont les vrais goulets d'étranglement ?

Lis les fichiers clés de moonlight-web-stream pour comprendre l'architecture réseau. Si les fichiers ne sont pas lisibles directement, donne ton analyse basée sur ce que tu sais de GStreamer/WebRTC et du protocole GameStream.

Résumé attendu : comparaison technique des deux approches, avec forces et faiblesses de chacune.
