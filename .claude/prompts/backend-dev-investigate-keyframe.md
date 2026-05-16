## Tâche : Investiguer le buffering de keyframe dans DataChannelRelay.cpp

### Contexte
L'utilisateur streame en H.264. Sur LAN, les couleurs sont correctes. Sur Internet via UPnP, les couleurs sont incorrectes. La seule difference est le timing du buffer keyframe :
- **LAN** : ICE rapide, DC s'ouvre vite, keyframe envoyee en temps reel
- **UPnP** : ICE plus lent, keyframe bufferisee puis envoyee quand le DC s'ouvre

### Fichiers a examiner
1. `d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.cpp` — regarde comment la keyframe est bufferisee et envoyee
2. `d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.h` — declaration

### Questions
1. `m_BufferedKeyframe = data` fait-il une copie profonde ? (QByteArray utilise implicit sharing, donc oui — mais verifie que le detach()/modification n'arrive pas ailleurs)
2. Quand la keyframe bufferisee est-elle envoyee ? Dans quel ordre arrive-t-elle vs les autres paquets ?
3. Y a-t-il un risque que la keyframe soit tronquee/corrompue ?
4. Que contient exactement la keyframe ? Est-ce juste des NAL units (SPS/PPS/IDR) ou y a-t-il un wrapping supplementaire ?
5. Quels sont les logs disponibles cote backend ?

### Rapport
Ecrit ton resume dans `.claude/results/backend-dev/2026-05-16-color-investigation/Resume-2026-05-16.md`.
Inclus le contenu pertinent des fonctions concernees.
