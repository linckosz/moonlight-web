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

#include "Ds4Mapping.h"

namespace mw::native::input {
namespace {

/// Four D-pad bits → one hat direction.
///
/// Opposite directions held together (a worn pad, or a client sending both)
/// cancel: a hat has no way to express "up and down at once", and picking one
/// would make the pad walk in a direction nobody asked for.
uint16_t dpadFrom(uint32_t flags)
{
    const bool up = (flags & pad::kDpadUp) != 0;
    const bool down = (flags & pad::kDpadDown) != 0;
    const bool left = (flags & pad::kDpadLeft) != 0;
    const bool right = (flags & pad::kDpadRight) != 0;

    const int vertical = (up && !down) ? 1 : ((down && !up) ? -1 : 0);
    const int horizontal = (right && !left) ? 1 : ((left && !right) ? -1 : 0);

    if (vertical > 0) {
        if (horizontal > 0) return ds4::kDpadNorthEast;
        if (horizontal < 0) return ds4::kDpadNorthWest;
        return ds4::kDpadNorth;
    }
    if (vertical < 0) {
        if (horizontal > 0) return ds4::kDpadSouthEast;
        if (horizontal < 0) return ds4::kDpadSouthWest;
        return ds4::kDpadSouth;
    }
    if (horizontal > 0) return ds4::kDpadEast;
    if (horizontal < 0) return ds4::kDpadWest;
    return ds4::kDpadNone;
}

} // namespace

Ds4Report toDs4(uint32_t buttonFlags, uint8_t leftTrigger, uint8_t rightTrigger, int16_t leftStickX,
                int16_t leftStickY, int16_t rightStickX, int16_t rightStickY)
{
    Ds4Report report;

    // Face buttons by POSITION, not by name. A is the bottom button on both
    // pads, so A → Cross; B is the right one, so B → Circle. Following the
    // letters instead (X → X-shape) would put every prompt in the wrong place:
    // a game asking for "the bottom button" would light up Square.
    uint16_t buttons = 0;
    if (buttonFlags & pad::kA) buttons |= ds4::kCross;
    if (buttonFlags & pad::kB) buttons |= ds4::kCircle;
    if (buttonFlags & pad::kX) buttons |= ds4::kSquare;
    if (buttonFlags & pad::kY) buttons |= ds4::kTriangle;

    if (buttonFlags & pad::kLeftShoulder) buttons |= ds4::kShoulderLeft;
    if (buttonFlags & pad::kRightShoulder) buttons |= ds4::kShoulderRight;
    if (buttonFlags & pad::kLeftThumb) buttons |= ds4::kThumbLeft;
    if (buttonFlags & pad::kRightThumb) buttons |= ds4::kThumbRight;

    // Back/Start are Share/Options — the same two buttons in the same two
    // places, renamed by Sony.
    if (buttonFlags & pad::kBack) buttons |= ds4::kShare;
    if (buttonFlags & pad::kStart) buttons |= ds4::kOptions;

    // The digital trigger bits. The analog values go through in full below.
    if (leftTrigger >= kTriggerThreshold) buttons |= ds4::kTriggerLeft;
    if (rightTrigger >= kTriggerThreshold) buttons |= ds4::kTriggerRight;

    buttons |= dpadFrom(buttonFlags);
    report.buttons = buttons;

    // Guide is the PS button, which lives in its own byte rather than in the
    // button word. The touchpad click shares that byte and is never set: the
    // browser's standard mapping has no touchpad button to send, so claiming
    // one would be inventing input. See §6.1.
    if (buttonFlags & pad::kGuide) report.special |= ds4::kSpecialPs;

    report.triggerL = leftTrigger;
    report.triggerR = rightTrigger;

    report.thumbLX = toStickByte(leftStickX, false);
    report.thumbLY = toStickByte(leftStickY, true);
    report.thumbRX = toStickByte(rightStickX, false);
    report.thumbRY = toStickByte(rightStickY, true);

    return report;
}

} // namespace mw::native::input
