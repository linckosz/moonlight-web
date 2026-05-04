#pragma once

#include <QString>
#include <QByteArray>

// Forward declarations — OpenSSL types
struct x509_st;
struct evp_pkey_st;
typedef struct x509_st X509;
typedef struct evp_pkey_st EVP_PKEY;

class IdentityManager
{
public:
    // PEM certificate and private key bytes
    QByteArray getCertificate();
    QByteArray getPrivateKey();

    // Unique client ID, persisted in QSettings
    QString getUniqueId();

    // OpenSSL parsed structs (caller must NOT free)
    X509* getCertStruct();
    EVP_PKEY* getKeyStruct();

    static IdentityManager* get();

private:
    IdentityManager();
    ~IdentityManager();

    void createCredentials();
    void loadOrGenerate();

    QByteArray m_CachedPemCert;
    QByteArray m_CachedPrivateKey;
    QString m_CachedUniqueId;

    X509* m_Cert = nullptr;
    EVP_PKEY* m_Key = nullptr;
    bool m_CredentialsLoaded = false;

    static IdentityManager* s_Instance;
};
