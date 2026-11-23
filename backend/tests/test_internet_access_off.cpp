/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * Internet Access turned off (server/InternetAccessOff.h). What matters: the
 * switch is off and InternetAccessManager stopped before the answer leaves;
 * the tunnels, the router's holes and the line go after it, together, once,
 * in that order; and in between, while the line is still up, the tunnel takes
 * no browser: an offer that arrives then is refused.
 */

#include "../src/server/AppSettings.h"
#include "../src/server/InternetAccessOff.h"

#include "test_framework.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

namespace {

void runPending()
{
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

} // namespace

void run_internet_access_off_tests()
{
    using namespace mw::internetoff;

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";

    SECTION("InternetAccessOff — the switch and InternetAccessManager at once, the rest after");
    {
        settings.setInternetAccessEnabled(true);
        CHECK(tunnelAdmits(settings));

        QStringList order;
        QString dropWhy;
        bool lineUp = true;
        Steps steps;
        steps.writeOff = [&]() {
            order << "write";
            settings.setInternetAccessEnabled(false);
        };
        steps.stopAccess = [&]() { order << "stop"; };
        steps.dropTunnels = [&](const QString& why) {
            order << "tunnels";
            dropWhy = why;
        };
        steps.switchChanged = [&]() { order << "switch"; };
        steps.lineOff = [&]() {
            order << "line";
            lineUp = false;
        };
        QObject context;

        turnOff(steps, &context, QStringLiteral("test"));
        // The answer leaves here: the switch is off, the manager stopped,
        // and nothing else has gone yet.
        CHECK_EQ(order.join(','), QString("write,stop"));
        CHECK_EQ(settings.internetAccessEnabled(), false);

        // An offer the line brings in between is refused: the line is still
        // up, the tunnel's gate already says no.
        CHECK(lineUp);
        CHECK(!tunnelAdmits(settings));

        runPending();
        CHECK_EQ(order.join(','), QString("write,stop,tunnels,switch,line"));
        CHECK(!dropWhy.isEmpty());
        CHECK(!lineUp);

        // Once: nothing runs again on later turns.
        runPending();
        CHECK_EQ(order.size(), 5);
    }

    SECTION("InternetAccessOff — the tunnel's gate follows the switch");
    {
        settings.setInternetAccessEnabled(false);
        CHECK(!tunnelAdmits(settings));
        settings.setInternetAccessEnabled(true);
        CHECK(tunnelAdmits(settings));
        settings.setInternetAccessEnabled(false);
        CHECK(!tunnelAdmits(settings));
    }

    SECTION("InternetAccessOff — a step not given is skipped, the others still run");
    {
        settings.setInternetAccessEnabled(true);
        int switched = 0;
        Steps steps;
        steps.writeOff = [&]() { settings.setInternetAccessEnabled(false); };
        steps.switchChanged = [&]() { ++switched; };
        QObject context;
        turnOff(steps, &context, QStringLiteral("test"));
        CHECK(!tunnelAdmits(settings));
        runPending();
        CHECK_EQ(switched, 1);
    }

    SECTION("InternetAccessOff — turned back on before the turn, the line is kept");
    {
        settings.setInternetAccessEnabled(true);
        QStringList order;
        bool lineUp = true;
        Steps steps;
        steps.writeOff = [&]() {
            order << "write";
            settings.setInternetAccessEnabled(false);
        };
        steps.stopAccess = [&]() { order << "stop"; };
        steps.dropTunnels = [&](const QString&) { order << "tunnels"; };
        steps.switchChanged = [&]() { order << "switch"; };
        steps.lineOff = [&]() {
            order << "line";
            lineUp = false;
        };
        steps.stillOff = [&]() { return !settings.internetAccessEnabled(); };
        QObject context;

        turnOff(steps, &context, QStringLiteral("test"));
        CHECK(lineUp);
        // A second request, handled before the turn: on again, line on.
        settings.setInternetAccessEnabled(true);
        lineUp = true;
        runPending();
        CHECK_EQ(order.join(','), QString("write,stop,tunnels,switch"));
        CHECK(lineUp);
        CHECK(tunnelAdmits(settings));
    }

    SECTION("InternetAccessOff — its context gone before the turn, nothing more runs");
    {
        int later = 0;
        Steps steps;
        steps.dropTunnels = [&](const QString&) { ++later; };
        steps.lineOff = [&]() { ++later; };
        auto context = std::make_unique<QObject>();
        turnOff(steps, context.get(), QStringLiteral("test"));
        context.reset();
        runPending();
        CHECK_EQ(later, 0);
    }
}
