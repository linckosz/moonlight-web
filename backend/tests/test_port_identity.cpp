/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "test_framework.h"
#include "network/PortIdentity.h"
#include "network/RouterHoles.h"
#include "network/RouterPortCore.h"
#include "network/RouterPortPools.h"

#include <QList>
#include <QPair>

namespace {

using mw::portidentity::Bench;
using mw::portidentity::benchVerdict;
using mw::portidentity::devPortsFor;
using mw::portidentity::Verdict;
using mw::sockettables::Listens;

/// Tables in which nothing is held but what `held` names, and which say
/// nothing of what `silent` names. Every question is written down.
struct Tables
{
    QList<QPair<uint16_t, bool>> held;
    QList<QPair<uint16_t, bool>> silent;
    QList<QPair<uint16_t, bool>> asked;

    mw::portidentity::Listens reader()
    {
        return [this](uint16_t port, bool tcp) {
            asked.append({port, tcp});
            if (held.contains({port, tcp})) return Listens::Yes;
            if (silent.contains({port, tcp})) return Listens::Unknown;
            return Listens::No;
        };
    }
};

} // namespace

void run_port_identity_tests()
{
    using namespace mw::routerports;

    SECTION("PortIdentity: the bench key is nothing unless set on a DEV identity");
    {
        Tables tables;
        CHECK(benchVerdict(true, false, tables.reader()).bench == Bench::Off);
        CHECK(benchVerdict(false, false, tables.reader()).bench == Bench::Off);
        CHECK(benchVerdict(false, true, tables.reader()).bench == Bench::NotDev);
        // The tables are not even read
        CHECK(tables.asked.isEmpty());
        CHECK(mw::portidentity::said(Verdict{}).isEmpty());
        CHECK(!mw::portidentity::said(benchVerdict(false, true, tables.reader())).isEmpty());
    }

    SECTION("PortIdentity: granted where nothing holds a conferencing port");
    {
        Tables tables;
        // Held, but none of the eight: the stream block, a DEV tunnel port
        tables.held = {{48550, false}, {48573, false}, {kDevTunnelPoolBegin, false}, {443, true}};
        const Verdict verdict = benchVerdict(true, true, tables.reader());
        CHECK(verdict.bench == Bench::Granted);
        // Each of the eight, in UDP and in TCP, and nothing else
        CHECK_EQ(static_cast<int>(tables.asked.size()), 16);
        for (const uint16_t port : kTunnelPreferred) {
            CHECK(tables.asked.contains({port, false}));
            CHECK(tables.asked.contains({port, true}));
        }
        CHECK(mw::portidentity::said(verdict).contains(QStringLiteral("is ON")));
    }

    SECTION("PortIdentity: refused when one of them is held, in either protocol");
    for (const uint16_t port : kTunnelPreferred) {
        for (const bool tcp : {false, true}) {
            Tables tables;
            tables.held = {{port, tcp}};
            const Verdict verdict = benchVerdict(true, true, tables.reader());
            CHECK(verdict.bench == Bench::Held);
            CHECK_EQ(static_cast<int>(verdict.port), static_cast<int>(port));
            CHECK(verdict.tcp == tcp);
            const QString text = mw::portidentity::said(verdict);
            CHECK(text.contains(QStringLiteral("REFUSED")));
            CHECK(text.contains(QString::number(port)));
            CHECK(text.contains(tcp ? QStringLiteral("TCP") : QStringLiteral("UDP")));
        }
    }

    SECTION("PortIdentity: refused when the tables say nothing");
    {
        Tables tables;
        tables.silent = {{3480, true}};
        CHECK(benchVerdict(true, true, tables.reader()).bench == Bench::Unknown);
        // A port held is said, even after an answer that said nothing
        tables.held = {{5352, true}};
        const Verdict verdict = benchVerdict(true, true, tables.reader());
        CHECK(verdict.bench == Bench::Held);
        CHECK_EQ(static_cast<int>(verdict.port), 5352);
        // No reader at all
        CHECK(benchVerdict(true, true, {}).bench == Bench::Unknown);
        CHECK(mw::portidentity::said(benchVerdict(true, true, {}))
                  .contains(QStringLiteral("REFUSED")));
    }

    SECTION("PortIdentity: only a granted key moves a DEV identity off its ports");
    CHECK(devPortsFor(true, Bench::Off));
    CHECK(devPortsFor(true, Bench::Held));
    CHECK(devPortsFor(true, Bench::Unknown));
    CHECK(!devPortsFor(true, Bench::Granted));
    for (const Bench bench : {Bench::Off, Bench::NotDev, Bench::Held, Bench::Unknown})
        CHECK(!devPortsFor(false, bench));

    SECTION("PortIdentity: what the ports are, refused and granted");
    {
        // Refused: as any DEV identity
        const bool refused = devPortsFor(true, Bench::Held);
        CHECK(!RouterPortCore::tunnelPortAllowed(refused, 3478));
        CHECK(RouterPortCore::tunnelRequest(refused, {}).preferred.isEmpty());
        CHECK(
            !RouterPortCore::muxRequest(refused, RouterPortCore::kMuxStandard, {}).hasCandidate());
        CHECK_EQ(
            static_cast<int>(
                RouterPortCore::muxRequest(refused, RouterPortCore::kMuxBlock, {}).preferred[0]),
            48573);

        // Granted: as the installed host, and none of the DEV identity's
        const bool granted = devPortsFor(true, Bench::Granted);
        CHECK(RouterPortCore::tunnelPortAllowed(granted, 3478));
        CHECK(!RouterPortCore::tunnelPortAllowed(granted, kDevTunnelPoolBegin));
        CHECK_EQ(static_cast<int>(RouterPortCore::tunnelRequest(granted, {}).preferred[0]), 3478);
        CHECK_EQ(
            static_cast<int>(
                RouterPortCore::muxRequest(granted, RouterPortCore::kMuxStandard, {}).preferred[0]),
            3478);
        CHECK_EQ(
            static_cast<int>(
                RouterPortCore::muxRequest(granted, RouterPortCore::kMuxBlock, {}).preferred[0]),
            48550);
        CHECK_EQ(static_cast<int>(slotMediaPort(0, true, granted)), 48550 + kMuxBlockSpan);

        // The record of router entries follows: a line written under the key
        // is a line of the installed host's lists, which a run without the
        // key does not act on.
        mw::routerholes::Hole line;
        line.kind = mw::routerholes::Kind::Streams;
        line.external = 3478;
        line.internal = 3478;
        CHECK(mw::routerholes::allowed(granted, line));
        CHECK(!mw::routerholes::allowed(refused, line));
    }
}
