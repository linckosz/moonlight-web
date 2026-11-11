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

#include "input/HidDescriptor.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace mw::native::input::hid {

namespace {

// Item types and tags of HID 1.11 §6.2.2.
enum : uint8_t
{
    kMain = 0,
    kGlobal = 1,
    kLocal = 2
};
enum : uint8_t
{
    kInput = 0x8,
    kOutput = 0x9,
    kCollection = 0xA,
    kFeature = 0xB,
    kEndCollection = 0xC,
};
enum : uint8_t
{
    kUsagePage = 0x0,
    kLogicalMin = 0x1,
    kLogicalMax = 0x2,
    kPhysicalMin = 0x3,
    kPhysicalMax = 0x4,
    kUnitExponent = 0x5,
    kUnit = 0x6,
    kReportSize = 0x7,
    kReportId = 0x8,
    kReportCount = 0x9,
    kPush = 0xA,
    kPop = 0xB,
};
enum : uint8_t
{
    kUsage = 0x0,
    kUsageMin = 0x1,
    kUsageMax = 0x2
};

constexpr uint32_t pageOf(uint32_t usage)
{
    return usage >> 16;
}
constexpr uint32_t idOf(uint32_t usage)
{
    return usage & 0xFFFF;
}

// ── Encoder ─────────────────────────────────────────────────────────────────

class Writer
{
public:
    explicit Writer(std::vector<uint8_t>& out)
        : m_out(out)
    {}

    void raw(uint8_t type, uint8_t tag, uint32_t value, int bytes)
    {
        const uint8_t sizeCode = bytes == 4 ? 3 : static_cast<uint8_t>(bytes);
        m_out.push_back(static_cast<uint8_t>((tag << 4) | (type << 2) | sizeCode));
        for (int i = 0; i < bytes; ++i)
            m_out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
    void u(uint8_t type, uint8_t tag, uint32_t v)
    {
        raw(type, tag, v, v <= 0xFF ? 1 : v <= 0xFFFF ? 2 : 4);
    }
    // Signed and minimal: 255 takes two bytes (0xFF 0x00), never one byte
    // that a parser would read as -1.
    void s(uint8_t type, uint8_t tag, int32_t v)
    {
        const int bytes = (v >= -128 && v <= 127) ? 1 : (v >= -32768 && v <= 32767) ? 2 : 4;
        raw(type, tag, static_cast<uint32_t>(v), bytes);
    }

private:
    std::vector<uint8_t>& m_out;
};

bool sameItem(const ReportItem& a, const ReportItem& b)
{
    return a.isAbsolute == b.isAbsolute && a.isArray == b.isArray &&
           a.isBufferedBytes == b.isBufferedBytes && a.isConstant == b.isConstant &&
           a.isLinear == b.isLinear && a.isRange == b.isRange && a.isVolatile == b.isVolatile &&
           a.hasNull == b.hasNull && a.hasPreferredState == b.hasPreferredState &&
           a.wrap == b.wrap && a.usages == b.usages && a.usageMinimum == b.usageMinimum &&
           a.usageMaximum == b.usageMaximum && a.reportSize == b.reportSize &&
           a.reportCount == b.reportCount && a.unitExponent == b.unitExponent &&
           a.unitSystem == b.unitSystem &&
           std::equal(std::begin(a.unitFactors), std::end(a.unitFactors),
                      std::begin(b.unitFactors)) &&
           a.logicalMinimum == b.logicalMinimum && a.logicalMaximum == b.logicalMaximum &&
           a.physicalMinimum == b.physicalMinimum && a.physicalMaximum == b.physicalMaximum;
}

const std::vector<Report>& listOf(const Collection& c, Kind kind)
{
    return kind == Kind::Input    ? c.inputReports
           : kind == Kind::Output ? c.outputReports
                                  : c.featureReports;
}

uint32_t unitOf(const ReportItem& it)
{
    uint32_t unit = static_cast<uint32_t>(it.unitSystem) & 0xF;
    for (int i = 0; i < 6; ++i)
        unit |= (static_cast<uint32_t>(it.unitFactors[i]) & 0xF) << (4 * (i + 1));
    return unit;
}

uint16_t flagsOf(const ReportItem& it, Kind kind)
{
    uint16_t f = 0;
    if (it.isConstant) f |= 0x01;
    if (!it.isArray) f |= 0x02;
    if (!it.isAbsolute) f |= 0x04;
    if (it.wrap) f |= 0x08;
    if (!it.isLinear) f |= 0x10;
    if (!it.hasPreferredState) f |= 0x20;
    if (it.hasNull) f |= 0x40;
    if (kind != Kind::Input && it.isVolatile) f |= 0x80;
    if (it.isBufferedBytes) f |= 0x100;
    return f;
}

/// Writes collections, emitting a global only when it changes: a device's own
/// descriptor does the same, and it keeps ours well under uhid's 4096 bytes.
class Encoder
{
public:
    explicit Encoder(std::vector<uint8_t>& out)
        : m_w(out)
    {}

    void collection(const Collection& c)
    {
        // A collection without a usage (EdgeTX's inner Physical one) stays without.
        if (c.usagePage != 0 || c.usage != 0) {
            page(c.usagePage);
            m_w.u(kLocal, kUsage, c.usage);
        }
        m_w.u(kMain, kCollection, c.type);
        reports(c.inputReports, Kind::Input);
        reports(c.outputReports, Kind::Output);
        reports(c.featureReports, Kind::Feature);
        // Lists that hold the children's items already hold every field.
        if (!childRuns(c))
            for (const Collection& child : c.children)
                collection(child);
        m_w.raw(kMain, kEndCollection, 0, 0);
    }

private:
    void page(uint32_t p)
    {
        if (m_page == p) return;
        m_page = p;
        m_w.u(kGlobal, kUsagePage, p);
    }

    template <class T> void global(std::optional<T>& cur, T v, uint8_t tag, bool isSigned)
    {
        if (cur && *cur == v) return;
        cur = v;
        if (isSigned)
            m_w.s(kGlobal, tag, static_cast<int32_t>(v));
        else
            m_w.u(kGlobal, tag, static_cast<uint32_t>(v));
    }

    void usages(const ReportItem& it)
    {
        if (it.isConstant) return;
        if (it.isRange) {
            if (pageOf(it.usageMinimum) == pageOf(it.usageMaximum)) {
                page(pageOf(it.usageMinimum));
                m_w.u(kLocal, kUsageMin, idOf(it.usageMinimum));
                m_w.u(kLocal, kUsageMax, idOf(it.usageMaximum));
            } else {
                m_w.raw(kLocal, kUsageMin, it.usageMinimum, 4);
                m_w.raw(kLocal, kUsageMax, it.usageMaximum, 4);
            }
            return;
        }
        if (it.usages.empty()) return;
        const uint32_t p = pageOf(it.usages.front());
        const bool onePage = std::all_of(it.usages.begin(), it.usages.end(),
                                         [p](uint32_t u) { return pageOf(u) == p; });
        if (onePage) page(p);
        for (uint32_t u : it.usages) {
            if (onePage)
                m_w.u(kLocal, kUsage, idOf(u));
            else
                m_w.raw(kLocal, kUsage, u, 4); // extended: page in the high half
        }
    }

    void reports(const std::vector<Report>& list, Kind kind)
    {
        static const uint8_t mainTag[] = {kInput, kOutput, kFeature};
        for (const Report& r : list) {
            for (const ReportItem& it : r.items) {
                if (r.reportId != 0 && m_reportId != r.reportId) {
                    m_reportId = r.reportId;
                    m_w.u(kGlobal, kReportId, r.reportId);
                }
                global(m_lmin, it.logicalMinimum, kLogicalMin, true);
                global(m_lmax, it.logicalMaximum, kLogicalMax, true);
                global(m_pmin, it.physicalMinimum, kPhysicalMin, true);
                global(m_pmax, it.physicalMaximum, kPhysicalMax, true);
                const uint32_t unit = unitOf(it);
                global(m_unit, unit, kUnit, false);
                if (unit != 0)
                    global(m_unitExp, static_cast<int32_t>(it.unitExponent & 0xF), kUnitExponent,
                           false);
                global(m_size, static_cast<uint32_t>(it.reportSize), kReportSize, false);
                global(m_count, static_cast<uint32_t>(it.reportCount), kReportCount, false);
                usages(it);
                m_w.u(kMain, mainTag[static_cast<int>(kind)], flagsOf(it, kind));
            }
        }
    }

    Writer m_w;
    std::optional<uint32_t> m_page;
    std::optional<int32_t> m_lmin, m_lmax, m_pmin, m_pmax, m_unitExp;
    std::optional<uint32_t> m_unit, m_size, m_count;
    uint8_t m_reportId = 0;
};

// ── Parser ──────────────────────────────────────────────────────────────────

struct Globals
{
    uint32_t usagePage = 0;
    int32_t lmin = 0;
    uint32_t lmaxRaw = 0;
    int lmaxBytes = 0;
    int32_t pmin = 0;
    uint32_t pmaxRaw = 0;
    int pmaxBytes = 0;
    int8_t unitExp = 0;
    uint32_t unit = 0;
    uint32_t size = 0;
    uint32_t count = 0;
    uint8_t reportId = 0;
};

int32_t signExtend(uint32_t raw, int bytes)
{
    if (bytes == 1) return static_cast<int8_t>(raw);
    if (bytes == 2) return static_cast<int16_t>(raw);
    return static_cast<int32_t>(raw);
}

/// A maximum is signed when its minimum is negative, unsigned otherwise: how
/// Linux and Windows read the many devices that declare 0..255 in one byte.
int32_t maximumOf(int32_t minimum, uint32_t raw, int bytes)
{
    return minimum < 0 ? signExtend(raw, bytes) : static_cast<int32_t>(raw);
}

std::string hex(uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof b, "0x%X", v);
    return b;
}

} // namespace

bool Field::operator==(const Field& o) const
{
    return kind == o.kind && reportId == o.reportId && bitOffset == o.bitOffset && size == o.size &&
           count == o.count && flags == o.flags && usages == o.usages && range == o.range &&
           usageMinimum == o.usageMinimum && usageMaximum == o.usageMaximum &&
           logicalMinimum == o.logicalMinimum && logicalMaximum == o.logicalMaximum &&
           physicalMinimum == o.physicalMinimum && physicalMaximum == o.physicalMaximum &&
           unit == o.unit && unitExponent == o.unitExponent;
}

std::optional<std::vector<ChildRun>> childRuns(const Collection& c)
{
    if (c.children.empty()) return std::nullopt;
    std::vector<ChildRun> runs;
    for (const Kind kind : {Kind::Input, Kind::Output, Kind::Feature}) {
        const std::vector<Report>& mine = listOf(c, kind);
        // Children come in descriptor order, so their runs do too.
        std::vector<size_t> next(mine.size(), 0);
        for (size_t ch = 0; ch < c.children.size(); ++ch) {
            const std::vector<Report>& theirs = listOf(c.children[ch], kind);
            for (size_t cr = 0; cr < theirs.size(); ++cr) {
                const Report& r = theirs[cr];
                if (r.items.empty()) continue;
                const auto same = std::find_if(mine.begin(), mine.end(), [&](const Report& m) {
                    return m.reportId == r.reportId;
                });
                if (same == mine.end()) return std::nullopt;
                const size_t pr = static_cast<size_t>(same - mine.begin());
                const std::vector<ReportItem>& in = same->items;
                const auto at = std::search(in.begin() + static_cast<std::ptrdiff_t>(next[pr]),
                                            in.end(), r.items.begin(), r.items.end(), sameItem);
                if (at == in.end()) return std::nullopt;
                ChildRun run;
                run.kind = kind;
                run.report = pr;
                run.child = ch;
                run.childReport = cr;
                run.at = static_cast<size_t>(at - in.begin());
                runs.push_back(run);
                next[pr] = run.at + r.items.size();
            }
        }
    }
    return runs;
}

std::vector<uint8_t> encode(const std::vector<Collection>& collections)
{
    std::vector<uint8_t> out;
    Encoder enc(out);
    for (const Collection& c : collections)
        enc.collection(c);
    return out;
}

void repairBounds(std::vector<Collection>& collections)
{
    auto repairItem = [](ReportItem& it) {
        if (it.isConstant || it.reportSize == 0) return;
        if (it.logicalMinimum < it.logicalMaximum) return;
        it.logicalMinimum = 0;
        it.logicalMaximum = it.reportSize >= 31 ? INT32_MAX : (int32_t{1} << it.reportSize) - 1;
    };
    auto repairReports = [&](std::vector<Report>& reports) {
        for (Report& r : reports)
            for (ReportItem& it : r.items)
                repairItem(it);
    };
    for (Collection& c : collections) {
        repairReports(c.inputReports);
        repairReports(c.outputReports);
        repairReports(c.featureReports);
        repairBounds(c.children);
    }
}

void repairUsages(std::vector<Collection>& collections)
{
    for (Collection& c : collections) {
        for (auto* list : {&c.inputReports, &c.outputReports, &c.featureReports})
            for (Report& r : *list)
                for (ReportItem& it : r.items) {
                    if (it.isConstant || it.isRange || !it.usages.empty()) continue;
                    if (it.usageMinimum != 0 && it.usageMinimum == it.usageMaximum)
                        it.usages = {it.usageMinimum};
                    else
                        it.isConstant = true;
                }
        repairUsages(c.children);
    }
}

Parsed parse(const uint8_t* data, size_t size)
{
    Parsed p;
    std::vector<Globals> stack(1);
    std::vector<uint32_t> usages;
    std::optional<uint32_t> umin, umax;
    std::map<std::pair<int, int>, uint32_t> offsets;
    std::set<uint8_t> ids;
    int depth = 0;
    uint32_t application = 0;

    auto fail = [&](const std::string& why) {
        p.ok = false;
        p.error = why;
        p.fields.clear();
        return p;
    };
    auto resetLocals = [&] {
        usages.clear();
        umin.reset();
        umax.reset();
    };

    size_t i = 0;
    while (i < size) {
        const uint8_t prefix = data[i++];
        if (prefix == 0xFE) return fail("long item at byte " + std::to_string(i - 1));
        const int bytes = (prefix & 3) == 3 ? 4 : (prefix & 3);
        const uint8_t type = (prefix >> 2) & 3;
        const uint8_t tag = prefix >> 4;
        if (i + bytes > size) return fail("truncated item at byte " + std::to_string(i - 1));
        uint32_t raw = 0;
        for (int b = 0; b < bytes; ++b)
            raw |= static_cast<uint32_t>(data[i + b]) << (8 * b);
        i += bytes;
        Globals& g = stack.back();
        // A short usage takes the page in force now; a 4-byte one carries its own.
        auto extended = [&](uint32_t v) {
            return bytes == 4 ? v : (g.usagePage << 16) | (v & 0xFFFF);
        };

        if (type == kMain) {
            if (tag == kCollection) {
                ParsedCollection c;
                c.usage = usages.empty() ? 0 : usages.front();
                c.type = static_cast<uint8_t>(raw);
                if (depth == 0) application = c.usage;
                c.depth = depth++;
                p.collections.push_back(c);
            } else if (tag == kEndCollection) {
                if (--depth < 0) return fail("End Collection without a collection");
            } else if (tag == kInput || tag == kOutput || tag == kFeature) {
                Field f;
                f.kind = tag == kInput    ? Kind::Input
                         : tag == kOutput ? Kind::Output
                                          : Kind::Feature;
                f.reportId = g.reportId;
                f.application = depth > 0 ? application : 0;
                f.size = static_cast<uint16_t>(g.size);
                f.count = static_cast<uint16_t>(g.count);
                f.flags = static_cast<uint16_t>(raw & 0x1FF);
                f.usages = usages;
                if (umin && umax) {
                    f.range = true;
                    f.usageMinimum = *umin;
                    f.usageMaximum = *umax;
                }
                f.logicalMinimum = g.lmin;
                f.logicalMaximum = maximumOf(g.lmin, g.lmaxRaw, g.lmaxBytes);
                f.physicalMinimum = g.pmin;
                f.physicalMaximum = maximumOf(g.pmin, g.pmaxRaw, g.pmaxBytes);
                f.unit = g.unit;
                f.unitExponent = g.unitExp;
                uint32_t& off = offsets[{static_cast<int>(f.kind), f.reportId}];
                f.bitOffset = off;
                off += g.size * g.count;
                ids.insert(g.reportId);
                p.fields.push_back(std::move(f));
            }
            // Reserved main tags carry nothing a parser keeps.
            resetLocals();
        } else if (type == kGlobal) {
            switch (tag) {
            case kUsagePage: g.usagePage = raw & 0xFFFF; break;
            case kLogicalMin: g.lmin = signExtend(raw, bytes); break;
            case kLogicalMax:
                g.lmaxRaw = raw;
                g.lmaxBytes = bytes;
                break;
            case kPhysicalMin: g.pmin = signExtend(raw, bytes); break;
            case kPhysicalMax:
                g.pmaxRaw = raw;
                g.pmaxBytes = bytes;
                break;
            case kUnitExponent:
                // A nibble by the spec; some devices write a whole signed byte.
                g.unitExp = raw <= 0xF ? static_cast<int8_t>(raw >= 8 ? static_cast<int>(raw) - 16
                                                                      : static_cast<int>(raw))
                                       : static_cast<int8_t>(raw);
                break;
            case kUnit: g.unit = raw; break;
            case kReportSize: g.size = raw; break;
            case kReportId:
                if (raw == 0 || raw > 0xFF)
                    return fail("report id " + std::to_string(raw) + " out of range");
                g.reportId = static_cast<uint8_t>(raw);
                break;
            case kReportCount: g.count = raw; break;
            case kPush: stack.push_back(g); break;
            case kPop:
                if (stack.size() < 2) return fail("Pop without Push");
                stack.pop_back();
                break;
            default: break;
            }
        } else if (type == kLocal) {
            if (tag == kUsage)
                usages.push_back(extended(raw));
            else if (tag == kUsageMin)
                umin = extended(raw);
            else if (tag == kUsageMax)
                umax = extended(raw);
            // Designators, strings and delimiters do not change a report's layout.
        }
    }
    if (depth != 0) return fail("unclosed collection");
    p.ok = true;
    p.reportIds.assign(ids.begin(), ids.end());
    return p;
}

std::vector<Field> normalise(const std::vector<Field>& fields)
{
    std::vector<Field> out;
    for (const Field& f0 : fields) {
        Field f = f0;
        if (f.kind == Kind::Input)
            f.flags &= static_cast<uint16_t>(~0x80); // Volatile means nothing on input
        if (f.physicalMinimum == 0 && f.physicalMaximum == 0) {
            f.physicalMinimum = f.logicalMinimum;
            f.physicalMaximum = f.logicalMaximum;
        }
        if (f.unit == 0) f.unitExponent = 0;
        const uint32_t bits = static_cast<uint32_t>(f.size) * f.count;
        if (bits == 0) continue;

        if (f.isConstant()) {
            Field pad;
            pad.kind = f.kind;
            pad.reportId = f.reportId;
            pad.bitOffset = f.bitOffset;
            pad.size = static_cast<uint16_t>(bits);
            pad.count = 1;
            pad.flags = 0x01;
            out.push_back(pad);
            continue;
        }
        if (f.isVariable()) {
            for (uint16_t k = 0; k < f.count; ++k) {
                Field e = f;
                e.bitOffset = f.bitOffset + static_cast<uint32_t>(k) * f.size;
                e.count = 1;
                uint32_t usage = 0;
                if (f.range)
                    usage = std::min(f.usageMinimum + k, f.usageMaximum);
                else if (!f.usages.empty())
                    usage = k < f.usages.size() ? f.usages[k] : f.usages.back();
                e.usages = {usage};
                e.range = false;
                e.usageMinimum = e.usageMaximum = 0;
                out.push_back(std::move(e));
            }
            continue;
        }
        // An array: its usage list is what matters, however it was spelled.
        if (f.range) {
            f.usages.clear();
            for (uint32_t u = f.usageMinimum; u <= f.usageMaximum && f.usages.size() < 4096; ++u)
                f.usages.push_back(u);
            f.range = false;
            f.usageMinimum = f.usageMaximum = 0;
        }
        out.push_back(std::move(f));
    }
    std::stable_sort(out.begin(), out.end(), [](const Field& a, const Field& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.reportId != b.reportId) return a.reportId < b.reportId;
        return a.bitOffset < b.bitOffset;
    });
    // Padding split in two places, or written as one item, is the same padding.
    std::vector<Field> merged;
    for (Field& f : out) {
        if (!merged.empty() && f.isConstant() && merged.back().isConstant() &&
            merged.back().kind == f.kind && merged.back().reportId == f.reportId &&
            merged.back().bitOffset + merged.back().size == f.bitOffset) {
            merged.back().size = static_cast<uint16_t>(merged.back().size + f.size);
            continue;
        }
        merged.push_back(std::move(f));
    }
    return merged;
}

size_t reportBytes(const Parsed& p, Kind kind, uint8_t reportId)
{
    uint32_t bits = 0;
    bool any = false;
    for (const Field& f : p.fields) {
        if (f.kind != kind || f.reportId != reportId) continue;
        any = true;
        bits = std::max(bits, f.bitOffset + static_cast<uint32_t>(f.size) * f.count);
    }
    if (!any) return 0;
    const bool ids = !(p.reportIds.size() == 1 && p.reportIds.front() == 0);
    return (bits + 7) / 8 + (ids ? 1 : 0);
}

namespace {

// HID packs fields little-endian, least significant bit first.
void setBits(std::vector<uint8_t>& r, uint32_t offset, uint32_t size, uint32_t value)
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

uint32_t getBits(const std::vector<uint8_t>& r, uint32_t offset, uint32_t size)
{
    uint32_t v = 0;
    for (uint32_t b = 0; b < size; ++b) {
        const uint32_t bit = offset + b;
        if (bit / 8 >= r.size()) break;
        if ((r[bit / 8] >> (bit % 8)) & 1u) v |= 1u << b;
    }
    return v;
}

/// Usages whose rest is the low end when nothing else says: pedals and
/// throttles (Simulation page), sliders and dials (Generic Desktop).
bool restsLow(uint32_t usage)
{
    switch (usage) {
    case 0x200BB: // Throttle
    case 0x200C4: // Accelerator
    case 0x200C5: // Brake
    case 0x200C6: // Clutch
    case 0x10036: // Slider
    case 0x10037: // Dial
        return true;
    default: return false;
    }
}

/// Where an axis rests: see restReport() in the header.
int64_t axisRest(uint32_t usage, int64_t lo, int64_t hi, const int64_t* first)
{
    if (first) {
        const int64_t near = (hi - lo) / 20; // 5 % of the range
        if (*first <= lo + near) return lo;
        if (*first >= hi - near) return hi;
    }
    if (restsLow(usage)) return lo;
    return lo + (hi - lo + 1) / 2;
}

} // namespace

std::vector<uint8_t> restReport(const Parsed& p, uint8_t reportId, const std::vector<uint8_t>& last,
                                const std::vector<uint8_t>& first)
{
    std::vector<uint8_t> r = last;
    for (const Field& f : p.fields) {
        if (f.kind != Kind::Input || f.reportId != reportId || f.isConstant() || f.size == 0 ||
            f.size > 32)
            continue;
        for (uint16_t k = 0; k < f.count; ++k) {
            const uint32_t usage = f.range            ? std::min(f.usageMinimum + k, f.usageMaximum)
                                   : f.usages.empty() ? 0
                                   : k < f.usages.size() ? f.usages[k]
                                                         : f.usages.back();
            const uint32_t offset = f.bitOffset + static_cast<uint32_t>(k) * f.size;
            const bool button = pageOf(usage) == 0x09;
            if (pageOf(usage) >= 0xFF00) continue; // vendor: opaque, as it was
            if (!f.isVariable()) {
                // An array of buttons selects none; any other array (vendor
                // bytes, HID++) is left as it was.
                if (button) setBits(r, offset, f.size, 0);
                continue;
            }
            if (f.flags & 0x40) {
                // Null state: the first value outside the logical range that the
                // field can hold (a 4-bit hat of 0..7 rests at 8).
                const int64_t above = static_cast<int64_t>(f.logicalMaximum) + 1;
                const int64_t below = static_cast<int64_t>(f.logicalMinimum) - 1;
                const bool isSigned = f.logicalMinimum < 0;
                const int64_t hi =
                    isSigned ? (int64_t{1} << (f.size - 1)) - 1 : (int64_t{1} << f.size) - 1;
                const int64_t lo = isSigned ? -(int64_t{1} << (f.size - 1)) : 0;
                const int64_t rest = above <= hi ? above : below >= lo ? below : f.logicalMinimum;
                setBits(r, offset, f.size, static_cast<uint32_t>(rest));
            } else if (button || f.size == 1) {
                setBits(r, offset, f.size, 0);
            } else if (f.logicalMinimum < f.logicalMaximum) {
                const int64_t lo = f.logicalMinimum;
                const int64_t hi = f.logicalMaximum;
                const bool haveFirst = (offset + f.size + 7) / 8 <= first.size();
                int64_t firstValue = 0;
                if (haveFirst) {
                    const uint32_t raw = getBits(first, offset, f.size);
                    // Signed when the range is: the top bit of the field is its sign.
                    firstValue = lo < 0 && f.size < 32 && (raw >> (f.size - 1)) & 1u
                                     ? static_cast<int64_t>(raw) - (int64_t{1} << f.size)
                                     : static_cast<int64_t>(raw);
                }
                const int64_t rest = axisRest(usage, lo, hi, haveFirst ? &firstValue : nullptr);
                setBits(r, offset, f.size, static_cast<uint32_t>(rest));
            }
        }
    }
    return r;
}

std::string validate(const std::vector<uint8_t>& descriptor)
{
    if (descriptor.empty()) return "empty descriptor";
    if (descriptor.size() > kMaxDescriptorBytes)
        return "descriptor of " + std::to_string(descriptor.size()) + " bytes, over 4096";
    const Parsed p = parse(descriptor);
    if (!p.ok) return "unreadable descriptor: " + p.error;
    if (p.collections.empty()) return "no collection";

    // Generic Desktop collections that make a keyboard, a mouse or a power key.
    auto forbiddenCollection = [](uint32_t u) {
        if (pageOf(u) != 0x01) return false;
        const uint32_t id = idOf(u);
        return id == 0x02 || id == 0x06 || id == 0x07 || (id >= 0x80 && id <= 0x8F);
    };
    bool anyGame = false;
    for (const ParsedCollection& c : p.collections) {
        if (c.depth == 0 && c.type != Application)
            return "top-level collection " + hex(c.usage) + " is not an application collection";
        if (c.type == Application) {
            const bool game =
                (pageOf(c.usage) == 0x01 &&
                 (idOf(c.usage) == 0x04 || idOf(c.usage) == 0x05 || idOf(c.usage) == 0x08)) ||
                pageOf(c.usage) == 0x02;
            const bool vendor = pageOf(c.usage) >= 0xFF00;
            if (!game && !vendor)
                return "application collection " + hex(c.usage) + " is not a game device";
            anyGame = anyGame || game;
        }
        if (forbiddenCollection(c.usage))
            return "collection " + hex(c.usage) + " is a keyboard, mouse or system control";
    }
    // A vendor interface alone (the G923's second one, page 0xFFFD) is no game
    // device: nothing for a game to read, everything for a tool to talk to.
    if (!anyGame) return "no joystick, game pad or simulation collection";

    // Keys and media keys injected by a remote page are exactly what this
    // must never become, whatever collection they hide in.
    auto forbiddenUsage = [](uint32_t u) {
        const uint32_t pg = pageOf(u);
        if (pg == 0x07 || pg == 0x0C) return true;
        return pg == 0x01 && idOf(u) >= 0x80 && idOf(u) <= 0x8F;
    };
    bool input = false;
    std::set<std::pair<int, int>> reports;
    for (const Field& f : p.fields) {
        if (f.kind == Kind::Input) input = true;
        reports.insert({static_cast<int>(f.kind), f.reportId});
        for (uint32_t u : f.usages)
            if (forbiddenUsage(u))
                return "usage " + hex(u) + " is a keyboard, consumer or system control usage";
        // A vendor collection carries vendor bytes only: an axis or a button
        // there would reach the OS's input layer under no game collection.
        if (pageOf(f.application) >= 0xFF00 && !f.isConstant()) {
            const bool vendorOnly =
                f.range ? pageOf(f.usageMinimum) >= 0xFF00 && pageOf(f.usageMaximum) >= 0xFF00
                        : std::all_of(f.usages.begin(), f.usages.end(),
                                      [](uint32_t u) { return pageOf(u) >= 0xFF00; });
            if (!vendorOnly)
                return "vendor collection " + hex(f.application) + " holds a non-vendor usage";
        }
        if (f.range) {
            for (uint32_t pg = pageOf(f.usageMinimum); pg <= pageOf(f.usageMaximum); ++pg)
                if (pg == 0x07 || pg == 0x0C) return "usage range on page " + hex(pg) + " refused";
            if (pageOf(f.usageMinimum) == 0x01 && idOf(f.usageMinimum) <= 0x8F &&
                idOf(f.usageMaximum) >= 0x80)
                return "usage range over system controls refused";
        }
    }
    if (!input) return "no input report";
    if (p.reportIds.size() > 1 && p.reportIds.front() == 0)
        return "report ids mix zero and non-zero";
    if (reports.size() > kMaxReports) return std::to_string(reports.size()) + " reports, over 64";
    for (const auto& [kind, id] : reports) {
        const size_t n = reportBytes(p, static_cast<Kind>(kind), static_cast<uint8_t>(id));
        if (n > kMaxReportBytes)
            return "report " + std::to_string(id) + " of " + std::to_string(n) +
                   " bytes, over 1024";
    }
    return {};
}

} // namespace mw::native::input::hid
