Tu es backend-dev, un agent specialise dans le code C++/Qt du projet Moonlight-Web.

## Mission

Lis les fichiers listes ci-dessous et produis une analyse detaillee dans ton fichier resultat.

### Fichiers a lire

- d:\Code\moonlight-web-deepseek\backend\src\session\session.h
- d:\Code\moonlight-web-deepseek\backend\src\session\session.cpp
- d:\Code\moonlight-web-deepseek\backend\src\main.cpp (parties liees au transport/stream)
- d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.h
- d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.cpp
- d:\Code\moonlight-web-deepseek\backend\src\streaming\MediaTrackRelay.h
- d:\Code\moonlight-web-deepseek\backend\src\streaming\MediaTrackRelay.cpp
- d:\Code\moonlight-web-deepseek\backend\src\streaming\StreamRelay.h
- d:\Code\moonlight-web-deepseek\backend\src\streaming\StreamRelay.cpp
- d:\Code\moonlight-web-deepseek\backend\src\network\UPNPClient.h
- d:\Code\moonlight-web-deepseek\backend\src\network\UPNPClient.cpp
- d:\Code\moonlight-web-deepseek\backend\src\signaling\SignalingServer.h
- d:\Code\moonlight-web-deepseek\backend\src\signaling\SignalingServer.cpp
- d:\Code\moonlight-web-deepseek\backend\src\settings\AppSettings.h
- d:\Code\moonlight-web-deepseek\backend\src\settings\AppSettings.cpp

### Questions a repondre (analyse)

1. Quel est l'enum/type pour le transport ? Quelles sont les valeurs possibles (enum+string) ?
2. Comment le mode "auto" est-il resolu en un transport concret ? Ou se fait cette resolution ?
3. Quel est le codec enum et comment est-il choisi ?
4. Comment UPnP est-il appele ? Pour quels transports ?
5. Comment le fallback est-il implemente actuellement (simple webrtc→wss ?) ?
6. Quelle est la structure du StartRequest (donnees envoyees par le frontend pour lancer un stream) ?
7. Comment le transport est-il reporte/renvoye au frontend ?

### Format du resultat

Ecris ton resume dans :
.claude/results/backend-dev/2026-05-25-transport-fallback/Resume-2026-05-25.md

Structure :
- Fichier : [chemin] → [resume de ce qui est pertinent]
- Points d'attention / architecture
- Reponses aux 7 questions

Ne modifie aucun fichier. Lis seulement.
