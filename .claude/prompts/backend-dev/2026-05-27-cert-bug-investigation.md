# Investigation: certificat explicite non chargé

## Contexte
Le serveur HTTPS de Moonlight-Web sert le certificat self-signed `CN=Moonlight-Web` au lieu du certificat explicite `brunoocto.moonlightweb.top`.

## Logs observés
```
[INFO] SSL certificate found in C:/Users/Minis/AppData/Roaming/Moonlight-Web/Moonlight-Web/cert/
[INFO] SSL certificate loaded: CN=Moonlight-Web, file=cert.pem, dir=C:/Users/Minis/AppData/Roaming/Moonlight-Web/Moonlight-Web/cert/
[INFO] SSL certificate valid until 2027-05-27, no renewal needed
```

Pas de ligne `[CERT] Loading explicit certificate:` donc soit `m_CertPath` est vide, soit le fichier n'existe pas.

## Tâches

### 1. Lire HttpServer.cpp — fonction `loadCert()`
Fichier : `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.cpp`
- Lis les lignes ~240 à ~350 (la fonction `loadCert()` entière)
- Cherche comment `m_CertPath` est utilisé
- Cherche comment le log `Loading explicit certificate:` est produit
- Cherche la logique de fallback vers cert.pem

### 2. Lire main.cpp — configuration du certificat
Fichier : `d:\Code\moonlight-web-deepseek\backend\src\main.cpp`
- Lis les lignes ~60 à ~90 (partie `setCertPath`)
- Lis autour de la ligne ~800 (appel `server.start()`)
- Vérifie l'ordre : `setCertPath` est-il appelé AVANT `start()` ?

### 3. Lire AppSettings.h et AppSettings.cpp
Fichiers :
- `d:\Code\moonlight-web-deepseek\backend\src\settings\AppSettings.h`
- `d:\Code\moonlight-web-deepseek\backend\src\settings\AppSettings.cpp`
- Cherche la méthode `certPath()` — comment lit-elle la valeur dans settings.json ?
- Cherche la clé utilisée (probablement `cert_path`)

### 4. Vérifier les fichiers sur le disque
Exécute ces commandes et rapporte les résultats :

```bash
# Liste le dossier cert principal
ls -la "/c/Users/Minis/AppData/Roaming/Moonlight-Web/Moonlight-Web/cert/"

# Liste le dossier brunoocto_moonlightweb_top
ls -la "/c/Users/Minis/AppData/Roaming/Moonlight-Web/Moonlight-Web/cert/brunoocto_moonlightweb_top/"

# Vérifie si key.pem existe dans un sous-dossier
ls -la "/c/Users/Minis/AppData/Roaming/Moonlight-Web/Moonlight-Web/cert/"*
```

### 5. Lire settings.json
Fichier : `C:/Users/Minis/AppData/Roaming/Moonlight-Web/Moonlight-Web/settings.json`
- Lis le fichier (cat plain)
- Cherche la clé `cert_path`

## Rapport
Pour chaque point, écris ce que tu trouves dans :
`.claude/results/backend-dev/2026-05-27-cert-bug-investigation/Resume-2026-05-27.md`

Inclus :
- Le contenu pertinent de chaque fichier (extraits)
- Ce qui est correct et ce qui ne l'est pas
- TA CONCLUSION : quelle est la cause racine du bug ?

En fin de travail, écris ton résumé dans
`.claude/results/backend-dev/2026-05-27-cert-bug-investigation/Resume-2026-05-27.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
