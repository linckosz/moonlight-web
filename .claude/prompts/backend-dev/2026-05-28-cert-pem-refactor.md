# Refactoring : cert_path / cert_expiry -> cert_pem / cert_key

Session: `2026-05-28-cert-pem-refactor`

## Objectif

Remplacer partout dans le code backend :
- `cert_path` → `cert_pem`
- `cert_expiry` → supprimer
- Ajouter `cert_key` (nouvelle clé)

## Principe

`cert_pem` et `cert_key` sont stockés dans settings.json. Par défaut, leur valeur est un nom de variable d'environnement (`MW_CERT_PEM`, `MW_CERT_KEY`). Quand Let's Encrypt émet un certificat, ces valeurs sont remplacées par des chemins de fichiers.

Résolution au runtime : si `qgetenv(valeur)` retourne non-vide → utiliser le contenu de l'env var. Sinon → traiter comme un chemin de fichier.

## Fichiers à modifier

1. `backend/src/server/AppSettings.h`
2. `backend/src/server/AppSettings.cpp`
3. `backend/src/server/HttpServer.h`
4. `backend/src/server/HttpServer.cpp`
5. `backend/src/network/InternetAccessManager.cpp`
6. `backend/src/network/InternetAccessManager.h`
7. `backend/src/main.cpp`

## Modifications détaillées

### Étape 1 : Rechercher et supprimer `cert_expiry`

Dans TOUS les fichiers, supprimer toute référence à `cert_expiry` ou `certExpiry` :
- `AppSettings.h/cpp` — vérifier qu'il n'y a plus rien (normalement déjà supprimé)
- `InternetAccessManager.cpp` — `statusJson()` vers ligne 355, `checkCertificate()` vers ligne 706-710, et toute autre occurrence
- `InternetAccessManager.h` — vérifier le signal `certificateChanged`
- `main.cpp` — toute occurrence
- Ne PAS toucher `docs/internet-plan.md`

### Étape 2 : Renommer `cert_path` → `cert_pem` partout

**AppSettings.h** :
- `certPath()` → `certPem()`
- `setCertPath()` → `setCertPem()`
- Supprimer `certExpiry()` / `setCertExpiry()` si encore présents

**AppSettings.cpp** :
- Même renommage
- Clé JSON `"cert_path"` → `"cert_pem"`
- Modifier la valeur par défaut : `"MW_CERT_PEM"` au lieu du chemin vide
- Supprimer tout code lié à `cert_expiry`

**HttpServer.h** :
- `m_CertPath` → `m_CertPem`
- `setCertPath()` → `setCertPem()`
- Ajouter `static QByteArray resolvePemValue(const QString& value);`

**HttpServer.cpp** :
- Renommage de toutes les occurrences
- `resolvePemValue` implémentation (voir détails plus bas)
- Modifier `loadCert()` (voir détails plus bas)

**InternetAccessManager.cpp et .h** :
- Toutes les occurrences de `certPath` / `cert_path` → `certPem` / `cert_pem`
- Signal `certificateChanged` — ajouter `keyPath` en paramètre (ou garder l'ancien et faire un overload)

**main.cpp** :
- `server.setCertPath(...)` → `server.setCertPem(...)`
- Handler `certificateChanged` : adapter pour inclure le key path

### Étape 3 : Ajouter `cert_key` dans AppSettings

```cpp
// AppSettings.h
QString certKey() const;       // default: "MW_CERT_KEY"
void setCertKey(const QString& key);

// AppSettings.cpp
QString AppSettings::certKey() const {
    QJsonObject obj = readAll();
    return obj.value("cert_key").toString(QStringLiteral("MW_CERT_KEY"));
}
void AppSettings::setCertKey(const QString& key) {
    QJsonObject obj = readAll();
    obj["cert_key"] = key;
    writeAll(obj);
}
```

Et modifier `certPem()` pour utiliser `"MW_CERT_PEM"` comme défaut :
```cpp
QString AppSettings::certPem() const {
    QJsonObject obj = readAll();
    return obj.value("cert_pem").toString(QStringLiteral("MW_CERT_PEM"));
}
```

### Étape 4 : resolvePemValue + loadCert dans HttpServer

Ajouter une méthode statique dans HttpServer :

```cpp
static QByteArray resolvePemValue(const QString& value) {
    if (value.isEmpty()) return {};
    QByteArray data = qgetenv(value.toUtf8());
    if (!data.isEmpty()) return data;
    QFile f(value);
    if (f.open(QIODevice::ReadOnly)) return f.readAll();
    return {};
}
```

Modifier `loadCert()` pour utiliser `resolvePemValue` :

```cpp
bool HttpServer::loadCert()
{
    QString certPemPath = m_Settings ? m_Settings->certPem() : QString();
    QString certKeyPath = m_Settings ? m_Settings->certKey() : QString();

    QByteArray certData = resolvePemValue(certPemPath);
    QByteArray keyData = resolvePemValue(certKeyPath);

    if (!certData.isEmpty() && !keyData.isEmpty()) {
        QList<QSslCertificate> chain = QSslCertificate::fromData(certData, QSsl::Pem);
        QSslKey key(keyData, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (key.isNull())
            key = QSslKey(keyData, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
        if (!chain.isEmpty() && !key.isNull()) {
            m_SslConfig = QSslConfiguration::defaultConfiguration();
            m_SslConfig.setLocalCertificateChain(chain);
            m_SslConfig.setPrivateKey(key);
            m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

            m_IdentityProvided = true;
            emit certChanged();

            Logger::info(QString("SSL certificate loaded from data: CN=%1")
                .arg(chain.first().subjectInfo(QSslCertificate::CommonName).first()));
            return true;
        }
    }

    // Si certData ou keyData est vide, fallback sur la méthode existante (findCertDir)
    // Conserver le reste de loadCert() inchangé
    ...
}
```

**Important** : Il faut garder le findCertDir() existant comme fallback. La logique actuelle de scan du répertoire cert doit rester intacte si `resolvePemValue` échoue pour les deux.

### Étape 5 : Mettre à jour InternetAccessManager

**`statusJson()`** :
- `"cert_path"` → `"cert_pem"`
- Ajouter `"cert_key"` avec la valeur de `m_Settings->certKey()`
- Supprimer `"cert_expiry"`

**`checkCertificate()`** :
- Remplacer `m_Settings->certPath()` par `m_Settings->certPem()`
- Supprimer la lecture de `certExpiry`
- Fonction `readCertExpiryFromFile()` si elle existe : à garder mais ne plus lire depuis settings

**`onAcmeFinished()`** :
- Remplacer :
  ```cpp
  m_Settings->setCertPath(fullchainPath);
  m_Settings->setCertExpiry(...);
  ```
  Par :
  ```cpp
  m_Settings->setCertPem(fullchainPath);
  m_Settings->setCertKey(domainKeyPath);
  ```

**Signal `certificateChanged`** :
- Soit : `certificateChanged(certPath, keyPath)` — deux QString
- Soit : garder le signal avec un seul paramètre et juste notifier que le certificat a changé (le handler dans main.cpp relit depuis AppSettings)

Je préfère la 2e option : garder `certificateChanged()` sans paramètre, et le handler dans la lambda de main.cpp lit `m_Settings->certPem()` et `m_Settings->certKey()` pour les passer à HttpServer.

### Étape 6 : Mettre à jour `main.cpp`

- `server.setCertPath(appSettings.certPath())` → `server.setCertPem(appSettings.certPem())` (si cette méthode setter existe encore)
- Dans le slot `certificateChanged` :
  ```cpp
  QObject::connect(&iam, &InternetAccessManager::certificateChanged, [&]() {
      server.setCertPem(settings.certPem());
      server.setCertKey(settings.certKey());
      server.loadCert();
  });
  ```
- Note : `setCertKey()` peut être une nouvelle méthode dans HttpServer, ou tu peux directement stocker dans m_Settings et laisser loadCert() les lire.

En fait, le plus simple : HttpServer.loadCert() lit déjà depuis `m_Settings`, donc il suffit d'appeler `loadCert()` dans le handler. Pas besoin de setters séparés pour certPem/certKey.

## Contraintes

- Commentaires en anglais, concis
- Ne pas casser la logique existante de fallback findCertDir()
- `QSslCertificate::fromData()` et `QSslKey(QByteArray, ...)` acceptent les données PEM directement
- La valeur par défaut de `cert_pem` est `"MW_CERT_PEM"`
- La valeur par défaut de `cert_key` est `"MW_CERT_KEY"`
- `resolvePemValue()` tente d'abord env var, puis fichier
- Garder `m_CertPath` / `m_CertPem` dans HttpServer si encore utilisé ailleurs que dans loadCert()

## Procédure

1. Lis chaque fichier listé ci-dessus
2. Applique les modifications dans l'ordre : AppSettings → HttpServer → InternetAccessManager → main.cpp
3. Compile avec `build_msvc.bat`
4. Corrige les éventuelles erreurs de compilation

## Résultat attendu

- `cert_expiry` n'apparaît plus dans aucun fichier .cpp/.h
- `cert_path` devient `cert_pem` partout (clé JSON, méthodes, variables)
- `cert_key` est ajouté dans AppSettings avec la clé JSON `"cert_key"`
- `loadCert()` résout les valeurs via env var puis fichier
- Le tout compile sans erreur

En fin de travail, écris ton résumé dans
`.claude/results/backend-dev/2026-05-28-cert-pem-refactor/Resume-2026-05-28.md`.
Inclus uniquement tes résultats/conclusions (pas la réflexion intermédiaire).
Format : tâche accomplie, fichiers modifiés, décisions prises, points bloquants.
