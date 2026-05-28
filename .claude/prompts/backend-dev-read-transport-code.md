Tu es backend-dev. Lis les fichiers suivants et pour chacun, donne un résumé concis de :

1. Comment le transport est sélectionné (enum, settings, logique de choix)
2. Comment le fallback est implémenté actuellement 
3. Comment le codec est sélectionné
4. Comment UPnP est intégré
5. Les fonctions/méthodes pertinentes (noms, signatures)

Fichiers à lire :
- d:\Code\moonlight-web-deepseek\backend\src\session\session.h
- d:\Code\moonlight-web-deepseek\backend\src\session\session.cpp
- d:\Code\moonlight-web-deepseek\backend\src\main.cpp
- d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.h
- d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.cpp
- d:\Code\moonlight-web-deepseek\backend\src\streaming\MediaTrackRelay.h
- d:\Code\moonlight-web-deepseek\backend\src\streaming\MediaTrackRelay.cpp
- d:\Code\moonlight-web-deepseek\backend\src\streaming\StreamRelay.h
- d:\Code\moonlight-web-deepseek\backend\src\streaming\StreamRelay.cpp
- d:\Code\moonlight-web-deepseek\backend\src\network\UPNPClient.h
- d:\Code\moonlight-web-deepseek\backend\src\network\UPNPClient.cpp
- d:\Code\moonlight-web-deepseek\backend\src\network\InternetAccessManager.h (si existe)
- d:\Code\moonlight-web-deepseek\backend\src\network\InternetAccessManager.cpp (si existe)
- d:\Code\moonlight-web-deepseek\backend\src\signaling\SignalingServer.h
- d:\Code\moonlight-web-deepseek\backend\src\signaling\SignalingServer.cpp
- d:\Code\moonlight-web-deepseek\backend\src\settings\AppSettings.h
- d:\Code\moonlight-web-deepseek\backend\src\settings\AppSettings.cpp

Résume dans ta réponse finale (pas de fichier). Sois complet sur :
- Les valeurs possibles du transport (enum/string)
- Le mapping entre le mode 'auto' et le transport réel choisi
- Où la décision du transport est prise (dans Session::start() ? dans main.cpp ?)
- Comment le codec (H264/HEVC/AV1) est choisi et où
- Comment UPnP est invoqué selon le transport

Ne modifie rien, lis seulement.
