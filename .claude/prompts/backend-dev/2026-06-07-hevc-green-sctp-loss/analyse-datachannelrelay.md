Tu es backend-dev. Analyse les fichiers suivants pour comprendre le mécanisme de backpressure et d'envoi des chunks vidéo SCTP.

Lis ces fichiers :
1. `d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.cpp`
2. `d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.h`
3. `d:\Code\moonlight-web-deepseek\backend\src\streaming\StreamRelay.cpp`
4. `d:\Code\moonlight-web-deepseek\backend\src\streaming\StreamRelay.h`

Réponds précisément à ces questions :
1. Comment les chunks vidéo sont-ils envoyés sur le DataChannel ? Décris le mécanisme complet (découpage, buffer, envoi).
2. Y a-t-il un mécanisme de backpressure ? Si oui, quel est le seuil (watermark) ?
3. Que se passe-t-il quand le buffer SCTP est plein ? Les chunks sont-ils drop ? Les frames entières sont-elles abandonnées ?
4. Le mode SCTP utilisé est-il `ordered: false, maxRetransmits: 0` ? Peut-on le configurer ?
5. Y a-t-il des logs de perte ou de congestion ?
6. Quelle est la taille typique d'un chunk vidéo ? Et d'une frame complète HEVC ?

En fin de travail, écris ton résumé dans `.claude/results/backend-dev/2026-06-07-hevc-green-sctp-loss/Resume-2026-06-07.md`. Inclus uniquement tes résultats/conclusions.
