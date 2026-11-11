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

#include "mw/native/HidPassthrough.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// A game device's HID report descriptor, rebuilt from what WebHID shows a page.
///
/// ── Why this exists ─────────────────────────────────────────────────────────
///
/// The HID passthrough (docs/design/hid-passthrough-study.md) recreates on the
/// host the device plugged into the client: same VID/PID, same name, same
/// reports. The browser never hands the page the device's report descriptor,
/// only a tree of `collections` describing it (§2.2 of the study). So the host
/// writes a descriptor back from that tree, one that lays out every report bit
/// for bit as the real one does. The encoding differs (Push/Pop, delimiters,
/// string indexes are gone); what a parser makes of it does not.
///
/// Three pieces, all pure, so mw-native-tests checks them with no device:
///  - encode(): collections → descriptor bytes;
///  - parse(): descriptor bytes → the fields a host parser sees, each with its
///    report, bit offset, size, usages and bounds;
///  - validate(): what the host accepts to create from network data (§5.5):
///    game usages only, bounded sizes.
///
/// "Equivalent" is checked by parsing both descriptors and comparing
/// normalise()d fields: a variable item split into its elements, a usage range
/// spelled out, physical bounds that default to the logical ones filled in.
/// That is the comparison the tests make against real descriptors, and the one
/// the H2 bench makes against the device's own.
namespace mw::native::input::hid {

// UnitSystem, ReportItem, Report, CollectionType and Collection: in the public
// header, where the relay fills them from the page's message.

enum class Kind : uint8_t
{
    Input = 0,
    Output = 1,
    Feature = 2
};

/// A main item as a host parser sees it, with its place in its report.
struct Field
{
    Kind kind = Kind::Input;
    uint8_t reportId = 0;
    uint32_t bitOffset = 0;
    uint16_t size = 0;
    uint16_t count = 0;
    /// The main item's flag bits (Constant, Variable, Relative, Wrap,
    /// Non Linear, No Preferred, Null State, Volatile, Buffered Bytes).
    uint16_t flags = 0;
    std::vector<uint32_t> usages;
    bool range = false;
    uint32_t usageMinimum = 0;
    uint32_t usageMaximum = 0;
    int32_t logicalMinimum = 0;
    int32_t logicalMaximum = 0;
    int32_t physicalMinimum = 0;
    int32_t physicalMaximum = 0;
    uint32_t unit = 0;
    int8_t unitExponent = 0;
    /// Usage of the top-level collection the item sits in (extended). Not part
    /// of the layout, so == ignores it; validate() reads it.
    uint32_t application = 0;

    bool isConstant() const { return flags & 0x01; }
    bool isVariable() const { return flags & 0x02; }

    bool operator==(const Field& o) const;
    bool operator!=(const Field& o) const { return !(*this == o); }
};

/// A collection as parse() found it, flattened: depth 0 is top level.
struct ParsedCollection
{
    uint32_t usage = 0; // extended
    uint8_t type = 0;
    int depth = 0;
};

struct Parsed
{
    bool ok = false;
    std::string error;
    std::vector<Field> fields;
    std::vector<ParsedCollection> collections;
    /// Every report id used; 0 alone when the descriptor declares none.
    std::vector<uint8_t> reportIds;
};

/// Where one child collection's items sit in its parent's own lists.
struct ChildRun
{
    Kind kind = Kind::Input;
    size_t report = 0;      // in the parent's list of that kind
    size_t child = 0;       // in parent.children
    size_t childReport = 0; // in the child's list of that kind
    size_t at = 0;          // the run's first item in the parent's report
};

/// The two shapes WebHID's collections come in (Chromium's source, 04/11):
///  - Chrome's own descriptor parser (macOS, Linux, ChromeOS) adds each item
///    to its collection and to every ancestor. A collection's lists then hold
///    its children's items too, each child's as one run, in descriptor order,
///    and a top-level collection holds every report whole;
///  - Chrome on Windows rebuilds one flat collection per top-level collection
///    from Windows' preparsed data: every report whole, no children.
/// A tree written by hand (the tests) lists each collection's own items only.
///
/// Where `c`'s children sit in its lists when c lists their items as well;
/// nothing when c has no children, or when a child's items are not in c's
/// lists.
std::optional<std::vector<ChildRun>> childRuns(const Collection& c);

/// The descriptor for these collections, as a device would declare them. A
/// collection that lists its children's items (childRuns()) is written from
/// its own lists alone: its reports are whole there, in order, and writing
/// the children too would declare each of their fields twice (the 552 refusal
/// of a Moza wheel from a Mac, 04/11). Its nested collections are not kept;
/// Chrome on Windows never had them, and games read fields, not nesting.
std::vector<uint8_t> encode(const std::vector<Collection>& collections);

/// Puts back the logical bounds Chrome loses on Windows, where it rebuilds
/// collections from the preparsed data instead of reading the descriptor:
/// buttons come with 0..0 instead of 0..1, and a vendor byte array declared
/// 0..255 comes with a minimum of 255 and a maximum of 0 (seen on the G923,
/// 03/10). An item whose bounds are empty or reversed gets the whole unsigned
/// range of its size, which is what such devices declare. Constant items and
/// sane bounds are left alone. Called on what a page sends, before encode().
void repairBounds(std::vector<Collection>& collections);

/// Puts back the usages Chrome's own descriptor parser (macOS, Linux) loses: a
/// one-usage range (Usage Minimum and Maximum both 1) comes with no usage, as
/// Chrome sets isRange only when the two differ, and its usageMinimum is the
/// usage. A data item left with no usage at all becomes padding: Linux reads
/// it as padding anyway, and Windows refuses a descriptor that has one
/// (HIDP_GETCOLDESC_MAIN_ITEM_NO_USAGE). Called with repairBounds().
void repairUsages(std::vector<Collection>& collections);

/// Short items only (a long item is refused, as no game device uses one).
Parsed parse(const uint8_t* data, size_t size);
inline Parsed parse(const std::vector<uint8_t>& d)
{
    return parse(d.data(), d.size());
}

/// The fields in a form two equivalent descriptors share: each element of a
/// variable item on its own with its usage, adjacent padding merged, physical
/// bounds of (0, 0) replaced by the logical ones, sorted by report and offset.
std::vector<Field> normalise(const std::vector<Field>& fields);

/// Size of a report in bytes on the wire, report id byte included when the
/// descriptor uses ids. 0 when no field belongs to it.
size_t reportBytes(const Parsed& p, Kind kind, uint8_t reportId);

/// Limits on what the host builds from network data (study §5.5).
constexpr size_t kMaxDescriptorBytes = 4096; // uhid's own limit
constexpr size_t kMaxReports = 64;           // distinct (kind, id) pairs
constexpr size_t kMaxReportBytes = 1024;

/// Empty when the host may create this device; otherwise why not, in a short
/// English sentence for the log. Checks: it parses; at least one application
/// collection is a joystick, game pad, multi-axis controller or simulation
/// device, and the others are vendor-defined (page 0xFF00 and up: Logitech's
/// HID++ beside the G923's joystick, opaque bytes that reach no key or
/// pointer); no keyboard, consumer or system-control usage anywhere; report
/// ids all zero or all non-zero; sizes within the limits above.
std::string validate(const std::vector<uint8_t>& descriptor);

/// The input report `reportId` put back to rest, from the last one the device
/// sent and the first one it sent (both without the id byte). Sent once after
/// a silence, so a lost connection never leaves the game with a held input.
///
/// Rest, field by field (Bruno's rule, 04/10):
///  - buttons and other one-bit inputs: released;
///  - hats and any variable with a null state: the null value (outside the
///    logical range, a 4-bit hat of 0..7 rests at 8);
///  - an axis that sat at one end of its range in the device's first report
///    rests at that end. That is how pedals and throttles are told apart when
///    their usage does not say it: the G923 declares its pedals as Y, Z and Rz
///    and reports them at 255 when released, the TX12's throttle stick sits at
///    its bottom when the radio is plugged in;
///  - otherwise an axis whose usage is a pedal or a throttle (Accelerator,
///    Brake, Clutch, Throttle on the Simulation page; Slider, Dial on Generic
///    Desktop) rests at its logical minimum;
///  - any other axis (X, Y, Z, Rx, Ry, Rz, Steering, Rudder…) rests at the
///    middle of its logical range;
///  - vendor fields, arrays and padding: as they were (opaque to us).
std::vector<uint8_t> restReport(const Parsed& p, uint8_t reportId, const std::vector<uint8_t>& last,
                                const std::vector<uint8_t>& first);

} // namespace mw::native::input::hid
