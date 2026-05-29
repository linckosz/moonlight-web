# Mission: Analyser le bug image verte HEVC (Backend)

## Contexte
Le streaming HEVC produit une image verte. Les logs backend montrent une anomalie dans la négociation du codec :

```
[Auto] Codec 1 not supported by host, falling back to H.264
[Session] Video codec preference set to 1     ← HEVC forcé quand même !
```

Le codec 1 est forcé (HEVC) malgré que le host ait répondu qu'il ne supporte pas l'HEVC.

## Tâche

### 1. Lis `d:\Code\moonlight-web-deepseek\backend\src\streaming\Session.cpp`
- Analyse le flow de négociation de codec vidéo
- Cherche où "Codec 1 not supported by host" est loggé
- Cherche comment `videoCodecPreference` est défini
- Vérifie le mapping entre les constantes de codec et leur signification (H.264=0, HEVC=1, AV1=2 ?)
- Vérifie la logique : si le host ne supporte pas HEVC, pourquoi est-il forcé ?

### 2. Vérifie aussi le codec negotiation dans le lancement du stream
- `negotiateCodec()` ou fonction similaire
- Comment les préférences utilisateur sont appliquées
- Si le fallback H.264 est correctement propagé

### 3. Regarde la structure des données video dans le DataChannel relay
- `libdatachannel/src/DataChannelRelay.cpp` ou fichier similaire — comment les frames sont relayées
- Est-ce que le type de codec est correctement passé au frontend ?

## Output
- Décris le problème exact que tu trouves dans la négociation de codec
- Si tu trouves le bug, écris la correction directement
- Résumé dans `.claude/results/backend-dev/2026-05-29-hevc-green-fix/Resume-2026-05-29.md`

En fin de travail, écris ton résumé dans `.claude/results/backend-dev/2026-05-29-hevc-green-fix/Resume-2026-05-29.md`. Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire). Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
