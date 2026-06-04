Session: 2026-06-03-host-offline-fix
Task: Investigate and fix host "offline" detection bug
Delegated by: engineering-manager

# Contexte

L'hote "780M" (adresse 192.168.1.9:47989) reste marque `state: "offline"` dans MWServer alors que Moonlight-QT le voit immediatement online.

Le backend C++/Qt (backend/src/backend/) semble avoir un probleme dans la detection de connectivite des hotes.

## Investigation a mener

1. **Cherche les resultats d'investigation precedents** : regarde dans `.claude/results/backend-dev/` et `.claude/results/engineering-manager/` pour un dossier recent (juin 2026) qui contiendrait une investigation sur le host offline.

2. **Lis les fichiers sources pertinents** :
   - `backend/src/backend/NvHTTP.cpp` (en entier)
   - `backend/src/backend/ComputerManager.cpp` (en entier)
   - `backend/src/backend/ComputerManager.h` (en entier)

3. **Analyse le flux de detection** :
   - Comment ComputerManager decouvre les hotes
   - Comment il verifie leur etat (online/offline)
   - Comment NvHTTP fait les requetes de test de connexion
   - Pourquoi un host avec un localAddress valide (192.168.1.9:47989) pourrait rester offline

4. **Compare avec moonlight-qt** si necessaire : utilise le skill `sync-moonlight-qt` pour voir comment moonlight-qt gere la detection online/offline.

5. **Identifie et applique les corrections** dans NvHTTP.cpp et/ou ComputerManager.cpp.

6. **Build et verifie** que ca compile avec `backend/build_msvc.bat`.

## Rapport

En fin de travail, ecris ton resume dans `.claude/results/backend-dev/2026-06-03-host-offline-fix/Resume-2026-06-03.md`.
Inclus : fichiers modifies, modifications apportees, raison du bug, resultat du build.
