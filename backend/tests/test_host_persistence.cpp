/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Host persistence: what survives a restart decides whether a paired Sunshine
 * is still reachable on the next launch. Issue #9 was exactly this — the address
 * that answered lived only in memory, so every restart fell back to the host's
 * self-reported <LocalIP> and stayed there, with no way back from the UI.
 */
#include "test_framework.h"

#include "backend/NvComputer.h"

#include <QSettings>
#include <QTemporaryDir>

namespace {

// A store of our own, so the suite never reads or writes the real host list.
QSettings* freshStore(const QString& dir)
{
    return new QSettings(dir + "/hosts.ini", QSettings::IniFormat);
}

NvComputer makeHost()
{
    NvComputer h;
    h.uuid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    h.name = QStringLiteral("BAZZITE");
    h.pairState = NvComputer::PS_PAIRED;
    h.activeHttpsPort = 47990;
    // What actually answered — mDNS-resolved, or typed in by an operator.
    h.activeAddress = NvAddress(QStringLiteral("192.168.1.20"), 47989);
    // What the host says about itself: here a container bridge, routable from
    // nowhere. The exact shape of the bug reported on Bazzite.
    h.localAddress = NvAddress(QStringLiteral("10.88.0.1"), 47989);
    return h;
}

void roundTrip(const NvComputer& in, NvComputer& out, const QString& dir)
{
    {
        QSettings* s = freshStore(dir);
        s->beginWriteArray("hosts");
        s->setArrayIndex(0);
        in.serialize(*s);
        s->endArray();
        s->sync();
        delete s;
    }
    QSettings* s = freshStore(dir);
    const int count = s->beginReadArray("hosts");
    CHECK_EQ(count, 1);
    s->setArrayIndex(0);
    out = NvComputer(*s);
    s->endArray();
    delete s;
}

} // namespace

void run_host_persistence_tests()
{
    SECTION("host persistence");

    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    // --- The address that answered survives, and stays the one we poll first ---
    {
        NvComputer restored;
        roundTrip(makeHost(), restored, tmp.path());

        CHECK_EQ(restored.uuid, QStringLiteral("11111111-2222-3333-4444-555555555555"));
        CHECK_EQ(restored.pairState, NvComputer::PS_PAIRED);
        CHECK_EQ(restored.activeAddress.address(), QStringLiteral("192.168.1.20"));
        CHECK_EQ(restored.activeAddress.port(), quint16(47989));

        // The whole point: a restarted host is polled at the address known to
        // work, not at the one the host claims for itself.
        const QVector<NvAddress> addrs = restored.uniqueAddresses();
        CHECK(!addrs.isEmpty());
        CHECK_EQ(addrs.first().address(), QStringLiteral("192.168.1.20"));
        // The self-reported one is kept as a fallback candidate, not dropped.
        CHECK(addrs.size() >= 2);
    }

    // --- A manually added address survives too ---
    {
        NvComputer h = makeHost();
        h.activeAddress = NvAddress();
        h.localAddress = NvAddress();
        h.manualAddress = NvAddress(QStringLiteral("192.168.1.42"), 47989);

        NvComputer restored;
        roundTrip(h, restored, tmp.path());
        CHECK_EQ(restored.manualAddress.address(), QStringLiteral("192.168.1.42"));
        CHECK_EQ(restored.uniqueAddresses().first().address(), QStringLiteral("192.168.1.42"));
    }

    // --- A host with nothing to poll is visibly that, not silently idle ---
    {
        NvComputer h = makeHost();
        h.activeAddress = NvAddress();
        h.localAddress = NvAddress();

        NvComputer restored;
        roundTrip(h, restored, tmp.path());
        CHECK(restored.uniqueAddresses().isEmpty());
    }

    // --- A poll now corrects a stale <LocalIP> instead of freezing it ---
    {
        NvComputer stored = makeHost();

        NvComputer polled;
        polled.uuid = stored.uuid;
        polled.state = NvComputer::CS_ONLINE;
        polled.activeAddress = NvAddress(QStringLiteral("192.168.1.20"), 47989);
        polled.localAddress = NvAddress(QStringLiteral("192.168.1.20"), 47989);

        CHECK(stored.update(polled));
        CHECK_EQ(stored.localAddress.address(), QStringLiteral("192.168.1.20"));
    }

    // --- A manual address reaches the stored host through update() ---
    {
        NvComputer stored = makeHost();

        NvComputer manual;
        manual.uuid = stored.uuid;
        manual.manualAddress = NvAddress(QStringLiteral("192.168.1.42"), 47989);

        CHECK(stored.update(manual));
        CHECK_EQ(stored.manualAddress.address(), QStringLiteral("192.168.1.42"));
    }

    // --- The native engine is this machine even though it has no address ---
    //
    // It is the one host where "is this us?" is certain, and the one host the
    // address matching cannot answer: it has nothing to reach. Since the
    // Windows installer stopped shipping Sunshine, this card is the only local
    // paired host a fresh install has, and the one-click update offers itself
    // on exactly that condition.
    {
        NvComputer native;
        native.uuid = QStringLiteral("native-0");
        native.backendType = QStringLiteral("native");
        native.pairState = NvComputer::PS_PAIRED;
        CHECK(native.isNativeEngine());
        CHECK(native.uniqueAddresses().isEmpty());
        CHECK(native.isLocalMachine());
        CHECK(native.toJson().value(QStringLiteral("isLocalHost")).toBool());

        // A GameStream host with no address stays what it was: unknown, not us.
        NvComputer orphan = makeHost();
        orphan.activeAddress = NvAddress();
        orphan.localAddress = NvAddress();
        orphan.manualAddress = NvAddress();
        CHECK(!orphan.isNativeEngine());
        CHECK(!orphan.isLocalMachine());
    }
}
