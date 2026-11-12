/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

// A pen as a pen (issue #50): its pressure, its tilt, its eraser and its barrel
// buttons, for a GameStream host that takes them — Sunshine's pen extension,
// which Wolf speaks too. The page sends a pen only once the relay has said the
// host takes one (inputcaps `pen`, IMediaEngine::supportsPen()).
//
// Kept free of moonlight-common-c and of Qt so the tests can hold it; the
// values are the pen packet's own, and MoonlightShim.cpp checks them against
// Limelight.h.

#include <cstdint>
#include <vector>

namespace Pen {

// LI_TOUCH_EVENT_*: the events a pen packet carries.
constexpr uint8_t kHover = 0x00;
constexpr uint8_t kDown = 0x01;
constexpr uint8_t kUp = 0x02;
constexpr uint8_t kMove = 0x03;
constexpr uint8_t kHoverLeave = 0x06;

// LI_TOOL_TYPE_*: which end of the pen. Unknown keeps the host's last one.
constexpr uint8_t kToolUnknown = 0x00;
constexpr uint8_t kToolPen = 0x01;
constexpr uint8_t kToolEraser = 0x02;

// LI_PEN_BUTTON_PRIMARY | _SECONDARY | _TERTIARY.
constexpr uint8_t kButtonsMask = 0x07;

constexpr uint16_t kRotationUnknown = 0xFFFF; // LI_ROT_UNKNOWN
constexpr uint8_t kTiltUnknown = 0xFF;        // LI_TILT_UNKNOWN

} // namespace Pen

/// One pen event, as the pen packet carries it. Positions and pressure are
/// fractions of the picture and of the pen's range (0..1); a hover's
/// `pressureOrDistance` is its distance, 0 when unknown — which is what a
/// browser always gives.
struct PenEvent
{
    uint8_t eventType = Pen::kHover;
    uint8_t toolType = Pen::kToolUnknown;
    uint8_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;
    float pressureOrDistance = 0.0f;
    uint16_t rotation = Pen::kRotationUnknown;
    uint8_t tilt = Pen::kTiltUnknown;
};

/// The pen as the host was told about it.
///
/// The page's own stroke is always whole (down, moves, up), but the host's can
/// be cut under it: the input watchdog lifts a pen held down through a link
/// gone silent, and a change of the guest's rights lifts it too. The page knows
/// nothing of that and goes on with its moves. So a move for a pen the host no
/// longer holds down puts it down again where the pen is — the stroke resumes,
/// with a gap — and the page's late up for it is not sent twice. The other way
/// round, a hover or a leave for a pen still down lifts it first.
class PenHostState
{
public:
    /// What to send the host for @p event, in order. Empty when nothing is.
    std::vector<PenEvent> apply(const PenEvent& event)
    {
        std::vector<PenEvent> out;
        switch (event.eventType) {
        case Pen::kDown:
        case Pen::kMove: {
            PenEvent e = event;
            e.eventType = m_Down ? Pen::kMove : Pen::kDown;
            out.push_back(e);
            m_Down = true;
            break;
        }
        case Pen::kUp:
            if (!m_Down) break;
            out.push_back(event);
            m_Down = false;
            break;
        case Pen::kHover:
        case Pen::kHoverLeave:
            if (m_Down) {
                PenEvent up = event;
                up.eventType = Pen::kUp;
                up.toolType = m_Last.toolType;
                up.pressureOrDistance = 0.0f;
                out.push_back(up);
                m_Down = false;
            }
            out.push_back(event);
            break;
        default: break;
        }
        if (!out.empty()) m_Last = out.back();
        return out;
    }

    /// What lifts a pen still down on the host, where it last was, and takes
    /// it out of range. Empty when it is not down: a pen left hovering does no
    /// harm, and the page's next event places it again.
    std::vector<PenEvent> lift()
    {
        std::vector<PenEvent> out;
        if (!m_Down) return out;
        PenEvent up = m_Last;
        up.eventType = Pen::kUp;
        up.buttons = 0;
        up.pressureOrDistance = 0.0f;
        PenEvent leave = up;
        leave.eventType = Pen::kHoverLeave;
        leave.toolType = Pen::kToolUnknown;
        out.push_back(up);
        out.push_back(leave);
        m_Down = false;
        m_Last = leave;
        return out;
    }

    /// Whether the host holds the pen down.
    bool down() const { return m_Down; }

private:
    bool m_Down = false;
    PenEvent m_Last;
};
