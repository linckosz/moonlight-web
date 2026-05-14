# Mission : Recherche API nport + NportClient existant

Tu travailles sur Moonlight-Web. Nous voulons éliminer la dépendance Node.js/npx/nport en lisant le code source du CLI nport pour comprendre l'API REST qu'il appelle, puis adapter notre NportClient.

## Tâche 1 : Lire le code actuel de NportClient

Lis les fichiers suivants et résume leur contenu :
- `d:\Code\moonlight-web-deepseek\backend\src\network\NportClient.h`
- `d:\Code\moonlight-web-deepseek\backend\src\network\NportClient.cpp`

## Tâche 2 : Récupérer le code source de nport depuis GitHub

Le CLI nport est sur https://github.com/tuanngocptn/nport

Utilise curl (ou tout autre moyen disponible) pour télécharger les fichiers source clés du CLI nport depuis GitHub raw :

1. D'abord, explore la structure du repo via l'API GitHub :
   ```
   curl -s https://api.github.com/repos/tuanngocptn/nport/contents/
   ```
   
2. Télécharge les fichiers suivants (et tout autre fichier pertinent découvert) :
   - Le point d'entrée principal du CLI (probablement `src/index.ts` ou `src/main.ts` ou `cli.ts`)
   - Tout fichier qui contient l'appel API REST à nport.io
   - Tout fichier qui montre le lancement de cloudflared

   Utilise des URLs comme :
   ```
   curl -s https://raw.githubusercontent.com/tuanngocptn/nport/main/src/index.ts
   ```

3. Si le repo est en TypeScript, concentre-toi sur :
   - Comment le CLI appelle l'API REST pour réserver un sous-domaine
   - Les paramètres de l'appel API (URL, méthode, headers, body)
   - Le format de la réponse
   - Comment cloudflared est lancé (binaire, arguments, flags)
   - Où le binaire cloudflared est cherché

## Ce que tu dois déterminer

1. **API REST nport** :
   - URL exacte de l'API (POST/GET ?)
   - Headers nécessaires
   - Body de la requête
   - Format de la réponse (JSON ?)
   - Sous-domaine généré ou fourni ?

2. **Lancement cloudflared** :
   - Commande exacte lancée
   - Arguments passés à cloudflared
   - Comment l'URL publique est déterminée

## Livrable

Écris un résumé complet dans `.claude/results/backend-dev/2026-05-14-cloudflared-migration/Resume-YYYY-MM-DD.md` avec :
- Le contenu résumé des fichiers NportClient actuels
- Les URLs exactes, paramètres, et format de réponse de l'API nport
- La commande cloudflared exacte à reproduire
- Les fichiers source nport que tu as lus

```
En fin de travail, écris ton résumé dans
`.claude/results/backend-dev/2026-05-14-cloudflared-migration/Resume-YYYY-MM-DD.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
```
