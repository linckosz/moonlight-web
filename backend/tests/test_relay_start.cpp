/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A stream starting while its media port is held. The relay itself needs a whole
 * stream around it; what decides is here.
 */
#include "test_framework.h"

#include "streaming/RelayStart.h"

#include <QUdpSocket>

#include <stdexcept>

using namespace mw::relaystart;

void run_relay_start_tests()
{
    SECTION("Relay start — a media port already held");

    // A port nothing holds, found by asking the system for one.
    QUdpSocket finder;
    CHECK(finder.bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::DontShareAddress));
    const auto port = static_cast<uint16_t>(finder.localPort());
    finder.close();

    CHECK(udpPortFree(port));
    CHECK(holeUsable(true, port));
    CHECK(!holeUsable(false, port));

    // Held, as by another stream on this machine: the hole is left alone.
    {
        QUdpSocket holder;
        CHECK(holder.bind(QHostAddress::AnyIPv4, port, QAbstractSocket::DontShareAddress));
        CHECK(!udpPortFree(port));
        CHECK(!holeUsable(true, port));
    }
    CHECK(udpPortFree(port));

    // The start that goes well says nothing, and did run.
    QString why;
    int runs = 0;
    CHECK(prepareCaught([&]() { ++runs; }, why));
    CHECK_EQ(runs, 1);
    CHECK(why.isEmpty());

    // What the library throws when it can bind nothing: the reason comes back, and
    // the process is still here to say it.
    CHECK(!prepareCaught(
        []() { throw std::runtime_error("Failed to gather local ICE candidates"); }, why));
    CHECK_EQ(why, QStringLiteral("Failed to gather local ICE candidates"));
}
