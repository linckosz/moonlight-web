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

#include "input/HidPid.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

namespace mw::native::input::hid {

namespace {

// Usage pages.
constexpr uint32_t kPageDesktop = 0x01;
constexpr uint32_t kPageOrdinal = 0x0A;
constexpr uint32_t kPagePid = 0x0F;

constexpr uint32_t pid(uint32_t id)
{
    return (kPagePid << 16) | id;
}

// PID 1.0 usages (USB "Device Class Definition for Physical Interface
// Devices", §5), the ones the block uses.
enum : uint16_t
{
    uSetEffect = 0x21,
    uEffectBlockIndex = 0x22,
    uParameterBlockOffset = 0x23,
    uEffectType = 0x25,
    uEtConstant = 0x26,
    uEtRamp = 0x27,
    uEtSquare = 0x30,
    uEtSine = 0x31,
    uEtTriangle = 0x32,
    uEtSawUp = 0x33,
    uEtSawDown = 0x34,
    uEtSpring = 0x40,
    uEtDamper = 0x41,
    uEtInertia = 0x42,
    uEtFriction = 0x43,
    uDuration = 0x50,
    uSamplePeriod = 0x51,
    uGain = 0x52,
    uTriggerButton = 0x53,
    uTriggerRepeat = 0x54,
    uAxesEnable = 0x55,
    uDirectionEnable = 0x56,
    uDirection = 0x57,
    uSetEnvelope = 0x5A,
    uAttackLevel = 0x5B,
    uAttackTime = 0x5C,
    uFadeLevel = 0x5D,
    uFadeTime = 0x5E,
    uSetCondition = 0x5F,
    uCpOffset = 0x60,
    uPositiveCoefficient = 0x61,
    uNegativeCoefficient = 0x62,
    uPositiveSaturation = 0x63,
    uNegativeSaturation = 0x64,
    uDeadBand = 0x65,
    uSetPeriodic = 0x6E,
    uOffset = 0x6F,
    uMagnitude = 0x70,
    uPhase = 0x71,
    uPeriod = 0x72,
    uSetConstant = 0x73,
    uSetRamp = 0x74,
    uRampStart = 0x75,
    uRampEnd = 0x76,
    uEffectOperationReport = 0x77,
    uEffectOperation = 0x78,
    uOpStart = 0x79,
    uOpSolo = 0x7A,
    uOpStop = 0x7B,
    uLoopCount = 0x7C,
    uDeviceGainReport = 0x7D,
    uDeviceGain = 0x7E,
    uPoolReport = 0x7F,
    uRamPoolSize = 0x80,
    uSimultaneousMax = 0x83,
    uBlockLoadReport = 0x89,
    uBlockLoadStatus = 0x8B,
    uLoadSuccess = 0x8C,
    uLoadFull = 0x8D,
    uLoadError = 0x8E,
    uBlockFree = 0x90,
    uStateReport = 0x92,
    uEffectPlaying = 0x94,
    uDeviceControlReport = 0x95,
    uDeviceControl = 0x96,
    uDcEnable = 0x97,
    uDcDisable = 0x98,
    uDcStopAll = 0x99,
    uDcReset = 0x9A,
    uDcPause = 0x9B,
    uDcContinue = 0x9C,
    uDevicePaused = 0x9F,
    uActuatorsEnabled = 0xA0,
    uSafetySwitch = 0xA4,
    uOverrideSwitch = 0xA5,
    uActuatorPower = 0xA6,
    uStartDelay = 0xA7,
    uDeviceManagedPool = 0xA9,
    uSharedParameterBlocks = 0xAA,
    uCreateNewEffect = 0xAB,
    uRamPoolAvailable = 0xAC,
};

// Report ids, as offsets from the block's first id.
enum : uint8_t
{
    rSetEffect,
    rSetEnvelope,
    rSetCondition,
    rSetPeriodic,
    rSetConstant,
    rSetRamp,
    rEffectOperation,
    rBlockFree,
    rDeviceControl,
    rDeviceGain,
    rCreateNewEffect, // feature
    rBlockLoad,       // feature
    rPool,            // feature
    rState,           // input
};
static_assert(rState + 1 == kPidReports, "kPidReports counts the block's reports");

// Effect types in the order of the Effect Type arrays (logical 1..11), with
// the name HidFfb gives them.
struct EffectType
{
    uint16_t usage;
    const char* name;
};
constexpr EffectType kEffectTypes[] = {
    {uEtConstant, "constant"},    {uEtRamp, "ramp"},
    {uEtSquare, "square"},        {uEtSine, "sine"},
    {uEtTriangle, "triangle"},    {uEtSawUp, "sawtoothUp"},
    {uEtSawDown, "sawtoothDown"}, {uEtSpring, "spring"},
    {uEtDamper, "damper"},        {uEtInertia, "inertia"},
    {uEtFriction, "friction"},
};

// Units: SI linear time in seconds with exponent -3 (milliseconds); English
// rotation in degrees with exponent -2 (hundredths of a degree).
constexpr uint32_t kUnitTime = 0x1003;
constexpr uint32_t kUnitDegrees = 0x0014;
constexpr int8_t kExpMilli = -3;
constexpr int8_t kExpCenti = -2;

// Main item flags.
constexpr uint8_t kData_Var = 0x02;
constexpr uint8_t kData_Array = 0x00;
constexpr uint8_t kConst = 0x03; // Constant, Variable: padding

/// A HID 1.11 short-item writer, with the globals it last wrote so it can
/// leave out the unchanged ones.
class Block
{
public:
    explicit Block(std::vector<uint8_t>& out)
        : m_out(out)
    {}

    void page(uint32_t p) { item(1, 0x0, p, false); }
    void usage(uint16_t u) { item(2, 0x0, u, false); }
    void reportId(uint8_t id) { item(1, 0x8, id, false); }
    void logical(int32_t lo, int32_t hi)
    {
        item(1, 0x1, static_cast<uint32_t>(lo), true);
        item(1, 0x2, static_cast<uint32_t>(hi), true);
    }
    void physical(int32_t lo, int32_t hi)
    {
        item(1, 0x3, static_cast<uint32_t>(lo), true);
        item(1, 0x4, static_cast<uint32_t>(hi), true);
    }
    void unit(uint32_t u, int8_t exp)
    {
        item(1, 0x6, u, false);
        item(1, 0x5, static_cast<uint32_t>(exp) & 0xF, false);
    }
    void size(uint16_t bits, uint16_t count)
    {
        item(1, 0x7, bits, false);
        item(1, 0x9, count, false);
    }
    void logicalCollection() { item(0, 0xA, 0x02, false); }
    void end() { m_out.push_back(0xC0); }
    void output(uint8_t flags) { item(0, 0x9, flags, false); }
    void feature(uint8_t flags) { item(0, 0xB, flags, false); }
    void input(uint8_t flags) { item(0, 0x8, flags, false); }

    /// One variable field: its usages, then its bounds and size.
    void field(std::initializer_list<uint16_t> usages, int32_t lo, int32_t hi, uint16_t bits,
               char kind)
    {
        for (uint16_t u : usages)
            usage(u);
        logical(lo, hi);
        size(bits, static_cast<uint16_t>(usages.size()));
        main(kind, kData_Var);
    }
    /// One array field selecting one of `usages` (logical 1..n), in a logical
    /// collection named `collectionUsage`.
    void array(uint16_t collectionUsage, std::initializer_list<uint16_t> usages, char kind)
    {
        usage(collectionUsage);
        logicalCollection();
        for (uint16_t u : usages)
            usage(u);
        logical(1, static_cast<int32_t>(usages.size()));
        size(8, 1);
        main(kind, kData_Array);
        end();
    }
    void padding(uint16_t bits, char kind)
    {
        size(bits, 1);
        main(kind, kConst);
    }
    void main(char kind, uint8_t flags)
    {
        if (kind == 'o')
            output(flags);
        else if (kind == 'f')
            feature(flags);
        else
            input(flags);
    }
    void effectBlockIndex(char kind) { field({uEffectBlockIndex}, 1, kPidMaxEffects, 8, kind); }

private:
    // Globals are written each time: the block is small, and a global left over
    // from the device's own items can never leak into it.
    void item(uint8_t type, uint8_t tag, uint32_t v, bool isSigned)
    {
        int bytes;
        if (isSigned) {
            const int32_t s = static_cast<int32_t>(v);
            bytes = (s >= -128 && s <= 127) ? 1 : (s >= -32768 && s <= 32767) ? 2 : 4;
        } else {
            bytes = v <= 0xFF ? 1 : v <= 0xFFFF ? 2 : 4;
        }
        const uint8_t sizeCode = bytes == 4 ? 3 : static_cast<uint8_t>(bytes);
        m_out.push_back(static_cast<uint8_t>((tag << 4) | (type << 2) | sizeCode));
        for (int i = 0; i < bytes; ++i)
            m_out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    std::vector<uint8_t>& m_out;
};

bool isGameApplication(const Collection& c)
{
    if (c.type != Application) return false;
    if (c.usagePage == 0x02) return true;
    return c.usagePage == 0x01 && (c.usage == 0x04 || c.usage == 0x05 || c.usage == 0x08);
}

void maxId(const Collection& c, uint8_t& id, bool& zero)
{
    for (const auto* list : {&c.inputReports, &c.outputReports, &c.featureReports})
        for (const Report& r : *list) {
            if (r.reportId == 0) zero = true;
            id = std::max(id, r.reportId);
        }
    for (const Collection& ch : c.children)
        maxId(ch, id, zero);
}

uint32_t bitsOf(const std::vector<uint8_t>& r, uint32_t offset, uint32_t size)
{
    uint32_t v = 0;
    for (uint32_t b = 0; b < size; ++b) {
        const uint32_t bit = offset + b;
        if (bit / 8 >= r.size()) break;
        if ((r[bit / 8] >> (bit % 8)) & 1u) v |= 1u << b;
    }
    return v;
}

void setBitsOf(std::vector<uint8_t>& r, uint32_t offset, uint32_t size, uint32_t value)
{
    for (uint32_t b = 0; b < size; ++b) {
        const uint32_t bit = offset + b;
        if (bit / 8 >= r.size()) return;
        const uint8_t mask = static_cast<uint8_t>(1u << (bit % 8));
        if ((value >> b) & 1u)
            r[bit / 8] |= mask;
        else
            r[bit / 8] &= static_cast<uint8_t>(~mask);
    }
}

bool onPidPage(uint32_t usage)
{
    return (usage >> 16) == kPagePid;
}

bool isPidItem(const ReportItem& it)
{
    if (it.isRange) return onPidPage(it.usageMinimum);
    return std::any_of(it.usages.begin(), it.usages.end(), onPidPage);
}

std::vector<Report>& listOf(Collection& c, Kind kind)
{
    return kind == Kind::Input    ? c.inputReports
           : kind == Kind::Output ? c.outputReports
                                  : c.featureReports;
}

ReportItem paddingFor(const ReportItem& it)
{
    ReportItem pad;
    pad.isConstant = true;
    pad.reportSize = it.reportSize;
    pad.reportCount = it.reportCount;
    return pad;
}

void padPid(Collection& c, bool inPid)
{
    inPid = inPid || c.usagePage == kPagePid;
    // Where the children's items sit in c's lists, read before anything changes.
    const std::optional<std::vector<ChildRun>> runs = childRuns(c);
    for (auto* list : {&c.inputReports, &c.outputReports, &c.featureReports})
        for (Report& r : *list)
            for (ReportItem& it : r.items)
                if (!it.isConstant && (inPid || isPidItem(it))) it = paddingFor(it);
    for (Collection& ch : c.children)
        padPid(ch, inPid);
    // c's copy of a child's items takes the child's padding: inside a PID
    // collection, a field without a PID usage (Axes Enable's X and Y,
    // Direction's ordinals) is padding too.
    if (runs)
        for (const ChildRun& run : *runs) {
            const std::vector<ReportItem>& from =
                listOf(c.children[run.child], run.kind)[run.childReport].items;
            std::vector<ReportItem>& to = listOf(c, run.kind)[run.report].items;
            std::copy(from.begin(), from.end(), to.begin() + static_cast<std::ptrdiff_t>(run.at));
        }
}

// (kind, id) of a report: kind 0 input, 1 output, 2 feature.
using ReportKey = std::pair<int, uint8_t>;

// Output and feature reports with a PID field are the PID spec's own reports,
// whole: Chrome on Windows sends no nesting, so their X, Y and ordinals are
// known for PID this way only.
void pidReports(const Collection& c, bool inPid, std::vector<ReportKey>& keys)
{
    inPid = inPid || c.usagePage == kPagePid;
    int kind = 1;
    for (const auto* list : {&c.outputReports, &c.featureReports}) {
        for (const Report& r : *list)
            if (inPid || std::any_of(r.items.begin(), r.items.end(), isPidItem))
                keys.emplace_back(kind, r.reportId);
        ++kind;
    }
    for (const Collection& ch : c.children)
        pidReports(ch, inPid, keys);
}

void padReports(Collection& c, const std::vector<ReportKey>& keys)
{
    int kind = 1;
    for (auto* list : {&c.outputReports, &c.featureReports}) {
        for (Report& r : *list)
            if (std::find(keys.begin(), keys.end(), ReportKey(kind, r.reportId)) != keys.end())
                for (ReportItem& it : r.items)
                    if (!it.isConstant) it = paddingFor(it);
        ++kind;
    }
    for (Collection& ch : c.children)
        padReports(ch, keys);
}

// Every report that still carries a field of the device's own.
void usedReports(const Collection& c, std::vector<ReportKey>& used)
{
    int kind = 0;
    for (const auto* list : {&c.inputReports, &c.outputReports, &c.featureReports}) {
        for (const Report& r : *list)
            if (std::any_of(r.items.begin(), r.items.end(),
                            [](const ReportItem& it) { return !it.isConstant; }))
                used.emplace_back(kind, r.reportId);
        ++kind;
    }
    for (const Collection& ch : c.children)
        usedReports(ch, used);
}

// Drops padding-only reports nobody else shares, then the collections left
// with nothing; true when `c` itself is left with nothing.
bool prune(Collection& c, const std::vector<ReportKey>& used)
{
    int kind = 0;
    bool empty = true;
    for (auto* list : {&c.inputReports, &c.outputReports, &c.featureReports}) {
        list->erase(std::remove_if(list->begin(), list->end(),
                                   [&](const Report& r) {
                                       return std::find(used.begin(), used.end(),
                                                        ReportKey(kind, r.reportId)) == used.end();
                                   }),
                    list->end());
        empty = empty && list->empty();
        ++kind;
    }
    c.children.erase(std::remove_if(c.children.begin(), c.children.end(),
                                    [&](Collection& ch) { return prune(ch, used); }),
                     c.children.end());
    return empty && c.children.empty();
}

} // namespace

void stripPid(std::vector<Collection>& collections)
{
    std::vector<ReportKey> whole;
    for (const Collection& c : collections)
        pidReports(c, false, whole);
    for (Collection& c : collections) {
        padPid(c, false);
        padReports(c, whole);
    }
    std::vector<ReportKey> used;
    for (const Collection& c : collections)
        usedReports(c, used);
    collections.erase(std::remove_if(collections.begin(), collections.end(),
                                     [&](Collection& c) { return prune(c, used); }),
                      collections.end());
}

uint8_t pidFirstId(const std::vector<Collection>& collections)
{
    uint8_t id = 0;
    bool zero = false;
    for (const Collection& c : collections)
        maxId(c, id, zero);
    if (zero || id == 0 || id + kPidReports > 0xFF) return 0;
    return static_cast<uint8_t>(id + 1);
}

std::vector<uint8_t> pidBlock(uint8_t firstId)
{
    std::vector<uint8_t> out;
    Block b(out);
    const auto id = [firstId](uint8_t offset) { return static_cast<uint8_t>(firstId + offset); };
    const std::initializer_list<uint16_t> types = {uEtConstant, uEtRamp,    uEtSquare,  uEtSine,
                                                   uEtTriangle, uEtSawUp,   uEtSawDown, uEtSpring,
                                                   uEtDamper,   uEtInertia, uEtFriction};
    b.page(kPagePid);
    b.physical(0, 0); // physical = logical throughout
    b.unit(0, 0);

    // Set Effect: the effect's header.
    b.usage(uSetEffect);
    b.logicalCollection();
    b.reportId(id(rSetEffect));
    b.effectBlockIndex('o');
    b.array(uEffectType, types, 'o');
    b.unit(kUnitTime, kExpMilli);
    b.field({uDuration, uTriggerRepeat, uSamplePeriod, uStartDelay}, 0, 0xFFFF, 16, 'o');
    b.unit(0, 0);
    b.field({uGain}, 0, 255, 8, 'o');
    b.field({uTriggerButton}, 0, 8, 8, 'o');
    b.usage(uAxesEnable);
    b.logicalCollection();
    b.page(kPageDesktop);
    b.field({0x30, 0x31}, 0, 1, 1, 'o'); // X, Y
    b.end();
    b.page(kPagePid);
    b.field({uDirectionEnable}, 0, 1, 1, 'o');
    b.padding(5, 'o');
    b.usage(uDirection);
    b.logicalCollection();
    b.page(kPageOrdinal);
    b.unit(kUnitDegrees, kExpCenti);
    b.field({0x01, 0x02}, 0, 35999, 16, 'o');
    b.unit(0, 0);
    b.end();
    b.page(kPagePid);
    b.end();

    // Set Envelope.
    b.usage(uSetEnvelope);
    b.logicalCollection();
    b.reportId(id(rSetEnvelope));
    b.effectBlockIndex('o');
    b.field({uAttackLevel, uFadeLevel}, 0, 10000, 16, 'o');
    b.unit(kUnitTime, kExpMilli);
    b.field({uAttackTime, uFadeTime}, 0, 0xFFFF, 16, 'o');
    b.unit(0, 0);
    b.end();

    // Set Condition: one per axis, the axis in Parameter Block Offset.
    b.usage(uSetCondition);
    b.logicalCollection();
    b.reportId(id(rSetCondition));
    b.effectBlockIndex('o');
    b.field({uParameterBlockOffset}, 0, 1, 4, 'o');
    b.padding(4, 'o');
    b.field({uCpOffset, uPositiveCoefficient, uNegativeCoefficient}, -10000, 10000, 16, 'o');
    b.field({uPositiveSaturation, uNegativeSaturation, uDeadBand}, 0, 10000, 16, 'o');
    b.end();

    // Set Periodic.
    b.usage(uSetPeriodic);
    b.logicalCollection();
    b.reportId(id(rSetPeriodic));
    b.effectBlockIndex('o');
    b.field({uMagnitude}, 0, 10000, 16, 'o');
    b.field({uOffset}, -10000, 10000, 16, 'o');
    b.unit(kUnitDegrees, kExpCenti);
    b.field({uPhase}, 0, 35999, 16, 'o');
    b.unit(kUnitTime, kExpMilli);
    b.field({uPeriod}, 0, 0xFFFF, 16, 'o');
    b.unit(0, 0);
    b.end();

    // Set Constant Force.
    b.usage(uSetConstant);
    b.logicalCollection();
    b.reportId(id(rSetConstant));
    b.effectBlockIndex('o');
    b.field({uMagnitude}, -10000, 10000, 16, 'o');
    b.end();

    // Set Ramp Force.
    b.usage(uSetRamp);
    b.logicalCollection();
    b.reportId(id(rSetRamp));
    b.effectBlockIndex('o');
    b.field({uRampStart, uRampEnd}, -10000, 10000, 16, 'o');
    b.end();

    // Effect Operation: start, start solo, stop.
    b.usage(uEffectOperationReport);
    b.logicalCollection();
    b.reportId(id(rEffectOperation));
    b.effectBlockIndex('o');
    b.array(uEffectOperation, {uOpStart, uOpSolo, uOpStop}, 'o');
    b.field({uLoopCount}, 0, 255, 8, 'o');
    b.end();

    // PID Block Free.
    b.usage(uBlockFree);
    b.logicalCollection();
    b.reportId(id(rBlockFree));
    b.effectBlockIndex('o');
    b.end();

    // PID Device Control.
    b.usage(uDeviceControlReport);
    b.logicalCollection();
    b.reportId(id(rDeviceControl));
    b.array(uDeviceControl, {uDcEnable, uDcDisable, uDcStopAll, uDcReset, uDcPause, uDcContinue},
            'o');
    b.end();

    // Device Gain.
    b.usage(uDeviceGainReport);
    b.logicalCollection();
    b.reportId(id(rDeviceGain));
    b.field({uDeviceGain}, 0, 255, 8, 'o');
    b.end();

    // Create New Effect (feature, written by pid.dll before Block Load).
    b.usage(uCreateNewEffect);
    b.logicalCollection();
    b.reportId(id(rCreateNewEffect));
    b.array(uEffectType, types, 'f');
    b.page(kPageDesktop);
    b.field({0x3B}, 0, 511, 10, 'f'); // Byte Count
    b.page(kPagePid);
    b.padding(6, 'f');
    b.end();

    // PID Block Load (feature, read back: the block the new effect got).
    b.usage(uBlockLoadReport);
    b.logicalCollection();
    b.reportId(id(rBlockLoad));
    b.effectBlockIndex('f');
    b.array(uBlockLoadStatus, {uLoadSuccess, uLoadFull, uLoadError}, 'f');
    b.field({uRamPoolAvailable}, 0, 0xFFFF, 16, 'f');
    b.end();

    // PID Pool (feature): the host manages the blocks.
    b.usage(uPoolReport);
    b.logicalCollection();
    b.reportId(id(rPool));
    b.field({uRamPoolSize}, 0, 0xFFFF, 16, 'f');
    b.field({uSimultaneousMax}, 0, 255, 8, 'f');
    b.field({uDeviceManagedPool, uSharedParameterBlocks}, 0, 1, 1, 'f');
    b.padding(6, 'f');
    b.end();

    // PID State (input). Declared for DirectInput's GetForceFeedbackState; the
    // page sends none, so it reads as never paused.
    b.usage(uStateReport);
    b.logicalCollection();
    b.reportId(id(rState));
    b.field({uDevicePaused, uActuatorsEnabled, uSafetySwitch, uOverrideSwitch, uActuatorPower}, 0,
            1, 1, 'i');
    b.padding(3, 'i');
    b.field({uEffectPlaying}, 0, 1, 1, 'i');
    b.field({uEffectBlockIndex}, 1, kPidMaxEffects, 7, 'i');
    b.end();
    return out;
}

std::vector<uint8_t> encodeWithPid(const std::vector<Collection>& collections, uint8_t firstId)
{
    std::vector<uint8_t> out;
    bool placed = false;
    for (const Collection& c : collections) {
        // Each collection encoded on its own: the encoder then writes every
        // global it needs afresh, whatever the block left set before it.
        std::vector<uint8_t> one = encode({c});
        if (!placed && isGameApplication(c) && !one.empty() && one.back() == 0xC0) {
            one.pop_back();
            const std::vector<uint8_t> block = pidBlock(firstId);
            one.insert(one.end(), block.begin(), block.end());
            one.push_back(0xC0);
            placed = true;
        }
        out.insert(out.end(), one.begin(), one.end());
    }
    return out;
}

// ── PidEngine ───────────────────────────────────────────────────────────────

PidEngine::PidEngine(const Parsed& parsed, uint8_t firstId)
    : m_parsed(parsed)
    , m_firstId(firstId)
{}

PidEngine::Place PidEngine::variable(Kind kind, uint8_t reportId, uint32_t usage) const
{
    for (const Field& f : m_parsed.fields) {
        if (f.kind != kind || f.reportId != reportId || !f.isVariable() || f.isConstant()) continue;
        if (f.range) {
            if (usage < f.usageMinimum || usage > f.usageMaximum) continue;
            const uint32_t i = usage - f.usageMinimum;
            if (i >= f.count) continue;
            return {f.bitOffset + i * f.size, f.size, f.logicalMinimum, true};
        }
        for (uint32_t i = 0; i < f.count && i < f.usages.size(); ++i)
            if (f.usages[i] == usage)
                return {f.bitOffset + i * f.size, f.size, f.logicalMinimum, true};
    }
    return {};
}

int32_t PidEngine::read(const std::vector<uint8_t>& body, Kind kind, uint8_t reportId,
                        uint32_t usage) const
{
    const Place p = variable(kind, reportId, usage);
    if (!p.found) return 0;
    const uint32_t raw = bitsOf(body, p.bitOffset, p.size);
    if (p.logicalMinimum < 0 && p.size < 32 && (raw >> (p.size - 1)) & 1u)
        return static_cast<int32_t>(raw | (~0u << p.size)); // sign-extend
    return static_cast<int32_t>(raw);
}

uint32_t PidEngine::selected(const std::vector<uint8_t>& body, Kind kind, uint8_t reportId,
                             uint32_t anyUsage) const
{
    for (const Field& f : m_parsed.fields) {
        if (f.kind != kind || f.reportId != reportId || f.isVariable() || f.isConstant()) continue;
        if (std::find(f.usages.begin(), f.usages.end(), anyUsage) == f.usages.end()) continue;
        const int32_t v = static_cast<int32_t>(bitsOf(body, f.bitOffset, f.size));
        const int32_t i = v - f.logicalMinimum;
        if (i < 0 || static_cast<size_t>(i) >= f.usages.size()) return 0;
        return f.usages[static_cast<size_t>(i)];
    }
    return 0;
}

void PidEngine::put(std::vector<uint8_t>& body, Kind kind, uint8_t reportId, uint32_t usage,
                    int32_t value) const
{
    const Place p = variable(kind, reportId, usage);
    if (p.found) setBitsOf(body, p.bitOffset, p.size, static_cast<uint32_t>(value));
}

void PidEngine::putSelected(std::vector<uint8_t>& body, Kind kind, uint8_t reportId,
                            uint32_t usage) const
{
    for (const Field& f : m_parsed.fields) {
        if (f.kind != kind || f.reportId != reportId || f.isVariable() || f.isConstant()) continue;
        auto it = std::find(f.usages.begin(), f.usages.end(), usage);
        if (it == f.usages.end()) continue;
        const int32_t v = f.logicalMinimum + static_cast<int32_t>(it - f.usages.begin());
        setBitsOf(body, f.bitOffset, f.size, static_cast<uint32_t>(v));
        return;
    }
}

std::vector<uint8_t> PidEngine::getFeature(uint8_t reportId) const
{
    if (!owns(reportId)) return {};
    const size_t n = reportBytes(m_parsed, Kind::Feature, reportId);
    if (n < 2) return {};
    std::vector<uint8_t> body(n - 1, 0);
    const uint8_t offset = static_cast<uint8_t>(reportId - m_firstId);
    if (offset == rBlockLoad) {
        int free = 0;
        for (int i = 1; i <= kPidMaxEffects; ++i)
            free += m_used[i] ? 0 : 1;
        put(body, Kind::Feature, reportId, pid(uEffectBlockIndex), m_loadIndex);
        putSelected(body, Kind::Feature, reportId,
                    pid(m_loadStatus == 1   ? uLoadSuccess
                        : m_loadStatus == 2 ? uLoadFull
                                            : uLoadError));
        put(body, Kind::Feature, reportId, pid(uRamPoolAvailable), free * 0x100);
    } else if (offset == rPool) {
        put(body, Kind::Feature, reportId, pid(uRamPoolSize), 0xFFFF);
        put(body, Kind::Feature, reportId, pid(uSimultaneousMax), kPidMaxEffects);
        put(body, Kind::Feature, reportId, pid(uDeviceManagedPool), 1);
        put(body, Kind::Feature, reportId, pid(uSharedParameterBlocks), 0);
    }
    body.insert(body.begin(), reportId);
    return body;
}

void PidEngine::write(const std::vector<uint8_t>& data, std::vector<HidFfb>& ops)
{
    if (data.empty() || !owns(data[0])) return;
    const uint8_t id = data[0];
    const std::vector<uint8_t> body(data.begin() + 1, data.end());
    const uint8_t offset = static_cast<uint8_t>(id - m_firstId);
    const Kind out = Kind::Output;
    auto blockIndex = [&](Kind kind) {
        const int32_t i = read(body, kind, id, pid(uEffectBlockIndex));
        return (i >= 1 && i <= kPidMaxEffects) ? i : 0;
    };
    auto op = [&](const char* name, int effect) -> HidFfb& {
        HidFfb o;
        o.op = name;
        o.effect = effect;
        ops.push_back(std::move(o));
        return ops.back();
    };
    auto get = [&](uint16_t usage) { return read(body, out, id, pid(usage)); };

    switch (offset) {
    case rCreateNewEffect: {
        // pid.dll reads the answer right after, from Block Load.
        const uint32_t type = selected(body, Kind::Feature, id, pid(uEtConstant));
        m_loadIndex = 0;
        m_loadStatus = 3;
        if (!type) break;
        for (int i = 1; i <= kPidMaxEffects; ++i) {
            if (m_used[i]) continue;
            m_used[i] = true;
            m_loadIndex = static_cast<uint8_t>(i);
            m_loadStatus = 1;
            break;
        }
        if (!m_loadIndex) m_loadStatus = 2;
        break;
    }
    case rSetEffect: {
        const int e = blockIndex(out);
        if (!e) break;
        const uint32_t type = selected(body, out, id, pid(uEtConstant));
        HidFfb& o = op("effect", e);
        for (const EffectType& t : kEffectTypes)
            if (pid(t.usage) == type) o.kind = t.name;
        const int32_t duration = get(uDuration);
        o.fields = {{"duration", duration == 0xFFFF ? -1 : duration},
                    {"delay", get(uStartDelay)},
                    {"gain", get(uGain)},
                    {"direction", read(body, out, id, (kPageOrdinal << 16) | 0x01)},
                    {"directionEnable", get(uDirectionEnable)},
                    {"axes", read(body, out, id, (kPageDesktop << 16) | 0x30) |
                                 (read(body, out, id, (kPageDesktop << 16) | 0x31) << 1)}};
        break;
    }
    case rSetEnvelope: {
        const int e = blockIndex(out);
        if (!e) break;
        op("envelope", e).fields = {{"attackLevel", get(uAttackLevel)},
                                    {"attackTime", get(uAttackTime)},
                                    {"fadeLevel", get(uFadeLevel)},
                                    {"fadeTime", get(uFadeTime)}};
        break;
    }
    case rSetCondition: {
        const int e = blockIndex(out);
        if (!e) break;
        op("condition", e).fields = {{"axis", get(uParameterBlockOffset)},
                                     {"offset", get(uCpOffset)},
                                     {"positiveCoefficient", get(uPositiveCoefficient)},
                                     {"negativeCoefficient", get(uNegativeCoefficient)},
                                     {"positiveSaturation", get(uPositiveSaturation)},
                                     {"negativeSaturation", get(uNegativeSaturation)},
                                     {"deadBand", get(uDeadBand)}};
        break;
    }
    case rSetPeriodic: {
        const int e = blockIndex(out);
        if (!e) break;
        op("periodic", e).fields = {{"magnitude", get(uMagnitude)},
                                    {"offset", get(uOffset)},
                                    {"phase", get(uPhase)},
                                    {"period", get(uPeriod)}};
        break;
    }
    case rSetConstant: {
        const int e = blockIndex(out);
        if (e) op("constant", e).fields = {{"magnitude", get(uMagnitude)}};
        break;
    }
    case rSetRamp: {
        const int e = blockIndex(out);
        if (e) op("ramp", e).fields = {{"start", get(uRampStart)}, {"end", get(uRampEnd)}};
        break;
    }
    case rEffectOperation: {
        const int e = blockIndex(out);
        const uint32_t what = selected(body, out, id, pid(uOpStart));
        if (!e || !what) break;
        const char* name = what == pid(uOpStart) ? "start" : what == pid(uOpSolo) ? "solo" : "stop";
        HidFfb& o = op(name, e);
        if (what != pid(uOpStop)) o.fields = {{"loops", get(uLoopCount)}};
        break;
    }
    case rBlockFree: {
        const int e = blockIndex(out);
        if (!e) break;
        m_used[e] = false;
        op("free", e);
        break;
    }
    case rDeviceControl: {
        const uint32_t what = selected(body, out, id, pid(uDcEnable));
        if (!what) break;
        static const std::pair<uint16_t, const char*> kinds[] = {
            {uDcEnable, "enable"}, {uDcDisable, "disable"}, {uDcStopAll, "stopAll"},
            {uDcReset, "reset"},   {uDcPause, "pause"},     {uDcContinue, "continue"},
        };
        HidFfb& o = op("control", 0);
        for (const auto& [u, name] : kinds)
            if (pid(u) == what) o.kind = name;
        if (what == pid(uDcReset))
            for (bool& used : m_used)
                used = false;
        break;
    }
    case rDeviceGain: op("gain", 0).fields = {{"gain", get(uDeviceGain)}}; break;
    default: break;
    }
}

std::vector<uint8_t> PidEngine::save() const
{
    // Block Load's last answer, then one byte per block, 1..kPidMaxEffects.
    std::vector<uint8_t> out{m_loadIndex, m_loadStatus};
    for (int i = 1; i <= kPidMaxEffects; ++i)
        out.push_back(m_used[i] ? 1 : 0);
    return out;
}

bool PidEngine::restore(const std::vector<uint8_t>& state)
{
    if (state.size() != static_cast<size_t>(2 + kPidMaxEffects) || state[0] > kPidMaxEffects ||
        state[1] > 3)
        return false;
    m_loadIndex = state[0];
    m_loadStatus = state[1];
    for (int i = 1; i <= kPidMaxEffects; ++i)
        m_used[i] = state[static_cast<size_t>(1 + i)] != 0;
    return true;
}

} // namespace mw::native::input::hid
