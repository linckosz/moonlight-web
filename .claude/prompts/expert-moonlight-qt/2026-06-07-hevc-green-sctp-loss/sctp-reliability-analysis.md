Tu es expert-moonlight-qt. Analyse comment moonlight-qt gère la fiabilité du transport vidéo UDP/RTSP.

Lis ces fichiers dans D:\Code\moonlight-qt\app :
1. streaming/Videorenderer.cpp — cherche les mécanismes de détection d'erreur de décodage
2. streaming/VideoDepacketizer.cpp — comment moonlight gère les paquets perdus

Réponds à ces questions :
1. Comment moonlight-qt gère-t-il la perte de paquets vidéo UDP ? Y a-t-il un mécanisme de retransmission ?
2. Comment moonlight-qt détecte-t-il que le décodeur produit des images corrompues (green screen) ?
3. Quand et comment moonlight-qt demande-t-il un IDR frame à Sunshine pour récupérer d'une erreur ?
4. Quelle est la stratégie de moonlight-qt pour le backpressure ou le pacing du flux vidéo ?
5. Comment moonlight-qt gère-t-il le découpage des frames NAL pour l'envoi ?

Retourne uniquement les faits pertinents — pas d'opinion, pas de suggestion de fix.
