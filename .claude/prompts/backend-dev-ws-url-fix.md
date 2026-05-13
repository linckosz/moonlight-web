# Tâche : Fix wsUrl — remplacer Host header par IP réelle

## Fichier concerné
`d:\Code\moonlight-web-deepseek\backend\src\main.cpp`

## Modification à appliquer

**Lignes 196-200** — Remplacer ce bloc :
```cpp
// Extract server host from the request Host header
QString serverHost = req.headers.value("host");
int colon = serverHost.indexOf(':');
if (colon >= 0)
    serverHost = serverHost.left(colon);
```

Par :
```cpp
// Use the discovered host's actual IP for WebSocket URL,
// not the browser's Host header (which can be "localhost").
QString serverHost = host->activeAddress.address();
```

## Contexte

Quand le navigateur est en HTTPS sur `localhost:48443`, le Host header vaut `localhost:48443`. Extraire `localhost` de ce header produit une URL WS `wss://localhost:...` qui marche pendant les tests locaux mais casse quand on accède par hostname ou IP réelle. Il faut utiliser l'adresse IP découverte du host Sunshine (`host->activeAddress`) qui est l'IP réelle du serveur.

## Instruction

1. Lis le fichier `d:\Code\moonlight-web-deepseek\backend\src\main.cpp` pour localiser le bloc concerné.
2. Applique le remplacement avec l'outil Edit.
3. Build le backend avec la commande : `cmd //c d:/Code/moonlight-web-deepseek/backend/build_msvc.bat`
4. Si le build échoue, corrige les erreurs et rebuild.
5. Écris ton résumé dans `.claude/results/backend-dev/2026-05-13-ws-url-host-fix/Resume-2026-05-13.md`

Format du résumé :
- Fichier modifié
- Remplacement exact effectué
- Build : succès ou échec (si échec, détail des erreurs et corrections)
