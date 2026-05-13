# Mission: Analyser la construction de wsUrl dans le backend

## Contexte
L'utilisateur signale que la réponse de lancement contient `wsUrl: "wss://localhost:48001"` au lieu de l'IP du host (ex: `wss://192.168.1.9:48001`).

## Tâche
1. Cherche dans tout le backend (`backend/src/`) les endroits où la chaîne `wsUrl` est construite, sérialisée, ou renvoyée dans une réponse JSON.
2. Cherche aussi `48001` (le port WebSocket) et `wss://` dans le backend pour trouver où l'URL est assemblée.
3. Identifie précisément la ou les lignes de code responsables.
4. Explique POURQUOI localhost est utilisé à la place de l'IP du host — est-ce que c'est parce que le code prend l'IP locale du serveur au lieu de l'IP du host Sunshine ? Ou parce qu'il y a une variable non initialisée ?
5. Propose une modification précise (fichier, lignes, code de remplacement) pour utiliser l'IP du host Sunshine tel que découvert dans le ComputerManager.

## Règles
- Ne modifie AUCUN fichier. Rapport uniquement.
- Inclus les chemins absolus des fichiers concernés et les numéros de ligne.
- Sois précis : montre le code existant et le code proposé.

En fin de travail, écris ton résumé dans `.claude/results/backend-dev/{session}/Resume-YYYY-MM-DD.md`.
Session id: `2026-05-13-wsurl-fix`
