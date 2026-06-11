# Session: 2026-06-10-android-freeze-diagnostic — backend-dev

## Tâche : Lecture de DataChannelRelay.h pour diagnostiquer le freeze Android

Tu travailles pour l'Engineering Manager. Tu dois lire le fichier backend suivant et rapporter les constantes et mécanismes pertinents.

### Contexte

Le stream WebRTC DC freeze sur Android après ~1 seconde. Une cause suspectée est que `kMaxPayloadSize` a été réduit à 14000, ce qui fragmente trop les frames H.264/HEVC en chunks SCTP, causant du reordering excessif qui trigger le dirty mode.

### Fichier à lire

**`d:\Code\moonlight-web-deepseek\backend\src\streaming\DataChannelRelay.h`**

Lis le fichier en entier. Rapporte :

1. **`kMaxPayloadSize`** — valeur actuelle, valeur originale
2. **`kMaxPacketSize`** — taille max SCTP
3. Toute constante de fragmentation
4. Comment les frames sont fragmentées en chunks (code pertinent)
5. Les commentaires ou todo liés à Android
6. Le header complet du fichier

### Instructions

Lis le fichier avec Read (fichier entier).

En fin de travail, écris ton résumé dans :
`.claude/results/backend-dev/2026-06-10-android-freeze-diagnostic/Resume-2026-06-10.md`

Inclus les valeurs exactes des constantes, la logique de fragmentation, et toute observation pertinente pour le diagnostic Android.
