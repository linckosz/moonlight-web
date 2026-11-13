/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The rendezvous line, against a local stand-in for the introduction server:
 * an error on a line that is still open brings it down at once; a name is
 * raced and the line opens on the address that answered, the name kept for the
 * server; an attempt that never comes up is cut at the open timeout, counts
 * toward the re-claim, and the next one leads with the other address family.
 *
 * Which Qt may open the line on an address at all (QTBUG-141396): on the others
 * it goes by name, with one connection, the name in Host, and a bound long
 * enough for Qt's own walk through the addresses. The by-name sections force
 * that path, so they run whatever Qt the suite loads; the race's sections run
 * only on a Qt that can pin the line, and say so when they are skipped.
 *
 * How a dead line is noticed: the keep-alive cadence and the pong wait; a line
 * whose pongs stop is rebuilt as a drop, never doubting the claim; a wall clock
 * that jumps ahead, as it does when the machine wakes, earns a ping at once.
 *
 * Behind production's Caddy, a restart of the introduction server reaches the
 * host as a TLS close_notify on a TCP connection that stays open. Qt reports it
 * through errorOccurred and then never emits disconnected, so the line looked
 * up until the next ping and its missing pong — up to 65 s, 31 s and 63 s on
 * the staging bench of 16/10/2026. That half-close cannot be staged on a local
 * socket (a plain FIN is a disconnect to Qt), so the test hands the client the
 * same error on a line that is genuinely still open, which is exactly the state
 * the bench left it in.
 */
#include "test_framework.h"

#include "../src/common/RendezvousId.h"
#include "../src/network/RendezvousClient.h"
#include "../src/server/AppSettings.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QVersionNumber>
#include <QWebSocket>
#include <QWebSocketServer>

namespace {

template <class F> bool waitFor(F done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

/// A claimed identity in a settings file of its own.
void claimedIdentity(AppSettings& settings)
{
    settings.setRendezvousId(RendezvousId::generate());
    settings.setRendezvousToken(QString(64, QLatin1Char('a')));
    settings.setRendezvousClaimed(true);
}

/// A server that takes every connection and never says a word: what a stalled
/// path looks like once TCP has gone through.
struct MuteServer
{
    QTcpServer server;
    int accepted = 0;

    bool listen(const QHostAddress& address, quint16 port = 0)
    {
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                ++accepted;
                s->setParent(&server);
            }
        });
        return server.listen(address, port);
    }
};

/// A stand-in that takes the line and keeps the Host it was asked for. Every
/// TCP connection is counted before the WebSocket server sees it, so a race's
/// probe shows up as one connection more than the line itself.
struct HostStandIn
{
    QTcpServer tcp;
    QWebSocketServer ws{QStringLiteral("rdv-test"), QWebSocketServer::NonSecureMode};
    int connections = 0;
    int lines = 0;
    QByteArray hostHeader;
    QHostAddress peer;

    bool listen()
    {
        QObject::connect(&tcp, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket* s = tcp.nextPendingConnection()) {
                ++connections;
                ws.handleConnection(s);
            }
        });
        QObject::connect(&ws, &QWebSocketServer::newConnection, [this]() {
            while (QWebSocket* line = ws.nextPendingConnection()) {
                ++lines;
                hostHeader = line->request().rawHeader("Host");
                peer = line->peerAddress();
                line->setParent(&ws);
            }
        });
        // "localhost" resolves to ::1 and 127.0.0.1 on most machines: both
        // families answer.
        return tcp.listen(QHostAddress::Any, 0);
    }
};

/// A stand-in that completes the WebSocket upgrade and then goes deaf: it
/// reads everything and answers nothing, the pings included. What a line looks
/// like from the host once the path under it is cut, or the NAT entry gone,
/// while the socket still says connected.
struct DeafServer
{
    QTcpServer server;
    int lines = 0;

    bool listen()
    {
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                s->setParent(&server);
                QObject::connect(s, &QTcpSocket::readyRead, [this, s]() {
                    if (s->property("upgraded").toBool()) {
                        s->readAll(); // pings and all: unanswered
                        return;
                    }
                    if (!s->peek(64 * 1024).contains("\r\n\r\n")) return;
                    QByteArray key;
                    for (const QByteArray& line : s->readAll().split('\n')) {
                        if (line.toLower().startsWith("sec-websocket-key:"))
                            key = line.mid(line.indexOf(':') + 1).trimmed();
                    }
                    const QByteArray accept =
                        QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                                 QCryptographicHash::Sha1)
                            .toBase64();
                    s->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                             "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                             accept + "\r\n\r\n");
                    s->setProperty("upgraded", true);
                    ++lines;
                });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
};

/// What the Qt this suite runs on does with a named line.
bool qtCanPinLine()
{
    return RendezvousClient::canPinLine(QVersionNumber::fromString(QLatin1String(qVersion())));
}

} // namespace

void run_rendezvous_line_tests()
{
    const QByteArray savedUrl = qgetenv("MW_RENDEZVOUS_URL");
    QTemporaryDir tmp;
    AppSettings::setFileOverride(tmp.filePath(QStringLiteral("settings.json")));

    SECTION("RendezvousClient — an error on an open line rebuilds it at once");
    {
        // A stand-in for the introduction server: it takes the line and holds it.
        QWebSocketServer server(QStringLiteral("rdv-test"), QWebSocketServer::NonSecureMode);
        CHECK(server.listen(QHostAddress::LocalHost, 0));
        int lines = 0;
        int dropped = 0;
        QObject::connect(&server, &QWebSocketServer::newConnection, [&]() {
            while (QWebSocket* ws = server.nextPendingConnection()) {
                ++lines;
                ws->setParent(&server);
                QObject::connect(ws, &QWebSocket::disconnected, [&]() { ++dropped; });
            }
        });

        // A URL that names an address: no lookup, no race, the line opens on it.
        qputenv("MW_RENDEZVOUS_URL",
                QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()).toUtf8());

        AppSettings settings;
        claimedIdentity(settings);

        RendezvousClient client(&settings);
        client.start();
        CHECK(waitFor([&]() { return client.isOnline() && lines == 1; }, 3000));

        // What Qt does on the half-close: the error, and nothing after it. The
        // socket underneath is still connected.
        const bool invoked = QMetaObject::invokeMethod(
            &client, "onSocketError", Qt::DirectConnection,
            Q_ARG(QAbstractSocket::SocketError, QAbstractSocket::RemoteHostClosedError));
        CHECK(invoked);

        // Down at once, not at the next ping (20 s) plus the pong wait (15 s).
        CHECK(waitFor([&]() { return !client.isOnline() && dropped == 1; }, 1000));

        // Back within the first retry, 1 to 2 s with its jitter: the bench
        // target is under 3 s from the moment the server is there again.
        CHECK(waitFor([&]() { return client.isOnline() && lines == 2; }, 3000));

        client.stop();
        waitFor([&]() { return dropped == 2; }, 1000);
    }

    SECTION("RendezvousClient — only a Qt with the fix for QTBUG-141396 opens the line on an "
            "address");
    {
        const auto pins = [](int major, int minor, int patch) {
            return RendezvousClient::canPinLine(QVersionNumber(major, minor, patch));
        };
        CHECK(!pins(6, 2, 4)); // Ubuntu 22.04
        CHECK(!pins(6, 4, 2)); // Debian 12, Ubuntu 24.04
        CHECK(!pins(6, 8, 2)); // Debian 13, the Docker image
        CHECK(!pins(6, 10, 2));
        CHECK(pins(6, 10, 3));
        CHECK(pins(6, 10, 4));
        CHECK(!pins(6, 11, 0)); // the fix came to the 6.11 branch after it
        CHECK(pins(6, 11, 1));
        CHECK(pins(6, 11, 2));
        CHECK(pins(6, 12, 0));
        CHECK(!RendezvousClient::canPinLine(QVersionNumber()));
        std::fprintf(stderr, "  (this suite runs on Qt %s: the line %s)\n", qVersion(),
                     qtCanPinLine() ? "is raced" : "goes by name");
    }

    SECTION("RendezvousClient — a name is raced, and the line opens on the address that answered");
    if (!qtCanPinLine()) {
        std::fprintf(stderr,
                     "  (skipped: Qt %s cannot pin the line; the by-name section "
                     "below is what it runs)\n",
                     qVersion());
    } else {
        // "localhost" is the one name every machine resolves without a network.
        HostStandIn server;
        CHECK(server.listen());
        const QString authority = QStringLiteral("localhost:%1").arg(server.tcp.serverPort());
        qputenv("MW_RENDEZVOUS_URL", (QStringLiteral("http://") + authority).toUtf8());

        AppSettings settings;
        claimedIdentity(settings);

        RendezvousClient client(&settings);
        QElapsedTimer opening;
        opening.start();
        client.start();
        CHECK(waitFor([&]() { return client.isOnline() && server.lines == 1; }, 5000));
        // Lookup, race and open on loopback: well under the stagger.
        CHECK(opening.elapsed() < 2000);
        // The race's probe, then the line on the address it named...
        CHECK_EQ(server.connections, 2);
        CHECK(server.peer.isLoopback());
        // ...and the server still saw the name, once.
        CHECK_EQ(QString::fromLatin1(server.hostHeader), authority);

        client.stop();
        waitFor([&]() { return !client.isOnline(); }, 1000);
    }

    SECTION("RendezvousClient — on a Qt that cannot pin the line, it goes by name");
    {
        HostStandIn server;
        CHECK(server.listen());
        const QString authority = QStringLiteral("localhost:%1").arg(server.tcp.serverPort());
        qputenv("MW_RENDEZVOUS_URL", (QStringLiteral("http://") + authority).toUtf8());

        AppSettings settings;
        claimedIdentity(settings);

        RendezvousClient client(&settings);
        client.forceLineByName();
        client.start();
        CHECK(waitFor([&]() { return client.isOnline() && server.lines == 1; }, 5000));
        // No probe: Qt opened the line by name, and the Host is the URL's own.
        CHECK_EQ(server.connections, 1);
        CHECK_EQ(QString::fromLatin1(server.hostHeader), authority);

        client.stop();
        waitFor([&]() { return !client.isOnline(); }, 1000);
    }

    SECTION("RendezvousClient — a line by name is not cut at the bound of a line on an address");
    {
        // Qt's own walk gives a silent address 21 to 30 s before the next one:
        // the bound that suits a raced line would cut it every time.
        MuteServer mute;
        CHECK(mute.listen(QHostAddress::Any));
        qputenv("MW_RENDEZVOUS_URL",
                QStringLiteral("http://localhost:%1").arg(mute.server.serverPort()).toUtf8());

        AppSettings settings;
        claimedIdentity(settings);

        RendezvousClient client(&settings);
        client.forceLineByName();
        client.setOpenTimeoutMs(300);
        client.start();
        CHECK(waitFor([&]() { return mute.accepted == 1; }, 3000));
        // Cut at 300 ms, a second attempt would follow within the first retry,
        // 1 to 2 s after the cut.
        CHECK(!waitFor([&]() { return mute.accepted > 1; }, 3000));
        CHECK(!client.isOnline());

        client.stop();
    }

    SECTION("RendezvousClient — the keep-alive: a ping every 20 s, the pong given 15 s; a line "
            "that answers is left alone");
    {
        QWebSocketServer server(QStringLiteral("rdv-test"), QWebSocketServer::NonSecureMode);
        CHECK(server.listen(QHostAddress::LocalHost, 0));
        int lines = 0;
        QObject::connect(&server, &QWebSocketServer::newConnection, [&]() {
            while (QWebSocket* ws = server.nextPendingConnection()) {
                ++lines;
                ws->setParent(&server); // Qt answers the pings by itself
            }
        });
        qputenv("MW_RENDEZVOUS_URL",
                QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()).toUtf8());

        AppSettings settings;
        claimedIdentity(settings);

        RendezvousClient client(&settings);
        // What production runs with: 35 s at worst to notice a silent cut.
        CHECK_EQ(client.keepAliveMs(), 20 * 1000);
        CHECK_EQ(client.pongTimeoutMs(), 15 * 1000);

        // Sped up: pings every 50 ms, each answered on loopback at once. The
        // pong wait, 120 ms, is shorter than the ten pings below: every pong
        // must stop the watchdog, or the next ping would leave it running and
        // it would cut a line that answers.
        client.setKeepAliveMs(50);
        client.setPongTimeoutMs(120);
        client.start();
        CHECK(waitFor([&]() { return client.isOnline() && lines == 1; }, 3000));
        CHECK(waitFor([&]() { return client.pingsSent() >= 10; }, 3000));
        // Every pong came back: nothing was rebuilt.
        CHECK(client.isOnline());
        CHECK_EQ(lines, 1);

        client.stop();
        waitFor([&]() { return !client.isOnline(); }, 1000);
    }

    SECTION("RendezvousClient — a line whose pongs stop is rebuilt at the pong wait, as a drop: "
            "the claim is never doubted");
    {
        DeafServer deaf;
        CHECK(deaf.listen());
        qputenv("MW_RENDEZVOUS_URL",
                QStringLiteral("http://127.0.0.1:%1").arg(deaf.server.serverPort()).toUtf8());

        AppSettings settings;
        claimedIdentity(settings);

        RendezvousClient client(&settings);
        client.setKeepAliveMs(100);
        client.setPongTimeoutMs(100);
        int drops = 0;
        QObject::connect(&client, &RendezvousClient::onlineChanged, [&](bool online) {
            if (!online) ++drops;
        });
        client.start();
        CHECK(waitFor([&]() { return client.isOnline() && deaf.lines == 1; }, 3000));

        // The first ping goes unanswered: down at the keep-alive plus the pong
        // wait, 200 ms here, where nothing else would ever have noticed.
        QElapsedTimer sinceUp;
        sinceUp.start();
        CHECK(waitFor([&]() { return drops == 1; }, 2000));
        CHECK(sinceUp.elapsed() < 1500);
        CHECK(client.pingsSent() >= 1);

        // Back within the first retry, 1 to 2 s, and dropped the same way
        // again, and again. A line that was up and went is a drop, not a
        // refusal: none of it counts toward the re-claim.
        CHECK(waitFor([&]() { return deaf.lines == 2; }, 4000));
        CHECK(waitFor([&]() { return drops == 2; }, 2000));
        CHECK(waitFor([&]() { return deaf.lines == 3; }, 4000));
        CHECK(settings.rendezvousClaimed());

        client.stop();
    }

    SECTION("RendezvousClient — a wall clock that jumps ahead (the machine slept) earns a ping "
            "at once, not at the next keep-alive");
    {
        QWebSocketServer server(QStringLiteral("rdv-test"), QWebSocketServer::NonSecureMode);
        CHECK(server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server, &QWebSocketServer::newConnection, [&]() {
            while (QWebSocket* ws = server.nextPendingConnection())
                ws->setParent(&server);
        });
        qputenv("MW_RENDEZVOUS_URL",
                QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()).toUtf8());

        AppSettings settings;
        claimedIdentity(settings);

        RendezvousClient client(&settings);
        client.setKeepAliveMs(60 * 1000); // no ping of its own within this test
        client.setClockWatch(50, 200);    // a tick every 50 ms, a jump is 200 ms or more
        client.start();
        CHECK(waitFor([&]() { return client.isOnline(); }, 3000));

        // Ticks on time: nothing to see, nothing pinged.
        waitFor([]() { return false; }, 300);
        CHECK_EQ(client.pingsSent(), 0);

        // The process stands still, as a sleeping machine's does, while the
        // wall clock runs on. The first tick after sees the jump.
        QThread::msleep(400);
        CHECK(waitFor([&]() { return client.pingsSent() == 1; }, 500));
        CHECK(client.isOnline());

        client.stop();
        waitFor([&]() { return !client.isOnline(); }, 1000);
    }

    SECTION("RendezvousClient — an attempt that never comes up is cut at the open timeout, "
            "and counts toward the re-claim");
    {
        MuteServer mute;
        CHECK(mute.listen(QHostAddress::LocalHost));
        qputenv("MW_RENDEZVOUS_URL",
                QStringLiteral("http://127.0.0.1:%1").arg(mute.server.serverPort()).toUtf8());

        AppSettings settings;
        claimedIdentity(settings);

        RendezvousClient client(&settings);
        int cameUp = 0;
        QObject::connect(&client, &RendezvousClient::onlineChanged, [&](bool online) {
            if (online) ++cameUp;
        });
        client.setOpenTimeoutMs(300);
        client.start();
        CHECK(waitFor([&]() { return mute.accepted == 1; }, 2000));

        // Nothing bounded this before: the upgrade would have waited forever.
        // Now the attempt is cut and a second one follows within the first
        // retry, 1 to 2 s.
        CHECK(waitFor([&]() { return mute.accepted == 2; }, 4000));
        CHECK(settings.rendezvousClaimed());

        // Two attempts that never came up: the claim is no longer trusted...
        CHECK(waitFor([&]() { return !settings.rendezvousClaimed(); }, 3000));
        // ...and the re-claim itself goes out, the third connection, bounded
        // too: the mute server would otherwise hold it forever.
        CHECK(waitFor([&]() { return mute.accepted == 3; }, 10000));
        CHECK_EQ(cameUp, 0);

        client.stop();
    }

    SECTION("RendezvousClient — after a timeout, the next attempt leads with the other family");
    {
        // Needs "localhost" in both families, and the same port free on both.
        // Where it is not, the rule is still covered by test_address_race's
        // interleave() with a lead; this section is then skipped, and says so.
        bool has4 = false;
        bool has6 = false;
        const QHostInfo lo = QHostInfo::fromName(QStringLiteral("localhost"));
        for (const QHostAddress& a : lo.addresses()) {
            has4 = has4 || a.protocol() == QAbstractSocket::IPv4Protocol;
            has6 = has6 || a.protocol() == QAbstractSocket::IPv6Protocol;
        }
        MuteServer mute4;
        MuteServer mute6;
        const bool staged = has4 && has6 && mute4.listen(QHostAddress::LocalHost) &&
                            mute6.listen(QHostAddress::LocalHostIPv6, mute4.server.serverPort());
        if (!qtCanPinLine()) {
            std::fprintf(stderr, "  (skipped: Qt %s cannot pin the line, nothing is raced)\n",
                         qVersion());
        } else if (!staged) {
            std::fprintf(stderr, "  (skipped: localhost is not dual-stack here)\n");
        } else {
            qputenv("MW_RENDEZVOUS_URL",
                    QStringLiteral("http://localhost:%1").arg(mute4.server.serverPort()).toUtf8());

            AppSettings settings;
            claimedIdentity(settings);

            RendezvousClient client(&settings);
            client.setOpenTimeoutMs(300);
            client.start();

            // Attempt 1: the resolver's first family wins the race at once (it
            // is loopback), the line opens on it and stalls. That side sees two
            // connections, the probe and the line; the other side none, the
            // stagger never having elapsed.
            CHECK(waitFor([&]() { return mute4.accepted + mute6.accepted == 2; }, 2000));
            const bool firstWas6 = mute6.accepted == 2;
            CHECK(firstWas6 ? mute4.accepted == 0 : mute6.accepted == 0);

            // Attempt 2, after the timeout and the retry: the other family
            // leads, and wins just as fast. Two connections on its side...
            MuteServer& other = firstWas6 ? mute4 : mute6;
            MuteServer& first = firstWas6 ? mute6 : mute4;
            CHECK(waitFor([&]() { return other.accepted == 2; }, 4000));
            // ...and none more on the first.
            CHECK_EQ(first.accepted, 2);

            client.stop();
        }
    }

    if (savedUrl.isEmpty())
        qunsetenv("MW_RENDEZVOUS_URL");
    else
        qputenv("MW_RENDEZVOUS_URL", savedUrl);
    AppSettings::setFileOverride(QString());
}
