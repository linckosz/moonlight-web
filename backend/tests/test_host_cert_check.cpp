/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A paired host follows a new address when the machine there holds its
 * certificate, and nothing else moves it. Real TLS, on the loopback.
 */
#include "test_framework.h"

#include "backend/HostCertCheck.h"
#include "backend/NvComputer.h"
#include "backend/NvHTTP.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
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

/// What reaches a host's HTTPS port when a launch is sent to it, and how the
/// reply ends. The key must be on the wire only for the host's own certificate.
struct LaunchSeen
{
    QByteArray received;
    bool refused = false;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
};

LaunchSeen launchAt(const Identity& shown, const QByteArray& onRecord, const Identity& client)
{
    QSslServer server;
    listen(server, shown);

    LaunchSeen seen;
    QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&]() {
        auto* peer = qobject_cast<QSslSocket*>(server.nextPendingConnection());
        if (!peer) return;
        QObject::connect(peer, &QIODevice::readyRead, &server, [&seen, peer]() {
            seen.received += peer->readAll();
            if (seen.received.contains("\r\n\r\n"))
                peer->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        });
    });

    QNetworkAccessManager nam;
    NvHTTP http(&nam);
    QNetworkReply* reply =
        http.launchAppAsync(NvAddress(QStringLiteral("127.0.0.1"), 47989), server.serverPort(), 1,
                            QStringLiteral("id"), QByteArray::fromHex("c0ffee"), 7, 1280, 720, 60,
                            0, client.certPem, client.keyPem, onRecord);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    loop.exec();

    // Long enough for anything sent before the abort to arrive.
    QEventLoop settle;
    QTimer::singleShot(500, &settle, &QEventLoop::quit);
    settle.exec();

    seen.refused = NvHTTP::serverCertRefused(reply);
    seen.error = reply->error();
    reply->deleteLater();
    return seen;
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

    // Not paired, but a backend's secrets would follow it: no proof to offer.
    tower.backendApiToken = QStringLiteral("t");
    CHECK(tower.movesOnlyWithProof(moved));
    CHECK(!tower.movesOnlyWithProof(current));
    tower.backendApiToken.clear();
    tower.backendPairPassword = QStringLiteral("p");
    CHECK(tower.movesOnlyWithProof(moved));
    tower.backendPairPassword.clear();
    // A user name alone is no secret, and an address not yet known has no rival.
    tower.backendPairUser = QStringLiteral("u");
    CHECK(!tower.movesOnlyWithProof(moved));
    NvComputer unplaced;
    unplaced.backendApiToken = QStringLiteral("t");
    CHECK(!unplaced.movesOnlyWithProof(moved));

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

    // A launch: the key goes to the host's own certificate, and to nothing else.
    {
        const LaunchSeen good = launchAt(host, host.certPem, client);
        CHECK(!good.refused);
        CHECK(good.error == QNetworkReply::NoError);
        CHECK(good.received.contains("rikey=c0ffee"));

        const LaunchSeen other = launchAt(impostor, host.certPem, client);
        CHECK(other.refused);
        CHECK(other.error != QNetworkReply::NoError);
        CHECK(other.received.isEmpty());

        // Same PEM, written differently: not a different certificate.
        QByteArray rewritten = host.certPem;
        rewritten.replace("\n", "\r\n");
        CHECK(!launchAt(host, rewritten, client).refused);

        // Nothing on record: nothing to compare, nothing refused.
        const LaunchSeen unpaired = launchAt(impostor, {}, client);
        CHECK(!unpaired.refused);
        CHECK(unpaired.received.contains("rikey=c0ffee"));
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
