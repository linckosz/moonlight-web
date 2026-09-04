/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "core/CadenceAlign.h"
#include "core/FrameCadence.h"
#include "native_test_framework.h"

using mw::native::alignCadence;
using mw::native::AlignedCadence;
using mw::native::FrameCadence;

void run_cadence_align_tests()
{
    SECTION("CadenceAlign — the plan's three examples");
    {
        // 144 Hz client, 60 set: every second refresh, 72 fps.
        AlignedCadence a = alignCadence(60, 144000, 165);
        CHECK(a.aligned);
        CHECK_EQ(a.divisor, 2);
        CHECK_EQ(a.fps, 72);
        CHECK_EQ(a.intervalUs, 2 * 1000000000LL / 144000); // 13888 µs

        // 120 Hz client: 60 already divides it — aligned, and the same rate.
        a = alignCadence(60, 120000, 165);
        CHECK(a.aligned);
        CHECK_EQ(a.divisor, 2);
        CHECK_EQ(a.fps, 60);
        CHECK_EQ(a.intervalUs, 16666);

        // 60 Hz client: every refresh.
        a = alignCadence(60, 60000, 165);
        CHECK(a.aligned);
        CHECK_EQ(a.divisor, 1);
        CHECK_EQ(a.fps, 60);
    }

    SECTION("CadenceAlign — 165 Hz client, 60 set: every third refresh, 55 fps");
    {
        const AlignedCadence a = alignCadence(60, 165000, 165);
        CHECK(a.aligned);
        CHECK_EQ(a.divisor, 3);
        CHECK_EQ(a.fps, 55);
        // Three 165 Hz periods exactly, not 1 s / 55.
        CHECK_EQ(a.intervalUs, 3 * 1000000000LL / 165000); // 18181 µs
        // 165 / 2 = 82.5 is out of the window; 165 / 4 = 41.25 too.
    }

    SECTION("CadenceAlign — nearest divisor wins, the faster one on a tie");
    {
        // 100 Hz client, 60 set: 50 (−17 %) is nearer than nothing else in
        // the window (100 is +67 %).
        AlignedCadence a = alignCadence(60, 100000, 165);
        CHECK(a.aligned);
        CHECK_EQ(a.fps, 50);
        CHECK_EQ(a.divisor, 2);

        // 144 Hz client, 60 set: 72 (+20 %) and 48 (−20 %) tie; 72 wins.
        a = alignCadence(60, 144000, 165);
        CHECK_EQ(a.fps, 72);

        // 240 Hz client, 60 set: 60 exactly (divisor 4); 80 and 48 lose.
        a = alignCadence(60, 240000, 240);
        CHECK(a.aligned);
        CHECK_EQ(a.fps, 60);
        CHECK_EQ(a.divisor, 4);
    }

    SECTION("CadenceAlign — no divisor within a fifth: the setting stands");
    {
        // 165 Hz client, 100 set: 82.5 (−17.5 %) is in; 165 is not. So aligned.
        AlignedCadence a = alignCadence(100, 165000, 165);
        CHECK(a.aligned);
        CHECK_EQ(a.fps, 83);
        CHECK_EQ(a.divisor, 2);

        // 165 Hz client, 120 set: 165 (+37 %) and 82.5 (−31 %) both out.
        a = alignCadence(120, 165000, 165);
        CHECK(!a.aligned);
        CHECK_EQ(a.fps, 120);
        CHECK_EQ(a.intervalUs, 8333);
        CHECK_EQ(a.divisor, 0);
    }

    SECTION("CadenceAlign — the host display bounds the answer");
    {
        // 60 Hz host, 144 Hz client, 60 set: 72 would be aligned but the host
        // has no 72 presents a second to give. The setting stands.
        AlignedCadence a = alignCadence(60, 144000, 60);
        CHECK(!a.aligned);
        CHECK_EQ(a.fps, 60);

        // Same host, 120 Hz client: 60 = the display's rate, allowed (it means
        // every present, which is what the gate does at that rate anyway).
        a = alignCadence(60, 120000, 60);
        CHECK(a.aligned);
        CHECK_EQ(a.fps, 60);

        // An unknown display (0) bounds nothing.
        a = alignCadence(60, 144000, 0);
        CHECK(a.aligned);
        CHECK_EQ(a.fps, 72);
    }

    SECTION("CadenceAlign — nothing to align: setting 0 or no client rate");
    {
        AlignedCadence a = alignCadence(0, 144000, 165);
        CHECK(!a.aligned);
        CHECK_EQ(a.fps, 0);
        CHECK_EQ(a.intervalUs, 0);

        a = alignCadence(60, 0, 165);
        CHECK(!a.aligned);
        CHECK_EQ(a.fps, 60);
        CHECK_EQ(a.intervalUs, 16666);

        a = alignCadence(60, -5, 165);
        CHECK(!a.aligned);
    }

    SECTION("CadenceAlign — a measured rate that is not a round number");
    {
        // A browser measures 164.8 Hz on a 165 Hz panel. Divisor 3, 54.9 fps,
        // reported as 55; the interval is the exact triple of what it measured.
        const AlignedCadence a = alignCadence(60, 164800, 165);
        CHECK(a.aligned);
        CHECK_EQ(a.divisor, 3);
        CHECK_EQ(a.fps, 55);
        CHECK_EQ(a.intervalUs, 3 * 1000000000LL / 164800);

        // 59.94 Hz (NTSC) client, 60 set: every refresh, 59.94 → 60.
        const AlignedCadence ntsc = alignCadence(60, 59940, 165);
        CHECK(ntsc.aligned);
        CHECK_EQ(ntsc.divisor, 1);
        CHECK_EQ(ntsc.fps, 60);
        CHECK_EQ(ntsc.intervalUs, 1000000000LL / 59940); // 16683 µs
    }

    SECTION("FrameCadence::fromIntervalNs — the aligned interval drives the gate exactly");
    {
        // 55 fps as three 165 Hz periods, on a 165 Hz display: over 10 minutes
        // the gate admits every third present, 33 000 of 99 000, and the grid
        // never slips — which a grid in whole microseconds would do every two
        // minutes (18181 µs against 18181.8).
        const AlignedCadence a = alignCadence(60, 165000, 165);
        CHECK_EQ(a.intervalNs, 3 * 1000000000000LL / 165000); // 18 181 818 ns
        FrameCadence c = FrameCadence::fromIntervalNs(a.intervalNs, 165);
        CHECK(c.enabled());
        CHECK_EQ(c.intervalUs(), a.intervalUs);
        int admitted = 0;
        int64_t lastAdmitted = -1;
        int64_t worstGap = 0;
        const double step = 1e6 / 165.0;
        for (int i = 0; i < 99000; ++i) {
            const int64_t now = static_cast<int64_t>(i * step);
            if (!c.admit(now)) continue;
            admitted++;
            if (lastAdmitted >= 0 && now - lastAdmitted > worstGap) worstGap = now - lastAdmitted;
            lastAdmitted = now;
        }
        CHECK_EQ(admitted, 33000);
        CHECK_EQ(c.skipped(), 66000);
        // Every gap is three presents: no fourth-present hiccup anywhere.
        CHECK(worstGap <= 18182);

        // Zero disables, like the constructor.
        FrameCadence off = FrameCadence::fromIntervalNs(0, 165);
        CHECK(!off.enabled());
        CHECK(off.admit(12345));
    }
}
