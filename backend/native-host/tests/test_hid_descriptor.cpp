/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 *
 * The HID passthrough's descriptor rebuilt from WebHID's collections: the
 * encoder, the parser it is checked with, and the gate on what the host agrees
 * to create.
 *
 * The reference is a real device. A radio running EdgeTX in its "Classic" USB
 * joystick layout (EdgeTX 2.10 and the Classic mode since) declares exactly the
 * descriptor below; the bytes are what the radio sends at enumeration. The
 * collections are written the way Chrome shows that radio to a page. If the
 * rebuilt descriptor lays out one bit differently, a game reads a stick from
 * the wrong place: this is the check that catches it.
 */
#include "native_test_framework.h"

#include "hid_chrome_shapes.h"
#include "input/HidDescriptor.h"

#include <cstdint>
#include <vector>

using namespace mw::native::input::hid;

namespace {

// EdgeTX Classic USB joystick: 24 buttons, 8 axes of 0..2047, 19 bytes, no
// report id. Comments name the items as a descriptor tool would.
const std::vector<uint8_t> kEdgeTxClassic = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Button)
    0x19, 0x01,       //     Usage Minimum (1)
    0x29, 0x18,       //     Usage Maximum (24)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x95, 0x18,       //     Report Count (24)
    0x75, 0x01,       //     Report Size (1)
    0x81, 0x02,       //     Input (Data, Var, Abs)
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x09, 0x32,       //     Usage (Z)
    0x09, 0x33,       //     Usage (Rx)
    0x09, 0x34,       //     Usage (Ry)
    0x09, 0x35,       //     Usage (Rz)
    0x09, 0x36,       //     Usage (Slider)
    0x09, 0x37,       //     Usage (Dial)
    0x16, 0x00, 0x00, //     Logical Minimum (0)
    0x26, 0xFF, 0x07, //     Logical Maximum (2047)
    0x75, 0x10,       //     Report Size (16)
    0x95, 0x08,       //     Report Count (8)
    0x81, 0x02,       //     Input (Data, Var, Abs)
    0xC0,             //   End Collection
    0xC0,             // End Collection
};

// A joystick as many declare it: X and Y in a Physical collection, then the
// buttons and a throttle at the application's own level. Report 1, 5 bytes.
const std::vector<uint8_t> kJoystickPointerFirst = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x04,       // Usage (Joystick)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x15, 0x00,       //     Logical Minimum (0)
    0x26, 0xFF, 0x03, //     Logical Maximum (1023)
    0x75, 0x0A,       //     Report Size (10)
    0x95, 0x02,       //     Report Count (2)
    0x81, 0x02,       //     Input (Data, Var, Abs)
    0xC0,             //   End Collection
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x0C,       //   Usage Maximum (12)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x0C,       //   Report Count (12)
    0x81, 0x02,       //   Input (Data, Var, Abs)
    0x05, 0x02,       //   Usage Page (Simulation Controls)
    0x09, 0xBB,       //   Usage (Throttle)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x02,       //   Input (Data, Var, Abs)
    0xC0,             // End Collection
};

// A game pad with a button declared as a range of one usage, then seven data
// bits with no usage at all. No report id, 3 bytes.
const std::vector<uint8_t> kOneUsageRange = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x05, // Usage (Game Pad)
    0xA1, 0x01, // Collection (Application)
    0x09, 0x30, //   Usage (X)
    0x09, 0x31, //   Usage (Y)
    0x15, 0x81, //   Logical Minimum (-127)
    0x25, 0x7F, //   Logical Maximum (127)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x02, //   Report Count (2)
    0x81, 0x02, //   Input (Data, Var, Abs)
    0x05, 0x09, //   Usage Page (Button)
    0x19, 0x01, //   Usage Minimum (1)
    0x29, 0x01, //   Usage Maximum (1)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data, Var, Abs)
    0x75, 0x07, //   Report Size (7)
    0x81, 0x02, //   Input (Data, Var, Abs), no usage
    0xC0,       // End Collection
};

ReportItem buttons(uint32_t first, uint32_t last)
{
    ReportItem it;
    it.isRange = true;
    it.usageMinimum = first;
    it.usageMaximum = last;
    it.reportSize = 1;
    it.reportCount = static_cast<uint16_t>(last - first + 1);
    it.logicalMinimum = 0;
    it.logicalMaximum = 1;
    return it;
}

ReportItem axes(std::vector<uint32_t> usages, uint16_t size, int32_t lmin, int32_t lmax)
{
    ReportItem it;
    it.reportCount = static_cast<uint16_t>(usages.size());
    it.usages = std::move(usages);
    it.reportSize = size;
    it.logicalMinimum = lmin;
    it.logicalMaximum = lmax;
    return it;
}

/// The radio as Chrome shows it: the application collection holds nothing
/// itself, its usage-less physical child holds the one input report.
std::vector<Collection> tx12AsChromeShowsIt(bool axesOneByOne)
{
    Report r;
    r.items.push_back(buttons(0x90001, 0x90018));
    const std::vector<uint32_t> xyz = {0x10030, 0x10031, 0x10032, 0x10033,
                                       0x10034, 0x10035, 0x10036, 0x10037};
    if (axesOneByOne) {
        // What a reconstruction from Windows' preparsed data can give: one
        // value cap, so one item, per axis.
        for (uint32_t u : xyz)
            r.items.push_back(axes({u}, 16, 0, 2047));
    } else {
        r.items.push_back(axes(xyz, 16, 0, 2047));
    }
    Collection physical;
    physical.type = Physical;
    physical.inputReports.push_back(r);
    Collection app;
    app.usagePage = 0x01;
    app.usage = 0x05;
    app.type = Application;
    app.children.push_back(physical);
    return {app};
}

/// A wheel with what the radio lacks: report ids, a signed centred axis with a
/// unit, a hat with a null state, padding, an output and a feature report.
std::vector<Collection> syntheticWheel()
{
    Report in;
    in.reportId = 1;
    ReportItem wheel = axes({0x10030}, 16, -32768, 32767);
    wheel.unitSystem = UnitSystem::EnglishRotation;
    wheel.unitFactors[0] = 1; // degrees
    wheel.unitExponent = -2;
    wheel.physicalMinimum = -45000;
    wheel.physicalMaximum = 45000;
    in.items.push_back(wheel);
    in.items.push_back(axes({0x200C4, 0x200C5, 0x200C6}, 8, 0, 255)); // accelerator, brake, clutch
    ReportItem hat = axes({0x10039}, 4, 0, 7);
    hat.hasNull = true;
    hat.physicalMaximum = 315;
    hat.unitSystem = UnitSystem::EnglishRotation;
    hat.unitFactors[0] = 1;
    in.items.push_back(hat);
    ReportItem pad;
    pad.isConstant = true;
    pad.reportSize = 4;
    pad.reportCount = 1;
    in.items.push_back(pad);
    in.items.push_back(buttons(0x90001, 0x90019));
    ReportItem pad2 = pad;
    pad2.reportSize = 7;
    in.items.push_back(pad2);

    Report out;
    out.reportId = 2;
    ReportItem cmd = axes({0xFF000001}, 8, 0, 255);
    cmd.reportCount = 7;
    out.items.push_back(cmd);

    Report feat;
    feat.reportId = 3;
    ReportItem range = axes({0x10030}, 16, 0, 65535);
    range.isVolatile = true;
    feat.items.push_back(range);

    Collection app;
    app.usagePage = 0x01;
    app.usage = 0x04;
    app.type = Application;
    app.inputReports.push_back(in);
    app.outputReports.push_back(out);
    app.featureReports.push_back(feat);
    return {app};
}

/// The G923 in PC mode as Chrome 154 showed it on Windows (H0 survey, 03/10):
/// a joystick with a hat, 23 buttons, a 16-bit wheel and three 8-bit pedals,
/// beside two HID++ collections of Logitech's (vendor page 0xFF43). Bounds as
/// Chrome gives them there: buttons at 0..0, the vendor arrays at 255..0.
std::vector<Collection> g923AsChromeShowsIt()
{
    Report in;
    in.reportId = 1;
    ReportItem hat = axes({0x10039}, 4, 0, 7);
    hat.hasNull = true;
    hat.physicalMaximum = 315;
    hat.unitSystem = UnitSystem::EnglishRotation;
    hat.unitFactors[0] = 1;
    in.items.push_back(hat);
    ReportItem b = buttons(0x90001, 0x90017);
    b.logicalMaximum = 0;
    in.items.push_back(b);
    ReportItem pad;
    pad.isConstant = true;
    pad.isArray = true;
    pad.reportSize = 5;
    pad.reportCount = 1;
    in.items.push_back(pad);
    in.items.push_back(axes({0x10030}, 16, 0, 65535));
    for (uint32_t u : {0x10031u, 0x10032u, 0x10035u})
        in.items.push_back(axes({u}, 8, 0, 255));
    ReportItem vendorBits = buttons(0xFF000001, 0xFF000003);
    vendorBits.logicalMaximum = 0;
    in.items.push_back(vendorBits);
    in.items.push_back(pad);

    Collection joystick;
    joystick.usagePage = 0x01;
    joystick.usage = 0x04;
    joystick.inputReports.push_back(in);

    auto hidpp = [](uint16_t usage, uint8_t id, uint16_t bytes) {
        ReportItem it = axes({0xFF430000u | (usage & 0xFF)}, 8, 255, 0);
        it.isArray = true;
        it.reportCount = bytes;
        Collection c;
        c.usagePage = 0xFF43;
        c.usage = usage;
        c.inputReports = {Report{id, {it}}};
        c.outputReports = {Report{id, {it}}};
        return c;
    };
    return {joystick, hidpp(0x0602, 17, 19), hidpp(0x0604, 18, 63)};
}

bool sameLayout(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    const Parsed pa = parse(a);
    const Parsed pb = parse(b);
    return pa.ok && pb.ok && normalise(pa.fields) == normalise(pb.fields);
}

} // namespace

void run_hid_descriptor_tests()
{
    SECTION("HID descriptor — the radio's own descriptor parses");
    {
        const Parsed p = parse(kEdgeTxClassic);
        CHECK(p.ok);
        CHECK_EQ(p.fields.size(), static_cast<size_t>(2));
        CHECK_EQ(reportBytes(p, Kind::Input, 0), static_cast<size_t>(19));
        CHECK_EQ(p.collections.size(), static_cast<size_t>(2));
        CHECK_EQ(p.collections[0].usage, 0x10005u);
        CHECK(p.fields[1].bitOffset == 24 && p.fields[1].logicalMaximum == 2047);
        CHECK(validate(kEdgeTxClassic).empty());
    }

    SECTION("HID descriptor — rebuilt from Chrome's collections, bit for bit the radio's layout");
    {
        const std::vector<uint8_t> rebuilt = encode(tx12AsChromeShowsIt(false));
        CHECK(sameLayout(rebuilt, kEdgeTxClassic));
        CHECK(validate(rebuilt).empty());
        CHECK(rebuilt.size() <= kEdgeTxClassic.size() + 8);
        // Axes reconstructed one item each give the same layout.
        CHECK(sameLayout(encode(tx12AsChromeShowsIt(true)), kEdgeTxClassic));
    }

    SECTION("HID descriptor — Chrome on a Mac lists each field in every parent: written once");
    {
        for (const std::vector<uint8_t>* real : {&kEdgeTxClassic, &kJoystickPointerFirst}) {
            for (bool mac : {true, false}) {
                std::vector<Collection> c =
                    mac ? hid_chrome::onMac(*real) : hid_chrome::onWindows(*real);
                CHECK_EQ(childRuns(c[0]).has_value(), mac);
                repairBounds(c);
                repairUsages(c);
                const std::vector<uint8_t> rebuilt = encode(c);
                CHECK(sameLayout(rebuilt, *real));
                CHECK(validate(rebuilt).empty());
                CHECK(hid_chrome::windowsAccepts(parse(rebuilt)));
            }
        }
        // X and Y sat in a Physical collection before the application's own
        // buttons: the report keeps that order, in its 5 bytes and its id.
        std::vector<Collection> c = hid_chrome::onMac(kJoystickPointerFirst);
        CHECK_EQ(reportBytes(parse(encode(c)), Kind::Input, 1), static_cast<size_t>(6));
        // A tree written by hand lists each collection's own items: its
        // children are written as before.
        CHECK(!childRuns(tx12AsChromeShowsIt(false)[0]).has_value());
    }

    SECTION("HID descriptor — a one-usage range keeps its usage, a data item without one is "
            "padding");
    {
        std::vector<Collection> c = hid_chrome::onMac(kOneUsageRange);
        // What Chrome hands the page: the button as no range, with no usage.
        const ReportItem& button = c[0].inputReports[0].items[1];
        CHECK(!button.isRange && button.usages.empty());
        CHECK_EQ(button.usageMinimum, 0x90001u);
        CHECK(!hid_chrome::windowsAccepts(parse(encode(c))));
        repairBounds(c);
        repairUsages(c);
        const Parsed p = parse(encode(c));
        CHECK(hid_chrome::windowsAccepts(p));
        CHECK_EQ(reportBytes(p, Kind::Input, 0), static_cast<size_t>(3));
        bool buttonOne = false;
        for (const Field& f : normalise(p.fields))
            buttonOne =
                buttonOne || (f.bitOffset == 16 && f.usages == std::vector<uint32_t>{0x90001});
        CHECK(buttonOne);
    }

    SECTION("HID descriptor — a wheel round-trips: ids, signed axis, unit, null hat, padding, "
            "output, feature");
    {
        const std::vector<uint8_t> d = encode(syntheticWheel());
        const Parsed p = parse(d);
        CHECK(p.ok);
        CHECK(validate(d).empty());
        CHECK_EQ(p.reportIds.size(), static_cast<size_t>(3));
        // 16 + 3*8 + 4 + 4 + 25 + 7 = 80 bits = 10 bytes, plus the id.
        CHECK_EQ(reportBytes(p, Kind::Input, 1), static_cast<size_t>(11));
        CHECK_EQ(reportBytes(p, Kind::Output, 2), static_cast<size_t>(8));
        CHECK_EQ(reportBytes(p, Kind::Feature, 3), static_cast<size_t>(3));
        const Field& wheel = p.fields[0];
        CHECK(wheel.logicalMinimum == -32768 && wheel.logicalMaximum == 32767);
        CHECK(wheel.physicalMinimum == -45000 && wheel.physicalMaximum == 45000);
        CHECK_EQ(wheel.unit, 0x14u);
        CHECK_EQ(static_cast<int>(wheel.unitExponent), -2);
        const Field& hat = p.fields[2];
        CHECK(hat.flags & 0x40);
        CHECK_EQ(hat.bitOffset, 40u);
        CHECK(p.fields[3].isConstant());
        CHECK_EQ(p.fields.back().kind, Kind::Feature);
        CHECK(p.fields.back().flags & 0x80);
        CHECK_EQ(p.fields.back().logicalMaximum, 65535);
        CHECK(p.fields[6].kind == Kind::Output &&
              p.fields[6].usages == std::vector<uint32_t>{0xFF000001}); // vendor page, extended
        // Writing the same collections twice gives the same bytes.
        CHECK(encode(syntheticWheel()) == d);
    }

    SECTION("HID descriptor — the G923 as Chrome shows it on Windows: bounds repaired, "
            "HID++ beside the joystick accepted");
    {
        std::vector<Collection> wheel = g923AsChromeShowsIt();
        repairBounds(wheel);
        const std::vector<uint8_t> d = encode(wheel);
        const Parsed p = parse(d);
        CHECK(p.ok);
        CHECK(validate(d).empty());
        // The sizes the survey received: 10 bytes, 19 and 63, each after its id.
        CHECK_EQ(reportBytes(p, Kind::Input, 1), static_cast<size_t>(11));
        CHECK_EQ(reportBytes(p, Kind::Input, 17), static_cast<size_t>(20));
        CHECK_EQ(reportBytes(p, Kind::Output, 18), static_cast<size_t>(64));
        int buttons = 0, vendorArrays = 0;
        for (const Field& f : p.fields) {
            if (f.range && f.size == 1) {
                ++buttons;
                CHECK(f.logicalMinimum == 0 && f.logicalMaximum == 1);
            }
            if (!f.isVariable() && !f.isConstant()) {
                ++vendorArrays;
                CHECK(f.logicalMinimum == 0 && f.logicalMaximum == 255);
            }
        }
        CHECK_EQ(buttons, 2);      // the 23 buttons and the 3 vendor bits
        CHECK_EQ(vendorArrays, 4); // reports 17 and 18, in and out
        // The axes Chrome gave right are left alone.
        CHECK(p.fields[3].logicalMaximum == 65535 && p.fields[4].logicalMaximum == 255);
        CHECK_EQ(p.fields[0].logicalMaximum, 7);
        // Unrepaired, the encoder writes what it is given: 255..0 parses back reversed.
        CHECK(encode(g923AsChromeShowsIt()) != d);
    }

    SECTION("HID descriptor — a vendor interface alone is no game device");
    {
        std::vector<Collection> wheel = g923AsChromeShowsIt();
        repairBounds(wheel);
        const std::vector<Collection> hidppOnly(wheel.begin() + 1, wheel.end());
        CHECK(!validate(encode(hidppOnly)).empty());
        // The G923's second interface: page 0xFFFD, usage 0xFD01, 63 bytes.
        std::vector<Collection> fffd = {hidppOnly[1]};
        fffd[0].usagePage = 0xFFFD;
        fffd[0].usage = 0xFD01;
        CHECK(!validate(encode(fffd)).empty());
        // Beside a joystick, a vendor collection hiding an X axis is refused.
        std::vector<Collection> hiding = wheel;
        hiding[1].inputReports[0].items.push_back(axes({0x10030}, 8, 0, 255));
        CHECK(!validate(encode(hiding)).empty());
    }

    SECTION(
        "HID descriptor — the rest report: sticks centred, pedals and throttles at their rest end");
    {
        // The G923's report 1 as received: hat, 23 buttons, wheel X, pedals Y Z
        // Rz at 255 when released, 3 vendor bits.
        std::vector<Collection> wheel = g923AsChromeShowsIt();
        repairBounds(wheel);
        const Parsed p = parse(encode(wheel));
        const std::vector<uint8_t> first = {0x08, 0x00, 0x00, 0x00, 0x29,
                                            0x80, 0xff, 0xff, 0xff, 0x05};
        // Held: hat right (2), buttons 1 and 9, full lock left, throttle down,
        // brake half, clutch released, a vendor bit set.
        const std::vector<uint8_t> held = {0x12, 0x01, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x80, 0xff, 0x05};
        const std::vector<uint8_t> rest = restReport(p, 1, held, first);
        CHECK_EQ(rest[0] & 0x0f, 8);                                  // hat null
        CHECK_EQ(rest[0] >> 4, 0);                                    // buttons 1 to 4 up
        CHECK(rest[1] == 0 && rest[2] == 0 && (rest[3] & 0x07) == 0); // the others up
        CHECK(rest[4] == 0x00 && rest[5] == 0x80);                    // wheel at 32768
        CHECK(rest[6] == 0xff && rest[7] == 0xff &&
              rest[8] == 0xff);      // pedals released, as first seen
        CHECK_EQ(rest[9] & 0x07, 5); // vendor bits as they were

        // The TX12: eight 0..2047 axes. First report: throttle (Z here) at its
        // bottom, the other sticks near the middle, a switch (Slider) at its top.
        std::vector<uint8_t> tx(19, 0);
        auto put = [](std::vector<uint8_t>& r, int axis, int v) {
            r[3 + 2 * axis] = static_cast<uint8_t>(v & 0xff);
            r[4 + 2 * axis] = static_cast<uint8_t>(v >> 8);
        };
        auto get = [](const std::vector<uint8_t>& r, int axis) {
            return r[3 + 2 * axis] | (r[4 + 2 * axis] << 8);
        };
        std::vector<uint8_t> txFirst = tx;
        for (int a = 0; a < 8; ++a)
            put(txFirst, a, 1020);
        put(txFirst, 2, 0);    // throttle down
        put(txFirst, 6, 2047); // a switch up
        std::vector<uint8_t> txHeld = tx;
        for (int a = 0; a < 8; ++a)
            put(txHeld, a, 1700);
        txHeld[0] = 0xff; // buttons 1-8 down
        const Parsed tp = parse(kEdgeTxClassic);
        const std::vector<uint8_t> txRest = restReport(tp, 0, txHeld, txFirst);
        CHECK_EQ(txRest[0], 0);
        CHECK_EQ(get(txRest, 0), 1024); // centred
        CHECK_EQ(get(txRest, 2), 0);    // throttle back down
        CHECK_EQ(get(txRest, 6), 2047); // the switch where it rests
        CHECK_EQ(get(txRest, 7), 0);    // Dial, mid-way at first: its usage says the low end
    }

    SECTION("HID descriptor — bounds are written so a parser reads them back");
    {
        Report r;
        r.items.push_back(axes({0x10030}, 8, 0, 255));
        r.items.push_back(axes({0x10031}, 16, 0, 65535));
        r.items.push_back(axes({0x10032}, 8, -127, 127));
        Collection app;
        app.usagePage = 0x01;
        app.usage = 0x04;
        app.inputReports.push_back(r);
        const Parsed p = parse(encode({app}));
        CHECK(p.ok);
        CHECK_EQ(p.fields[0].logicalMaximum, 255);
        CHECK_EQ(p.fields[1].logicalMaximum, 65535);
        CHECK(p.fields[2].logicalMinimum == -127 && p.fields[2].logicalMaximum == 127);
    }

    SECTION("HID descriptor — what the host refuses to create");
    {
        // A keyboard.
        const std::vector<uint8_t> keyboard = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
                                               0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
                                               0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
        CHECK(!validate(keyboard).empty());
        // A joystick hiding a media key.
        Report r;
        r.items.push_back(axes({0x10030}, 8, 0, 255));
        r.items.push_back(axes({0xC00E9}, 1, 0, 1));
        Collection app;
        app.usagePage = 0x01;
        app.usage = 0x04;
        app.inputReports.push_back(r);
        CHECK(!validate(encode({app})).empty());
        // A mouse at top level.
        Collection mouse = app;
        mouse.usage = 0x02;
        mouse.inputReports = {Report{0, {axes({0x10030}, 8, -127, 127)}}};
        CHECK(!validate(encode({mouse})).empty());
        // Report ids that mix zero and non-zero.
        Collection mixed = app;
        mixed.inputReports = {Report{0, {axes({0x10030}, 8, 0, 255)}},
                              Report{5, {axes({0x10031}, 8, 0, 255)}}};
        CHECK(!validate(encode({mixed})).empty());
        // Output only.
        Collection silent = app;
        silent.inputReports.clear();
        silent.outputReports = {Report{0, {axes({0x10030}, 8, 0, 255)}}};
        CHECK(!validate(encode({silent})).empty());
        // Too large.
        CHECK(!validate(std::vector<uint8_t>(5000, 0x00)).empty());
        // A report over 1024 bytes.
        Collection huge = app;
        ReportItem big = axes({0x10030}, 8, 0, 255);
        big.reportCount = 2000;
        huge.inputReports = {Report{0, {big}}};
        CHECK(!validate(encode({huge})).empty());
    }

    SECTION("HID descriptor — malformed bytes are refused, not guessed at");
    {
        CHECK(!parse(std::vector<uint8_t>{0x05}).ok);                   // truncated
        CHECK(!parse(std::vector<uint8_t>{0x05, 0x01, 0xA1, 0x01}).ok); // unclosed
        CHECK(!parse(std::vector<uint8_t>{0xC0}).ok);                   // stray End Collection
        CHECK(!parse(std::vector<uint8_t>{0xFE, 0x01, 0x00, 0x00}).ok); // long item
        CHECK(!parse(std::vector<uint8_t>{0x85, 0x00}).ok);             // report id 0
        CHECK(!validate(std::vector<uint8_t>{0x05, 0x01, 0x09, 0x04}).empty()); // no collection
    }
}
