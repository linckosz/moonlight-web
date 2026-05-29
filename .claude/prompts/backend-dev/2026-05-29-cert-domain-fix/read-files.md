## Tâche : Lecture des fichiers sources pour analyse de la gestion cert_pem/cert_key

Lis et résume les fichiers suivants. Pour chaque fichier, donne les informations clés : fonctions importantes, variables, logique de gestion du domaine et des certificats.

### Fichiers à lire

1. `d:\Code\moonlight-web-deepseek\backend\src\main.cpp`
   - Porte attention aux lignes 130-200 (setDomain, loadCert, certificateChanged handler)
   - La séquence de démarrage complète
   - Comment unique_id est utilisé

2. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.h`
   - Interface publique : setDomain(), domain(), loadCert(), reloadTls()
   - Variables membres liées au domaine et certificats

3. `d:\Code\moonlight-web-deepseek\backend\src\server\HttpServer.cpp`
   - loadCert() complet
   - reloadTls() complet
   - resolvePemValue()
   - La logique de validation CN
   - certificateChanged handler / signal

4. `d:\Code\moonlight-web-deepseek\backend\src\server\AppSettings.h`
   - Interface : domain(), setDomain(), uniqueId(), certPem(), setCertPem(), certKey(), setCertKey()

5. `d:\Code\moonlight-web-deepseek\backend\src\server\AppSettings.cpp`
   - Implémentation des getters/setters ci-dessus
   - Comment domain est stocké/chargé depuis settings.json

6. `d:\Code\moonlight-web-deepseek\backend\src\network\InternetAccessManager.h`
   - Interface : checkCertificate(), onAcmeFinished(), signaux

7. `d:\Code\moonlight-web-deepseek\backend\src\network\InternetAccessManager.cpp`
   - checkCertificate() — logique de validation du CN
   - onAcmeFinished() — logique d'écrasement cert_pem/cert_key
   - Comment le domaine est utilisé

8. `d:\Code\moonlight-web-deepseek\backend\src\streaming\Session.cpp`
   - Uniquement la partie qui utilise unique_id (si présente)

### Format de résumé attendu

Pour chaque fichier, donne :
- Les fonctions clés avec leur numéro de ligne
- Les variables/constantes importantes
- Le flux logique pour la gestion domaine/certificat
- Tout bug ou incohérence que tu remarques

Ne modifie rien, lis seulement.
