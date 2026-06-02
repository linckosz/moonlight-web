Tu es backend-dev, tu travailles sur le projet Moonlight-Web (Phase 7 Input).

## Tache : Analyser le traitement des evenements clavier dans le backend C++

Le user rapporte un bug : clavier FR (AZERTY) se comporte en QWERTY pendant le streaming.

1. Liste les fichiers C++ dans `backend/src/` qui gerent les evenements clavier.
2. Pour chaque fichier, identifie :
   - Comment les donnees clavier arrivent (DataChannel callback, format attendu)
   - Comment elles sont transformees/applaties
   - Comment elles sont envoyees a Sunshine (via quelle API moonlight-common-c)
3. Verifie si un mapping de scancodes est applique quelque part
4. Verifie si LiSendKeyboardEvent est appele correctement

Cherche en particulier :
- Des fichiers InputManager, InputHandler, KeyboardHandler
- L'usage de LiSendKeyboardEvent
- Tout mapping de scancodes ou virtual keys
- La structure de donnees contenant les evenements clavier

En fin de travail, ecris ton resume dans
`.claude/results/backend-dev/{session}/Resume-YYYY-MM-DD.md`.
