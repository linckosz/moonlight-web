# Certificat Load Refactor

Session ID: `2026-05-27-cert-refactor`

## Tache

Refactorer le chargement de certificat dans `HttpServer` pour qu'un chemin de certificat explicite (`m_CertPath`) soit utilise directement, sans forcer des noms de fichiers hardcodes (`fullchain.pem`, `cert.pem`).

## Probleme

Dans `HttpServer.cpp` :

1. `findCertDir()` (l.98-135) — si `m_CertPath` est defini, il extrait le **dossier** avec `fi.absolutePath()` et jette le nom du fichier
2. `loadCertFiles(certDir)` (l.191-235) — cherche des fichiers **hardcodes** dans le dossier : `fullchain.pem`, `cert.pem`, `key.pem`

Donc si l'utilisateur a un certificat avec un nom personnalise (ex: `mon_cert.pem`), le chemin complet est stocke dans `cert_path` mais le serveur ne trouve jamais le fichier car il cherche `fullchain.pem`/`cert.pem`.

## Solution

Modifier `loadCert()` pour deux cas :

### Cas 1 : `m_CertPath` est defini (chemin explicite)

Utiliser `m_CertPath` directement comme fichier certificat, chercher `key.pem` dans le meme dossier, et charger avec une nouvelle methode `loadCertFilesExplicit()`.

### Cas 2 : Pas de `m_CertPath` defini

Comportement existant inchangé : `findCertDir()` -> `loadCertFiles(certDir)`.

### Changements detailles

**1. `findCertDir()`** — Supprimer le bloc `if (!m_CertPath.isEmpty())` (lignes 100-108). `findCertDir()` ne doit que scanner par domaine + fallback.

**2. Nouvelle methode `loadCertFilesExplicit(certFilePath, keyFilePath)`** — Comme `loadCertFiles()` mais prend des chemins de fichiers complets :
```cpp
bool HttpServer::loadCertFilesExplicit(const QString& certFilePath, const QString& keyFilePath)
{
    QFile certFile(certFilePath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open cert file: " + certFile.errorString());
        return false;
    }
    QList<QSslCertificate> chain = QSslCertificate::fromDevice(&certFile, QSsl::Pem);
    certFile.close();

    QFile keyFile(keyFilePath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        Logger::warning("Failed to open key file: " + keyFile.errorString());
        return false;
    }
    QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    keyFile.close();

    if (chain.isEmpty() || key.isNull()) {
        Logger::warning("SSL cert chain / key invalid");
        return false;
    }

    m_SslConfig = QSslConfiguration::defaultConfiguration();
    m_SslConfig.setLocalCertificateChain(chain);
    m_SslConfig.setPrivateKey(key);
    m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);

    QString cn;
    if (!chain.isEmpty()) {
        QStringList cns = chain.first().subjectInfo(QSslCertificate::CommonName);
        cn = cns.isEmpty() ? "(no CN)" : cns.first();
    }
    Logger::info(QString("SSL certificate loaded: CN=%1, file=%2")
        .arg(cn, certFilePath));
    return true;
}
```

**3. `loadCert()`** — Reecrire en deux branches (cas explicite vs scan).

```cpp
bool HttpServer::loadCert()
{
    // Case 1: explicit cert_path
    if (!m_CertPath.isEmpty()) {
        QFileInfo fi(m_CertPath);
        if (fi.exists()) {
            QString keyPath = QDir(fi.absolutePath()).filePath("key.pem");
            Logger::info("[CERT] Loading explicit certificate: " + m_CertPath);
            if (loadCertFilesExplicit(m_CertPath, keyPath)) {
                // ... expiry check logic (same as current) ...
                return true;
            }
            Logger::warning("Failed to load explicit certificate, falling back to scan");
        } else {
            Logger::warning("[CERT] Explicit cert file not found: " + m_CertPath);
        }
    }

    // Case 2: scan by domain + fallback
    QString certDir = findCertDir();
    if (certDir.isEmpty()) {
        Logger::warning("No certificate directory found, generating self-signed");
        return generateSelfSignedCert();
    }
    if (!loadCertFiles(certDir)) {
        Logger::warning("Failed to load cert from: " + certDir);
        return generateSelfSignedCert();
    }
    return true;
}
```

**4. `HttpServer.h`** — Declarer `loadCertFilesExplicit()` en private.

## Fichiers a lire

- `backend/src/server/HttpServer.cpp` — lire le contenu actuel pour bien voir `findCertDir()`, `loadCertFiles()`, `loadCert()`
- `backend/src/server/HttpServer.h` — lire les declarations actuelles

## Instructions

1. Lis les deux fichiers source pour comprendre l'etat actuel
2. Applique les modifications decrites ci-dessus
3. Verifie que le code compile (lance le build)
4. Ecris ton resume dans `.claude/results/backend-dev/2026-05-27-cert-refactor/Resume-2026-05-27.md`

## Regles

- Commentaires en anglais, concis
- La cle privee est toujours cherchee sous le nom `key.pem` dans le meme dossier que le certificat
- Logger le chemin complet du fichier certificat charge et son CN
- Ne touche a rien d'autre
- En cas d'erreur de build, corrige-la immediatement
