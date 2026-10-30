/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * MWSR's zones on the wire (MwsrZoneBlock.h; plan B, upscaler-mwsr-b.md
 * §12 c) and the chunks that carry them (FrameChunks.h).
 *
 * What the frontend relies on (stream/MwsrZoneBlock.js, _onVideoChunk): the
 * block's length first, its codes two to a byte, high half first; the flag in
 * byte 8 of every chunk of the frame, and only when a block is there; the
 * chunks joined back give the block, then the bitstream, byte for byte; a
 * frame without a block is cut exactly as before.
 */

#include "../src/streaming/FrameChunks.h"
#include "../src/streaming/MwsrZoneBlock.h"

#include "test_framework.h"

#include <cstdint>
#include <vector>

namespace {

uint32_t read32(const std::byte* p)
{
    return (std::to_integer<uint32_t>(p[0]) << 24) | (std::to_integer<uint32_t>(p[1]) << 16) |
           (std::to_integer<uint32_t>(p[2]) << 8) | std::to_integer<uint32_t>(p[3]);
}

uint16_t read16(const std::byte* p)
{
    return static_cast<uint16_t>((std::to_integer<uint32_t>(p[0]) << 8) |
                                 std::to_integer<uint32_t>(p[1]));
}

// The chunks' payloads end to end, as the frontend joins them.
std::vector<uint8_t> joined(const std::vector<framechunks::Chunk>& chunks)
{
    std::vector<uint8_t> out;
    for (const auto& c : chunks)
        for (size_t i = framechunks::kHeaderSize; i < c.size(); ++i)
            out.push_back(std::to_integer<uint8_t>(c[i]));
    return out;
}

} // namespace

void run_mwsr_zone_block_tests()
{
    SECTION("MwsrZoneBlock — 32 x 18 codes in 295 bytes");
    {
        std::vector<uint8_t> codes(32 * 18);
        for (size_t i = 0; i < codes.size(); ++i)
            codes[i] = static_cast<uint8_t>(i % 12);
        mwsrzoneblock::Zones z;
        z.codes = codes.data();
        z.cols = 32;
        z.rows = 18;
        z.scale = 2560.0f / 1920.0f;
        const std::vector<uint8_t> b = mwsrzoneblock::write(z);
        CHECK_EQ(b.size(), size_t(295));
        CHECK_EQ(int(b[0]), 1); // 295 = 0x0127
        CHECK_EQ(int(b[1]), 0x27);
        CHECK_EQ(int(b[2]), int(mwsrzoneblock::kVersion));
        CHECK_EQ(int(b[3]), 32);
        CHECK_EQ(int(b[4]), 18);
        CHECK_EQ((int(b[5]) << 8) | b[6], 1333); // ×1.333 in thousandths
        // Zone 0 in the high half, zone 1 in the low one.
        CHECK_EQ(int(b[7]), 0x01);
        CHECK_EQ(int(b[8]), 0x23);
        bool same = true;
        for (size_t i = 0; i < codes.size(); ++i) {
            const uint8_t byte = b[mwsrzoneblock::kHeaderSize + i / 2];
            const uint8_t code = (i % 2 == 0) ? byte >> 4 : byte & 0x0F;
            same = same && code == codes[i];
        }
        CHECK(same);
    }

    SECTION("MwsrZoneBlock — an odd count leaves the last low half at zero");
    {
        const uint8_t codes[3] = {11, 5, 7};
        mwsrzoneblock::Zones z{codes, 3, 1, 2.0f};
        const std::vector<uint8_t> b = mwsrzoneblock::write(z);
        CHECK_EQ(b.size(), size_t(9));
        CHECK_EQ(int(b[7]), 0xB5);
        CHECK_EQ(int(b[8]), 0x70);
        CHECK_EQ((int(b[5]) << 8) | b[6], 2000);
    }

    SECTION("MwsrZoneBlock — nothing written that a reader could misread");
    {
        const uint8_t good[4] = {0, 1, 2, 3};
        const uint8_t wide[4] = {0, 16, 2, 3};
        CHECK(mwsrzoneblock::write({nullptr, 2, 2, 2.0f}).empty());
        CHECK(mwsrzoneblock::write({good, 0, 2, 2.0f}).empty());
        CHECK(mwsrzoneblock::write({good, 256, 1, 2.0f}).empty());
        CHECK(mwsrzoneblock::write({wide, 2, 2, 2.0f}).empty());
        CHECK(mwsrzoneblock::write({good, 2, 2, 0.0f}).empty());
        CHECK(mwsrzoneblock::write({good, 2, 2, 70.0f}).empty());
        CHECK(!mwsrzoneblock::write({good, 2, 2, 1.25f}).empty());
    }

    SECTION("FrameChunks — without a block, the frame cut as it always was");
    {
        std::vector<uint8_t> frame(40000);
        for (size_t i = 0; i < frame.size(); ++i)
            frame[i] = static_cast<uint8_t>(i * 7);
        const auto chunks = framechunks::cut(frame.data(), frame.size(), true, 77, 1234);
        CHECK_EQ(chunks.size(), size_t(3));
        bool header = true;
        for (size_t k = 0; k < chunks.size(); ++k) {
            const std::byte* h = chunks[k].data();
            header = header && read32(h) == 77 && read16(h + 4) == k && read16(h + 6) == 3 &&
                     std::to_integer<int>(h[8]) == 1 && read32(h + 13) == 1234 &&
                     read32(h + 9) == chunks[k].size() - framechunks::kHeaderSize;
        }
        CHECK(header);
        CHECK(joined(chunks) == frame);
        // A delta: byte 8 is 0, as an older frontend expects.
        const auto delta = framechunks::cut(frame.data(), 100, false, 78, 1);
        CHECK_EQ(std::to_integer<int>(delta[0][8]), 0);
    }

    SECTION("FrameChunks — the block ahead of the bitstream, flagged in every chunk");
    {
        std::vector<uint8_t> codes(32 * 18, 3);
        const std::vector<uint8_t> block =
            mwsrzoneblock::write({codes.data(), 32, 18, 2560.0f / 1280.0f});
        // Chunks of 1024 bytes: the first chunk holds the block and the start
        // of the bitstream, and the bitstream's end falls in the middle of
        // the last one.
        std::vector<uint8_t> frame(3000);
        for (size_t i = 0; i < frame.size(); ++i)
            frame[i] = static_cast<uint8_t>(255 - i % 251);
        for (bool key : {false, true}) {
            const auto chunks = framechunks::cut(frame.data(), frame.size(), key, 9, 5, 1024,
                                                 block.data(), block.size());
            CHECK_EQ(chunks.size(), size_t(4)); // 295 + 3000 = 3295 bytes
            bool flagged = true;
            for (const auto& c : chunks)
                flagged = flagged && std::to_integer<int>(c[8]) == (key ? 0x03 : 0x02) &&
                          read16(c.data() + 6) == 4;
            CHECK(flagged);
            std::vector<uint8_t> want = block;
            want.insert(want.end(), frame.begin(), frame.end());
            CHECK(joined(chunks) == want);
        }
    }

    SECTION("FrameChunks — a block longer than a chunk crosses into the next");
    {
        std::vector<uint8_t> side(2500, 0xAA);
        const uint8_t frame[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        const auto chunks =
            framechunks::cut(frame, sizeof(frame), false, 1, 1, 1024, side.data(), side.size());
        CHECK_EQ(chunks.size(), size_t(3));
        std::vector<uint8_t> want = side;
        want.insert(want.end(), frame, frame + sizeof(frame));
        CHECK(joined(chunks) == want);
        // No bitstream: nothing at all, block or not.
        CHECK(framechunks::cut(frame, 0, false, 1, 1, 1024, side.data(), side.size()).empty());
    }
}
