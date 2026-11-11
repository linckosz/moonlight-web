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

#pragma once

// What a page receives from WebHID for a device whose descriptor is known, in
// the two shapes Chrome gives (HidDescriptor.h, childRuns()). The macOS shape
// follows Chromium's own descriptor parser (services/device/hid:
// HidCollection::BuildCollections, HidItemStateTable, HidReportItem):
//  - each Input, Output or Feature item goes to its collection's list for its
//    report, and to the same list of every ancestor;
//  - a report id counts only inside a collection, and Push and Pop leave it;
//  - a one-usage range (minimum == maximum) is no range, and its usages are
//    the Usage items alone: none;
//  - bounds are read signed from their size, so 0..255 in one byte is 0..-1.
// The Windows shape is one flat collection per top-level collection, every
// report whole: what Chrome rebuilds from Windows' preparsed data, give or
// take the padding it adds and its order by bit index.

#include "input/HidDescriptor.h"
#include "mw/native/HidPassthrough.h"

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace hid_chrome {

using namespace mw::native::input::hid;

namespace detail {

struct Node
{
    Node* parent = nullptr;
    uint16_t page = 0;
    uint16_t usage = 0;
    uint8_t type = 0;
    std::vector<std::unique_ptr<Node>> kids;
    std::map<uint8_t, std::vector<ReportItem>> lists[3]; // input, output, feature
};

inline int32_t signedOf(uint32_t v, int bytes)
{
    if (bytes == 0) return 0;
    if (bytes == 1) return static_cast<int8_t>(v);
    if (bytes == 2) return static_cast<int16_t>(v);
    return static_cast<int32_t>(v);
}

inline int8_t nibble(uint32_t v)
{
    const int n = static_cast<int>(v & 0xF);
    return static_cast<int8_t>(n >= 8 ? n - 16 : n);
}

inline Collection toCollection(const Node& n)
{
    Collection c;
    c.usagePage = n.page;
    c.usage = n.usage;
    c.type = n.type;
    std::vector<Report>* out[3] = {&c.inputReports, &c.outputReports, &c.featureReports};
    for (int k = 0; k < 3; ++k)
        for (const auto& [id, items] : n.lists[k])
            out[k]->push_back(Report{id, items});
    for (const auto& kid : n.kids)
        c.children.push_back(toCollection(*kid));
    return c;
}

} // namespace detail

/// The collections Chrome on macOS (and Linux) shows a page for `d`.
inline std::vector<Collection> onMac(const std::vector<uint8_t>& d)
{
    using detail::Node;
    struct Globals
    {
        uint32_t page = 0, unit = 0, unitExp = 0, size = 0, count = 0;
        int32_t lmin = 0, lmax = 0, pmin = 0, pmax = 0;
    };
    std::vector<std::unique_ptr<Node>> tops;
    Node* cur = nullptr;
    std::vector<Globals> stack(1);
    uint8_t reportId = 0;
    std::vector<uint32_t> usages;
    uint32_t umin = 0, umax = 0;
    auto resetLocals = [&] {
        usages.clear();
        umin = umax = 0;
    };
    for (size_t i = 0; i < d.size();) {
        const uint8_t prefix = d[i++];
        const int bytes = (prefix & 3) == 3 ? 4 : (prefix & 3);
        uint32_t v = 0;
        for (int b = 0; b < bytes && i + b < d.size(); ++b)
            v |= static_cast<uint32_t>(d[i + b]) << (8 * b);
        i += bytes;
        const uint8_t type = (prefix >> 2) & 3;
        const uint8_t tag = prefix >> 4;
        Globals& g = stack.back();
        // A short usage takes the usage page in force.
        auto extended = [&](uint32_t u) { return bytes == 4 ? u : (g.page << 16) | u; };
        if (type == 0) {
            if (tag == 0xA) {
                auto node = std::make_unique<Node>();
                const uint32_t u = usages.empty() ? 0 : usages.front();
                node->page = static_cast<uint16_t>((u >> 16) ? (u >> 16) : g.page);
                node->usage = static_cast<uint16_t>(u);
                node->type = static_cast<uint8_t>(v);
                node->parent = cur;
                Node* raw = node.get();
                (cur ? cur->kids : tops).push_back(std::move(node));
                cur = raw;
            } else if (tag == 0xC) {
                if (cur) cur = cur->parent;
            } else if (tag == 0x8 || tag == 0x9 || tag == 0xB) {
                ReportItem it;
                it.isConstant = v & 0x01;
                it.isArray = !(v & 0x02);
                it.isAbsolute = !(v & 0x04);
                it.wrap = v & 0x08;
                it.isLinear = !(v & 0x10);
                it.hasPreferredState = !(v & 0x20);
                it.hasNull = v & 0x40;
                it.isVolatile = v & 0x80;
                it.isBufferedBytes = v & 0x100;
                it.isRange = umin != umax;
                it.usages = usages;
                it.usageMinimum = umin;
                it.usageMaximum = umax;
                it.reportSize = static_cast<uint16_t>(g.size);
                it.reportCount = static_cast<uint16_t>(g.count);
                it.unitSystem = static_cast<UnitSystem>(g.unit & 0xF);
                for (int f = 0; f < 6; ++f)
                    it.unitFactors[f] = detail::nibble(g.unit >> (4 * (f + 1)));
                it.unitExponent = detail::nibble(g.unitExp);
                it.logicalMinimum = g.lmin;
                it.logicalMaximum = g.lmax;
                it.physicalMinimum = g.pmin;
                it.physicalMaximum = g.pmax;
                const int k = tag == 0x8 ? 0 : tag == 0x9 ? 1 : 2;
                for (Node* c = cur; c; c = c->parent)
                    c->lists[k][reportId].push_back(it);
            }
            resetLocals();
        } else if (type == 1) {
            switch (tag) {
            case 0x0: g.page = v; break;
            case 0x1: g.lmin = detail::signedOf(v, bytes); break;
            case 0x2: g.lmax = detail::signedOf(v, bytes); break;
            case 0x3: g.pmin = detail::signedOf(v, bytes); break;
            case 0x4: g.pmax = detail::signedOf(v, bytes); break;
            case 0x5: g.unitExp = v; break;
            case 0x6: g.unit = v; break;
            case 0x7: g.size = v; break;
            case 0x8:
                if (cur) reportId = static_cast<uint8_t>(v);
                break;
            case 0x9: g.count = v; break;
            case 0xA: stack.push_back(g); break;
            case 0xB:
                if (stack.size() > 1) stack.pop_back();
                break;
            default: break;
            }
        } else if (type == 2) {
            if (tag == 0x0)
                usages.push_back(extended(v));
            else if (tag == 0x1)
                umin = extended(v);
            else if (tag == 0x2)
                umax = extended(v);
        }
    }
    std::vector<Collection> out;
    for (const auto& t : tops)
        out.push_back(detail::toCollection(*t));
    return out;
}

/// The collections Chrome on Windows shows a page for `d`.
inline std::vector<Collection> onWindows(const std::vector<uint8_t>& d)
{
    std::vector<Collection> c = onMac(d);
    for (Collection& top : c)
        top.children.clear(); // its lists already hold every report whole
    return c;
}

/// Two of the rules Windows' HID parser refuses a descriptor for (WDK
/// hidpddi.h): a report that is not a whole number of bytes
/// (HIDP_GETCOLDESC_BYTE_ALLIGN), a data item with no usage
/// (HIDP_GETCOLDESC_MAIN_ITEM_NO_USAGE). VhfCreate then fails with
/// STATUS_COULD_NOT_INTERPRET, error 552: the Moza wheel of 04/11.
inline bool windowsAccepts(const Parsed& p)
{
    if (!p.ok) return false;
    std::map<std::pair<int, int>, uint32_t> bits;
    for (const Field& f : p.fields) {
        bits[{static_cast<int>(f.kind), f.reportId}] += static_cast<uint32_t>(f.size) * f.count;
        if (!f.isConstant() && f.usages.empty() && !f.range) return false;
    }
    for (const auto& kv : bits)
        if (kv.second % 8) return false;
    return true;
}

} // namespace hid_chrome
