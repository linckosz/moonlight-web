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

#include "input/HidDescriptor.h"

#include "mw/native/HidPassthrough.h"

#include <cstdint>
#include <string>
#include <vector>

/// Force feedback for a recreated wheel (plan "Passthrough HID", P3/P4).
///
/// ── Why a PID block ─────────────────────────────────────────────────────────
///
/// A game asks DirectInput for force feedback, and Windows gives it through
/// pid.dll to any HID device whose descriptor carries the USB PID 1.0 usages
/// (page 0x0F). Real wheels that speak PID (Moza, Simucube…) carry that block
/// themselves; the G923 does not — Logitech drives its motor through HID++
/// feature 0x8123 instead. So the host gives the recreated wheel a PID block of
/// its own, decodes what pid.dll writes into neutral operations (an effect and
/// its parameters, start, stop, gain) and the page plays them on the real wheel
/// through whatever that wheel speaks (frontend/js/hid/hidppFfb.js for 0x8123).
/// No DLL of ours registers anywhere.
///
/// pid.dll also asks synchronous questions (GET_FEATURE): which effect block a
/// new effect got, how big the pool is. Those are answered here, at once, from
/// the host's own table: a round trip to the page would hold the game's
/// CreateEffect for a network round trip, and over the Internet that is tens of
/// milliseconds per effect.
///
/// Linux is out of scope: hid-pidff only binds to usbhid devices, never uhid.
namespace mw::native::input::hid {

/// Report ids the block uses: firstId up to firstId + kPidReports - 1.
constexpr int kPidReports = 14;
/// Effect blocks the host hands out (indexes 1..kPidMaxEffects).
constexpr int kPidMaxEffects = 40;

/// The first report id free for the block after the device's own, or 0 when
/// none fits: a device without report ids (the TX12) cannot take one, as ids
/// must be all zero or all non-zero.
uint8_t pidFirstId(const std::vector<Collection>& collections);

/// Takes the device's own PID block out of what the page sent (P3: Moza,
/// Simucube, Fanatec, Thrustmaster… carry one). The recreated device must not
/// show it: two blocks would confuse pid.dll, and the device's would talk to
/// a motor the host cannot reach. With force feedback, the host's block takes
/// its place and the page re-encodes what it says into the device's own PID
/// reports (frontend/js/hid/pidFfb.js).
///
/// Every item inside a PID collection, or with a PID usage, becomes padding
/// of the same size, so a report the device shares with its own fields keeps
/// its layout. An output or feature report with a PID field is padded whole:
/// it is one of the PID spec's reports, and Chrome on Windows, which sends no
/// nesting, gives no other way to know its X, Y and ordinals are PID's. Both
/// of Chrome's shapes are understood (childRuns()). Reports left with padding
/// alone, and collections left empty, are dropped. Call pidFirstId() before,
/// on the device's collections as they came: the block's ids must stay clear
/// of the reports the device still sends.
void stripPid(std::vector<Collection>& collections);

/// The PID block alone: logical collections for each PID report, to sit inside
/// a game application collection. Its globals are its own; encodeWithPid()
/// re-emits the device's after it.
std::vector<uint8_t> pidBlock(uint8_t firstId);

/// encode(collections) with pidBlock(firstId) inside the first game
/// application collection (Joystick, Game Pad, Multi-axis or Simulation), the
/// one pid.dll looks at. The other collections are encoded as before.
std::vector<uint8_t> encodeWithPid(const std::vector<Collection>& collections, uint8_t firstId);

/// The host's side of one wheel's PID block: answers GET_FEATURE, keeps the
/// effect block table, and turns each PID report written by the game into
/// HidFfb operations. Fields are found by usage in the parsed descriptor, so
/// the layout lives in pidBlock() alone. Not thread-safe: the caller locks.
class PidEngine
{
public:
    PidEngine(const Parsed& parsed, uint8_t firstId);

    bool owns(uint8_t reportId) const
    {
        return m_firstId && reportId >= m_firstId && reportId < m_firstId + kPidReports;
    }

    /// The feature report `reportId`, its id first; empty when not one of ours.
    std::vector<uint8_t> getFeature(uint8_t reportId) const;

    /// An output report or SET_FEATURE of the block, its id first. Appends the
    /// operations it means to `ops` (slot left at 0 for the caller).
    void write(const std::vector<uint8_t>& data, std::vector<HidFfb>& ops);

    /// The block table, for a wheel that moves to another process between
    /// two streams: the game still holds the blocks it was given, and the
    /// next Create New Effect must not hand one of them out again.
    std::vector<uint8_t> save() const;
    /// The table save() gave; false (and the table untouched) when it is not one.
    bool restore(const std::vector<uint8_t>& state);

private:
    struct Place
    {
        uint32_t bitOffset = 0;
        uint16_t size = 0;
        int32_t logicalMinimum = 0;
        bool found = false;
    };
    Place variable(Kind kind, uint8_t reportId, uint32_t usage) const;
    int32_t read(const std::vector<uint8_t>& body, Kind kind, uint8_t reportId,
                 uint32_t usage) const;
    /// The usage an array field selects: the field holding `anyUsage` in its list.
    uint32_t selected(const std::vector<uint8_t>& body, Kind kind, uint8_t reportId,
                      uint32_t anyUsage) const;
    void put(std::vector<uint8_t>& body, Kind kind, uint8_t reportId, uint32_t usage,
             int32_t value) const;
    void putSelected(std::vector<uint8_t>& body, Kind kind, uint8_t reportId, uint32_t usage) const;

    Parsed m_parsed;
    uint8_t m_firstId = 0;
    bool m_used[kPidMaxEffects + 1] = {};
    uint8_t m_loadIndex = 0;
    uint8_t m_loadStatus = 0; // 1 success, 2 full, 3 error
};

} // namespace mw::native::input::hid
