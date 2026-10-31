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

// windows.h's min/max macros would eat the std:: ones the reference uses.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "native_test_framework.h"

#if defined(_WIN32)
#include "convert/windows/ColorConvert.h"
#include "convert/windows/MwsrStats.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace mw::native;
#endif

// MWSR's statistics pass on WARP, against a CPU reference that repeats the
// shader's operations on every pixel in the same order and the same float
// precision (the cell sums in double, the GPU's in float: a few ulps apart);
// and the light pass, one pixel in 16, against the same reference.
// The picture is 1000 × 600, so that the cells are 7 or 8 pixels wide and 8 or
// 9 high, as the bench cut them; it holds flat blocks, text-like strokes, a
// smooth gradient and saturated noise. The bench's own captures, against its
// own cells, are checked out of the repository (plan B §12 f 2.3).
void run_mwsr_stats_tests()
{
#if defined(_WIN32)
    using convert::MwsrStats;
    using Microsoft::WRL::ComPtr;
    namespace mz = convert::mwsrzones;

    SECTION("MwsrStats: the statistics pass on WARP");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, wanted, 2, D3D11_SDK_VERSION,
                                   device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
        std::fprintf(stderr, "  no WARP device here — skipped\n");
        return;
    }

    // BGRA bytes, as the capture hands them over, and a view of them through
    // the UNORM format: the encoded values.
    auto upload = [&](const std::vector<uint8_t>& px, int w, int h,
                      ComPtr<ID3D11ShaderResourceView>& view) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA seed = {};
        seed.pSysMem = px.data();
        seed.SysMemPitch = static_cast<UINT>(w * 4);
        ComPtr<ID3D11Texture2D> tex;
        return SUCCEEDED(device->CreateTexture2D(&desc, &seed, tex.GetAddressOf())) &&
               SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, view.GetAddressOf()));
    };

    // The shader's arithmetic on one picture: per pixel in float, in its
    // order; per cell, the sums in double. @p step 4 is the light pass's:
    // every 4th column of every 4th row from the cell's first.
    auto reference = [](const std::vector<uint8_t>& px, int w, int h, int step) {
        auto rgb = [&](int x, int y, int c) {
            // BGRA: R is byte 2, B byte 0
            return static_cast<float>(px[(static_cast<size_t>(y) * w + x) * 4 + 2 - c]) / 255.0f;
        };
        auto luma = [&](int x, int y) {
            return rgb(x, y, 0) * 0.2126f + rgb(x, y, 1) * 0.7152f + rgb(x, y, 2) * 0.0722f;
        };
        std::vector<double> cells(MwsrStats::kFloats, 0.0);
        for (int cy = 0; cy < mz::kCellsY; ++cy) {
            for (int cx = 0; cx < mz::kCellsX; ++cx) {
                const int x0 = cx * w / mz::kCellsX, x1 = (cx + 1) * w / mz::kCellsX;
                const int y0 = cy * h / mz::kCellsY, y1 = (cy + 1) * h / mz::kCellsY;
                double* s = &cells[(static_cast<size_t>(cy) * mz::kCellsX + cx) * mz::kStats];
                int n = 0;
                for (int y = y0; y < y1; y += step) {
                    for (int x = x0; x < x1; x += step) {
                        ++n;
                        const float c = luma(x, y);
                        const float lx = luma(std::min(x + 1, w - 1), y);
                        const float lw = luma(std::max(x - 1, 0), y);
                        const float ly = luma(x, std::min(y + 1, h - 1));
                        const float ln = luma(x, std::max(y - 1, 0));
                        const float g = std::fabs(lx - c) + std::fabs(ly - c);
                        const float lap = std::fabs(lx + ly + lw + ln - 4.0f * c);
                        const float r = rgb(x, y, 0), gr = rgb(x, y, 1), b = rgb(x, y, 2);
                        const float sat =
                            std::max(r, std::max(gr, b)) - std::min(r, std::min(gr, b));
                        const float v[mz::kStats] = {c,
                                                     c * c,
                                                     g,
                                                     g > 0.05f ? 1.0f : 0.0f,
                                                     g < 0.01f ? 1.0f : 0.0f,
                                                     lap,
                                                     sat,
                                                     c < 0.15f ? 1.0f : 0.0f};
                        for (int k = 0; k < mz::kStats; ++k)
                            s[k] += v[k];
                    }
                }
                for (int k = 0; k < mz::kStats; ++k)
                    s[k] /= n;
            }
        }
        return cells;
    };

    // ── A picture of every kind of zone, at a size the grid does not divide ──
    {
        const int w = 1000, h = 600;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        uint32_t seed = 12345;
        auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 24;
        };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                uint8_t r, g, b;
                if (x < w / 4) {
                    // flat blocks, some of them dark
                    const uint32_t k = static_cast<uint32_t>((x / 37) * 31 + (y / 41) * 17);
                    r = static_cast<uint8_t>((k * 53) & 255);
                    g = static_cast<uint8_t>((k * 97) & 255);
                    b = static_cast<uint8_t>((k * 29) & 255);
                } else if (x < w / 2) {
                    // text: dark strokes on a light page, in lines
                    const bool ink = (y % 14) < 10 && ((x * 7 + (y / 14) * 13) % 11) < 3;
                    r = g = b = ink ? 24 : 236;
                } else if (x < 3 * w / 4) {
                    // a smooth gradient
                    r = static_cast<uint8_t>(x * 255 / w);
                    g = static_cast<uint8_t>(y * 255 / h);
                    b = static_cast<uint8_t>(((x + y) / 4) & 255);
                } else {
                    // saturated noise
                    r = static_cast<uint8_t>(next());
                    g = static_cast<uint8_t>(next() & 0x3f);
                    b = static_cast<uint8_t>(255 - r);
                }
                p[0] = b;
                p[1] = g;
                p[2] = r;
                p[3] = 255;
            }
        }

        ComPtr<ID3D11ShaderResourceView> view;
        CHECK(upload(px, w, h, view));
        for (const bool light : {false, true}) {
            MwsrStats stats;
            std::string error;
            CHECK(stats.init(device.Get(), w, h, light, error));
            if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
            CHECK(stats.light() == light);
            const uint64_t ticket = stats.dispatch(view.Get());
            CHECK(ticket != 0);
            std::vector<float> cells(MwsrStats::kFloats);
            CHECK(stats.read(ticket, cells.data(), true));

            const std::vector<double> want = reference(px, w, h, light ? 4 : 1);
            double worst[mz::kStats] = {};
            for (size_t i = 0; i < cells.size(); ++i) {
                const double d = std::fabs(static_cast<double>(cells[i]) - want[i]);
                double& wk = worst[i % mz::kStats];
                wk = std::max(wk, d);
            }
            std::fprintf(stderr, "  %s pass, largest differences:", light ? "light" : "full");
            for (double d : worst)
                std::fprintf(stderr, " %.2g", d);
            std::fprintf(stderr, "\n");
            // The sums in float against double: a few ulps of values up to 4
            // (the laplacian); a share differs by 1 / 72 at the least when one
            // pixel is counted differently (1 / 4 on the light pass), so 1e-6
            // says none was.
            for (int k = 0; k < mz::kStats; ++k)
                CHECK(worst[k] < 1e-6);
        }
    }

    // ── A flat grey capture, all the way to the codes: the law everywhere ──
    {
        const int w = 256, h = 144;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 128);
        ComPtr<ID3D11ShaderResourceView> view;
        CHECK(upload(px, w, h, view));
        for (const bool light : {false, true}) {
            MwsrStats stats;
            std::string error;
            CHECK(stats.init(device.Get(), w, h, light, error));
            std::vector<float> cells(MwsrStats::kFloats);
            CHECK(stats.read(stats.dispatch(view.Get()), cells.data(), true));
            CHECK_EQ(cells[4], 1.0f); // every pixel flat
            CHECK_EQ(cells[2], 0.0f); // no gradient
            const mz::Grid& grid = light ? mz::kLightGrid : mz::kFullGrid;
            uint8_t codes[mz::kZones];
            mz::decideAll(cells.data(), 2.0f, true, codes, grid);
            int law = 0;
            for (int z = 0; z < grid.zones(); ++z)
                law += codes[z] == 0;
            CHECK_EQ(law, grid.zones());
        }
    }

    // ── What the pass costs, timed on its own dispatches: once ──
    {
        const int w = 256, h = 144;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 90);
        ComPtr<ID3D11ShaderResourceView> view;
        CHECK(upload(px, w, h, view));
        for (const bool light : {false, true}) {
            MwsrStats stats;
            std::string error;
            CHECK(stats.init(device.Get(), w, h, light, error));
            std::vector<float> cells(MwsrStats::kFloats);
            int64_t costUs = -1;
            int frames = 0;
            bool taken = false;
            // Each dispatch reads what the GPU has finished of the ones
            // before; the read that waits keeps it from falling behind.
            while (!taken && frames < 200) {
                CHECK(stats.read(stats.dispatch(view.Get()), cells.data(), true));
                ++frames;
                taken = stats.takeCost(costUs);
            }
            CHECK(taken);
            // the warm-up and the median's samples, at the least
            CHECK(frames >= convert::ResampleCost::kWarmupFrames +
                                static_cast<int>(convert::ResampleCost::kSamples));
            CHECK(costUs >= 0);
            CHECK(!stats.takeCost(costUs)); // once
        }
    }

    // ── The ring: a ticket reads until its buffer goes to a later dispatch ──
    {
        const int w = 128, h = 72;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 200);
        ComPtr<ID3D11ShaderResourceView> view;
        CHECK(upload(px, w, h, view));
        MwsrStats stats;
        std::string error;
        CHECK(stats.init(device.Get(), w, h, false, error));
        std::vector<float> cells(MwsrStats::kFloats);
        const uint64_t first = stats.dispatch(view.Get());
        uint64_t last = first;
        for (int k = 1; k < MwsrStats::kSlots; ++k)
            last = stats.dispatch(view.Get());
        CHECK(stats.read(first, cells.data(), true));
        CHECK(stats.read(last, cells.data(), true));
        stats.dispatch(view.Get());
        CHECK(!stats.read(first, cells.data(), true));
        CHECK(!stats.read(0, cells.data(), true));
    }

    // ── Refused: a capture with a cell of no pixel ──
    {
        MwsrStats stats;
        std::string error;
        CHECK(!stats.init(device.Get(), 100, 72, false, error));
        CHECK(!stats.init(device.Get(), 100, 72, true, error));
        CHECK(!error.empty());
        CHECK_EQ(stats.dispatch(nullptr), uint64_t(0));
    }

    SECTION("MwsrStats: taken by the colour conversion, on WARP");

    // ── The converter takes them where the grid was measured, nowhere else ──
    {
        using convert::ColorConvert;
        using convert::ScaleFilter;
        const auto build = [&](int sw, int sh, int ow, int oh, ScaleFilter filter,
                               ColorConvert& conv) {
            std::string error;
            return conv.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, sw, sh, ow, oh,
                             ColorConvert::Chroma::C420, false, filter, error);
        };
        std::string why;
        {
            ColorConvert conv;
            CHECK(build(512, 288, 512, 288, ScaleFilter::Bilinear, conv));
            CHECK(!conv.enableMwsrStats(why)); // 1:1
            CHECK(!why.empty());
        }
        {
            ColorConvert conv;
            CHECK(build(512, 288, 256, 256, ScaleFilter::Bilinear, conv));
            CHECK(!conv.enableMwsrStats(why)); // another shape
        }
        {
            ColorConvert conv;
            CHECK(build(512, 288, 384, 216, ScaleFilter::Lanczos2, conv));
            CHECK(conv.enableMwsrStats(why));
            CHECK(!conv.mwsrStatsLight());
            CHECK(conv.mwsrScale() > 1.333f && conv.mwsrScale() < 1.334f);
            CHECK(conv.reducesInLinearLight());
            CHECK(conv.dropResample());
            CHECK(!conv.reducesInLinearLight());
            // the light pass in its place, then none
            CHECK(conv.enableMwsrStats(why, true));
            CHECK(conv.mwsrStatsLight());
            int64_t costUs = 0;
            CHECK(!conv.takeMwsrStatsCost(costUs)); // nothing timed yet
            conv.disableMwsrStats();
            CHECK(!conv.mwsrStatsLight());
            std::vector<float> cells(MwsrStats::kFloats);
            CHECK(!conv.readMwsrCells(cells.data()));
        }
    }

    // ── A converted picture's cells, read at once: the read waits for the GPU ──
    {
        using convert::ColorConvert;
        const int w = 512, h = 288;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                const bool ink = x > w / 2 && (y % 12) < 8 && ((x * 5 + y) % 9) < 3;
                p[0] = p[1] = p[2] = ink ? 30 : static_cast<uint8_t>(x * 255 / w);
                p[3] = 255;
            }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA seed = {};
        seed.pSysMem = px.data();
        seed.SysMemPitch = static_cast<UINT>(w * 4);
        ComPtr<ID3D11Texture2D> source;
        CHECK(SUCCEEDED(device->CreateTexture2D(&desc, &seed, source.GetAddressOf())));
        ComPtr<ID3D11ShaderResourceView> view;
        CHECK(SUCCEEDED(
            device->CreateShaderResourceView(source.Get(), nullptr, view.GetAddressOf())));

        ColorConvert conv;
        std::string error;
        CHECK(conv.init(device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, w, h, w / 2, h / 2,
                        ColorConvert::Chroma::C420, false, convert::ScaleFilter::Bilinear, error));
        std::vector<float> cells(MwsrStats::kFloats, -1.0f);
        CHECK(!conv.readMwsrCells(cells.data())); // not taken yet
        CHECK(conv.enableMwsrStats(error));
        // Every picture its own cells, none missed: the stream's read waits.
        for (int picture = 0; picture < 3; ++picture) {
            std::fill(cells.begin(), cells.end(), -1.0f);
            CHECK(conv.convert(source.Get(), capture::CursorState{}, convert::CursorDraw{}, error));
            CHECK(conv.readMwsrCells(cells.data()));
        }

        MwsrStats direct;
        CHECK(direct.init(device.Get(), w, h, false, error));
        std::vector<float> want(MwsrStats::kFloats);
        CHECK(direct.read(direct.dispatch(view.Get()), want.data(), true));
        CHECK(cells == want);

        // And the zones: the gradient on the left, text on the right.
        uint8_t codes[mz::kZones];
        mz::decideAll(cells.data(), conv.mwsrScale(), conv.reducesInLinearLight(), codes);
        int valid = 0;
        for (uint8_t c : codes)
            valid += c < mz::kCodes;
        CHECK_EQ(valid, mz::kZones);
    }
#endif
}
