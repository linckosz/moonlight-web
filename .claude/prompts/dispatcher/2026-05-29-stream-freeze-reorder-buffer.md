Demande utilisateur :
Bug : le stream freeze après ~1 seconde à chaque lancement. Régression introduite par le reorder buffer du fix de l'image verte.

Cause identifiée : le `_reorderBuffer` dans StreamView.js attend les frames manquantes indéfiniment. SCTP unordered + no-retransmit perd des frames. Le buffer attend frameId manquante pour drainer, freeze complet.

Solution attendue : gap tolerance avec timeout dans le reorder buffer :
- Si gap persiste ~200-300ms, avancer `_expectedFrameId`
- Ou si buffer dépasse N frames en attente, skipper gap et drainer
- Important : skipper une gap avec keyframe manquante produit des artefacts jusqu'au prochain IDR — acceptable vs freeze

Fichier : `d:\Code\moonlight-web-deepseek\frontend\js\ui\StreamView.js`
