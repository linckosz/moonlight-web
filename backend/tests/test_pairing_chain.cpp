/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The pairing chain announces the PIN a moment after stage 1 has left
 * (backend/PairingChain.h). By then whoever asked may be gone — a backend
 * replaced, a host removed — and the announce must go with it.
 */
#include "test_framework.h"

#include "backend/IdentityManager.h"
#include "backend/NvPairingManager.h"
#include "backend/PairingChain.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <memory>

namespace {

/// Pump this thread's events until @p done or @p ms pass.
template <class F> bool waitFor(F done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

/// A host that turns every pairing down at once, over plain HTTP.
class RefusingHost : public QObject
{
public:
    RefusingHost()
    {
        m_Server.listen(QHostAddress::LocalHost);
        connect(&m_Server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* peer = m_Server.nextPendingConnection();
            connect(peer, &QIODevice::readyRead, this, [peer]() {
                if (!peer->peek(8192).contains("\r\n\r\n")) return;
                peer->readAll();
                const QByteArray body = "<root status_code=\"200\"><paired>0</paired></root>";
                peer->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                            QByteArray::number(body.size()) + "\r\n\r\n" + body);
                peer->disconnectFromHost();
            });
        });
    }

    quint16 port() const { return m_Server.serverPort(); }

private:
    QTcpServer m_Server;
};

/// One run of the chain against @p host, with an announcer that counts. Returns
/// once the chain has reported its outcome, so nothing of it is left in flight.
struct Run
{
    int announced = 0;
    QString pin;
    int results = 0;
};

void runChain(RefusingHost& host, QObject* owner, Run& out, const std::function<void()>& afterStart)
{
    const QString seat = QStringLiteral("test-pairing-chain");
    NvPairingManager pm(QStringLiteral("7.1.431.-1"), QStringLiteral("127.0.0.1"), host.port(),
                        host.port(), IdentityManager::get()->identityForSeat(seat), seat);
    PairingChain::run(
        &pm, QStringLiteral("0421"), owner,
        [&out](const QString& pin) {
            ++out.announced;
            out.pin = pin;
        },
        [&out](const PairingChain::Result&) { ++out.results; }, 60);
    // Stage 1 went to the network: nothing is announced before its time.
    CHECK_EQ(out.announced, 0);
    if (afterStart) afterStart();
    CHECK(waitFor([&out]() { return out.results == 1; }, 10000));
    // Well past the moment of the announce.
    waitFor([]() { return false; }, 250);
}

} // namespace

void run_pairing_chain_tests()
{
    RefusingHost host;

    SECTION("pairing chain — the PIN is announced once, a moment after stage 1 left");
    {
        QObject owner;
        Run out;
        runChain(host, &owner, out, {});
        CHECK_EQ(out.announced, 1);
        CHECK(out.pin == QStringLiteral("0421"));
        CHECK_EQ(out.results, 1);
    }

    SECTION("pairing chain — an owner gone before the announce takes it with it");
    {
        auto owner = std::make_unique<QObject>();
        Run out;
        runChain(host, owner.get(), out, [&owner]() { owner.reset(); });
        CHECK_EQ(out.announced, 0);
        CHECK_EQ(out.results, 1); // the chain itself still reports its outcome
    }

    SECTION("pairing chain — an announcer given without its owner is not called");
    {
        Run out;
        runChain(host, nullptr, out, {});
        CHECK_EQ(out.announced, 0);
        CHECK_EQ(out.results, 1);
    }
}
