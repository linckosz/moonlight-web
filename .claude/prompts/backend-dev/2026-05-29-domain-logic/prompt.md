# Tâche : Implémenter la logique MW_DOMAIN dans AppSettings

Session : `2026-05-29-domain-logic`
Fichier de résultat : `.claude/results/backend-dev/2026-05-29-domain-logic/Resume-2026-05-29.md`

## Contexte

Le projet Moonlight-Web utilise un domaine pour le DNS dynamique. Ce domaine doit être configurable via une variable d'environnement `MW_DOMAIN` avec une valeur par défaut, et utilisé intelligemment dans `AppSettings::domain()`.

Actuellement, le champ `domain` dans settings.json contient probablement un domaine fixe. On veut rendre ça dynamique.

## Spécifications précises

### 1. Fichier `.env` (à la racine du projet)
- Ajouter : `MW_DOMAIN=moonlightweb.top`

### 2. `AppSettings::defaults()` — valeur par défaut de `"domain"`
- La valeur par défaut doit être la chaîne littérale `"MW_DOMAIN"` (une sentinelle)

### 3. `AppSettings::domain()` — nouvelle logique
```
AppSettings::domain() -> std::string
```

Lire la valeur de `domain` dans settings.json :

1. Si la valeur est un FQDN valide (contient au moins un point, e.g. `exemple.com`, `sub.exemple.com`), la retourner telle quelle.
2. Sinon (valeur = `"MW_DOMAIN"`, chaîne vide, ou n'importe quoi d'autre) :
   - Lire `unique_id` depuis settings.json (généré si absent, déjà géré par AppSettings)
   - Lire `MW_DOMAIN` depuis la variable d'environnement `MW_DOMAIN`
   - Fallback si la variable d'environnement n'existe pas : `"moonlightweb.top"`
   - Retourner `"{unique_id}.{MW_DOMAIN}"`

### 4. Helper de validation FQDN
Ajoute une méthode privée (ou fonction free dans le .cpp) :
```
bool isValidFqdn(const std::string& domain)
```
Un FQDN valide = chaîne non vide, contient au moins un point, caractères alphanumériques/tirets/points uniquement (regex simple : `^[a-zA-Z0-9.-]+$`), et le premier/dernier caractère n'est pas un point.

### 5. Vérifier tous les appels à `AppSettings::domain()`
- Assure-toi que tous les appels existants utilisent bien la méthode `AppSettings::domain()` (et non une valeur brute du settings JSON).
- Si tu trouves des appels qui lisent directement `_settings["domain"]` ou équivalent, remplace-les par l'appel à la méthode.

## Fichiers à lire et modifier

1. `.env` (racine du projet) — ajouter MW_DOMAIN
2. `backend/src/server/AppSettings.h` — déclarer/ajuster `domain()`, ajouter helper privé
3. `backend/src/server/AppSettings.cpp` — implémenter la logique, modifier `defaults()`
4. Chercher tous les fichiers qui appellent `AppSettings::domain()` ou lisent `"domain"` dans les settings — les vérifier

## Instructions

1. Lis d'abord `AppSettings.h` et `AppSettings.cpp` pour comprendre la structure actuelle
2. Lis les fichiers qui utilisent le domaine
3. Implémente les modifications
4. Vérifie qu'il n'y a pas de régressions (les appels existants à `domain()` doivent continuer à fonctionner)
5. Écris ton résumé dans `.claude/results/backend-dev/2026-05-29-domain-logic/Resume-2026-05-29.md`

## Format du résumé

```
# Résumé — MW_DOMAIN Logic
Date : 2026-05-29

## Fichiers modifiés
- fichier1 — ce qui a changé
- fichier2 — ce qui a changé

## Décisions prises
- point 1
- point 2

## Points bloquants éventuels
- ...
```
