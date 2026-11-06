/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The GameStream pairing checks the server's signature with its certificate.
 * Whatever the answer, the check leaves the thread's OpenSSL error queue empty.
 */
#include "test_framework.h"

#include "backend/NvPairingManager.h"

#include <QByteArray>
#include <QList>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace {

struct Signer
{
    QByteArray certPem;
    EVP_PKEY* key = nullptr;
};

Signer mintSigner()
{
    Signer out;
    out.key = EVP_RSA_gen(2048);
    X509* cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, out.key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("NVIDIA GameStream Server"),
                               -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509_sign(cert, out.key, EVP_sha256());

    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    out.certPem = QByteArray(data, static_cast<qsizetype>(size));
    BIO_free(bio);
    X509_free(cert);
    return out;
}

QByteArray signWith(EVP_PKEY* key, const QByteArray& data)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key);
    EVP_DigestSignUpdate(ctx, data.constData(), static_cast<size_t>(data.size()));
    size_t length = 0;
    EVP_DigestSignFinal(ctx, nullptr, &length);
    QByteArray sig(static_cast<qsizetype>(length), '\0');
    EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(sig.data()), &length);
    sig.resize(static_cast<qsizetype>(length));
    EVP_MD_CTX_free(ctx);
    return sig;
}

} // namespace

void run_nv_pairing_signature_tests()
{
    SECTION("GameStream pairing — the server signature check leaves no OpenSSL error behind");

    const Signer server = mintSigner();
    const Signer other = mintSigner();
    const QByteArray data("server secret and its salt");

    QByteArray noise(256, Qt::Uninitialized);
    for (int i = 0; i < noise.size(); ++i)
        noise[i] = static_cast<char>(0x5A ^ (i * 37));

    struct Refused
    {
        QByteArray sig;
        QByteArray cert;
    };
    const QList<Refused> refused = {
        {signWith(other.key, data), server.certPem},
        {noise, server.certPem},
        {QByteArray(), server.certPem},
        {signWith(server.key, data), QByteArray("not a certificate")},
    };
    int left = 0;
    for (const Refused& r : refused) {
        ERR_clear_error();
        CHECK(!NvPairingManager::verifySignature(data, r.sig, r.cert));
        if (ERR_peek_error() != 0) ++left;
    }
    CHECK_EQ(left, 0);

    ERR_clear_error();
    CHECK(NvPairingManager::verifySignature(data, signWith(server.key, data), server.certPem));
    CHECK_EQ(ERR_peek_error(), 0ul);

    EVP_PKEY_free(server.key);
    EVP_PKEY_free(other.key);
}
