/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Whether TCP 443 is wanted on the router (network/Tcp443Demand): only once a
 * browser that has a say needs it, kept while one is connected through it and
 * ten minutes more, never by an ask alone, and asked for through the
 * introduction server a few times an hour at most. No router here: what the
 * guard did is told by hand (settled()).
 */
#include "test_framework.h"
#include "network/Tcp443Demand.h"

using mw::tcp443::Asker;
using mw::tcp443::Demand;

namespace {

constexpr uint64_t kGrace = Demand::kGraceMs;
constexpr uint64_t kRetry = Demand::kRetryMs;
constexpr uint64_t kHour = Demand::kHourMs;

/// Asked, held by the guard at `at`: the usual way in.
void heldAt(Demand& d, Asker who, uint64_t at)
{
    CHECK(d.ask(who, true, at));
    CHECK(d.state() == Demand::State::Opening);
    CHECK(!d.settled(true, at));
    CHECK(d.state() == Demand::State::Held);
}

void nobodyAsks()
{
    SECTION("Tcp443Demand: nothing is wanted before a browser that has a say asks");
    {
        Demand d;
        CHECK(!d.wanted());
        CHECK(d.dueAtMs() == 0);
        // Not from one that has no say (Q1): whatever else holds
        CHECK(!d.ask(Asker::Other, true, 1000));
        CHECK(!d.wanted());
        // Not while a switch it takes is off (P2): nothing, and nothing said
        QList<Demand::Line> said;
        CHECK(!d.ask(Asker::Session, false, 1000, &said));
        CHECK(!d.ask(Asker::Rendezvous, false, 1000, &said));
        CHECK(!d.wanted() && said.isEmpty());
        // A tick, a guard's word, a browser gone: nothing either
        CHECK(!d.tick(kHour));
        CHECK(!d.settled(true, kHour) && !d.settled(false, kHour));
        d.left(kHour);
        CHECK(!d.wanted() && d.peers() == 0);
    }
}

void oneOpeningAtATime()
{
    SECTION("Tcp443Demand: one opening at a time, and an ask keeps nothing longer (P3, Q2)");
    {
        Demand d;
        CHECK(d.ask(Asker::Session, true, 1000));
        // Asked again while the guard is at it: nothing new
        CHECK(!d.ask(Asker::Session, true, 1100));
        CHECK(!d.ask(Asker::Rendezvous, true, 1200));
        CHECK(!d.settled(true, 2000));
        CHECK(d.dueAtMs() == 2000 + kGrace);
        // Held with nobody on it: asks, as many as they come, do not move the
        // end of the grace
        for (uint64_t t = 3000; t < 2000 + kGrace; t += 30000) {
            CHECK(!d.ask(Asker::Rendezvous, true, t));
            CHECK(!d.ask(Asker::Session, true, t));
            CHECK(d.dueAtMs() == 2000 + kGrace);
        }
        CHECK(!d.tick(2000 + kGrace - 1));
        CHECK(d.tick(2000 + kGrace));
        CHECK(!d.wanted());
        CHECK(d.dueAtMs() == 0);
    }
}

void browsersKeepIt()
{
    SECTION("Tcp443Demand: a browser on 443 keeps it, and the grace runs from the last (Q2, P4)");
    {
        Demand d;
        heldAt(d, Asker::Rendezvous, 0);
        d.arrived(1000);
        CHECK(d.dueAtMs() == 0);
        CHECK(!d.tick(10 * kGrace)); // on it: never over
        d.arrived(2000);
        d.left(5 * kGrace);
        CHECK(d.dueAtMs() == 0); // one is still on it
        CHECK(!d.tick(20 * kGrace));
        d.left(20 * kGrace);
        CHECK(d.dueAtMs() == 21 * kGrace);
        CHECK(!d.tick(21 * kGrace - 1));
        // One comes back within the grace: kept, the grace starts again when
        // it goes
        d.arrived(21 * kGrace - 1);
        CHECK(!d.tick(21 * kGrace));
        d.left(22 * kGrace);
        CHECK(d.dueAtMs() == 23 * kGrace);
        CHECK(d.tick(23 * kGrace));
        CHECK(!d.wanted());
        // More left than came: never below none
        d.left(24 * kGrace);
        CHECK(d.peers() == 0);
    }
}

void askedDuringRemoval()
{
    SECTION("Tcp443Demand: a browser that asks once it is no longer wanted has it asked anew (P4)");
    {
        Demand d;
        heldAt(d, Asker::Session, 0);
        CHECK(d.tick(kGrace));
        CHECK(!d.wanted());
        // The guard takes it off; an ask that comes meanwhile wants it again
        // at once: the grace's end is no failure, and nothing waits
        CHECK(d.ask(Asker::Session, true, kGrace + 1));
        CHECK(d.state() == Demand::State::Opening);
        // The guard, which does one thing at a time, kept it or asked anew:
        // held again
        CHECK(!d.settled(true, kGrace + 5000));
        CHECK(d.state() == Demand::State::Held);
        CHECK(d.dueAtMs() == 2 * kGrace + 5000);
    }
}

void failuresWait()
{
    SECTION("Tcp443Demand: not held, nothing is asked again for a minute (P3, 443RETRY)");
    {
        Demand d;
        CHECK(d.ask(Asker::Session, true, 1000));
        // The guard left 443 as it is: no longer wanted, looked at again
        CHECK(d.settled(false, 2000));
        CHECK(!d.wanted());
        CHECK(!d.ask(Asker::Session, true, 2000 + kRetry - 1));
        CHECK(!d.ask(Asker::Rendezvous, true, 2000 + kRetry - 1));
        CHECK(!d.wanted());
        CHECK(d.ask(Asker::Session, true, 2000 + kRetry));
        // Held, then lost (another took it, the router forgot it): the same
        CHECK(!d.settled(true, 2000 + kRetry));
        d.arrived(2000 + kRetry);
        CHECK(d.settled(false, 3 * kRetry));
        CHECK(!d.wanted());
        CHECK(!d.ask(Asker::Session, true, 4 * kRetry - 1));
        CHECK(d.ask(Asker::Session, true, 4 * kRetry));
        // A word of the guard said again changes nothing
        CHECK(!d.settled(true, 4 * kRetry));
        CHECK(!d.settled(true, 4 * kRetry));
        CHECK(d.state() == Demand::State::Held);
    }
}

void hourlyBound()
{
    SECTION("Tcp443Demand: through the introduction server, six openings an hour at most (Q3)");
    {
        Demand d;
        uint64_t t = 0;
        // Six that open and come to nothing, each after the minute's wait
        for (int i = 0; i < Demand::kRendezvousPerHour; ++i) {
            CHECK(d.ask(Asker::Rendezvous, true, t));
            CHECK(d.settled(false, t + 1000));
            t += 1000 + kRetry;
        }
        // The seventh within the hour: nothing, said once
        QList<Demand::Line> said;
        CHECK(!d.ask(Asker::Rendezvous, true, t, &said));
        CHECK(!d.wanted());
        CHECK(said.size() == 1 && said.first().warning);
        said.clear();
        CHECK(!d.ask(Asker::Rendezvous, true, t + 1000, &said));
        CHECK(!d.ask(Asker::Rendezvous, true, t + 60000, &said));
        CHECK(said.isEmpty());
        // A signed-in session is not bound by it
        CHECK(d.ask(Asker::Session, true, t + 60000));
        CHECK(d.settled(false, t + 61000));
        // Once the first of them is an hour old, one more is taken
        CHECK(!d.ask(Asker::Rendezvous, true, kHour - 1));
        CHECK(d.ask(Asker::Rendezvous, true, kHour + 2 * kRetry));
        CHECK(d.settled(false, kHour + 2 * kRetry));
        // Said again only after an hour
        said.clear();
        uint64_t later = kHour + 4 * kRetry;
        uint64_t lastTaken = 0;
        while (d.ask(Asker::Rendezvous, true, later, &said)) {
            lastTaken = later;
            CHECK(d.settled(false, later));
            later += kRetry;
        }
        CHECK(said.size() == 1);
        // An ask that is not taken counts for nothing: half an hour of
        // refused asks does not hold the next one back once the hour of the
        // last taken is over
        for (uint64_t r = later; r < later + kHour / 2; r += 10000)
            CHECK(!d.ask(Asker::Rendezvous, true, r, &said));
        CHECK(!d.wanted());
        CHECK(d.ask(Asker::Rendezvous, true, lastTaken + kHour));
    }
}

void neverAnswered()
{
    SECTION("Tcp443Demand: an ask the guard never answers is given up after 2 min (R2)");
    {
        Demand d;
        CHECK(d.ask(Asker::Rendezvous, true, 1000));
        // Due when the bound runs out, and not before
        CHECK(d.dueAtMs() == 1000 + Demand::kOpeningMaxMs);
        CHECK(!d.tick(1000 + Demand::kOpeningMaxMs - 1));
        CHECK(d.state() == Demand::State::Opening);
        // Asks meanwhile are not taken
        CHECK(!d.ask(Asker::Session, true, 5000));
        CHECK(d.tick(1000 + Demand::kOpeningMaxMs));
        CHECK(!d.wanted());
        // As a run that did not hold it: the minute's wait, then taken again
        const uint64_t over = 1000 + Demand::kOpeningMaxMs;
        CHECK(!d.ask(Asker::Session, true, over + kRetry - 1));
        CHECK(d.ask(Asker::Session, true, over + kRetry));
        // A word that comes after the bound changes nothing of what is new
        CHECK(!d.settled(true, over + kRetry + 1));
        CHECK(d.state() == Demand::State::Held);
        // Held: the bound is no longer what is due, the grace is
        CHECK(d.dueAtMs() == over + kRetry + 1 + kGrace);
        CHECK(!d.tick(over + kRetry + 1 + Demand::kOpeningMaxMs));
        CHECK(d.wanted());
    }
}

void switchedOff()
{
    SECTION("Tcp443Demand: a switch that goes off ends the demand; it is asked again after");
    {
        Demand d;
        heldAt(d, Asker::Session, 0);
        d.arrived(1);
        CHECK(d.off());
        CHECK(!d.wanted());
        CHECK(!d.off());
        CHECK(d.dueAtMs() == 0);
        // Back on: nothing until a browser asks, and no minute to wait
        CHECK(d.ask(Asker::Session, true, 2));
        CHECK(d.wanted());
    }
}

} // namespace

void run_tcp443_demand_tests()
{
    nobodyAsks();
    oneOpeningAtATime();
    browsersKeepIt();
    askedDuringRemoval();
    failuresWait();
    hourlyBound();
    neverAnswered();
    switchedOff();
}
