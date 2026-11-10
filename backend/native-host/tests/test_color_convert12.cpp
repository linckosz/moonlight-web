/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "capture/windows/DisplayRotation.h"
#include "convert/windows/ColorConvert.h"
#include "convert/windows/d3d12/ColorConvert12.h"
#include "platform/windows/IndirectDisplay.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace mw::native;
using Microsoft::WRL::ComPtr;

namespace {

/// Round to nearest even; the test's values stay clear of subnormals.
uint16_t half(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) return static_cast<uint16_t>(sign);
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    uint32_t h = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h;
    return static_cast<uint16_t>(h);
}

/// A picture with something different in every pixel, so a shifted or
/// mis-scaled conversion cannot pass for the right one: BGRA8, or FP16 scRGB
/// from 0 to about 6 (SDR white and well above it).
std::vector<uint8_t> picture(int w, int h, bool fp16, bool black)
{
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * (fp16 ? 8 : 4), 0);
    if (black) return out;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!fp16) {
                uint8_t* p = &out[i * 4];
                p[0] = static_cast<uint8_t>(x * 255 / (w - 1));
                p[1] = static_cast<uint8_t>(y * 255 / (h - 1));
                p[2] = static_cast<uint8_t>(((x + y) * 7) & 0xff);
                p[3] = 255;
            } else {
                const uint16_t rgba[4] = {half(6.0f * x / (w - 1)), half(3.0f * y / (h - 1)),
                                          half(static_cast<float>((x * 3 + y) % 17) / 4.0f),
                                          half(1.0f)};
                std::memcpy(&out[i * 8], rgba, sizeof(rgba));
            }
        }
    }
    return out;
}

/// @p upright as Desktop Duplication hands it over for a display turned
/// @p turn quarter turns against its panel (DisplayRotation.h's panelTexel):
/// h × w for an odd turn.
std::vector<uint8_t> panelPicture(const std::vector<uint8_t>& upright, int w, int h, int bpp,
                                  int turn)
{
    const int tw = (turn & 1) ? h : w;
    std::vector<uint8_t> out(upright.size(), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int tx = 0, ty = 0;
            capture::panelTexel(x, y, w, h, turn, tx, ty);
            std::memcpy(&out[(static_cast<size_t>(ty) * tw + tx) * bpp],
                        &upright[(static_cast<size_t>(y) * w + x) * bpp], static_cast<size_t>(bpp));
        }
    }
    return out;
}

/// The largest difference between two conversions' samples, in codes (P010's
/// in its ten bits), and in @p differ how many samples differ at all.
int worstCodes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int bytesPerSample,
               size_t& differ)
{
    differ = 0;
    if (a.size() != b.size()) return 1 << 16;
    int worst = 0;
    for (size_t i = 0; i + bytesPerSample <= a.size(); i += bytesPerSample) {
        int va = a[i], vb = b[i];
        if (bytesPerSample == 2) {
            va = (a[i] | (a[i + 1] << 8)) >> 6;
            vb = (b[i] | (b[i + 1] << 8)) >> 6;
        }
        const int d = va > vb ? va - vb : vb - va;
        if (d) ++differ;
        if (d > worst) worst = d;
    }
    return worst;
}

/// A 16×16 pointer: a coloured, half-transparent disc, and an inverting bar.
capture::CursorState pointer(int x, int y)
{
    capture::CursorState cursor;
    cursor.visible = true;
    cursor.x = x;
    cursor.y = y;
    cursor.width = 16;
    cursor.height = 16;
    cursor.shapeVersion = 7;
    cursor.pixels.assign(16 * 16 * 4, 0);
    cursor.invert.assign(16 * 16, 0);
    for (int py = 0; py < 16; ++py) {
        for (int px = 0; px < 16; ++px) {
            uint8_t* p = &cursor.pixels[(static_cast<size_t>(py) * 16 + px) * 4];
            const int dx = px - 6, dy = py - 6;
            if (dx * dx + dy * dy <= 25) {
                p[0] = 40;
                p[1] = 200;
                p[2] = 250;
                p[3] = static_cast<uint8_t>(128 + px * 8);
            }
            if (px >= 12 && py >= 2 && py < 14)
                cursor.invert[static_cast<size_t>(py) * 16 + px] = 255;
        }
    }
    return cursor;
}

ComPtr<ID3D11Texture2D> texture11(ID3D11Device* device, int w, int h, DXGI_FORMAT format,
                                  const std::vector<uint8_t>& data)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA seed = {};
    seed.pSysMem = data.data();
    seed.SysMemPitch = static_cast<UINT>(data.size() / h);
    ComPtr<ID3D11Texture2D> texture;
    device->CreateTexture2D(&desc, &seed, &texture);
    return texture;
}

/// An NV12 or P010 texture's two planes, rows packed: luma, then chroma.
std::vector<uint8_t> planes11(ID3D11Device* device, ID3D11DeviceContext* context,
                              ID3D11Texture2D* planar, int w, int h, int bytesPerSample)
{
    std::vector<uint8_t> out;
    D3D11_TEXTURE2D_DESC desc = {};
    planar->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return out;
    context->CopyResource(staging.Get(), planar);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return out;
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    const size_t row = static_cast<size_t>(w) * bytesPerSample;
    for (int y = 0; y < h; ++y)
        out.insert(out.end(), bytes + y * mapped.RowPitch, bytes + y * mapped.RowPitch + row);
    const uint8_t* chroma = bytes + static_cast<size_t>(mapped.RowPitch) * desc.Height;
    for (int y = 0; y < h / 2; ++y)
        out.insert(out.end(), chroma + y * mapped.RowPitch, chroma + y * mapped.RowPitch + row);
    context->Unmap(staging.Get(), 0);
    return out;
}

/// WARP's D3D12 side: a queue, a list, a fence, and the uploads and
/// readbacks a test needs around ColorConvert12's recording.
struct Rig
{
    std::shared_ptr<d3d12::D3d12Device> device;
    d3d12::Queue queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    d3d12::GpuFence fence;
    std::vector<ComPtr<ID3D12Resource>> keep;

    bool init(IDXGIAdapter1* adapter, std::string& error)
    {
        device = d3d12::D3d12Device::forAdapter(adapter, error);
        if (!device) return false;
        d3d12::QueueRequest request;
        request.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!device->createQueue(request, queue, error)) return false;
        if (FAILED(device->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator))) ||
            FAILED(device->device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       allocator.Get(), nullptr,
                                                       IID_PPV_ARGS(&list))) ||
            FAILED(list->Close())) {
            error = "no command list";
            return false;
        }
        return fence.create(device->device(), false, error);
    }

    ID3D12GraphicsCommandList* begin()
    {
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        return list.Get();
    }

    bool run(std::string& error)
    {
        if (FAILED(list->Close())) {
            error = "the list was refused";
            return false;
        }
        ID3D12CommandList* lists[] = {list.Get()};
        queue.queue->ExecuteCommandLists(1, lists);
        const uint64_t value = fence.signal(queue.queue.Get(), error);
        const bool done = value && fence.wait(value, 10000, error) == d3d12::GpuFence::Wait::Done;
        keep.clear();
        return done;
    }

    ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;
        device->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                  nullptr, IID_PPV_ARGS(&out));
        return out;
    }

    /// A texture holding @p data, left in COMMON as the duplication's surface
    /// arrives.
    ComPtr<ID3D12Resource> texture(int w, int h, DXGI_FORMAT format,
                                   const std::vector<uint8_t>& data, std::string& error)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(w);
        desc.Height = static_cast<UINT>(h);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        if (FAILED(device->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST,
                                                             nullptr, IID_PPV_ARGS(&texture))))
            return nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
        UINT64 total = 0;
        device->device()->GetCopyableFootprints(&desc, 0, 1, 0, &foot, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> upload =
            buffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* mapped = nullptr;
        if (!upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
            return nullptr;
        const size_t row = data.size() / h;
        for (int y = 0; y < h; ++y)
            std::memcpy(mapped + foot.Offset + y * static_cast<size_t>(foot.Footprint.RowPitch),
                        data.data() + y * row, row);
        upload->Unmap(0, nullptr);
        ID3D12GraphicsCommandList* l = begin();
        D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
        to.pResource = texture.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = foot;
        l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        l->ResourceBarrier(1, &barrier);
        keep.push_back(upload);
        if (!run(error)) return nullptr;
        return texture;
    }

    /// The two planes of @p planar (in COMMON), rows packed: the top-left
    /// @p w × @p h of luma, then the matching chroma.
    std::vector<uint8_t> planes(ID3D12Resource* planar, int w, int h, int bytesPerSample,
                                std::string& error)
    {
        std::vector<uint8_t> out;
        const D3D12_RESOURCE_DESC desc = planar->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT feet[2] = {};
        UINT rows[2] = {};
        UINT64 rowBytes[2] = {};
        UINT64 total = 0;
        device->device()->GetCopyableFootprints(&desc, 0, 2, 0, feet, rows, rowBytes, &total);
        ComPtr<ID3D12Resource> readback =
            buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!readback) return out;
        ID3D12GraphicsCommandList* l = begin();
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = planar;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        l->ResourceBarrier(1, &barrier);
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
            to.pResource = readback.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = feet[plane];
            from.pResource = planar;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = plane;
            l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        l->ResourceBarrier(1, &barrier);
        if (!run(error)) return out;
        uint8_t* mapped = nullptr;
        const D3D12_RANGE range = {0, static_cast<SIZE_T>(total)};
        if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) return out;
        const size_t row = static_cast<size_t>(w) * bytesPerSample;
        for (int y = 0; y < h; ++y) {
            const uint8_t* p =
                mapped + feet[0].Offset + y * static_cast<size_t>(feet[0].Footprint.RowPitch);
            out.insert(out.end(), p, p + row);
        }
        for (int y = 0; y < h / 2; ++y) {
            const uint8_t* p =
                mapped + feet[1].Offset + y * static_cast<size_t>(feet[1].Footprint.RowPitch);
            out.insert(out.end(), p, p + row);
        }
        const D3D12_RANGE none = {0, 0};
        readback->Unmap(0, &none);
        return out;
    }
};

struct Case
{
    const char* name;
    bool fp16;
    int sourceW, sourceH, outputW, outputH;
    bool hdr;
    convert::ScaleFilter filter;
    float sdrWhite;
    bool cursor;
    float magnify;
    bool black;
    /// The capture's turn (IWindowsCapture::turn): the source handed over as
    /// a turned display's panel scans it out, sourceW × sourceH being the
    /// upright picture.
    int turn = 0;
};

} // namespace
#endif

// ColorConvert12 against ColorConvert (plan pipeline-video-d3d12-v2, C3.2-
// C3.3): the same HLSL, the same bytecode, WARP on both sides — so the same
// bytes, exactly, for every path a stream can take through the conversion.
void run_color_convert12_tests()
{
#if defined(_WIN32)
    SECTION("ColorConvert12 — the same bytes as ColorConvert, WARP D3D11 against WARP D3D12");

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> warp;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
        std::fprintf(stderr, "  no WARP adapter here — skipped\n");
        return;
    }
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(::D3D11CreateDevice(warp.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                   &device11, nullptr, &context))) {
        std::fprintf(stderr, "  no D3D11 on WARP — skipped\n");
        return;
    }
    Rig rig;
    std::string error;
    if (!rig.init(warp.Get(), error)) {
        std::fprintf(stderr, "  no D3D12 on WARP (%s) — skipped\n", error.c_str());
        return;
    }

    using SF = convert::ScaleFilter;
    const Case cases[] = {
        {"NV12 1:1", false, 64, 64, 64, 64, false, SF::Bilinear, 1.0f, false, 1.0f, false},
        {"NV12 halved, bilinear in the pass", false, 64, 64, 32, 32, false, SF::Bilinear, 1.0f,
         false, 1.0f, false},
        {"Lanczos-2 at 1440 -> 1080's ratio", false, 256, 144, 192, 108, false, SF::Lanczos2, 1.0f,
         false, 1.0f, false},
        {"Lanczos-2 letterboxed", false, 64, 32, 32, 32, false, SF::Lanczos2, 1.0f, false, 1.0f,
         false},
        {"pointer, colour and inversion", false, 64, 64, 64, 64, false, SF::Bilinear, 1.0f, true,
         1.0f, false},
        {"pointer magnified, through the resample", false, 256, 144, 192, 108, false, SF::Lanczos2,
         1.0f, true, 2.0f, false},
        {"tone map of FP16 at SDR white 2.5", true, 64, 64, 64, 64, false, SF::Bilinear, 2.5f, true,
         1.0f, false},
        {"tone map through the resample", true, 128, 72, 96, 54, false, SF::Lanczos2, 2.5f, false,
         1.0f, false},
        {"P010, BT.2020 PQ", true, 64, 64, 64, 64, true, SF::Bilinear, 1.0f, true, 1.0f, false},
        {"black, 8-bit", false, 64, 64, 64, 64, false, SF::Bilinear, 1.0f, false, 1.0f, true},
        {"black, P010", true, 64, 64, 64, 64, true, SF::Bilinear, 1.0f, false, 1.0f, true},
        // Issue #54: a display turned against its panel, of another width
        // than height so that a swapped axis cannot pass.
        {"turned a quarter, 1:1, pointer", false, 64, 48, 64, 48, false, SF::Bilinear, 1.0f, true,
         1.0f, false, 1},
        {"turned a half, halved bilinear", false, 64, 48, 32, 24, false, SF::Bilinear, 1.0f, false,
         1.0f, false, 2},
        {"turned three quarters, Lanczos-2, pointer", false, 256, 144, 192, 108, false,
         SF::Lanczos2, 1.0f, true, 1.0f, false, 3},
        {"turned a quarter, tone map through the resample", true, 128, 72, 96, 54, false,
         SF::Lanczos2, 2.5f, false, 1.0f, false, 1},
        {"turned three quarters, P010 1:1, pointer", true, 64, 48, 64, 48, true, SF::Bilinear, 1.0f,
         true, 1.0f, false, 3},
    };

    for (const Case& c : cases) {
        const DXGI_FORMAT format =
            c.fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        const int bytesPerSample = c.hdr ? 2 : 1;
        const std::vector<uint8_t> pixels = picture(c.sourceW, c.sourceH, c.fp16, c.black);
        // What the capture hands over: the picture, or a turned display's as
        // its panel scans it out.
        const bool odd = (c.turn & 1) != 0;
        const int texW = odd ? c.sourceH : c.sourceW;
        const int texH = odd ? c.sourceW : c.sourceH;
        const std::vector<uint8_t> panel =
            c.turn ? panelPicture(pixels, c.sourceW, c.sourceH, c.fp16 ? 8 : 4, c.turn) : pixels;
        const capture::CursorState cursor = c.cursor ? pointer(20, 9) : capture::CursorState{};
        convert::CursorDraw draw;
        draw.magnify = c.magnify;
        draw.hotspotX = 3;
        draw.hotspotY = 2;

        convert::ColorConvert direct;
        std::string why;
        ComPtr<ID3D11Texture2D> source11 = texture11(device11.Get(), texW, texH, format, panel);
        if (!source11 ||
            !direct.init(device11.Get(), format, c.sourceW, c.sourceH, c.outputW, c.outputH,
                         convert::ColorConvert::Chroma::C420, c.hdr, c.filter, why, c.turn)) {
            std::fprintf(stderr, "  %s: D3D11 on WARP refuses it (%s) — skipped\n", c.name,
                         why.c_str());
            continue;
        }
        direct.setSdrWhite(c.sdrWhite);
        CHECK(direct.convert(source11.Get(), cursor, draw, why));
        const std::vector<uint8_t> expected =
            planes11(device11.Get(), context.Get(), direct.output(), direct.outputWidth(),
                     direct.outputHeight(), bytesPerSample);

        // Turned, the picture is the upright one's: the same conversion of
        // the upright texture, within a code — the bilinear fetch averages
        // the same texels with the axes the other way round.
        if (c.turn) {
            convert::ColorConvert upright;
            ComPtr<ID3D11Texture2D> straight =
                texture11(device11.Get(), c.sourceW, c.sourceH, format, pixels);
            const bool built =
                straight &&
                upright.init(device11.Get(), format, c.sourceW, c.sourceH, c.outputW, c.outputH,
                             convert::ColorConvert::Chroma::C420, c.hdr, c.filter, why);
            CHECK(built);
            if (built) {
                upright.setSdrWhite(c.sdrWhite);
                CHECK(upright.convert(straight.Get(), cursor, draw, why));
                const std::vector<uint8_t> want =
                    planes11(device11.Get(), context.Get(), upright.output(), upright.outputWidth(),
                             upright.outputHeight(), bytesPerSample);
                size_t off = 0;
                const int worst = worstCodes(want, expected, bytesPerSample, off);
                const bool close = !want.empty() && worst <= 1;
                CHECK(close);
                std::fprintf(stderr, "  %s: %s the upright picture's (%zu samples off, by %d)\n",
                             c.name, close ? "as" : "NOT", off, worst);
            }
        }

        convert::ColorConvert12 converter;
        if (!converter.init(rig.device->device(), rig.queue.queue.Get(), format, c.sourceW,
                            c.sourceH, c.outputW, c.outputH, 0, 0, c.hdr, c.filter, why, c.turn)) {
            std::fprintf(stderr, "  %s: D3D12 on WARP refuses it (%s)\n", c.name, why.c_str());
            CHECK(false);
            continue;
        }
        converter.setSdrWhite(c.sdrWhite);
        CHECK(converter.scaleFilter() == direct.scaleFilter());
        CHECK(converter.letterboxed() == direct.letterboxed());
        CHECK(converter.outputWidth() == direct.outputWidth());
        CHECK(converter.outputHeight() == direct.outputHeight());

        ComPtr<ID3D12Resource> source12 = rig.texture(texW, texH, format, panel, error);
        CHECK(source12 != nullptr);
        if (!source12) continue;
        ID3D12GraphicsCommandList* list = rig.begin();
        const bool recorded =
            c.black ? converter.recordClearBlack(list, why)
                    : converter.recordConvert(list, source12.Get(), cursor, draw, why);
        CHECK(recorded);
        CHECK(rig.run(error));
        converter.frameCompleted();
        const std::vector<uint8_t> got =
            rig.planes(converter.output(), converter.outputWidth(), converter.outputHeight(),
                       bytesPerSample, error);

        const bool same = !expected.empty() && got == expected;
        CHECK(same);
        size_t differ = 0, first = 0;
        for (size_t i = 0; i < expected.size() && i < got.size(); ++i)
            if (expected[i] != got[i] && differ++ == 0) first = i;
        std::fprintf(stderr, "  %s: %s", c.name, same ? "identical" : "DIFFERENT");
        if (!same && differ)
            std::fprintf(stderr, " (%zu of %zu bytes, first at %zu: %u vs %u)", differ,
                         expected.size(), first, expected[first], got[first]);
        std::fprintf(stderr, "\n");
    }

    // The coded size: the picture top-left, the band outside it black once.
    {
        const std::vector<uint8_t> pixels = picture(64, 60, false, false);
        convert::ColorConvert direct;
        convert::ColorConvert12 converter;
        std::string why;
        ComPtr<ID3D11Texture2D> source11 =
            texture11(device11.Get(), 64, 60, DXGI_FORMAT_B8G8R8A8_UNORM, pixels);
        CHECK(direct.init(device11.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 64, 60, 64, 60,
                          convert::ColorConvert::Chroma::C420, false, SF::Bilinear, why));
        CHECK(direct.convert(source11.Get(), capture::CursorState{}, convert::CursorDraw{}, why));
        CHECK(converter.init(rig.device->device(), rig.queue.queue.Get(),
                             DXGI_FORMAT_B8G8R8A8_UNORM, 64, 60, 64, 60, 64, 64, false,
                             SF::Bilinear, why));
        CHECK(converter.codedWidth() == 64 && converter.codedHeight() == 64);
        ComPtr<ID3D12Resource> source12 =
            rig.texture(64, 60, DXGI_FORMAT_B8G8R8A8_UNORM, pixels, error);
        if (source12 && converter.output()) {
            ID3D12GraphicsCommandList* list = rig.begin();
            CHECK(converter.recordConvert(list, source12.Get(), capture::CursorState{},
                                          convert::CursorDraw{}, why));
            CHECK(rig.run(error));
            const std::vector<uint8_t> coded = rig.planes(converter.output(), 64, 64, 1, error);
            const std::vector<uint8_t> expected =
                planes11(device11.Get(), context.Get(), direct.output(), 64, 60, 1);
            bool picture = coded.size() == 64 * 64 * 3 / 2 && expected.size() == 64 * 60 * 3 / 2;
            bool band = picture;
            if (picture) {
                for (size_t i = 0; i < 64 * 60; ++i)
                    picture = picture && coded[i] == expected[i];
                for (size_t i = 0; i < 64 * 30; ++i)
                    picture = picture && coded[64 * 64 + i] == expected[64 * 60 + i];
                for (size_t i = 64 * 60; i < 64 * 64; ++i)
                    band = band && coded[i] == 16;
                for (size_t i = 64 * 64 + 64 * 30; i < coded.size(); ++i)
                    band = band && coded[i] == 128;
            }
            CHECK(picture);
            CHECK(band);
            std::fprintf(stderr, "  coded 64x64 around a 64x60 picture: picture %s, band %s\n",
                         picture ? "identical" : "DIFFERENT", band ? "black" : "NOT black");
        }
    }

    // The desktop kept for re-sends: a copy of the source, converted again
    // without it — the same bytes as converting the source.
    {
        const std::vector<uint8_t> pixels = picture(64, 64, false, false);
        convert::ColorConvert12 converter;
        std::string why;
        CHECK(converter.init(rig.device->device(), rig.queue.queue.Get(),
                             DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64, 64, 64, 0, 0, false, SF::Bilinear,
                             why));
        ComPtr<ID3D12Resource> source12 =
            rig.texture(64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, pixels, error);
        if (source12 && converter.output()) {
            ID3D12GraphicsCommandList* list = rig.begin();
            CHECK(converter.recordCopy(list, source12.Get(), why));
            CHECK(converter.recordConvert(list, source12.Get(), capture::CursorState{},
                                          convert::CursorDraw{}, why));
            CHECK(rig.run(error));
            const std::vector<uint8_t> fromSource =
                rig.planes(converter.output(), 64, 64, 1, error);
            CHECK(converter.held() != nullptr);
            list = rig.begin();
            CHECK(converter.recordConvert(list, converter.held(), capture::CursorState{},
                                          convert::CursorDraw{}, why));
            CHECK(rig.run(error));
            CHECK(rig.planes(converter.output(), 64, 64, 1, error) == fromSource);
            Microsoft::WRL::ComPtr<ID3D12Resource> held = converter.takeHeld();
            CHECK(held != nullptr && converter.held() == nullptr);
            converter.setHeld(held);
            CHECK(converter.held() == held.Get());
        }
    }

    // Two outputs (plan Phase 10, pipelined=1): the second written exactly as
    // the first — its band cleared on its own first use — and each left alone
    // while the other is written.
    {
        const std::vector<uint8_t> pixels = picture(64, 60, false, false);
        convert::ColorConvert12 converter;
        std::string why;
        CHECK(converter.init(rig.device->device(), rig.queue.queue.Get(),
                             DXGI_FORMAT_B8G8R8A8_UNORM, 64, 60, 64, 60, 64, 64, false,
                             SF::Bilinear, why));
        CHECK(converter.outputs() == 1);
        CHECK(converter.addOutput(why));
        CHECK(converter.outputs() == 2 && converter.output(1) != nullptr &&
              converter.output(1) != converter.output(0));
        ComPtr<ID3D12Resource> source12 =
            rig.texture(64, 60, DXGI_FORMAT_B8G8R8A8_UNORM, pixels, error);
        if (source12 && converter.outputs() == 2) {
            const capture::CursorState cursor = pointer(20, 9);
            ID3D12GraphicsCommandList* list = rig.begin();
            converter.selectOutput(0);
            CHECK(
                converter.recordConvert(list, source12.Get(), cursor, convert::CursorDraw{}, why));
            CHECK(rig.run(error));
            const std::vector<uint8_t> first = rig.planes(converter.output(0), 64, 64, 1, error);
            list = rig.begin();
            converter.selectOutput(1);
            CHECK(converter.output() == converter.output(1));
            CHECK(
                converter.recordConvert(list, source12.Get(), cursor, convert::CursorDraw{}, why));
            CHECK(rig.run(error));
            const std::vector<uint8_t> second = rig.planes(converter.output(1), 64, 64, 1, error);
            const bool same = !first.empty() && first == second;
            CHECK(same);
            list = rig.begin();
            CHECK(converter.recordClearBlack(list, why));
            CHECK(rig.run(error));
            const std::vector<uint8_t> black = rig.planes(converter.output(1), 64, 64, 1, error);
            bool cleared = black.size() == 64 * 64 * 3 / 2;
            for (size_t i = 0; cleared && i < black.size(); ++i)
                cleared = black[i] == (i < 64 * 64 ? 16 : 128);
            CHECK(cleared);
            const bool untouched = rig.planes(converter.output(0), 64, 64, 1, error) == first;
            CHECK(untouched);
            std::fprintf(stderr, "  two outputs: second %s, black on the second %s, first %s\n",
                         same ? "identical" : "DIFFERENT", cleared ? "cleared" : "NOT cleared",
                         untouched ? "untouched" : "OVERWRITTEN");
        }
    }

    std::string gone;
    CHECK(!rig.device->removed(gone));
#endif
}

// ColorConvert12 on each real GPU, not WARP (plan C8.2): the steps of a
// session's first picture, each waited for. The N95's UHD Graphics lost its
// device (DXGI_ERROR_DEVICE_HUNG) on the first picture of every HDR session
// on D3D12, whatever the scaling (28/09/2026), while D3D12 Video Encode took
// P010 pictures uploaded by copy. MW_TEST_CONVERT12_STEP=clear|draw runs one
// P010 step alone, in a process of its own: a hung device is the process's
// device on that GPU, gone for every test after it.
void run_color_convert12_gpu_tests()
{
#if defined(_WIN32)
    SECTION("ColorConvert12 — a session's first picture, step by step, on each GPU");

    std::string step = "all";
    if (const char* value = std::getenv("MW_TEST_CONVERT12_STEP"); value && *value) step = value;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "  no DXGI factory — skipped\n");
        return;
    }
    // As the product's first HDR picture: the desktop in FP16, 1440p, to 1080p
    // coded at 1088 — the band below cleared once.
    struct GpuCase
    {
        const char* name;
        bool hdr;
    };
    const GpuCase cases[] = {{"NV12 (tone map)", false}, {"P010 (BT.2020 PQ)", true}};
    std::set<uint64_t> seen;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
            !seen.insert(d3d12::luidValue(desc.AdapterLuid)).second ||
            platform::isIndirectDisplayOnly(desc.AdapterLuid))
            continue;
        Rig rig;
        std::string error;
        if (!rig.init(adapter.Get(), error)) continue;
        const std::string gpu = rig.device->name();
        const std::vector<uint8_t> pixels = picture(2560, 1440, true, false);
        ComPtr<ID3D12Resource> source =
            rig.texture(2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT, pixels, error);
        CHECK(source != nullptr);
        if (!source) continue;
        for (const GpuCase& c : cases) {
            // One P010 step alone asks for P010 alone.
            if (step != "all" && !c.hdr) continue;
            convert::ColorConvert12 converter;
            std::string why;
            if (!converter.init(rig.device->device(), rig.queue.queue.Get(),
                                DXGI_FORMAT_R16G16B16A16_FLOAT, 2560, 1440, 1920, 1080, 1920, 1088,
                                c.hdr, convert::ScaleFilter::Bilinear, why)) {
                std::fprintf(stderr, "  %s, %s: refused (%s)\n", gpu.c_str(), c.name, why.c_str());
                CHECK(false);
                continue;
            }
            converter.setSdrWhite(2.5f);
            const auto runStep = [&](const char* what, bool draw) {
                ID3D12GraphicsCommandList* list = rig.begin();
                const bool recorded =
                    draw ? converter.recordConvert(list, source.Get(), capture::CursorState{},
                                                   convert::CursorDraw{}, why)
                         : converter.recordClearBlack(list, why);
                const bool ran = recorded && rig.run(error);
                std::string removed;
                const bool lost = rig.device->removed(removed);
                std::fprintf(stderr, "  %s, %s, %s: %s\n", gpu.c_str(), c.name, what,
                             lost  ? ("DEVICE LOST — " + removed).c_str()
                             : ran ? "done"
                                   : ("failed — " + (recorded ? error : why)).c_str());
                CHECK(ran && !lost);
                return ran && !lost;
            };
            bool alive = true;
            if (step == "all" || step == "clear") alive = runStep("the clear to black", false);
            if (alive && (step == "all" || step == "draw")) {
                if (step == "draw") converter.assumeOutputCleared();
                alive =
                    runStep(step == "draw" ? "the conversion, no clear" : "the conversion", true);
            }
            if (!alive) break;
        }
    }
#endif
}
