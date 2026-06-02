Tu es frontend-dev, tu travailles sur le projet Moonlight-Web (Phase 7 Input).

## Tache : Analyser la capture des evenements clavier dans le frontend

Le user rapporte un bug : clavier FR (AZERTY) se comporte en QWERTY pendant le streaming.

1. Liste tous les fichiers JS dans `frontend/src/` qui gerent les evenements clavier.
2. Pour chaque fichier trouve, identifie :
   - Comment l'evenement clavier est capture (keydown/keyup)
   - Si `event.code` (physique, layout-independent) ou `event.key` (caractere, layout-dependent) est utilise
   - Comment le scancode/message est encode
   - Quel format est envoye sur le DataChannel
3. Verifie s'il y a un mapping de scancodes (USB HID, etc.) et si ce mapping est correct
4. Analyse si le traitement des keyCodes est correct pour un clavier AZERTY

Commence par lister les fichiers pertinents, puis lis les sections concernees.

Cherche en particulier :
- Des mappings de type keyCode → scancode qui supposent un layout QWERTY
- L'usage de `event.key` la ou `event.code` serait plus approprie
- Tout code qui fait `event.keyCode` ou `event.which` (deprecated, layout-dependent)

En fin de travail, ecris ton resume dans
`.claude/results/frontend-dev/{session}/Resume-YYYY-MM-DD.md`.
