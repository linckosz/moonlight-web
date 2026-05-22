# Prompt pour backend-dev — Build & Compilation

## Contexte

Un nouveau fichier `AcmeClient.cpp` a ete cree dans `backend/src/network/` et le fichier `backend.pro` a ete mis a jour pour l'inclure. De plus :
- `DeSecClient.h/.cpp` a ete modifie (ajout de `createTxtRecord`, `deleteTxtRecord`)
- `InternetAccessManager.h/.cpp` a ete modifie (remplacement de acme.sh par AcmeClient)

Nous devons verifier que tout compile correctement.

## Tache

1. Execute la compilation via le build script :
   ```
   cmd //c d:/Code/moonlight-web-deepseek/backend/build_msvc.bat
   ```
   ou utilise qmake + nmake manuellement.

2. Si la compilation echoue, analyse les erreurs et corrige-les dans les fichiers concernes.

3. Continue a iterer compilation → correction jusqu'a ce que le build soit propre.

## Fichiers modifies/crees

- `backend/src/network/AcmeClient.h` (NOUVEAU)
- `backend/src/network/AcmeClient.cpp` (NOUVEAU)
- `backend/src/network/DeSecClient.h` (MODIFIE: createTxtRecord, deleteTxtRecord)
- `backend/src/network/DeSecClient.cpp` (MODIFIE: implementation TXT)
- `backend/src/network/InternetAccessManager.h` (MODIFIE: remplace acme.sh par AcmeClient)
- `backend/src/network/InternetAccessManager.cpp` (MODIFIE: utilise AcmeClient)
- `backend/backend.pro` (MODIFIE: ajout AcmeClient.cpp et .h)
- `backend/libs/windows/include/x64/` (OpenSSL headers - deja present)

## Rapport

Ecris ton rapport dans `.claude/results/backend-dev/2026-05-19-acme-native/Resume-2026-05-19.md` avec :
- Resultat de la compilation (succes/echec)
- Si echec : liste des erreurs et corrections apportees
- Si succes : confirmation que le build est propre
