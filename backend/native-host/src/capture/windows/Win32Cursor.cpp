/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "Win32Cursor.h"

#include "../../core/Log.h"

#include <string>

namespace mw::native::capture {
namespace {

/// Read one HBITMAP as top-down 32-bit BGRA into @p out.
///
/// Negative height in the header is what asks GDI for top-down rows; left
/// bottom-up, every shape would come out mirrored — and a mirrored arrow still
/// looks like an arrow, which is exactly the kind of bug that ships.
bool readBitmap(HDC dc, HBITMAP bitmap, int width, int height, std::vector<uint8_t>& out)
{
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    out.assign(static_cast<size_t>(width) * height * 4, 0);
    return ::GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), out.data(), &info,
                       DIB_RGB_COLORS) != 0;
}

/// Read a 1-bit mask as packed bits, the layout the monochrome decoder expects:
/// one bit per pixel, most significant first, rows padded to 32 bits.
bool readMask(HDC dc, HBITMAP bitmap, int width, int height, std::vector<uint8_t>& out, int& pitch)
{
    pitch = ((width + 31) / 32) * 4;

    // A 1-bit DIB needs its two palette entries; BITMAPINFO declares only one,
    // so the extra RGBQUAD is why this is a byte buffer rather than the struct.
    std::vector<uint8_t> header(sizeof(BITMAPINFOHEADER) + 2 * sizeof(RGBQUAD), 0);
    auto* info = reinterpret_cast<BITMAPINFO*>(header.data());
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 1;
    info->bmiHeader.biCompression = BI_RGB;

    out.assign(static_cast<size_t>(pitch) * height, 0);
    return ::GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), out.data(), info,
                       DIB_RGB_COLORS) != 0;
}

/// True when any pixel carries a non-zero alpha.
///
/// This is what tells a modern 32-bit cursor from an old one whose colour
/// bitmap has no alpha channel at all: the first is plain colour with real
/// coverage, the second is a masked colour that needs the AND mask to know
/// what is transparent. Guessing wrong paints the pointer's whole canvas
/// opaque black.
bool hasAlpha(const std::vector<uint8_t>& bgra)
{
    for (size_t i = 3; i < bgra.size(); i += 4)
        if (bgra[i] != 0) return true;
    return false;
}

} // namespace

Win32Cursor::~Win32Cursor() = default;

bool Win32Cursor::refreshShape(HCURSOR handle, CursorState& cursor)
{
    if (!handle) return false;
    if (handle == m_Shape) return false;

    ICONINFO icon = {};
    if (!::GetIconInfo(handle, &icon)) return false;

    // GetIconInfo hands over bitmaps the caller owns. Every path out of here
    // has to free them, which is what this scope guard is for.
    struct Bitmaps
    {
        HBITMAP mask;
        HBITMAP color;
        ~Bitmaps()
        {
            if (mask) ::DeleteObject(mask);
            if (color) ::DeleteObject(color);
        }
    } owned{icon.hbmMask, icon.hbmColor};

    BITMAP maskInfo = {};
    if (!::GetObjectW(icon.hbmMask, sizeof(maskInfo), &maskInfo)) return false;

    HDC dc = ::GetDC(nullptr);
    if (!dc) return false;
    struct DcGuard
    {
        HDC dc;
        ~DcGuard() { ::ReleaseDC(nullptr, dc); }
    } dcGuard{dc};

    ShapeSource source;
    source.hotspotX = static_cast<int>(icon.xHotspot);
    source.hotspotY = static_cast<int>(icon.yHotspot);

    if (!icon.hbmColor) {
        // Monochrome: the mask holds BOTH 1-bit planes stacked, AND over XOR,
        // so the pointer is half as tall as the bitmap. Same layout Desktop
        // Duplication reports, which is why the decoder is shared.
        const int width = maskInfo.bmWidth;
        const int height = maskInfo.bmHeight / 2;
        if (width <= 0 || height <= 0) return false;

        int pitch = 0;
        if (!readMask(dc, icon.hbmMask, width, maskInfo.bmHeight, m_Buffer, pitch)) return false;

        source.encoding = ShapeSource::Encoding::Monochrome;
        source.width = width;
        source.height = height;
        source.pitch = pitch;
    } else {
        BITMAP colorInfo = {};
        if (!::GetObjectW(icon.hbmColor, sizeof(colorInfo), &colorInfo)) return false;
        const int width = colorInfo.bmWidth;
        const int height = colorInfo.bmHeight;
        if (width <= 0 || height <= 0) return false;

        if (!readBitmap(dc, icon.hbmColor, width, height, m_Buffer)) return false;

        if (hasAlpha(m_Buffer)) {
            source.encoding = ShapeSource::Encoding::Color;
        } else {
            // No alpha channel: the AND mask decides what is transparent. Fold
            // it into the alpha byte in the encoding the shared decoder already
            // understands — 0xFF means "XOR with the screen", which for a black
            // pixel is "leave it alone", i.e. transparent.
            std::vector<uint8_t> mask;
            int maskPitch = 0;
            if (!readMask(dc, icon.hbmMask, width, height, mask, maskPitch)) return false;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const size_t bit = static_cast<size_t>(y) * maskPitch + (x / 8);
                    const bool andBit = (mask[bit] & static_cast<uint8_t>(0x80 >> (x % 8))) != 0;
                    m_Buffer[(static_cast<size_t>(y) * width + x) * 4 + 3] = andBit ? 0xFF : 0x00;
                }
            }
            source.encoding = ShapeSource::Encoding::MaskedColor;
        }

        source.width = width;
        source.height = height;
        source.pitch = width * 4;
    }

    source.data = m_Buffer.data();
    source.size = m_Buffer.size();

    decodeCursorShape(source, cursor);
    m_Shape = handle;
    m_HotspotX = source.hotspotX;
    m_HotspotY = source.hotspotY;
    return true;
}

bool Win32Cursor::update(CursorState& cursor, const DesktopRect& rect, int captureWidth,
                         int captureHeight)
{
    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    if (!::GetCursorInfo(&info)) {
        const bool changed = cursor.visible;
        cursor.visible = false;
        return changed;
    }

    const bool showing = (info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr;
    // On THIS display: the pointer is one object on a virtual desktop, and a
    // capture of one monitor must not draw a pointer sitting on another.
    const bool onDisplay = showing && rect.valid() && info.ptScreenPos.x >= rect.left &&
                           info.ptScreenPos.x < rect.right && info.ptScreenPos.y >= rect.top &&
                           info.ptScreenPos.y < rect.bottom;

    bool changed = false;
    if (onDisplay) changed |= refreshShape(info.hCursor, cursor);

    // Desktop coordinates are DPI-virtualized; the captured texture is in real
    // pixels. On a 125% display the two differ by that factor, and skipping the
    // scale puts the drawn pointer a fifth of the screen away from the real one.
    const int rectWidth = rect.width();
    const int rectHeight = rect.height();
    const double scaleX =
        rectWidth > 0 && captureWidth > 0 ? static_cast<double>(captureWidth) / rectWidth : 1.0;
    const double scaleY =
        rectHeight > 0 && captureHeight > 0 ? static_cast<double>(captureHeight) / rectHeight : 1.0;

    // Hotspot subtracted, as CursorState::x/y is defined: this is where the
    // IMAGE goes, not where the pointer aims.
    const int x = static_cast<int>((info.ptScreenPos.x - rect.left) * scaleX) - m_HotspotX;
    const int y = static_cast<int>((info.ptScreenPos.y - rect.top) * scaleY) - m_HotspotY;

    if (cursor.visible != onDisplay || cursor.x != x || cursor.y != y) changed = true;
    cursor.visible = onDisplay;
    cursor.x = x;
    cursor.y = y;
    return changed;
}

} // namespace mw::native::capture
