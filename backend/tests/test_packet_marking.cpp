/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The DSCP the stream's packets leave with (streaming/PacketMarking.h): EF for
 * the sound and AF41 for the rest by default, nothing when the setting says so,
 * and the bench's MW_DSCP only when it is three values that fit in six bits.
 */
#include "../src/streaming/PacketMarking.h"

#include "test_framework.h"

void run_packet_marking_tests()
{
    using PacketMarking::resolve;
    using V = std::array<unsigned int, 3>;

    SECTION("PacketMarking — EF for the sound, AF41 for video and data by default; 0 when off");
    {
        CHECK(resolve(true, "") == V({46, 34, 34}));
        CHECK(resolve(false, "") == V({0, 0, 0}));
    }

    SECTION("PacketMarking — MW_DSCP overrides the setting, both ways");
    {
        CHECK(resolve(true, "34,34,34") == V({34, 34, 34})); // the sound back with the rest
        CHECK(resolve(false, "46,34,34") == V({46, 34, 34}));
        CHECK(resolve(true, "0,0,0") == V({0, 0, 0}));
        CHECK(resolve(true, "63,0,10") == V({63, 0, 10}));
    }

    SECTION("PacketMarking — a malformed MW_DSCP is no override");
    {
        for (const char* bad : {"46", "46,34", "46,34,34,10", "64,34,34", "46,,34", "a,b,c",
                                "-1,34,34", "46,34,34,", " 46,34,34", "100,34,34"})
            CHECK(resolve(true, bad) == V({46, 34, 34}));
    }

    SECTION("PacketMarking — the server's handover wins, and spares reading the worker's own");
    {
        int reads = 0;
        const auto ownOn = [&reads] {
            ++reads;
            return true;
        };
        const auto ownDscp = [&reads] {
            ++reads;
            return std::string("46,46,46");
        };
        PacketMarking::Handover off;
        off.marking = false;
        off.dscp = std::string();
        CHECK(resolve(off, ownOn, ownDscp) == V({0, 0, 0})); // the owner unticked the box
        CHECK_EQ(reads, 0);

        PacketMarking::Handover on;
        on.marking = true;
        on.dscp = std::string();
        CHECK(resolve(on, ownOn, ownDscp) == V({46, 34, 34}));
        CHECK_EQ(reads, 0);

        PacketMarking::Handover bench;
        bench.marking = false;
        bench.dscp = std::string("46,34,34");
        CHECK(resolve(bench, ownOn, ownDscp) == V({46, 34, 34}));
        CHECK_EQ(reads, 0);

        // An older parent hands nothing over: the worker reads its own, as before.
        CHECK(resolve(PacketMarking::Handover{}, ownOn, ownDscp) == V({46, 46, 46}));
        CHECK_EQ(reads, 2);
        reads = 0;
        PacketMarking::Handover markingOnly;
        markingOnly.marking = false;
        CHECK(resolve(markingOnly, ownOn, [] { return std::string(); }) == V({0, 0, 0}));
        CHECK_EQ(reads, 0);
    }

    SECTION("PacketMarking — the handover is kept process-wide until replaced");
    {
        const PacketMarking::Handover before = PacketMarking::handover();
        CHECK(!before.marking.has_value());
        CHECK(!before.dscp.has_value());

        PacketMarking::Handover h;
        h.marking = false;
        h.dscp = std::string("0,34,34");
        PacketMarking::setHandover(h);
        CHECK(PacketMarking::handover().marking == std::optional<bool>(false));
        CHECK(PacketMarking::handover().dscp == std::optional<std::string>("0,34,34"));

        PacketMarking::setHandover({}); // leave the runner as it found it
        CHECK(!PacketMarking::handover().marking.has_value());
        CHECK(!PacketMarking::handover().dscp.has_value());
    }
}
