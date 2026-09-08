/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/linux/XkbTextMap.h"

#include <cstdio>
#include <string>
#include <vector>

#if defined(__linux__)
#include <linux/input-event-codes.h>
#endif

using namespace mw::native::input;

// Typing text on a Linux host.
//
// A phone's soft keyboard has no key positions to send, so it sends TEXT — and a
// Linux host cannot inject a character, only a key position, which the
// compositor then reads through the user's own layout. The two halves of getting
// that right are checked in two different ways here: the plumbing (decoding the
// browser's UTF-8, reading the system's layout out of /etc/default/keyboard) is
// plain data and is tested everywhere, and the keymap itself needs a real
// libxkbcommon and is exercised only where there is one.

namespace {

/// The layouts below are named in the test rather than taken from the host, so
/// the same expectations hold on a machine set to any layout at all.
void checkPlumbing()
{
    SECTION("Text input — decoding what the browser sent");

    std::vector<char32_t> out;
    decodeUtf8("abc", out);
    CHECK_EQ(out.size(), size_t(3));
    CHECK_EQ(out[0], U'a');
    CHECK_EQ(out[2], U'c');

    // Two, three and four bytes: é, € and an emoji — the last being where a walk
    // written for two-byte sequences falls over. The bytes are spelled out rather
    // than written as characters so this file stays pure ASCII and the test does
    // not depend on how a compiler reads its own source encoding.
    out.clear();
    decodeUtf8("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x91\x8D", out);
    CHECK_EQ(out.size(), size_t(3));
    CHECK_EQ(out[0], char32_t(0x00E9)); // é
    CHECK_EQ(out[1], char32_t(0x20AC)); // €
    CHECK_EQ(out[2], char32_t(0x1F44D));

    // Malformed input costs the bad character and nothing else. A password field
    // is exactly where a run off the end of the buffer would be found.
    out.clear();
    decodeUtf8("a\xFF"
               "b",
               out); // 0xFF is not a lead byte anywhere in UTF-8
    CHECK_EQ(out.size(), size_t(2));
    CHECK_EQ(out[0], U'a');
    CHECK_EQ(out[1], U'b');

    out.clear();
    decodeUtf8("a\xC3", out); // truncated two-byte sequence at the very end
    CHECK_EQ(out.size(), size_t(1));
    CHECK_EQ(out[0], U'a');

    out.clear();
    decodeUtf8("\xE9\x80", out); // a three-byte lead with one continuation byte
    CHECK_EQ(out.size(), size_t(0));

    SECTION("Text input — reading the host's layout");

    // Ubuntu's own file, verbatim from the machine this was written against.
    XkbNames names;
    CHECK(parseKeyboardConfig("# KEYBOARD CONFIGURATION FILE\n"
                              "\n"
                              "# Consult the keyboard(5) manual page.\n"
                              "\n"
                              "XKBMODEL=\"pc105\"\n"
                              "XKBLAYOUT=\"fr\"\n"
                              "XKBVARIANT=\"azerty\"\n"
                              "XKBOPTIONS=\"\"\n"
                              "\n"
                              "BACKSPACE=\"guess\"\n",
                              names));
    CHECK_EQ(names.layout, std::string("fr"));
    CHECK_EQ(names.variant, std::string("azerty"));
    CHECK_EQ(names.model, std::string("pc105"));
    CHECK_EQ(names.options, std::string());

    // Unquoted values and trailing CR — the file is a shell fragment, and a host
    // that has been edited by hand is still a host.
    XkbNames bare;
    CHECK(parseKeyboardConfig("XKBLAYOUT=de\r\nXKBVARIANT=nodeadkeys\r\n", bare));
    CHECK_EQ(bare.layout, std::string("de"));
    CHECK_EQ(bare.variant, std::string("nodeadkeys"));

    // No layout named is not a layout of "": the caller must fall back rather
    // than compile a keymap for nothing.
    XkbNames empty;
    CHECK(!parseKeyboardConfig("# nothing here\nBACKSPACE=\"guess\"\n", empty));
}

#if defined(__linux__)

/// What the character @p cp is typed with, printed the way a person can check it
/// against their own keyboard.
void describe(const XkbTextMap& map, char32_t cp, const char* label)
{
    XkbStroke strokes[2];
    int count = 0;
    if (!map.find(cp, strokes, count)) {
        std::fprintf(stderr, "      %-6s not typable on this layout\n", label);
        return;
    }
    std::fprintf(stderr, "      %-6s", label);
    for (int i = 0; i < count; ++i)
        std::fprintf(stderr, " %skey %u%s%s", i ? "then " : "", unsigned(strokes[i].code),
                     strokes[i].mods[0] ? "+mod" : "", strokes[i].mods[1] ? "+mod" : "");
    std::fprintf(stderr, "\n");
}

void checkRealKeymap()
{
    SECTION("Text input — the host's own keymap (Linux)");

    XkbTextMap map;
    if (!map.open()) {
        std::fprintf(stderr, "  [SKIP] no libxkbcommon here — text injection is off on such a "
                             "host, which is the documented behaviour\n");
        return;
    }

    std::fprintf(stderr, "      layout: %s, %zu characters reachable\n", map.description().c_str(),
                 map.size());

    // Whatever the layout, a keyboard types the alphabet, both cases, the digits
    // and a space. A map that cannot is a map that was built against nothing.
    XkbStroke strokes[2];
    int count = 0;
    for (char32_t c = U'a'; c <= U'z'; ++c)
        CHECK(map.find(c, strokes, count));
    for (char32_t c = U'A'; c <= U'Z'; ++c)
        CHECK(map.find(c, strokes, count));
    for (char32_t c = U'0'; c <= U'9'; ++c)
        CHECK(map.find(c, strokes, count));
    CHECK(map.find(U' ', strokes, count));

    // A capital is the lower-case key plus a modifier, and the SAME key: the one
    // relation that holds on every Latin layout, and the one that breaks if the
    // level-to-modifier translation is wrong — which is how a password field
    // ends up full of lower-case letters.
    XkbStroke lower[2];
    XkbStroke upper[2];
    int lowerCount = 0;
    int upperCount = 0;
    CHECK(map.find(U'a', lower, lowerCount));
    CHECK(map.find(U'A', upper, upperCount));
    CHECK_EQ(lowerCount, 1);
    CHECK_EQ(upperCount, 1);
    CHECK_EQ(lower[0].code, upper[0].code);
    CHECK_EQ(lower[0].mods[0], uint16_t(0));
    CHECK_EQ(upper[0].mods[0], uint16_t(KEY_LEFTSHIFT));

    // Never CapsLock, never Control, never a plain Alt: those are the modifiers
    // we refuse to hold on somebody's desktop.
    for (char32_t c = U'!'; c < char32_t(0x7F); ++c) {
        if (!map.find(c, strokes, count)) continue;
        for (int i = 0; i < count; ++i)
            for (uint16_t mod : strokes[i].mods)
                CHECK(mod == 0 || mod == KEY_LEFTSHIFT || mod == KEY_RIGHTALT);
    }

    // An accent the keyboard has no key for is built the way a person builds it:
    // the dead key, then the letter. Two strokes, and the second is the plain
    // letter's own — checked only where the layout HAS the dead key, since a
    // bare `us` layout genuinely cannot type these and must say so.
    XkbStroke accented[2];
    int accentedCount = 0;
    if (map.find(char32_t(0x00EA), accented, accentedCount)) { // ê
        if (accentedCount == 2) {
            XkbStroke plain[2];
            int plainCount = 0;
            CHECK(map.find(U'e', plain, plainCount));
            CHECK_EQ(accented[1].code, plain[0].code);
            CHECK(accented[0].code != 0);
        } else {
            // A layout with a key for it outright — that is an answer too.
            CHECK_EQ(accentedCount, 1);
        }
    }

    // Printed, not asserted: on the AZERTY host this was written against, 'a' is
    // the key a US keyboard calls Q (evdev 16) and '1' takes Shift. A machine set
    // to another layout will print other numbers, and that is the point.
    describe(map, U'a', "a");
    describe(map, U'q', "q");
    describe(map, U'1', "1");
    describe(map, U'@', "@");
    describe(map, char32_t(0x00E9), "e-acute");
    describe(map, char32_t(0x00EA), "e-circ");
}

#endif // __linux__

} // namespace

void run_xkb_text_map_tests()
{
    checkPlumbing();
#if defined(__linux__)
    checkRealKeymap();
#endif
}
