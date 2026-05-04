#pragma once

#include <QString>
#include <QByteArray>
#include <QNetworkAccessManager>

struct x509_st;
struct evp_pkey_st;
typedef struct x509_st X509;
typedef struct evp_pkey_st EVP_PKEY;

class NvPairingManager
{
public:
    enum PairState {
        PAIRED,
        PIN_WRONG,
        FAILED,
        ALREADY_IN_PROGRESS
    };

    enum InitResult {
        INIT_OK,
        INIT_FAILED,
        INIT_ALREADY_IN_PROGRESS
    };

    NvPairingManager(const QString& appVersion,
                     const QString& host, quint16 httpPort, quint16 httpsPort);
    ~NvPairingManager();

    // Stage 1: send salt + client cert → server displays PIN
    InitResult initiatePairing();

    // Stages 2-5: complete pairing with user-provided PIN
    PairState completePairing(const QString& pin, QByteArray& outServerCertPem);

private:
    QByteArray generateRandomBytes(int length);
    QByteArray saltPin(const QByteArray& salt, const QString& pin);
    QByteArray encrypt(const QByteArray& plaintext, const QByteArray& key);
    QByteArray decrypt(const QByteArray& ciphertext, const QByteArray& key);
    QByteArray getSignatureFromCert(X509* cert);
    QByteArray getSignatureFromPemCert(const QByteArray& certificate);
    bool verifySignature(const QByteArray& data, const QByteArray& signature,
                         const QByteArray& serverCertificate);
    QByteArray signMessage(const QByteArray& message);

    // Synchronous HTTP request
    QString openConnection(const QString& scheme, const QString& command,
                           const QString& arguments, int timeoutMs);

    QByteArray m_ServerVersion;       // "7.1.431" from serverinfo
    QString m_Host;
    quint16 m_HttpPort;
    quint16 m_HttpsPort;

    X509* m_Cert = nullptr;
    EVP_PKEY* m_PrivateKey = nullptr;
    QNetworkAccessManager* m_Nam = nullptr;

    // State preserved between initiatePairing() and completePairing()
    QByteArray m_Salt;                // 16 random bytes from stage 1
    QByteArray m_ServerCertPem;       // server cert PEM received in stage 1
    bool m_Stage1Done = false;

    int m_HashLength = 32;
    int m_ServerMajorVersion = 7;
};
