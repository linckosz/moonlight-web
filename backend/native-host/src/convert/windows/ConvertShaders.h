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

#include "../../capture/windows/IWindowsCapture.h"
#include "../CursorDraw.h"
#include "../ScaleFilter.h"

#include <d3dcommon.h>
#include <wrl/client.h>

#include <string>

// What the D3D11 converter (ColorConvert) and the D3D12 one (ColorConvert12)
// must do identically, in one place: compile the HLSL of ConvertHlsl.h, decide
// the geometry from the sizes, and place the pointer. Two copies of any of it
// would be two chances for a stream to look different depending on the
// pipeline that carried it — which the WARP tests would catch, but only after
// the fact.

namespace mw::native::convert {

/// One entry point of kShaderSource, compiled by FXC at O3. @p scRgbSource
/// picks what the BT.709 entry points read: the 8-bit desktop, or the FP16 one
/// through the tone map.
bool compileConvertShader(const char* entryPoint, const char* target, bool scRgbSource,
                          Microsoft::WRL::ComPtr<ID3DBlob>& blob, std::string& error);

/// One direction of the resample pass (kScaleShaderSource). @p length is the
/// source extent along the filtered axis, @p output the picture's extent along
/// it, @p fixed the extent of the other axis (rows for horizontal, columns for
/// vertical).
bool compileScaleShader(bool horizontal, bool decode, int length, int output, int fixed,
                        Microsoft::WRL::ComPtr<ID3DBlob>& blob, std::string& error);

/// MWSR's statistics pass (kMwsrStatsSource) for a @p width × @p height
/// capture, as a cs_5_0 compute shader.
bool compileMwsrStatsShader(int width, int height, Microsoft::WRL::ComPtr<ID3DBlob>& blob,
                            std::string& error);

/// What a converter makes of the sizes it is given.
struct ConvertGeometry
{
    /// Rounded down to even numbers: NV12 and P010 chroma are half
    /// resolution, so an odd size has no representation.
    int outputWidth = 0;
    int outputHeight = 0;
    /// Whether the output differs from the source at all.
    bool scaling = false;
    /// The filter in effect: Bilinear whenever nothing is scaled.
    ScaleFilter filter = ScaleFilter::Bilinear;
    /// A source of another shape, on the resample path, fitted between bars.
    bool letterboxed = false;
    /// Where the picture lands in the scaled picture: all of it, or the fitted
    /// rectangle.
    float pictureX = 0.0f;
    float pictureY = 0.0f;
    float pictureWidth = 0.0f;
    float pictureHeight = 0.0f;
};

/// The geometry for a @p sourceWidth × @p sourceHeight capture converted to
/// @p outputWidth × @p outputHeight (0 = the source's) with @p filter. False,
/// with the reason, for a degenerate output.
bool convertGeometry(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight,
                     ScaleFilter filter, ConvertGeometry& out, std::string& error);

/// The Overlay constant buffer of kShaderSource (b0), eight floats: the
/// pointer's rectangle in the uv of whatever the conversion samples, whether
/// to draw it, and where SDR white sits.
struct OverlayConstants
{
    float values[8] = {};
};

/// @p cursorTexture: whether the pointer's textures exist to be sampled.
/// @p resampled: whether the conversion reads the scaled picture rather than
/// the capture.
OverlayConstants overlayConstants(const capture::CursorState& cursor, const CursorDraw& draw,
                                  bool cursorTexture, int sourceWidth, int sourceHeight,
                                  bool resampled, const ConvertGeometry& geometry, float sdrWhite);

} // namespace mw::native::convert
