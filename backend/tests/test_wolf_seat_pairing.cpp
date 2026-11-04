/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A browser's first launch on a Wolf host pairs a seat first. That handshake
 * starts over plain HTTP, so the machine at the host's address has to show the
 * host's certificate before it hears anything. Real TLS, on the loopback.
 */
#include "test_framework.h"

#include "backend/HostCertCheck.h"
#include "backend/NvAddress.h"
#include "backend/NvComputer.h"
#include "backend/NvHTTP.h"
#include "backend/streambackend/WolfBackend.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <functional>
#include <memory>
#include <optional>

namespace {

struct Pem
{
    QByteArray cert;
    QByteArray key;
};

QByteArray fromBio(BIO* bio)
{
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    QByteArray out(data, static_cast<qsizetype>(size));
    BIO_free(bio);
    return out;
}

Pem mint(const char* commonName)
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
    Pem out{fromBio(certBio), fromBio(keyBio)};
    X509_free(cert);
    EVP_PKEY_free(key);
    return out;
}

/// A plain HTTP port that counts what reaches it and answers the first request
/// with `first`. Every later one is cut off, as a host tearing the pairing down.
class HttpStub : public QObject
{
public:
    explicit HttpStub(QByteArray first)
        : m_First(std::move(first))
    {
        m_Server.listen(QHostAddress::LocalHost);
        connect(&m_Server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* peer = m_Server.nextPendingConnection();
            connect(peer, &QIODevice::readyRead, this, [this, peer]() {
                m_Buffer += peer->readAll();
                if (!m_Buffer.contains("\r\n\r\n")) return;
                m_Buffer.clear();
                if (++hits > 1 && !m_First.isEmpty()) {
                    peer->abort();
                    return;
                }
                peer->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                            QByteArray::number(m_First.size()) + "\r\n\r\n" + m_First);
                peer->disconnectFromHost();
            });
        });
    }

    quint16 port() const { return m_Server.serverPort(); }

    int hits = 0;

private:
    QTcpServer m_Server;
    QByteArray m_First;
    QByteArray m_Buffer;
};

/// One launch on a Wolf host, through the real backend.
struct Outcome
{
    std::optional<BackendError::Kind> refusal; ///< empty when nothing refused it
    int pairHits = 0;                          ///< requests on the host's plain HTTP port
    int apiHits = 0;                           ///< requests on its control API
    bool seatRecorded = false;
};

Outcome launchOn(const std::optional<Pem>& shown, const Pem& onRecord, const QByteArray& pairAnswer)
{
    QSslServer tls;
    quint16 tlsPort = 0;
    if (shown) {
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificate(QSslCertificate(shown->cert, QSsl::Pem));
        config.setPrivateKey(QSslKey(shown->key, QSsl::Rsa, QSsl::Pem));
        config.setPeerVerifyMode(QSslSocket::QueryPeer);
        tls.setSslConfiguration(config);
        tls.listen(QHostAddress::LocalHost);
        tlsPort = tls.serverPort();
    } else {
        // A port nothing listens on.
        QTcpServer finder;
        finder.listen(QHostAddress::LocalHost);
        tlsPort = finder.serverPort();
    }

    HttpStub pair(pairAnswer);
    HttpStub api("{\"success\":true,\"requests\":[]}");

    NvComputer host;
    host.uuid = QStringLiteral("00000000-5ec0-4ec0-8ec0-000000000011");
    host.name = QStringLiteral("Wolf");
    host.appVersion = QStringLiteral("7.1.431.-1");
    host.activeAddress = NvAddress(QStringLiteral("127.0.0.1"), pair.port());
    host.activeHttpsPort = tlsPort;
    host.serverCertPem = onRecord.cert;
    host.pairState = NvComputer::PS_PAIRED;

    const QString device = QStringLiteral("device-%1").arg(pair.port());
    const QString seat = QString::fromLatin1(
        QCryptographicHash::hash(device.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    const QString seatKey = QStringLiteral("wolfSeatPairings/%1/%2").arg(host.uuid, seat);

    QNetworkAccessManager nam;
    NvHTTP http(&nam);
    WolfBackend backend(
        host.uuid, [&host]() { return &host; }, &http, &nam,
        QStringLiteral("http://127.0.0.1:%1").arg(api.port()), QStringLiteral("t"),
        [](const QByteArray&) {});

    LaunchRequest req;
    req.appId = 1;
    req.clientUniqueId = device;

    Outcome out;
    QEventLoop loop;
    backend.launch(QString(), req, [&](bool ok, const BackendError& err, const MediaDescriptor&) {
        if (!ok) out.refusal = err.kind;
        QTimer::singleShot(0, &loop, &QEventLoop::quit);
    });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();

    // Long enough for anything still on its way to arrive.
    QEventLoop settle;
    QTimer::singleShot(300, &settle, &QEventLoop::quit);
    settle.exec();

    out.pairHits = pair.hits;
    out.apiHits = api.hits;
    QSettings settings;
    out.seatRecorded = settings.value(seatKey, false).toBool();
    settings.remove(seatKey);
    return out;
}

QByteArray stageOne(const QByteArray& serverCertPem)
{
    return "<root status_code=\"200\"><paired>1</paired><plaincert>" + serverCertPem.toHex() +
           "</plaincert></root>";
}

} // namespace

void run_wolf_seat_pairing_tests()
{
    SECTION("Wolf seat pairing — the host's certificate first");

    if (!QSslSocket::supportsSsl()) {
        CHECK(!"TLS is available to the tests");
        return;
    }

    const Pem host = mint("wolf");
    const Pem impostor = mint("impostor");
    const QByteArray refused = "<root status_code=\"200\"><paired>0</paired></root>";

    // Another machine at the address: it hears nothing, and neither does the
    // control API, and the viewer is told why.
    {
        const Outcome out = launchOn(impostor, host, refused);
        CHECK(out.refusal == BackendError::CertificateChanged);
        CHECK_EQ(out.pairHits, 0);
        CHECK_EQ(out.apiHits, 0);
        CHECK(!out.seatRecorded);
    }

    // Nothing there: nothing is sent either, and it is not called a new certificate.
    {
        const Outcome out = launchOn(std::nullopt, host, refused);
        CHECK(out.refusal == BackendError::Unreachable);
        CHECK_EQ(out.pairHits, 0);
        CHECK_EQ(out.apiHits, 0);
        CHECK(!out.seatRecorded);
    }

    // The host itself: the handshake goes ahead. This one turns it down.
    {
        const Outcome out = launchOn(host, host, refused);
        CHECK(out.refusal == BackendError::Protocol);
        CHECK(out.pairHits > 0);
        CHECK(out.apiHits > 0);
        CHECK(!out.seatRecorded);
    }

    // A handshake that names another certificate, then hangs up, which a pairing
    // reads as already completed: the seat is not recorded as paired.
    {
        const Outcome out = launchOn(host, host, stageOne(impostor.cert));
        CHECK(out.refusal == BackendError::CertificateChanged);
        CHECK(out.pairHits > 0);
        CHECK(!out.seatRecorded);
    }

    // The same, naming the host's own: the seat is paired, the launch moves on.
    {
        const Outcome out = launchOn(host, host, stageOne(host.cert));
        CHECK(out.pairHits > 0);
        CHECK(out.seatRecorded);
    }

    // What the check reports, for the callers that read more than yes or no.
    {
        QSslServer server;
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificate(QSslCertificate(impostor.cert, QSsl::Pem));
        config.setPrivateKey(QSslKey(impostor.key, QSsl::Rsa, QSsl::Pem));
        config.setPeerVerifyMode(QSslSocket::QueryPeer);
        server.setSslConfiguration(config);
        server.listen(QHostAddress::LocalHost);

        QObject scope;
        QEventLoop loop;
        std::optional<HostCertCheck::Shown> shown;
        HostCertCheck::look(QStringLiteral("127.0.0.1"), server.serverPort(), host.cert, host.cert,
                            host.key, &scope, 5000, [&](HostCertCheck::Shown answer) {
                                shown = answer;
                                QTimer::singleShot(0, &loop, &QEventLoop::quit);
                            });
        QTimer::singleShot(8000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(shown == HostCertCheck::Shown::Another);
    }
}
