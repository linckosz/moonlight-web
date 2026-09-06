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

#include "encode/ReferenceSlots.h"
#include "native_test_framework.h"

using mw::native::encode::ReferenceSlots;

void run_reference_slots_tests()
{
    SECTION("ReferenceSlots — disabled table answers nothing");
    {
        ReferenceSlots none;
        CHECK(!none.enabled());
        CHECK(none.count() == 0);
        CHECK(none.slotFor(0) == -1);
        CHECK(none.slotFor(7, true) == -1);
        CHECK(none.cleanSlotBefore(100) == -1);
        none.marked(0, 5); // ignored, no slots
        CHECK(none.held() == 0);

        ReferenceSlots clamped(100);
        CHECK(clamped.count() == ReferenceSlots::kMaxSlots);
        ReferenceSlots negative(-3);
        CHECK(!negative.enabled());
    }

    SECTION("ReferenceSlots — every frame marked, 4 slots: reach is 3 frames");
    {
        ReferenceSlots slots(4);
        CHECK(slots.stride() == 1);
        CHECK(slots.reachFrames() == 4);
        for (uint32_t n = 0; n < 8; ++n) {
            const int s = slots.slotFor(n);
            CHECK(s == static_cast<int>(n % 4));
            slots.marked(s, n);
        }
        CHECK(slots.held() == 4); // frames 4..7 in slots 0..3

        // Frame 6 lost: 5 is the newest clean one, in slot 1.
        CHECK(slots.cleanSlotBefore(6) == 1);
        // Frame 4 lost and nothing older held: out of reach.
        CHECK(slots.cleanSlotBefore(4) == -1);
        // A loss that is still in the future of everything held: newest = 7.
        CHECK(slots.cleanSlotBefore(9) == 3);
        // Slot bitfields are one bit per slot.
        CHECK(ReferenceSlots::bitFor(0) == 1u);
        CHECK(ReferenceSlots::bitFor(3) == 8u);
        CHECK(ReferenceSlots::bitFor(-1) == 0u);

        // Forget the tainted ones: 6 and 7 go, 4 and 5 stay.
        slots.dropFrom(6);
        CHECK(slots.held() == 2);
        CHECK(slots.cleanSlotBefore(100) == 1); // 5
        slots.dropFrom(0);
        CHECK(slots.held() == 0);
    }

    SECTION("ReferenceSlots — the stride spans the reach at any frame rate");
    {
        // 125 ms of frames, divided over the slots, rounded up.
        CHECK(ReferenceSlots::strideFor(60, 4) == 2);  // 8 frames  = 133 ms
        CHECK(ReferenceSlots::strideFor(165, 4) == 6); // 24 frames = 145 ms
        CHECK(ReferenceSlots::strideFor(30, 4) == 1);  // 4 frames  = 133 ms
        CHECK(ReferenceSlots::strideFor(240, 4) == 8); // 32 frames = 133 ms
        CHECK(ReferenceSlots::strideFor(60, 8) == 1);  // 8 frames  = 133 ms
        CHECK(ReferenceSlots::strideFor(0, 4) == 1);   // nonsense in, harmless out
        CHECK(ReferenceSlots::strideFor(60, 0) == 1);

        ReferenceSlots slots(4, ReferenceSlots::strideFor(60, 4));
        CHECK(slots.stride() == 2);
        CHECK(slots.reachFrames() == 8);
        // Only even frames take a turn; the slot advances per marked frame.
        CHECK(slots.slotFor(0) == 0);
        CHECK(slots.slotFor(1) == -1);
        CHECK(slots.slotFor(2) == 1);
        CHECK(slots.slotFor(6) == 3);
        CHECK(slots.slotFor(8) == 0);
        // A keyframe marks whatever its number.
        CHECK(slots.slotFor(1, true) == 0);
        CHECK(slots.slotFor(3, true) == 1);

        for (uint32_t n = 0; n <= 20; ++n) {
            const int s = slots.slotFor(n);
            if (s >= 0) slots.marked(s, n);
        }
        // Held: 14, 16, 18, 20. A loss at 15, reported at frame 21: 14 is
        // clean — 6 frames back, healed by a delta where stride 1 could not.
        CHECK(slots.cleanSlotBefore(15) == slots.slotFor(14));
        // At 13 the newest clean would be 12, already overwritten: keyframe.
        CHECK(slots.cleanSlotBefore(13) == -1);
    }

    SECTION("ReferenceSlots — a keyframe empties the table, then refills one slot");
    {
        ReferenceSlots slots(4);
        for (uint32_t n = 0; n < 4; ++n)
            slots.marked(slots.slotFor(n), n);
        CHECK(slots.held() == 4);
        slots.clear();
        CHECK(slots.held() == 0);
        CHECK(slots.cleanSlotBefore(10) == -1);
        // The keyframe (frame 4) is marked into its slot; the next loss finds it.
        slots.marked(slots.slotFor(4, true), 4);
        CHECK(slots.held() == 1);
        CHECK(slots.cleanSlotBefore(6) == slots.slotFor(4, true));
    }

    SECTION("ReferenceSlots — the driver's answer is what counts, not the request");
    {
        ReferenceSlots slots(4);
        // Out-of-range indices from a confused driver are ignored, never stored.
        slots.marked(4, 1);
        slots.marked(-1, 1);
        CHECK(slots.held() == 0);
        // The same slot re-marked keeps the newest frame.
        slots.marked(2, 10);
        slots.marked(2, 14);
        CHECK(slots.held() == 1);
        CHECK(slots.cleanSlotBefore(12) == -1); // 10 is gone, 14 is too new
        CHECK(slots.cleanSlotBefore(15) == 2);
    }
}
