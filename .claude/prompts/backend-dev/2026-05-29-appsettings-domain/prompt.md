## Tache : Corriger `AppSettings::domain()` pour respecter la valeur stockee dans settings.json

### Contexte

Actuellement, `AppSettings::domain()` ignore toujours le champ `"domain"` de settings.json et calcule systematiquement `{uniqueId}.{MW_DOMAIN}`. C'est un bug : la documentation dans le header dit qu'elle doit d'abord lire settings.json.

### Comportement attendu

La methode `domain()` doit suivre cette logique :

1. Lire la cle `"domain"` depuis settings.json (QSettings)
2. Si la valeur est definie, **non vide**, differente de `"MW_DOMAIN"` (le sentinel), ET est un FQDN valide (contient au moins un point) → retourner cette valeur telle quelle
3. Sinon → fallback : construire `{uniqueId}.{MW_DOMAIN}` ou MW_DOMAIN est la variable d'env (fallback "moonlightweb.top"). Si uniqueId est vide, retourner juste le base domain.

### Definition d'un FQDN valide

Un FQDN valide contient au moins un point `.` et n'est pas vide. Par exemples valides : `mon-super-host.moonlightweb.top`, `example.com`. Non valides : `MW_DOMAIN`, ``, `localhost`, `monserveur`.

### Fichier a modifier

- `backend/src/server/AppSettings.cpp` — la methode `domain()` (lignes 285-300 environ)

### Helper a utiliser ou creer

Tu peux ajouter une petite methode privee `isValidFqdn(const QString &domain)` dans AppSettings (header + cpp) ou faire la verification inline. Comme tu preferes.

### Note importante

Le sentinel `"MW_DOMAIN"` a le meme role que `"MW_CERT_PEM"` et `"MW_CERT_KEY"` deja presents dans le code — il signifie "non configure". Ne pas confondre avec la variable d'env `MW_DOMAIN` qui est le domaine de base (ex: "moonlightweb.top").

### Validation

Apres la modification :
- Si settings.json contient `"domain": "monsite.example.com"` → `domain()` retourne `"monsite.example.com"`
- Si settings.json contient `"domain": ""` → fallback → `"{uniqueId}.moonlightweb.top"`
- Si settings.json contient `"domain": "MW_DOMAIN"` (sentinel, valeur par defaut) → fallback → `"{uniqueId}.moonlightweb.top"`
- Si settings.json ne contient pas la cle `"domain"` → fallback → `"{uniqueId}.moonlightweb.top"`
- Si uniqueId est vide → retourne juste `"moonlightweb.top"`

### Resultat attendu

En fin de travail, ecris ton resume dans `.claude/results/backend-dev/2026-05-29-appsettings-domain/Resume-2026-05-29.md`.
Inclus : fichiers modifies, changements effectues, tests de validation.
