# Fix Domain Sentinel — AppSettings::domain() + InternetAccessManager::ensureIdentifiers()

## Contexte

Le sentinel `"MW_DOMAIN"` n'est jamais ecrit dans settings.json car `InternetAccessManager::ensureIdentifiers()` ecrase le champ `"domain"` avec le FQDN calcule (`uniqueId.baseDomain`) avant que `AppSettings::domain()` puisse distinguer un domaine custom d'un domaine par defaut.

## Fichiers a modifier

### 1. `d:\Code\moonlight-web-deepseek\backend\src\server\AppSettings.cpp`

**Methode `domain()`** (vers ligne 285-303) — renforcer la logique :

En plus de verifier `!= "MW_DOMAIN"`, comparer le stored domain avec le computed fallback. Si stored == computed, c'est le domaine par defaut, pas un custom -> retourner le computed (pour que si uniqueId change, le domaine soit recalcule).

Code attendu (approximatif — adapte a l'existant) :

```cpp
QString AppSettings::domain() const
{
    QJsonObject obj = readAll();
    QString stored = obj.value("domain").toString();

    // Compute fallback domain
    QString baseDomain = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    if (baseDomain.isEmpty())
        baseDomain = QStringLiteral("moonlightweb.top");

    QString uid = uniqueId();
    QString computed = uid.isEmpty() ? baseDomain : (uid + QLatin1Char('.') + baseDomain);

    // If stored is a real FQDN AND differs from computed -> custom domain, use it
    if (!stored.isEmpty() && stored != QStringLiteral("MW_DOMAIN") && isValidFqdn(stored) && stored != computed)
        return stored;

    return computed;
}
```

Tu devras peut-etre ajouter la methode `isValidFqdn()` si elle n'existe pas dans AppSettings. Utilise `QHostInfo::fromName()` ou un simple regex `^[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$`.

### 2. `d:\Code\moonlight-web-deepseek\backend\src\network\InternetAccessManager.cpp`

**Methode `ensureIdentifiers()`** (vers ligne 428-429) — remplacer :

```cpp
m_Settings->setDomain(m_Domain);
```

par :

```cpp
m_Settings->setDomain(QStringLiteral("MW_DOMAIN"));
```

Le `m_Domain` interne reste calcule et disponible pour le reste d'InternetAccessManager (ACME, DNS, etc.). On stocke juste le sentinel dans le JSON pour que `AppSettings::domain()` puisse faire la distinction.

## Instructions

1. **Lis** AppSettings.cpp autour de la methode `domain()` et InternetAccessManager.cpp autour de `ensureIdentifiers()`
2. **Modifie** les deux fichiers selon les specs ci-dessus
3. **Verifie** que `AppSettings::setDomain()` existe bien et prend un QString
4. **Compile** avec le skill `build` pour verifier que ca passe

## En fin de travail

Ecris ton resume dans `.claude/results/backend-dev/2026-05-29-domain-sentinel-fix/Resume-2026-05-29.md`. Inclus :
- Les modifications apportees a chaque fichier
- Le resultat du build
- Tout point bloquant
