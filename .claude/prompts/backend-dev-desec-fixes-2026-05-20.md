# Prompt pour backend-dev — Fixes DeSEC API Throttling

## Contexte
Tu travailles sur le projet Moonlight-Web, un client de streaming GameStream compatible Sunshine. Tu modifies le backend C++/Qt (MSVC 2022, Qt 6.11).

## Fichiers à modifier

### 1) `backend/src/network/InternetAccessManager.h`
Ajouter 2 nouveaux membres privés :
```cpp
int m_PendingRetryCount = 0;           ///< Current retry attempt (0..3) for pending registration
QDateTime m_LastDnsCheck;              ///< Last time DNS resolution was checked
```

Modifier le commentaire du slot `onPendingRegistrationRetry()` :
```cpp
/// Called when pending registration is active. Retries with backoff (30s, 60s, 120s), max 3 attempts.
void onPendingRegistrationRetry();
```

### 2) `backend/src/network/InternetAccessManager.cpp`

#### a) Modifier `onPendingRegistrationRetry()` (lignes 617-655)
Remplacer la boucle de retry infinie toutes les 30s par :

```cpp
void InternetAccessManager::onPendingRegistrationRetry()
{
    m_PendingRetryCount++;

    if (m_PendingRetryCount > 3) {
        // Max retries exceeded — give up
        m_LastError = QStringLiteral(
            "deSEC domain registration failed after %1 attempts. "
            "Internet Access has been disabled. Check your network connectivity "
            "and deSEC token, then re-enable Internet Access in Settings.")
            .arg(m_PendingRetryCount);
        qWarning() << "[InternetAccess]" << m_LastError;

        m_Settings->setPendingRegistration(false);
        m_Settings->setInternetAccessEnabled(false);
        m_PendingRegistrationTimer->stop();
        m_PendingRetryCount = 0;

        emit error(m_LastError);
        emit statusChanged(statusJson());
        return;
    }

    // Exponential backoff: 30s, 60s, 120s
    int delays[] = { 30, 60, 120 };
    int delaySec = delays[m_PendingRetryCount - 1];
    int delayMs = delaySec * 1000;

    qInfo() << "[InternetAccess] Retrying pending domain registration..."
            << "attempt" << m_PendingRetryCount << "/3"
            << "next retry in" << delaySec << "s";

    // Regenerate unique ID and domain for each retry
    m_UniqueId = generateUniqueId();
    m_Settings->setUniqueId(m_UniqueId);
    m_Domain = buildDomain();
    m_Settings->setDomain(m_Domain);

    // Re-set token (env var may have been configured since last attempt)
    QString token = effectiveToken();
    if (token.isEmpty()) {
        qInfo() << "[InternetAccess] Token still empty — will retry in" << delaySec << "s";
        m_PendingRegistrationTimer->start(delayMs);
        return;
    }
    m_DeSec.setToken(token);

    if (createOrUpdateARecord()) {
        m_Settings->setPendingRegistration(false);
        m_PendingRetryCount = 0;
        qInfo() << "[InternetAccess] A record created on retry:" << m_Domain;

        // Continue with the rest of the setup
        if (m_Settings->autoIpDetection()) {
            detectPublicIp();
        }
        if (!m_PublicIp.isEmpty()) {
            updateARecord();
        }
        checkCertificate();

        m_Active = true;
        m_PeriodicCheckTimer->start();

        emit ready(m_Domain, m_PublicIp);
    } else {
        qInfo() << "[InternetAccess] Registration retry failed (" << m_PendingRetryCount
                << "/3) — will retry in" << delaySec << "s";
        m_PendingRegistrationTimer->start(delayMs);
    }
}
```

#### b) Modifier `start()` (apres la ligne 138)
Apres `m_Settings->setPendingRegistration(false);` et le log "Step 3 OK — A record exists", ajouter un DNS check initial :

```cpp
    // Step 3b: Initial DNS check when A record already exists
    QString resolvedIp = resolveDomain(m_Domain);
    if (!resolvedIp.isEmpty()) {
        qInfo() << "[InternetAccess] Initial DNS check:" << m_Domain << "->" << resolvedIp;
    } else {
        qWarning() << "[InternetAccess] Initial DNS check failed for" << m_Domain;
    }
    m_LastDnsCheck = QDateTime::currentDateTimeUtc();
```

#### c) Modifier `onPeriodicCheck()` (lignes 571-615)
Remplacer la section 2 (DNS check, lignes 588-598) par :

```cpp
    // 2. Check DNS resolution (max once every 24h)
    QDateTime now = QDateTime::currentDateTimeUtc();
    if (!m_LastDnsCheck.isValid() || m_LastDnsCheck.secsTo(now) >= 86400) {
        QString resolvedIp = resolveDomain(m_Domain);
        if (!resolvedIp.isEmpty()) {
            if (!m_PublicIp.isEmpty() && resolvedIp != m_PublicIp) {
                qInfo() << "[InternetAccess] DNS resolved to" << resolvedIp
                        << "but expected" << m_PublicIp << "— updating A record";
                updateARecord();
            }
        } else {
            qWarning() << "[InternetAccess] DNS resolution failed for" << m_Domain;
        }
        m_LastDnsCheck = now;
    }
```

#### d) Dans `stop()`, ajouter la reinitialisation du retry counter
Apres `m_PendingRegistrationTimer->stop();` (ligne 196) :
```cpp
    m_PendingRetryCount = 0;
```

### 3) `backend/src/network/DeSecClient.h`
Supprimer la declaration de `listDomains` (lignes 77-78) :
```cpp
    // Retirer ces 3 lignes :
    bool listDomains(QStringList& domains, QString& errorMsg);
```
Et supprimer le commentaire correspondant (lignes 73-76).

### 4) `backend/src/network/DeSecClient.cpp`

#### a) Supprimer l'implementation de `listDomains` (lignes 324-364)
Supprimer toute la fonction `listDomains` (du `bool DeSecClient::listDomains` jusqu'a l'accolade fermante).

ATTENTION : Garder la ligne vide et le commentaire "// TXT record management" qui suit.

#### b) Ajouter la gestion HTTP 429 dans `sendRequest()` (apres la ligne 63)
Dans la methode `sendRequest()`, apres `timer.stop();` et avant `return reply;`, ajouter :

```cpp
    // Check for HTTP 429 (Rate Limited) and log the retry-after detail
    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 429) {
        QByteArray responseBody = reply->readAll();
        // re-read buffer — reply->readAll() can only be called once
        // Actually we haven't read it yet, but we need to check.
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        QString detail;
        if (doc.isObject()) {
            detail = doc.object().value(QStringLiteral("detail")).toString();
        }
        // Parse "Expected available in X seconds" from detail
        int waitSeconds = 60; // default fallback
        QRegularExpression re(QStringLiteral("(\\d+)\\s*seconds?"));
        QRegularExpressionMatch m = re.match(detail);
        if (m.hasMatch()) {
            waitSeconds = m.captured(1).toInt();
        }
        qWarning() << "[DeSecClient] HTTP 429 (Rate Limited) —" << detail
                   << "— waiting" << waitSeconds << "seconds recommended"
                   << "for" << method << path;
    }
    // Reset read position since we may have consumed the body
    // Actually in a synchronous model we can't "unread", but we can store the status and body
    // before the caller reads them. However, since this is synchronous and reply will be
    // processed immediately, we just log a warning.
```

ATTENTION : Cette approche a un probleme car on fait `readAll()` ici, et l'appelant fera aussi `readAll()` apres. Il faut restructurer.

**Meilleure approche** : Creer une methode privee `logRateLimit(QNetworkReply* reply)` qui verifie le status code sans consommer le body :

```cpp
/// Log a warning if the reply indicates HTTP 429 (Rate Limited).
/// Does NOT consume the reply body — safe to call before the caller reads it.
void DeSecClient::logRateLimit(QNetworkReply* reply)
{
    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 429) {
        // Read the body for the detail field
        QByteArray responseBody = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        QString detail;
        if (doc.isObject()) {
            detail = doc.object().value(QStringLiteral("detail")).toString();
        }
        // Parse "Expected available in X seconds" from detail
        int waitSeconds = 60; // default fallback
        QRegularExpression re(QStringLiteral("(\\d+)\\s*seconds?"),
                              QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch m = re.match(detail);
        if (m.hasMatch()) {
            waitSeconds = m.captured(1).toInt();
        }
        qWarning() << "[DeSecClient] HTTP 429 (Rate Limited) —" << qPrintable(detail)
                   << "— waiting" << waitSeconds << "seconds recommended";
    }
}
```

Ajouter la declaration de `logRateLimit` dans `DeSecClient.h` (dans la section private).

Ajouter un appel a `logRateLimit(reply)` dans `sendRequest()`, juste apres `timer.stop();` et avant `return reply;`.

MAIS ATTENTION : `logRateLimit` lit le body avec `readAll()`. Quand l'appelant va appeler `reply->readAll()`, il va obtenir une QByteArray vide car le buffer a deja ete lu. Il faut donc soit :
1. Stocker le body dans la methode et le remettre dans le reply (pas possible)
2. Faire en sorte que `sendRequest` retourne un struct avec status + body + reply
3. Juste logger le 429 avec les headers, sans lire le body

Option 3 est la plus simple et suffisante :
```cpp
void DeSecClient::logRateLimit(QNetworkReply* reply)
{
    int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 429) {
        QByteArray retryAfter = reply->rawHeader("Retry-After");
        qWarning() << "[DeSecClient] HTTP 429 (Rate Limited) —"
                   << "Retry-After:" << QString::fromUtf8(retryAfter)
                   << "for" << reply->url().toString();
    }
}
```

Utilise cette derniere approche (simple, basee sur le header Retry-After et le URL).

### 5) `backend/src/network/AcmeClient.cpp`
Ajouter la gestion HTTP 429 dans `createChallengeTxtRecord()` et `deleteChallengeTxtRecord()`.

#### a) Dans `createChallengeTxtRecord()` (apres la ligne 473)
Apres `int code = reply->attribute(...)` et avant `reply->deleteLater();`, ajouter :
```cpp
    // Log deSEC rate limiting
    if (code == 429) {
        QByteArray retryAfter = reply->rawHeader("Retry-After");
        qWarning() << "[AcmeClient] deSEC TXT creation HTTP 429 (Rate Limited) —"
                   << "Retry-After:" << QString::fromUtf8(retryAfter);
    }
```

#### b) Dans `deleteChallengeTxtRecord()` (apres la ligne 512)
Apres `int code = reply->attribute(...)` et avant `reply->deleteLater();`, ajouter :
```cpp
    // Log deSEC rate limiting
    if (code == 429) {
        QByteArray retryAfter = reply->rawHeader("Retry-After");
        qWarning() << "[AcmeClient] deSEC TXT deletion HTTP 429 (Rate Limited) —"
                   << "Retry-After:" << QString::fromUtf8(retryAfter);
    }
```

### 6) `backend/src/network/AcmeClient.h` (optionnel)
Ajouter les includes manquants (QRegularExpression) si necessaire. Probablement pas necessaire si on utilise juste `Retry-After` header.

## Verification finale
Apres tous les changements, compile avec `build` skill pour t'assurer que tout compile.

## Fichier de resultat
En fin de travail, ecris ton resume dans `.claude/results/backend-dev/desec-fixes-2026-05-20/Resume-2026-05-20.md`. 
Inclus : chaque fichier modifie, ce qui a ete change, et le resultat de la compilation.
