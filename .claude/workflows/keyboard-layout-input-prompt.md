Ce fichier sert de coordination pour le debogage du layout clavier. Rempli par les agents.

## Frontend (StreamView.js / WebRtcMedia.js / InputManager.js)

Comment sont captures les evenements clavier ?
- `event.code` (physique) ou `event.key` (caractere) ?
- Mapping vers scancode USB HID ?
- Quel format est envoye sur le DataChannel ?

## Backend (InputHandler / StreamSession / ...)

Comment les evenements clavier sont-ils recus depuis le WebSocket/DataChannel ?
- Quel format attendu ?
- Mapping vers LiSendKeyboardEvent ?
- Y a-t-il un remapping de scancodes quelque part ?

## moonlight-qt (reference)

Comment moonlight-qt gere-t-il le layout clavier dans InputManager.cpp ?
Mapping scancode, virtual key, layout detection ?

## Analyse en cours - EM en direct

BUG TROUVE : InputEncoder.cpp ligne 66 attend `msg["keyCode"]` comme Windows VK.
A verifier dans le frontend si c'est `event.keyCode` (layout-dependent) qui est envoye.
