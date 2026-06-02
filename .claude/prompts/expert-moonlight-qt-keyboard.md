Tu es expert-moonlight-qt.

## Tache : Expliquer comment moonlight-qt gere le layout clavier (AZERTY vs QWERTY)

Dans le projet moonlight-qt (`D:\Code\moonlight-qt\app`), comment le keyboard input est-il capture et transmis a Sunshine ?

1. Trouve et lis les fichiers de gestion du clavier dans moonlight-qt (probablement dans `streaming/input/`)
2. Explique le pipeline complet :
   - Comment Qt capture les evenements clavier
   - Comment le mapping est fait (scancode, virtual key, etc.)
   - Comment LiSendKeyboardEvent est appele
3. Est-ce que moonlight-qt a un mecanisme specifique pour detecter le layout clavier du systeme ?
4. Y a-t-il un remapping de scancodes qui pourrait etre sensible au layout ?

Lis au moins :
- `streaming/input/InputManager.cpp`
- `streaming/input/SdlInput.cpp` (ou equivalent Qt)
- Les fichiers d'en-tete

Sois tres precis sur le mapping key → scancode. C'est critique pour comprendre un bug AZERTY → QWERTY.

En fin de travail, ecris ton resume dans
`.claude/results/expert-moonlight-qt/{session}/Resume-YYYY-MM-DD.md`.
