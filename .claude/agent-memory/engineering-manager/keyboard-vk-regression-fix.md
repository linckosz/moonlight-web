---
name: Keyboard VK Regression Fix
description: Correction regression clavier : le frontend envoyait des codes USB HID (avec SS_KBE_FLAG_NON_NORMALIZED) au lieu de Windows VK codes
metadata:
  type: project
---

## Correction regression clavier (2026-06-02)

Un fix anterieur avait casse l'input clavier en envoyant des **USB HID usage codes**
(0x04 pour A, 0x14 pour Q...) avec le flag `SS_KBE_FLAG_NON_NORMALIZED`.
Sunshine attend des **Windows Virtual Key codes** (0x41 pour A, 0x51 pour Q...)
avec flags=0 pour les touches standard.

### Cause racine

Le frontend (`StreamView.js`) avait une methode `codeToUsbHid()` qui mappait
`KeyboardEvent.code` vers USB HID usage IDs. Le message JSON envoyait `scancode`
(USB HID code) et le backend utilisait `scancode != 0` pour basculer en mode
`SS_KBE_FLAG_NON_NORMALIZED`. Sunshine recevait donc des codes USB HID interpretes
comme des raw scancodes Windows -- ce qui ne correspond a rien, tuant tout l'input.

### Correction

**Frontend** (`StreamView.js`) :
- `codeToUsbHid()` remplacee par `codeToWindowsVk()` qui retourne des VK codes
- `handleKeyDown/Up` utilisent `e.keyCode` comme VK primaire (correct Chrome/Edge)
  avec fallback `codeToWindowsVk(e.code)` si keyCode est 0
- Champ `scancode` supprime du message JSON
- `code: e.code` conserve pour que le backend detecte IntlBackslash/IntlRo

**Backend** (`DataChannelRelay.cpp`, `MediaTrackRelay.cpp`) :
- Suppression de la logique `scancode != 0 -> NON_NORMALIZED`
- Nouvelle condition : si `code == "IntlBackslash"` -> scancode 0x56 + NON_NORMALIZED
- Nouvelle condition : si `code == "IntlRo"` -> scancode 0x73 + NON_NORMALIZED
- Toutes les autres touches : VK code + flags=0 (standard)

**N'est pas modifie** : `StreamRelay.cpp` (WSS mode) -- utilisait deja VK+flags=0.

### Fichiers modifies

- `frontend/js/ui/StreamView.js` : codeToUsbHid -> codeToWindowsVk, keyCode fix
- `backend/src/streaming/DataChannelRelay.cpp` : condition IntlBackslash/IntlRo
- `backend/src/streaming/MediaTrackRelay.cpp` : condition IntlBackslash/IntlRo
