/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A paired host follows a new address when the machine there holds its
 * certificate, and nothing else moves it. Real TLS, on the loopback.
 */
#include "test_framework.h"

#include "backend/HostCertCheck.h"
#include "backend/NvComputer.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <functional>
#include <optional>

namespace {

struct Identity
{
    QByteArray certPem;
    QByteArray keyPem;
};

QByteArray fromBio(BIO* bio)
{
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    QByteArray out(data, static_cast<qsizetype>(size));
    BIO_free(bio);
    return out;
}

Identity mint(const char* commonName)
{
    EVP_PKEY* key = EVP_RSA_gen(2048);
    X509* cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(commonName), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509_sign(cert, key, EVP_sha256());

    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, cert);
    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, key, nullptr, nullptr, 0, nullptr, nullptr);
    Identity out{fromBio(certBio), fromBio(keyBio)};
    X509_free(cert);
    EVP_PKEY_free(key);
    return out;
}

/// What a GameStream host does on its HTTPS port: its own certificate, and it
/// asks for the client's.
void listen(QSslServer& server, const Identity& identity)
{
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    config.setLocalCertificate(QSslCertificate(identity.certPem, QSsl::Pem));
    config.setPrivateKey(QSslKey(identity.keyPem, QSsl::Rsa, QSsl::Pem));
    config.setPeerVerifyMode(QSslSocket::QueryPeer);
    server.setSslConfiguration(config);
    server.listen(QHostAddress::LocalHost);
}

std::optional<bool> ask(quint16 port, const QByteArray& expected, const Identity& client,
                        int timeoutMs = 5000)
{
    QObject scope;
    QEventLoop loop;
    std::optional<bool> answer;
    HostCertCheck::verify(QStringLiteral("127.0.0.1"), port, expected, client.certPem,
                          client.keyPem, &scope, timeoutMs, [&](bool proven) {
                              answer = proven;
                              QTimer::singleShot(0, &loop, &QEventLoop::quit);
                          });
    QTimer::singleShot(timeoutMs + 3000, &loop, &QEventLoop::quit);
    loop.exec();
    return answer;
}

const char* kServerInfo = "<root status_code=\"200\"><hostname>Tower</hostname>"
                          "<uniqueid>uuid-a</uniqueid></root>";

} // namespace

void run_host_cert_check_tests()
{
    SECTION("Host certificate check");

    if (!QSslSocket::supportsSsl()) {
        CHECK(!"TLS is available to the tests");
        return;
    }

    const Identity host = mint("host");
    const Identity impostor = mint("impostor");
    const Identity client = mint("client");

    // The comparison, whichever way the PEM is written.
    QByteArray crlf = host.certPem;
    crlf.replace("\n", "\r\n");
    CHECK(HostCertCheck::sameCertificate(host.certPem, host.certPem));
    CHECK(HostCertCheck::sameCertificate(host.certPem, crlf));
    CHECK(!HostCertCheck::sameCertificate(host.certPem, impostor.certPem));
    CHECK(!HostCertCheck::sameCertificate({}, {}));
    CHECK(!HostCertCheck::sameCertificate("not a certificate", "not a certificate"));

    // Who needs a proof, and from where.
    const NvAddress current(QStringLiteral("192.0.2.10"), 47989);
    const NvAddress moved(QStringLiteral("192.0.2.77"), 47989);
    NvComputer tower(QString::fromLatin1(kServerInfo), current);
    CHECK(!tower.movesOnlyWithProof(moved)); // not paired: nothing to compare
    tower.serverCertPem = host.certPem;
    CHECK(tower.movesOnlyWithProof(moved));
    CHECK(!tower.movesOnlyWithProof(current));

    // The host, come back on a new lease: same certificate, and the client's is asked for.
    {
        QSslServer server;
        listen(server, host);
        QSslCertificate clientSeen;
        QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&]() {
            auto* peer = qobject_cast<QSslSocket*>(server.nextPendingConnection());
            if (peer) clientSeen = peer->peerCertificate();
        });
        CHECK(ask(server.serverPort(), host.certPem, client) == std::optional<bool>(true));

        NvComputer follower(QString::fromLatin1(kServerInfo), current);
        follower.serverCertPem = host.certPem;
        follower.update(NvComputer(QString::fromLatin1(kServerInfo), moved));
        CHECK_EQ(follower.activeAddress.toString(), moved.toString());
        CHECK_EQ(follower.serverCertPem, host.certPem);
    }

    // Another machine answering for it: refused, however it refuses.
    {
        QSslServer server;
        listen(server, impostor);
        CHECK(ask(server.serverPort(), host.certPem, client) == std::optional<bool>(false));
    }
    {
        // Something listening that never speaks TLS: the wait is bounded.
        QTcpServer silent;
        silent.listen(QHostAddress::LocalHost);
        QElapsedTimer waited;
        waited.start();
        CHECK(ask(silent.serverPort(), host.certPem, client, 300) == std::optional<bool>(false));
        CHECK(waited.elapsed() < 3000);
    }
    {
        // Nothing listening at all.
        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost);
        const quint16 port = closed.serverPort();
        closed.close();
        CHECK(ask(port, host.certPem, client) == std::optional<bool>(false));
    }

    // Nothing is reported to a caller that has gone.
    {
        QSslServer server;
        listen(server, host);
        bool reported = false;
        {
            QObject scope;
            HostCertCheck::verify(QStringLiteral("127.0.0.1"), server.serverPort(), host.certPem,
                                  client.certPem, client.keyPem, &scope, 1000,
                                  [&](bool) { reported = true; });
        }
        QEventLoop loop;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(!reported);
    }
}
