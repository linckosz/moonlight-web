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

#include "native_test_framework.h"

#include "hid_chrome_shapes.h"
#include "input/HidDescriptor.h"
#include "input/HidFfbJournal.h"
#include "input/HidPid.h"
#include "input/VirtualHid.h"
#include "mw/native/HidPassthrough.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace mw::native;
using namespace mw::native::input::hid;

namespace {

constexpr uint32_t P(uint16_t id)
{
    return 0x000F0000u | id;
}

/// A wheel with report id 1 and a vendor collection with ids 17 and 18, as the
/// G923 has: the PID block must start after 18.
std::vector<Collection> wheel()
{
    ReportItem x;
    x.usages = {0x10030};
    x.reportSize = 16;
    x.reportCount = 1;
    x.logicalMaximum = 65535;
    Report r;
    r.reportId = 1;
    r.items = {x};
    Collection joystick;
    joystick.usagePage = 0x01;
    joystick.usage = 0x04;
    joystick.inputReports = {r};

    ReportItem bytes;
    bytes.isArray = true;
    bytes.usages = {0xFF430602};
    bytes.reportSize = 8;
    bytes.reportCount = 19;
    bytes.logicalMaximum = 255;
    Report v17;
    v17.reportId = 17;
    v17.items = {bytes};
    Report v18 = v17;
    v18.reportId = 18;
    v18.items[0].reportCount = 63;
    Collection vendor;
    vendor.usagePage = 0xFF43;
    vendor.usage = 0x0602;
    vendor.inputReports = {v17, v18};
    vendor.outputReports = {v17, v18};
    return {joystick, vendor};
}

/// A PID wheel (Moza, Simucube…): the wheel above with a PID block of its own,
/// a Set Effect output report 2 in a logical collection, a Block Load feature
/// report 3, and a PID "Device Paused" bit sharing input report 1 with the axis.
std::vector<Collection> pidWheel()
{
    std::vector<Collection> c = wheel();
    ReportItem paused;
    paused.usages = {P(0x9F)};
    paused.reportSize = 1;
    paused.reportCount = 1;
    paused.logicalMaximum = 1;
    ReportItem pad;
    pad.isConstant = true;
    pad.reportSize = 7;
    pad.reportCount = 1;
    c[0].inputReports[0].items.push_back(paused);
    c[0].inputReports[0].items.push_back(pad);

    ReportItem index;
    index.usages = {P(0x22)};
    index.reportSize = 8;
    index.reportCount = 1;
    index.logicalMinimum = 1;
    index.logicalMaximum = 40;
    ReportItem x;
    x.usages = {0x10030};
    x.reportSize = 1;
    x.reportCount = 1;
    x.logicalMaximum = 1;
    Report setEffect;
    setEffect.reportId = 2;
    setEffect.items = {index};
    Report axes;
    axes.reportId = 2;
    axes.items = {x};
    Collection axesEnable;
    axesEnable.usagePage = 0x0F;
    axesEnable.usage = 0x55;
    axesEnable.type = Logical;
    axesEnable.outputReports = {axes};
    Collection effect;
    effect.usagePage = 0x0F;
    effect.usage = 0x21;
    effect.type = Logical;
    effect.outputReports = {setEffect};
    effect.children = {axesEnable};
    Report load;
    load.reportId = 3;
    load.items = {index};
    Collection blockLoad;
    blockLoad.usagePage = 0x0F;
    blockLoad.usage = 0x89;
    blockLoad.type = Logical;
    blockLoad.featureReports = {load};
    c[0].children = {effect, blockLoad};
    return c;
}

/// A wheel that carries its own PID block, as Moza's and Simucube's do: the
/// steering axis in a Physical collection, 16 buttons at the joystick's own
/// level (report 1), then the block in reports 2 to 15, the host's own layout
/// standing in for the USB PID spec's. Without the block when `withBlock` is
/// false: the same wheel's report 1 alone.
std::vector<uint8_t> nativePidWheel(bool withBlock)
{
    std::vector<uint8_t> d = {
        0x05, 0x01,                   // Usage Page (Generic Desktop)
        0x09, 0x04,                   // Usage (Joystick)
        0xA1, 0x01,                   // Collection (Application)
        0x85, 0x01,                   //   Report ID (1)
        0x09, 0x01,                   //   Usage (Pointer)
        0xA1, 0x00,                   //   Collection (Physical)
        0x09, 0x30,                   //     Usage (X)
        0x15, 0x00,                   //     Logical Minimum (0)
        0x27, 0xFF, 0xFF, 0x00, 0x00, //     Logical Maximum (65535)
        0x75, 0x10,                   //     Report Size (16)
        0x95, 0x01,                   //     Report Count (1)
        0x81, 0x02,                   //     Input (Data, Var, Abs)
        0xC0,                         //   End Collection
        0x05, 0x09,                   //   Usage Page (Button)
        0x19, 0x01,                   //   Usage Minimum (1)
        0x29, 0x10,                   //   Usage Maximum (16)
        0x25, 0x01,                   //   Logical Maximum (1)
        0x75, 0x01,                   //   Report Size (1)
        0x95, 0x10,                   //   Report Count (16)
        0x81, 0x02,                   //   Input (Data, Var, Abs)
    };
    if (withBlock) {
        const std::vector<uint8_t> block = pidBlock(2);
        d.insert(d.end(), block.begin(), block.end());
    }
    d.push_back(0xC0);
    return d;
}

std::vector<Field> inputReport(const Parsed& p, uint8_t id)
{
    std::vector<Field> out;
    for (const Field& f : normalise(p.fields))
        if (f.kind == Kind::Input && f.reportId == id) out.push_back(f);
    return out;
}

bool anyPid(const Parsed& p)
{
    for (const Field& f : p.fields) {
        if ((f.application >> 16) == 0x0F) return true;
        for (uint32_t u : f.usages)
            if ((u >> 16) == 0x0F) return true;
        if (f.range && (f.usageMinimum >> 16) == 0x0F) return true;
    }
    return false;
}

/// Writes reports the way pid.dll would, by usage, from the parsed block.
struct Writer
{
    const Parsed& p;

    const Field* variable(Kind kind, uint8_t id, uint32_t usage, uint32_t& offset) const
    {
        for (const Field& f : p.fields) {
            if (f.kind != kind || f.reportId != id || !f.isVariable() || f.isConstant()) continue;
            for (uint32_t i = 0; i < f.count && i < f.usages.size(); ++i)
                if (f.usages[i] == usage) {
                    offset = f.bitOffset + i * f.size;
                    return &f;
                }
        }
        return nullptr;
    }
    static void bits(std::vector<uint8_t>& r, uint32_t offset, uint32_t size, uint32_t v)
    {
        for (uint32_t b = 0; b < size; ++b) {
            const uint32_t bit = offset + b;
            if ((v >> b) & 1u)
                r[1 + bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
            else
                r[1 + bit / 8] &= static_cast<uint8_t>(~(1u << (bit % 8)));
        }
    }
    std::vector<uint8_t> report(Kind kind, uint8_t id) const
    {
        std::vector<uint8_t> r(reportBytes(p, kind, id), 0);
        if (!r.empty()) r[0] = id;
        return r;
    }
    void set(std::vector<uint8_t>& r, Kind kind, uint32_t usage, int32_t v) const
    {
        uint32_t offset = 0;
        const Field* f = variable(kind, r[0], usage, offset);
        CHECK(f != nullptr);
        if (f) bits(r, offset, f->size, static_cast<uint32_t>(v));
    }
    void select(std::vector<uint8_t>& r, Kind kind, uint32_t usage) const
    {
        for (const Field& f : p.fields) {
            if (f.kind != kind || f.reportId != r[0] || f.isVariable()) continue;
            auto it = std::find(f.usages.begin(), f.usages.end(), usage);
            if (it == f.usages.end()) continue;
            bits(r, f.bitOffset, f.size,
                 static_cast<uint32_t>(f.logicalMinimum + (it - f.usages.begin())));
            return;
        }
        CHECK(false);
    }
    int32_t get(const std::vector<uint8_t>& r, Kind kind, uint32_t usage) const
    {
        uint32_t offset = 0;
        const Field* f = variable(kind, r[0], usage, offset);
        if (!f) return -999;
        uint32_t v = 0;
        for (uint32_t b = 0; b < f->size; ++b)
            if ((r[1 + (offset + b) / 8] >> ((offset + b) % 8)) & 1u) v |= 1u << b;
        return static_cast<int32_t>(v);
    }
};

const HidFfb* find(const std::vector<HidFfb>& ops, const std::string& op)
{
    for (const HidFfb& o : ops)
        if (o.op == op) return &o;
    return nullptr;
}

int32_t field(const HidFfb& o, const std::string& name)
{
    for (const auto& [k, v] : o.fields)
        if (k == name) return v;
    return -999;
}

/// A backend that answers GET_FEATURE the way the Windows one does.
class FeatureFake : public input::IVirtualHid
{
public:
    explicit FeatureFake(FeatureFake** self) { *self = this; }
    bool create(const input::VirtualHidIdentity& id, std::string&) override
    {
        descriptor = id.descriptor;
        return true;
    }
    bool input(const uint8_t*, size_t) override { return true; }
    void destroy() override {}
    std::vector<uint8_t> getFeature(uint8_t id)
    {
        std::vector<uint8_t> r = m_onGetFeature ? m_onGetFeature(id) : std::vector<uint8_t>{};
        if (m_onRequest) m_onRequest(Request::GetFeature, id, {});
        return r;
    }
    void write(Request kind, const std::vector<uint8_t>& data)
    {
        if (m_onRequest) m_onRequest(kind, data[0], data);
    }
    std::vector<uint8_t> descriptor;
};

} // namespace

void run_hid_pid_tests()
{
    SECTION("HID PID — the block goes after the device's own report ids, never on id 0");
    {
        CHECK_EQ(static_cast<int>(pidFirstId(wheel())), 19);
        std::vector<Collection> unnumbered = wheel();
        unnumbered.resize(1);
        unnumbered[0].inputReports[0].reportId = 0;
        CHECK_EQ(static_cast<int>(pidFirstId(unnumbered)), 0);
    }

    SECTION("HID PID — the wheel with its block is a valid game device, block inside the joystick");
    {
        const std::vector<uint8_t> d = encodeWithPid(wheel(), 19);
        CHECK(validate(d).empty());
        CHECK(d.size() < kMaxDescriptorBytes);
        const Parsed p = parse(d);
        CHECK(p.ok);
        // Every PID report sits in the joystick's application collection.
        bool allInJoystick = true;
        int pidFields = 0;
        for (const Field& f : p.fields) {
            if (f.reportId < 19) continue;
            ++pidFields;
            allInJoystick = allInJoystick && f.application == 0x00010004;
        }
        CHECK(pidFields > 20);
        CHECK(allInJoystick);
        // The device's own reports are laid out as without the block.
        const Parsed plain = parse(encode(wheel()));
        std::vector<Field> own;
        for (const Field& f : p.fields)
            if (f.reportId < 19) own.push_back(f);
        CHECK(normalise(own) == normalise(plain.fields));
        for (int id = 19; id < 19 + kPidReports; ++id) {
            const bool any = reportBytes(p, Kind::Output, static_cast<uint8_t>(id)) ||
                             reportBytes(p, Kind::Feature, static_cast<uint8_t>(id)) ||
                             reportBytes(p, Kind::Input, static_cast<uint8_t>(id));
            CHECK(any);
        }
    }

    SECTION("HID PID — a new effect gets a block from Create New Effect + Block Load");
    {
        const std::vector<uint8_t> d = encodeWithPid(wheel(), 19);
        const Parsed p = parse(d);
        PidEngine e(p, 19);
        Writer w{p};
        const uint8_t create = 19 + 10, load = 19 + 11, pool = 19 + 12;
        CHECK(e.owns(create));
        CHECK(!e.owns(18));

        const std::vector<uint8_t> poolReport = e.getFeature(pool);
        CHECK(!poolReport.empty());
        CHECK_EQ(poolReport[0], pool);
        CHECK_EQ(w.get(poolReport, Kind::Feature, P(0x83)), kPidMaxEffects);
        CHECK_EQ(w.get(poolReport, Kind::Feature, P(0xA9)), 1);

        std::vector<HidFfb> ops;
        for (int expect = 1; expect <= 2; ++expect) {
            std::vector<uint8_t> c = w.report(Kind::Feature, create);
            w.select(c, Kind::Feature, P(0x26)); // constant force
            e.write(c, ops);
            const std::vector<uint8_t> l = e.getFeature(load);
            CHECK_EQ(w.get(l, Kind::Feature, P(0x22)), expect);
        }
        CHECK(ops.empty()); // creating is the host's business only

        // Freed, the first block is handed out again.
        std::vector<uint8_t> f = w.report(Kind::Output, 19 + 7);
        w.set(f, Kind::Output, P(0x22), 1);
        e.write(f, ops);
        CHECK(find(ops, "free") != nullptr);
        std::vector<uint8_t> c = w.report(Kind::Feature, create);
        w.select(c, Kind::Feature, P(0x40)); // spring
        e.write(c, ops);
        CHECK_EQ(w.get(e.getFeature(load), Kind::Feature, P(0x22)), 1);
    }

    SECTION("HID PID — a game's constant force comes out as effect, constant and start");
    {
        const std::vector<uint8_t> d = encodeWithPid(wheel(), 19);
        const Parsed p = parse(d);
        PidEngine e(p, 19);
        Writer w{p};
        std::vector<HidFfb> ops;

        std::vector<uint8_t> se = w.report(Kind::Output, 19);
        w.set(se, Kind::Output, P(0x22), 3);
        w.select(se, Kind::Output, P(0x26));
        w.set(se, Kind::Output, P(0x50), 0xFFFF); // infinite
        w.set(se, Kind::Output, P(0xA7), 20);
        w.set(se, Kind::Output, P(0x52), 255);
        w.set(se, Kind::Output, 0x000A0001, 9000); // 90°
        w.set(se, Kind::Output, 0x00010030, 1);    // X enabled
        w.set(se, Kind::Output, P(0x56), 1);
        e.write(se, ops);

        std::vector<uint8_t> cf = w.report(Kind::Output, 19 + 4);
        w.set(cf, Kind::Output, P(0x22), 3);
        w.set(cf, Kind::Output, P(0x70), static_cast<uint16_t>(-2500));
        e.write(cf, ops);

        std::vector<uint8_t> op = w.report(Kind::Output, 19 + 6);
        w.set(op, Kind::Output, P(0x22), 3);
        w.select(op, Kind::Output, P(0x79));
        w.set(op, Kind::Output, P(0x7C), 1);
        e.write(op, ops);

        const HidFfb* effect = find(ops, "effect");
        CHECK(effect != nullptr);
        if (effect) {
            CHECK_EQ(effect->effect, 3);
            CHECK_EQ(effect->kind, std::string("constant"));
            CHECK_EQ(field(*effect, "duration"), -1);
            CHECK_EQ(field(*effect, "delay"), 20);
            CHECK_EQ(field(*effect, "gain"), 255);
            CHECK_EQ(field(*effect, "direction"), 9000);
            CHECK_EQ(field(*effect, "directionEnable"), 1);
            CHECK_EQ(field(*effect, "axes"), 1);
        }
        const HidFfb* constant = find(ops, "constant");
        CHECK(constant != nullptr);
        if (constant) CHECK_EQ(field(*constant, "magnitude"), -2500);
        const HidFfb* start = find(ops, "start");
        CHECK(start != nullptr);
        if (start) {
            CHECK_EQ(start->effect, 3);
            CHECK_EQ(field(*start, "loops"), 1);
        }
    }

    SECTION("HID PID — conditions, gain and device control");
    {
        const std::vector<uint8_t> d = encodeWithPid(wheel(), 19);
        const Parsed p = parse(d);
        PidEngine e(p, 19);
        Writer w{p};
        std::vector<HidFfb> ops;

        std::vector<uint8_t> sc = w.report(Kind::Output, 19 + 2);
        w.set(sc, Kind::Output, P(0x22), 2);
        w.set(sc, Kind::Output, P(0x60), static_cast<uint16_t>(-100));
        w.set(sc, Kind::Output, P(0x61), 8000);
        w.set(sc, Kind::Output, P(0x62), 7000);
        w.set(sc, Kind::Output, P(0x63), 10000);
        w.set(sc, Kind::Output, P(0x64), 9000);
        w.set(sc, Kind::Output, P(0x65), 50);
        e.write(sc, ops);
        const HidFfb* c = find(ops, "condition");
        CHECK(c != nullptr);
        if (c) {
            CHECK_EQ(c->effect, 2);
            CHECK_EQ(field(*c, "offset"), -100);
            CHECK_EQ(field(*c, "positiveCoefficient"), 8000);
            CHECK_EQ(field(*c, "negativeCoefficient"), 7000);
            CHECK_EQ(field(*c, "positiveSaturation"), 10000);
            CHECK_EQ(field(*c, "negativeSaturation"), 9000);
            CHECK_EQ(field(*c, "deadBand"), 50);
        }

        std::vector<uint8_t> g = w.report(Kind::Output, 19 + 9);
        w.set(g, Kind::Output, P(0x7E), 128);
        e.write(g, ops);
        const HidFfb* gain = find(ops, "gain");
        CHECK(gain != nullptr);
        if (gain) CHECK_EQ(field(*gain, "gain"), 128);

        std::vector<uint8_t> dc = w.report(Kind::Output, 19 + 8);
        w.select(dc, Kind::Output, P(0x99));
        e.write(dc, ops);
        const HidFfb* control = find(ops, "control");
        CHECK(control != nullptr);
        if (control) CHECK_EQ(control->kind, std::string("stopAll"));

        // A block index out of range is ignored rather than passed on.
        const size_t before = ops.size();
        std::vector<uint8_t> bad = w.report(Kind::Output, 19 + 4);
        w.set(bad, Kind::Output, P(0x22), 0);
        e.write(bad, ops);
        CHECK_EQ(ops.size(), before);
    }

    SECTION("HID PID — through HidPassthrough: features answered, operations to the FFB sink only");
    {
        FeatureFake* dev = nullptr;
        std::vector<HidRequest> requests;
        std::vector<HidFfb> ffb;
        HidPassthrough hp([&](const HidRequest& r) { requests.push_back(r); },
                          [&] { return std::make_unique<FeatureFake>(&dev); });
        hp.setFfbSink([&](const HidFfb& o) { ffb.push_back(o); });
        HidDeviceInfo info;
        info.name = "Wheel";
        info.vendorId = 0x046d;
        info.productId = 0xc26e;
        info.collections = wheel();
        info.forceFeedback = true;
        CHECK(hp.attach(2, info).empty());
        CHECK(dev != nullptr);
        if (!dev) return;
        const Parsed p = parse(dev->descriptor);
        Writer w{p};

        std::vector<uint8_t> c = w.report(Kind::Feature, 19 + 10);
        w.select(c, Kind::Feature, P(0x31)); // sine
        dev->write(input::IVirtualHid::Request::SetFeature, c);
        const std::vector<uint8_t> load = dev->getFeature(19 + 11);
        CHECK_EQ(w.get(load, Kind::Feature, P(0x22)), 1);

        std::vector<uint8_t> sp = w.report(Kind::Output, 19 + 3);
        w.set(sp, Kind::Output, P(0x22), 1);
        w.set(sp, Kind::Output, P(0x70), 5000);
        w.set(sp, Kind::Output, P(0x72), 100);
        dev->write(input::IVirtualHid::Request::Output, sp);
        CHECK_EQ(ffb.size(), static_cast<size_t>(1));
        if (!ffb.empty()) {
            CHECK_EQ(ffb[0].slot, 2);
            CHECK_EQ(ffb[0].op, std::string("periodic"));
            CHECK_EQ(field(ffb[0], "magnitude"), 5000);
            CHECK_EQ(field(ffb[0], "period"), 100);
        }
        CHECK(requests.empty()); // the PID block never reaches the page raw

        // The device's own vendor reports still go to the page as before.
        std::vector<uint8_t> hidpp(20, 0);
        hidpp[0] = 17;
        dev->write(input::IVirtualHid::Request::Output, hidpp);
        CHECK_EQ(requests.size(), static_cast<size_t>(1));
    }

    SECTION("HID PID — a wheel's own PID block is taken out, its input layout kept");
    {
        std::vector<Collection> c = pidWheel();
        CHECK_EQ(static_cast<int>(pidFirstId(c)), 19);
        stripPid(c);
        const Parsed p = parse(encode(c));
        CHECK(p.ok);
        CHECK(!anyPid(p));
        // Report 1 still carries the axis and 8 bits after it, now padding.
        CHECK_EQ(reportBytes(p, Kind::Input, 1), static_cast<size_t>(1 + 3));
        CHECK_EQ(reportBytes(p, Kind::Output, 2), static_cast<size_t>(0));
        CHECK_EQ(reportBytes(p, Kind::Feature, 3), static_cast<size_t>(0));
        CHECK(c[0].children.empty());
        // The vendor collection is untouched.
        CHECK_EQ(c.size(), static_cast<size_t>(2));
        CHECK_EQ(reportBytes(p, Kind::Output, 18), static_cast<size_t>(64));
        // A wheel without PID comes out as it went in.
        std::vector<Collection> plain = wheel();
        stripPid(plain);
        CHECK(encode(plain) == encode(wheel()));
    }

    SECTION("HID PID — a PID wheel through HidPassthrough: the host's block, or none");
    {
        for (bool ffb : {false, true}) {
            FeatureFake* dev = nullptr;
            std::vector<HidRequest> requests;
            HidPassthrough hp([&](const HidRequest& r) { requests.push_back(r); },
                              [&] { return std::make_unique<FeatureFake>(&dev); });
            HidDeviceInfo info;
            info.collections = pidWheel();
            info.forceFeedback = ffb;
            CHECK(hp.attach(0, info).empty());
            CHECK(dev != nullptr);
            if (!dev) continue;
            const Parsed p = parse(dev->descriptor);
            CHECK(p.ok);
            CHECK_EQ(anyPid(p), ffb);
            // Never the wheel's own Set Effect (2) or Block Load (3).
            CHECK_EQ(reportBytes(p, Kind::Output, 2), static_cast<size_t>(0));
            CHECK_EQ(reportBytes(p, Kind::Feature, 3), static_cast<size_t>(0));
            if (ffb) {
                // The host's Block Load answers at once, from ids after the wheel's.
                const std::vector<uint8_t> load = dev->getFeature(19 + 11);
                CHECK(!load.empty());
                CHECK(requests.empty());
            }
        }
    }

    SECTION("HID PID — a native PID wheel from a Mac or from Windows: each field once, its block "
            "out, the host's in");
    {
        const std::vector<uint8_t> real = nativePidWheel(true);
        const std::vector<Field> report1 = inputReport(parse(nativePidWheel(false)), 1);
        for (bool mac : {true, false}) {
            std::vector<Collection> c = mac ? hid_chrome::onMac(real) : hid_chrome::onWindows(real);
            repairBounds(c);
            repairUsages(c);
            const uint8_t first = pidFirstId(c);
            CHECK_EQ(static_cast<int>(first), 16);
            stripPid(c);
            const std::vector<uint8_t> d = encodeWithPid(c, first);
            CHECK(validate(d).empty());
            const Parsed p = parse(d);
            CHECK(hid_chrome::windowsAccepts(p));
            // The wheel's own report 1, field for field.
            CHECK(inputReport(p, 1) == report1);
            // None of the wheel's own PID reports, the host's from 16.
            bool own = false;
            for (const Field& f : p.fields)
                own = own || (f.reportId >= 2 && f.reportId <= 15);
            CHECK(!own);
            CHECK(reportBytes(p, Kind::Feature, 16 + 11) > 0);

            // Through HidPassthrough, as the page sends it.
            FeatureFake* dev = nullptr;
            std::vector<HidRequest> requests;
            HidPassthrough hp([&](const HidRequest& r) { requests.push_back(r); },
                              [&] { return std::make_unique<FeatureFake>(&dev); });
            HidDeviceInfo info;
            info.name = "PID wheel";
            info.collections = mac ? hid_chrome::onMac(real) : hid_chrome::onWindows(real);
            info.forceFeedback = true;
            CHECK(hp.attach(0, info).empty());
            CHECK(dev != nullptr);
            if (!dev) continue;
            CHECK(dev->descriptor == d);
            CHECK(!dev->getFeature(16 + 11).empty()); // the host's Block Load answers
            CHECK(requests.empty());
        }
    }

    SECTION("HID PID — without force feedback asked, no block");
    {
        FeatureFake* dev = nullptr;
        HidPassthrough hp(nullptr, [&] { return std::make_unique<FeatureFake>(&dev); });
        HidDeviceInfo info;
        info.collections = wheel();
        CHECK(hp.attach(0, info).empty());
        CHECK(dev != nullptr);
        if (dev) CHECK(dev->descriptor == encode(wheel()));
    }
}

// ── Between two streams: the journal, the table, the parking ───────────────

namespace {

HidFfb ffbOp(const char* op, int effect, std::vector<std::pair<std::string, int32_t>> fields = {},
             const char* kind = "")
{
    HidFfb o;
    o.op = op;
    o.effect = effect;
    o.kind = kind;
    o.fields = std::move(fields);
    return o;
}

std::vector<std::string> namesOf(const std::vector<HidFfb>& ops)
{
    std::vector<std::string> out;
    for (const HidFfb& o : ops)
        out.push_back(o.op + (o.effect ? ":" + std::to_string(o.effect) : std::string()));
    return out;
}

/// What the parking's backends were asked, for the test to read.
struct ParkLog
{
    int created = 0;
    int destroyed = 0;
    int released = 0;
    int paused = 0;
    int resumed = 0;
    std::vector<std::vector<uint8_t>> inputs;
    std::vector<uint8_t> descriptor;
};

/// A backend that can cross processes, like the Windows one: a handle, a pump
/// that pauses and resumes, and release() apart from destroy() to tell them
/// apart here.
class ParkFake : public input::IVirtualHid
{
public:
    ParkFake(ParkLog& log, uint64_t handle = 0)
        : m_log(log)
        , m_handle(handle)
    {}
    ~ParkFake() override { destroy(); }
    bool create(const input::VirtualHidIdentity& id, std::string&) override
    {
        ++m_log.created;
        m_log.descriptor = id.descriptor;
        m_alive = true;
        return true;
    }
    bool input(const uint8_t* r, size_t n) override
    {
        m_log.inputs.emplace_back(r, r + n);
        return true;
    }
    void destroy() override
    {
        if (m_alive) ++m_log.destroyed;
        m_alive = false;
    }
    uint64_t nativeHandle() const override { return m_alive ? m_handle : 0; }
    void pause() override { ++m_log.paused; }
    void resume() override { ++m_log.resumed; }
    void release() override
    {
        if (m_alive) ++m_log.released;
        m_alive = false;
    }
    void adopted() { m_alive = true; }
    std::vector<uint8_t> getFeature(uint8_t id)
    {
        return m_onGetFeature ? m_onGetFeature(id) : std::vector<uint8_t>{};
    }
    void write(Request kind, const std::vector<uint8_t>& data)
    {
        if (m_onRequest) m_onRequest(kind, data[0], data);
    }

private:
    ParkLog& m_log;
    uint64_t m_handle = 0;
    bool m_alive = false;
};

HidDeviceInfo ffbWheel(const char* name = "Wheel")
{
    HidDeviceInfo info;
    info.name = name;
    info.vendorId = 0x046d;
    info.productId = 0xc26e;
    info.collections = wheel();
    info.forceFeedback = true;
    return info;
}

/// The game, through pid.dll: a constant force in its own block, started.
void playConstant(ParkFake& dev, const Writer& w, int magnitude, bool create)
{
    if (create) {
        std::vector<uint8_t> c = w.report(Kind::Feature, 19 + 10);
        w.select(c, Kind::Feature, P(0x26));
        dev.write(input::IVirtualHid::Request::SetFeature, c);
        std::vector<uint8_t> se = w.report(Kind::Output, 19);
        w.set(se, Kind::Output, P(0x22), 1);
        w.select(se, Kind::Output, P(0x26));
        w.set(se, Kind::Output, P(0x50), 0xFFFF);
        w.set(se, Kind::Output, P(0x52), 255);
        dev.write(input::IVirtualHid::Request::Output, se);
    }
    std::vector<uint8_t> cf = w.report(Kind::Output, 19 + 4);
    w.set(cf, Kind::Output, P(0x22), 1);
    w.set(cf, Kind::Output, P(0x70), static_cast<uint16_t>(magnitude));
    dev.write(input::IVirtualHid::Request::Output, cf);
    if (create) {
        std::vector<uint8_t> op = w.report(Kind::Output, 19 + 6);
        w.set(op, Kind::Output, P(0x22), 1);
        w.select(op, Kind::Output, P(0x79));
        w.set(op, Kind::Output, P(0x7C), 1);
        dev.write(input::IVirtualHid::Request::Output, op);
    }
}

} // namespace

void run_hid_parking_tests()
{
    SECTION("HID parking — the journal rebuilds the force feedback a game left behind");
    {
        FfbJournal j;
        j.record(ffbOp("gain", 0, {{"gain", 200}}), 0);
        j.record(ffbOp("effect", 1, {{"duration", -1}}, "constant"), 0);
        j.record(ffbOp("constant", 1, {{"magnitude", -2500}}), 0);
        j.record(ffbOp("constant", 1, {{"magnitude", -3000}}), 0); // the latest wins
        j.record(ffbOp("start", 1, {{"loops", 1}}), 0);
        j.record(ffbOp("effect", 2, {{"duration", -1}}, "spring"), 0);
        j.record(ffbOp("condition", 2, {{"axis", 1}, {"offset", 5}}), 0);
        j.record(ffbOp("condition", 2, {{"axis", 0}, {"offset", 7}}), 0);
        j.record(ffbOp("effect", 3, {{"duration", 100}, {"delay", 0}}, "sine"), 0);
        j.record(ffbOp("periodic", 3, {{"magnitude", 4000}}), 0);
        j.record(ffbOp("start", 3, {{"loops", 1}}), 0);
        j.record(ffbOp("effect", 4, {{"duration", -1}}, "ramp"), 0);
        j.record(ffbOp("free", 4), 0);
        CHECK_EQ(j.effects(), static_cast<size_t>(3));

        // At 50 ms the sine still plays; at 1 s it is over and stays stopped.
        CHECK(namesOf(j.replay(50)) ==
              (std::vector<std::string>{"gain", "effect:1", "constant:1", "start:1", "effect:2",
                                        "condition:2", "condition:2", "effect:3", "periodic:3",
                                        "start:3"}));
        const std::vector<HidFfb> later = j.replay(1000);
        CHECK(namesOf(later) ==
              (std::vector<std::string>{"gain", "effect:1", "constant:1", "start:1", "effect:2",
                                        "condition:2", "condition:2", "effect:3", "periodic:3"}));
        CHECK_EQ(field(later[2], "magnitude"), -3000);
        CHECK_EQ(field(later[5], "axis"), 0); // conditions by axis
        CHECK_EQ(field(later[6], "axis"), 1);

        // Solo stops the others; stopAll every one; pause comes last.
        j.record(ffbOp("solo", 2, {{"loops", 255}}), 2000);
        std::vector<std::string> n = namesOf(j.replay(1'000'000));
        CHECK(std::count(n.begin(), n.end(), "start:1") == 0);
        CHECK(std::count(n.begin(), n.end(), "start:2") == 1);
        j.record(ffbOp("control", 0, {}, "pause"), 2000);
        CHECK_EQ(j.replay(2000).back().kind, std::string("pause"));
        j.record(ffbOp("control", 0, {}, "stopAll"), 2000);
        n = namesOf(j.replay(2000));
        CHECK(std::none_of(n.begin(), n.end(),
                           [](const std::string& s) { return s.rfind("start", 0) == 0; }));
        // Reset forgets every effect, not the device's gain.
        j.record(ffbOp("control", 0, {}, "reset"), 2000);
        CHECK(namesOf(j.replay(2000)) == (std::vector<std::string>{"gain"}));
    }

    SECTION("HID parking — the PID table goes along: blocks the game holds stay given");
    {
        const Parsed p = parse(encodeWithPid(wheel(), 19));
        Writer w{p};
        PidEngine a(p, 19);
        std::vector<HidFfb> ops;
        for (int i = 0; i < 2; ++i) {
            std::vector<uint8_t> c = w.report(Kind::Feature, 19 + 10);
            w.select(c, Kind::Feature, P(0x26));
            a.write(c, ops);
        }
        PidEngine b(p, 19);
        CHECK(!b.restore({1, 2, 3}));
        CHECK(b.restore(a.save()));
        CHECK_EQ(w.get(b.getFeature(19 + 11), Kind::Feature, P(0x22)), 2);
        std::vector<uint8_t> c = w.report(Kind::Feature, 19 + 10);
        w.select(c, Kind::Feature, P(0x40));
        b.write(c, ops);
        CHECK_EQ(w.get(b.getFeature(19 + 11), Kind::Feature, P(0x22)), 3);
    }

    SECTION("HID parking — a stream that ends leaves its wheel at rest, the next takes it back");
    {
        ParkLog log;
        int64_t now = 0;
        HidParking parking([&] { return now; });
        ParkFake* live = nullptr;
        auto factory = [&] {
            auto d = std::make_unique<ParkFake>(log);
            live = d.get();
            return d;
        };
        std::vector<HidFfb> first;
        {
            HidPassthrough a(nullptr, factory, [&] { return now; });
            a.setParking(&parking);
            a.setFfbSink([&](const HidFfb& o) { first.push_back(o); });
            CHECK(a.attach(0, ffbWheel()).empty());
            const uint8_t steer[2] = {0x00, 0x10}; // well left of the middle
            a.input(0, 1, 10, steer, 2);
            playConstant(*live, Writer{parse(log.descriptor)}, -2500, true);
            CHECK(!first.empty());
        }
        // The stream is gone, the device is not: at rest, still answering.
        CHECK_EQ(log.created, 1);
        CHECK_EQ(log.destroyed, 0);
        CHECK_EQ(parking.size(), static_cast<size_t>(1));
        CHECK(!log.inputs.empty() &&
              log.inputs.back() == (std::vector<uint8_t>{1, 0x00, 0x80})); // wheel centred
        const Writer w{parse(log.descriptor)};
        ParkFake* parked = live;
        const size_t heard = first.size();
        playConstant(*parked, w, 1234, false); // the game goes on alone
        CHECK_EQ(first.size(), heard);
        CHECK_EQ(w.get(parked->getFeature(19 + 11), Kind::Feature, P(0x22)), 1);

        std::vector<HidFfb> second;
        {
            HidPassthrough b(nullptr, factory, [&] { return now; });
            b.setParking(&parking);
            b.setFfbSink([&](const HidFfb& o) { second.push_back(o); });
            CHECK(b.attach(3, ffbWheel()).empty());
            CHECK_EQ(log.created, 1); // taken back, not made again
            CHECK_EQ(parking.size(), static_cast<size_t>(0));
            playConstant(*parked, w, 4321, false); // before the page knows: journal only
            CHECK(second.empty());
            b.attached(3);
            CHECK(namesOf(second) ==
                  (std::vector<std::string>{"effect:1", "constant:1", "start:1"}));
            if (second.size() == 3) {
                CHECK_EQ(second[1].slot, 3);
                CHECK_EQ(field(second[1], "magnitude"), 4321);
            }
            b.attached(3); // once only
            CHECK_EQ(second.size(), static_cast<size_t>(3));
            playConstant(*parked, w, 99, false);
            CHECK_EQ(second.size(), static_cast<size_t>(4));
            // The new page counts from 0: not late after the old page's 10.
            const size_t inputs = log.inputs.size();
            const uint8_t steer[2] = {0x00, 0x20};
            b.input(3, 1, 0, steer, 2);
            CHECK_EQ(log.inputs.size(), inputs + 1);
        }
        CHECK_EQ(parking.size(), static_cast<size_t>(1));
        CHECK_EQ(log.destroyed, 0);
    }

    SECTION("HID parking — another device, an explicit detach and time let a wheel go");
    {
        ParkLog log;
        int64_t now = 0;
        HidParking parking([&] { return now; });
        auto factory = [&] { return std::make_unique<ParkFake>(log); };
        {
            HidPassthrough a(nullptr, factory, [&] { return now; });
            a.setParking(&parking);
            CHECK(a.attach(0, ffbWheel()).empty());
        }
        {
            HidPassthrough b(nullptr, factory, [&] { return now; });
            b.setParking(&parking);
            CHECK(b.attach(0, ffbWheel("Another wheel")).empty());
            CHECK_EQ(log.created, 2); // not the parked one
            b.detach(0);              // unplugged: gone at once, never parked
            CHECK_EQ(log.destroyed, 1);
        }
        CHECK_EQ(parking.size(), static_cast<size_t>(1));
        parking.expire(HidParking::kKeepMs - 1);
        CHECK_EQ(parking.size(), static_cast<size_t>(1));
        parking.expire(HidParking::kKeepMs);
        CHECK_EQ(parking.size(), static_cast<size_t>(0));
        CHECK_EQ(log.released, 1);
        // Without a parking, the old rule: removed with the stream.
        {
            HidPassthrough c(nullptr, factory, [&] { return now; });
            CHECK(c.attach(0, ffbWheel()).empty());
        }
        CHECK_EQ(log.destroyed, 2);
    }

    SECTION("HID parking — handed to another process: paused, then let go or answering again");
    {
        ParkLog log;
        int64_t now = 0;
        HidParking server([&] { return now; });
        ParkFake* live = nullptr;
        {
            HidPassthrough a(
                nullptr,
                [&] {
                    auto d = std::make_unique<ParkFake>(log, 42);
                    live = d.get();
                    return d;
                },
                [&] { return now; });
            a.setParking(&server);
            CHECK(a.attach(0, ffbWheel()).empty());
            playConstant(*live, Writer{parse(log.descriptor)}, -2500, true);
        }
        now = 60'000;
        std::vector<HidHandedDevice> out;
        const uint64_t batch = server.handOut(out);
        CHECK(batch != 0);
        CHECK_EQ(out.size(), static_cast<size_t>(1));
        CHECK_EQ(log.paused, 1);
        if (out.size() != 1) return;
        const HidParkedState& st = out[0].state;
        CHECK_EQ(out[0].handle, uint64_t{42});
        CHECK(st.descriptor == log.descriptor);
        CHECK_EQ(static_cast<int>(st.pidFirstId), 19);
        CHECK_EQ(st.pidState.size(), static_cast<size_t>(2 + kPidMaxEffects));
        CHECK(namesOf(st.effects) ==
              (std::vector<std::string>{"effect:1", "constant:1", "start:1"}));
        CHECK_EQ(st.keepMs, HidParking::kKeepMs - 60'000);
        CHECK_EQ(st.name, std::string("Wheel"));

        std::vector<HidHandedDevice> again;
        CHECK_EQ(server.handOut(again), uint64_t{0}); // already out
        server.settle(batch, false);                  // the worker never came
        CHECK_EQ(log.resumed, 1);
        const uint64_t batch2 = server.handOut(again);
        CHECK(batch2 != 0 && batch2 != batch);
        server.settle(batch2, true); // the worker holds its copy now
        CHECK_EQ(server.size(), static_cast<size_t>(0));
        CHECK_EQ(log.released, 1);

        // The worker's side: the copy, the state, a page that takes it back.
        HidParking worker([&] { return now; });
        auto copy = std::make_unique<ParkFake>(log, 42);
        copy->adopted();
        ParkFake* adopted = copy.get();
        CHECK(worker.takeIn(std::move(copy), st, HidParking::kUnclaimedMs).empty());
        CHECK_EQ(log.resumed, 2);
        std::vector<HidFfb> ops;
        HidPassthrough b(
            nullptr, [&] { return std::make_unique<ParkFake>(log); }, [&] { return now; });
        b.setParking(&worker);
        b.setFfbSink([&](const HidFfb& o) { ops.push_back(o); });
        CHECK(b.attach(1, ffbWheel()).empty());
        CHECK_EQ(log.created, 1);
        b.attached(1);
        CHECK(namesOf(ops) == (std::vector<std::string>{"effect:1", "constant:1", "start:1"}));
        // The table came along: the game's next effect gets block 2.
        const Writer w{parse(st.descriptor)};
        std::vector<uint8_t> c = w.report(Kind::Feature, 19 + 10);
        w.select(c, Kind::Feature, P(0x40));
        adopted->write(input::IVirtualHid::Request::SetFeature, c);
        CHECK_EQ(w.get(adopted->getFeature(19 + 11), Kind::Feature, P(0x22)), 2);

        // A device no page takes in a stream's process goes after kUnclaimedMs.
        auto other = std::make_unique<ParkFake>(log, 43);
        other->adopted();
        HidParkedState st2 = st;
        st2.name = "Spare wheel";
        CHECK(worker.takeIn(std::move(other), st2, HidParking::kUnclaimedMs).empty());
        worker.expire(now + HidParking::kUnclaimedMs - 1);
        CHECK_EQ(worker.size(), static_cast<size_t>(1));
        worker.expire(now + HidParking::kUnclaimedMs);
        CHECK_EQ(worker.size(), static_cast<size_t>(0));
        CHECK_EQ(log.released, 2);

        // Not a game device: refused, and let go.
        auto bad = std::make_unique<ParkFake>(log, 44);
        bad->adopted();
        HidParkedState st3 = st;
        st3.descriptor = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0xC0}; // a keyboard
        CHECK(!worker.takeIn(std::move(bad), st3, 0).empty());
        CHECK_EQ(log.released, 3);
    }
}
