/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The address race: which of a host's addresses the line is opened on, and how
 * soon. The legs are answered by hand, so an address that drops packets in
 * silence is simply one that is never answered, and nothing here touches a
 * network.
 */
#include "test_framework.h"

#include "../src/network/AddressRace.h"
#include "../src/network/GeoIpService.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

#include <deque>

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

void sleepFor(int ms)
{
    waitFor([]() { return false; }, ms);
}

const QHostAddress k6a(QStringLiteral("2001:db8::1"));
const QHostAddress k6b(QStringLiteral("2001:db8::2"));
const QHostAddress k4a(QStringLiteral("192.0.2.1"));
const QHostAddress k4b(QStringLiteral("192.0.2.2"));

// The stagger of the tests that time it, and the least gap they accept. A coarse QTimer
// may fire 5 % early and Windows rounds to its 15.6 ms tick: 47 ms was seen for 60. The
// gap only has to show that the next leg waited, and half a stagger still shows it.
constexpr int kStaggerMs = 60;
constexpr int kLeastGapMs = kStaggerMs / 2;

/// What the race did to one leg, kept outside the leg: the race deletes its
/// legs once it is over, and the test reads this afterwards.
struct LegRecord
{
    QHostAddress address;
    quint16 port = 0;
    qint64 startedAt = -1;
    bool aborted = false;
};

class FakeLeg final : public AddressRaceLeg
{
public:
    FakeLeg(QObject* parent, LegRecord* record, const QElapsedTimer* clock)
        : AddressRaceLeg(parent)
        , m_Record(record)
        , m_Clock(clock)
    {}

    void connectTo(const QHostAddress& address, quint16 port) override
    {
        m_Record->address = address;
        m_Record->port = port;
        m_Record->startedAt = m_Clock->elapsed();
    }
    void abort() override { m_Record->aborted = true; }

    void answerConnected() { emit connected(); }
    void answerFailed() { emit failed(QStringLiteral("refused")); }

private:
    LegRecord* m_Record;
    const QElapsedTimer* m_Clock;
};

/// A race with hand-answered legs, and everything it said.
struct Bench
{
    QElapsedTimer clock;
    AddressRace race;
    std::deque<LegRecord> records; // a deque: records keep their address as more are added
    QList<QPointer<FakeLeg>> legs;
    QList<QHostAddress> wins;
    qint64 wonAt = -1;
    int losses = 0;

    explicit Bench(int staggerMs)
    {
        race.setStaggerMs(staggerMs);
        race.setLegFactory([this](QObject* parent) -> AddressRaceLeg* {
            records.emplace_back();
            auto* leg = new FakeLeg(parent, &records.back(), &clock);
            legs.append(leg);
            return leg;
        });
        QObject::connect(&race, &AddressRace::won, [this](const QHostAddress& a, qint64 ms) {
            wins.append(a);
            wonAt = ms;
        });
        QObject::connect(&race, &AddressRace::lost, [this](qint64) { ++losses; });
        clock.start();
    }

    int started() const { return int(records.size()); }
};

} // namespace

void run_address_race_tests()
{
    SECTION("AddressRace — the families alternate, the resolver's first leads");
    {
        const QList<QHostAddress> resolved{k6a, k6b, k4a, k4b};
        CHECK(AddressRace::interleave(resolved) == (QList<QHostAddress>{k6a, k4a, k6b, k4b}));
        CHECK(AddressRace::interleave(resolved, QAbstractSocket::IPv4Protocol) ==
              (QList<QHostAddress>{k4a, k6a, k4b, k6b}));
        CHECK(AddressRace::interleave(resolved, QAbstractSocket::IPv6Protocol) ==
              (QList<QHostAddress>{k6a, k4a, k6b, k4b}));
        // A resolver that puts IPv4 first is obeyed...
        CHECK(AddressRace::interleave({k4a, k6a}) == (QList<QHostAddress>{k4a, k6a}));
        // ...a lead with no address of its own changes nothing...
        CHECK(AddressRace::interleave({k4a, k4b}, QAbstractSocket::IPv6Protocol) ==
              (QList<QHostAddress>{k4a, k4b}));
        // ...and the longer family carries on alone.
        CHECK(AddressRace::interleave({k6a, k6b, k4a}) == (QList<QHostAddress>{k6a, k4a, k6b}));
        CHECK(AddressRace::interleave({}).isEmpty());
    }

    SECTION("AddressRace — one leg at a time, a stagger apart; cancel aborts them all");
    {
        Bench b(kStaggerMs);
        b.race.start({k6a, k4a, k6b}, 443);
        CHECK(b.race.isRunning());
        CHECK_EQ(b.started(), 1);
        CHECK(b.records[0].address == k6a);
        CHECK_EQ(b.records[0].port, 443);

        CHECK(waitFor([&]() { return b.started() == 2; }, 1000));
        CHECK(b.records[1].address == k4a);
        CHECK(b.records[1].startedAt - b.records[0].startedAt >= kLeastGapMs);
        CHECK(waitFor([&]() { return b.started() == 3; }, 1000));
        CHECK(b.records[2].address == k6b);
        CHECK(b.records[2].startedAt - b.records[1].startedAt >= kLeastGapMs);
        CHECK(b.wins.isEmpty());
        CHECK_EQ(b.losses, 0);

        b.race.cancel();
        CHECK(!b.race.isRunning());
        for (const LegRecord& r : b.records)
            CHECK(r.aborted);
        sleepFor(20);
        CHECK(b.wins.isEmpty());
        CHECK_EQ(b.losses, 0);
    }

    SECTION("AddressRace — the first to connect wins, the rest are aborted or never tried");
    {
        Bench b(kStaggerMs);
        b.race.start({k6a, k4a, k6b}, 443);
        CHECK(waitFor([&]() { return b.started() == 2; }, 1000));

        // The silent IPv6 is still "connecting" when the IPv4 answers.
        b.legs[1]->answerConnected();
        CHECK_EQ(b.wins.size(), 1);
        CHECK(b.wins.value(0) == k4a);
        CHECK(b.wonAt >= kLeastGapMs);
        CHECK(!b.race.isRunning());
        CHECK(b.records[0].aborted);
        CHECK(!b.records[1].aborted); // the winner hung up by itself

        // A late answer from the loser changes nothing...
        b.legs[0]->answerConnected();
        CHECK_EQ(b.wins.size(), 1);
        // ...and the third address is never tried.
        sleepFor(200);
        CHECK_EQ(b.started(), 2);
        CHECK_EQ(b.losses, 0);
    }

    SECTION("AddressRace — a failure frees the stagger: the next address starts at once");
    {
        Bench b(500);
        b.race.start({k6a, k4a}, 443);
        CHECK_EQ(b.started(), 1);
        b.legs[0]->answerFailed();
        CHECK_EQ(b.started(), 2);
        CHECK(b.records[1].startedAt - b.records[0].startedAt < 100);
        CHECK(b.race.isRunning());
        b.legs[1]->answerConnected();
        CHECK(b.wins.value(0) == k4a);
        CHECK(b.wonAt < 400);
    }

    SECTION("AddressRace — every address failing is one loss");
    {
        Bench b(500);
        b.race.start({k6a, k4a}, 443);
        b.legs[0]->answerFailed();
        b.legs[1]->answerFailed();
        CHECK_EQ(b.losses, 1);
        CHECK(!b.race.isRunning());
        CHECK(b.wins.isEmpty());
        sleepFor(100);
        CHECK_EQ(b.losses, 1);
        CHECK_EQ(b.started(), 2);
    }

    SECTION("AddressRace — a single address, a single family: no stagger to wait for");
    {
        Bench b(500);
        b.race.start({k4a}, 8443);
        CHECK_EQ(b.started(), 1);
        CHECK_EQ(b.records[0].port, 8443);
        b.legs[0]->answerConnected();
        CHECK(b.wins.value(0) == k4a);
        CHECK(b.wonAt < 100);
        sleepFor(100);
        CHECK_EQ(b.started(), 1);
    }

    SECTION("AddressRace — no address at all is a loss, reported later, never from start()");
    {
        Bench b(kStaggerMs);
        b.race.start({}, 443);
        CHECK_EQ(b.losses, 0);
        CHECK(waitFor([&]() { return b.losses == 1; }, 1000));
        CHECK(!b.race.isRunning());
        CHECK_EQ(b.started(), 0);
    }

    SECTION("AddressRace — a new start cancels the race in progress");
    {
        Bench b(500);
        b.race.start({k6a}, 443);
        b.race.start({k4a}, 443);
        CHECK_EQ(b.started(), 2);
        CHECK(b.records[0].aborted);
        CHECK(!b.records[1].aborted);
        b.legs[0]->answerConnected(); // the old leg: nobody listens
        CHECK(b.wins.isEmpty());
        b.legs[1]->answerConnected();
        CHECK(b.wins.value(0) == k4a);
    }

    SECTION("AddressRace — a QNAM request is told to race too: no HTTP/2");
    {
        QNetworkRequest req(QUrl(QStringLiteral("https://stream.example.invalid/v1/claim")));
        AddressRace::enableOn(req);
        const QVariant h2 = req.attribute(QNetworkRequest::Http2AllowedAttribute);
        CHECK(h2.isValid());
        CHECK(!h2.toBool());
    }

    SECTION("AddressRace — the session location lookup races too: no HTTP/2");
    {
        // ipwho.is has IPv6 addresses and the lookup gives up after 5 s: over
        // HTTP/2, a silent IPv6 would hold it until then. Built, never sent.
        const QNetworkRequest req = GeoIpService::lookupRequest(QStringLiteral("203.0.113.7"));
        CHECK_EQ(req.url().host(), QStringLiteral("ipwho.is"));
        CHECK_EQ(req.url().path(), QStringLiteral("/203.0.113.7"));
        const QVariant h2 = req.attribute(QNetworkRequest::Http2AllowedAttribute);
        CHECK(h2.isValid());
        CHECK(!h2.toBool());
        CHECK_EQ(req.transferTimeout(), 5000);
    }

    SECTION("GeoIpService — only a global address is ever looked up");
    {
        CHECK(GeoIpService::worthLocating(QStringLiteral("8.8.8.8")));
        CHECK(GeoIpService::worthLocating(QStringLiteral("2a01:e0a:1:2::5")));
        CHECK(GeoIpService::worthLocating(QStringLiteral("::ffff:8.8.8.8")));
        for (const char* local : {"", "garbage", "127.0.0.1", "::1", "10.1.2.3", "172.20.0.1",
                                  "192.168.1.10", "169.254.3.4", "100.101.102.103",
                                  "fd12:3456:789a::1", "fe80::1", "::ffff:192.168.1.10"})
            CHECK(!GeoIpService::worthLocating(QString::fromLatin1(local)));
    }
}
