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

// windows.h's min/max macros would eat the std:: ones FrameFit.h and the
// resample geometry use.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ConvertShaders.h"
#include "ConvertHlsl.h"

#include "../MwsrZones.h"
#include "../../platform/macos/FrameFit.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace mw::native::convert {

// Spelled "0"/"1" rather than left undefined, so the shader's #if never
// depends on what fxc makes of an unknown name.
bool compileConvertShader(const char* entryPoint, const char* target, bool scRgbSource,
                          ComPtr<ID3DBlob>& blob, std::string& error)
{
    const D3D_SHADER_MACRO defines[] = {{"MW_SCRGB_SOURCE", scRgbSource ? "1" : "0"},
                                        {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompile(
        kShaderSource, sizeof(kShaderSource) - 1, "ColorConvert.hlsl", defines, nullptr, entryPoint,
        target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;

    error = std::string("could not compile ") + entryPoint;
    if (errors && errors->GetBufferPointer()) {
        error += ": ";
        error += static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
}

bool compileScaleShader(bool horizontal, bool decode, int length, int output, int fixed,
                        ComPtr<ID3DBlob>& blob, std::string& error)
{
    const double dilate = std::max(1.0, static_cast<double>(length) / output);
    const int taps = static_cast<int>(std::ceil(4.0 * dilate)) + 1;
    // The dilation goes in as the ratio it is, never as a formatted double:
    // "%f" follows LC_NUMERIC, and a host whose region writes decimals with a
    // comma would emit `1,166667` — which HLSL reads as two arguments and the
    // compile fails, with an error about Lanczos2 that says nothing about the
    // locale. The ratio is also exact where nine digits are not.
    char dilateText[48];
    if (length <= output)
        std::snprintf(dilateText, sizeof(dilateText), "1.0");
    else
        std::snprintf(dilateText, sizeof(dilateText), "(%d.0 / %d.0)", length, output);
    const std::string tapsText = std::to_string(taps);
    const std::string lengthText = std::to_string(length) + ".0";
    const std::string fixedText = std::to_string(fixed) + ".0";
    const D3D_SHADER_MACRO defines[] = {{"MW_HORIZONTAL", horizontal ? "1" : "0"},
                                        {"MW_DECODE", decode ? "1" : "0"},
                                        {"MW_DILATE", dilateText},
                                        {"MW_TAPS", tapsText.c_str()},
                                        {"MW_LEN", lengthText.c_str()},
                                        {"MW_FIXED", fixedText.c_str()},
                                        {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    const HRESULT hr =
        ::D3DCompile(kScaleShaderSource, sizeof(kScaleShaderSource) - 1, "ColorScale.hlsl", defines,
                     nullptr, "PsScale", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                     blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;

    error = std::string("could not compile the ") + (horizontal ? "horizontal" : "vertical") +
            " resample";
    if (errors && errors->GetBufferPointer()) {
        error += ": ";
        error += static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
}

bool compileMwsrStatsShader(int width, int height, ComPtr<ID3DBlob>& blob, std::string& error)
{
    const std::string widthText = std::to_string(width);
    const std::string heightText = std::to_string(height);
    const std::string cellsX = std::to_string(mwsrzones::kCellsX);
    const std::string cellsY = std::to_string(mwsrzones::kCellsY);
    const D3D_SHADER_MACRO defines[] = {{"MW_WIDTH", widthText.c_str()},
                                        {"MW_HEIGHT", heightText.c_str()},
                                        {"MW_CELLS_X", cellsX.c_str()},
                                        {"MW_CELLS_Y", cellsY.c_str()},
                                        {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    const HRESULT hr =
        ::D3DCompile(kMwsrStatsSource, sizeof(kMwsrStatsSource) - 1, "MwsrStats.hlsl", defines,
                     nullptr, "CsStats", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                     blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;

    error = "could not compile MWSR's statistics pass";
    if (errors && errors->GetBufferPointer()) {
        error += ": ";
        error += static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
}

bool convertGeometry(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight,
                     ScaleFilter filter, ConvertGeometry& out, std::string& error)
{
    out = ConvertGeometry{};
    // NV12 chroma is half resolution in both axes, so an odd dimension has no
    // representation at all. Round down rather than up: growing the image would
    // sample outside the captured area.
    out.outputWidth = (outputWidth > 0 ? outputWidth : sourceWidth) & ~1;
    out.outputHeight = (outputHeight > 0 ? outputHeight : sourceHeight) & ~1;
    if (out.outputWidth <= 0 || out.outputHeight <= 0) {
        error = "output size is degenerate";
        return false;
    }

    // The resample pass only where there is something to resample: at 1:1
    // the conversion reads the capture as it always did, and pays nothing.
    out.scaling = out.outputWidth != sourceWidth || out.outputHeight != sourceHeight;
    out.filter = out.scaling ? filter : ScaleFilter::Bilinear;
    out.letterboxed = false;
    out.pictureX = out.pictureY = 0.0f;
    out.pictureWidth = static_cast<float>(out.outputWidth);
    out.pictureHeight = static_cast<float>(out.outputHeight);
    if (out.filter != ScaleFilter::Bilinear) {
        // A source of another shape is fitted between bars, as macOS does
        // (FrameFit.h), rather than stretched as the bilinear path does: the
        // Selector never starts a session this way, so this is a display that
        // changed mode under a session told not to follow it.
        const platform::FrameFit fit =
            platform::frameFit(sourceWidth, sourceHeight, out.outputWidth, out.outputHeight);
        const float w = std::floor(sourceWidth * fit.scale + 0.5f);
        const float h = std::floor(sourceHeight * fit.scale + 0.5f);
        if (w < out.outputWidth - 1 || h < out.outputHeight - 1) {
            out.letterboxed = true;
            out.pictureWidth = std::max(2.0f, w);
            out.pictureHeight = std::max(2.0f, h);
            out.pictureX = std::floor((out.outputWidth - out.pictureWidth) / 2.0f);
            out.pictureY = std::floor((out.outputHeight - out.pictureHeight) / 2.0f);
        }
    }
    return true;
}

OverlayConstants overlayConstants(const capture::CursorState& cursor, const CursorDraw& draw,
                                  bool cursorTexture, int sourceWidth, int sourceHeight,
                                  bool resampled, const ConvertGeometry& geometry, float sdrWhite)
{
    // The cursor rectangle, expressed in the uv of whatever the conversion
    // samples: the capture, or the scaled picture — the same square when the
    // shapes match, the fitted one between the bars when they do not. Drawing
    // in output pixels instead would misplace the pointer by the scale factor
    // on any stream that is not native resolution.
    const bool drawCursor = cursor.visible && cursorTexture && cursor.width > 0 &&
                            cursor.height > 0 && sourceWidth > 0 && sourceHeight > 0;

    // Magnified around the hotspot, not around the top-left: the pointer has
    // to keep aiming at the same pixel while it grows, or a bigger cursor
    // would also be a cursor that clicks somewhere else.
    const float magnify = draw.magnify > 1.0f ? draw.magnify : 1.0f;
    const float width = static_cast<float>(cursor.width) * magnify;
    const float height = static_cast<float>(cursor.height) * magnify;
    const float left =
        static_cast<float>(cursor.x + draw.hotspotX) - static_cast<float>(draw.hotspotX) * magnify;
    const float top =
        static_cast<float>(cursor.y + draw.hotspotY) - static_cast<float>(draw.hotspotY) * magnify;

    OverlayConstants out;
    float* p = out.values;
    p[0] = left / static_cast<float>(sourceWidth);
    p[1] = top / static_cast<float>(sourceHeight);
    p[2] = width / static_cast<float>(sourceWidth);
    p[3] = height / static_cast<float>(sourceHeight);
    if (resampled) {
        // Source uv → scaled-picture uv: the identity unless letterboxed.
        const float sx = geometry.pictureWidth / static_cast<float>(geometry.outputWidth);
        const float sy = geometry.pictureHeight / static_cast<float>(geometry.outputHeight);
        p[0] = geometry.pictureX / static_cast<float>(geometry.outputWidth) + p[0] * sx;
        p[1] = geometry.pictureY / static_cast<float>(geometry.outputHeight) + p[1] * sy;
        p[2] *= sx;
        p[3] *= sy;
    }
    p[4] = drawCursor ? 1.0f : 0.0f;
    p[5] = sdrWhite;
    p[6] = p[7] = 0.0f;
    return out;
}

} // namespace mw::native::convert
