// MWSR — MoonlightWeb Super Resolution: a whole Direct3D 11 integration, as an example and a test.
// Copyright (c) 2026 Bruno Martin <brunoocto@gmail.com>
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Reads a PNG, draws it at another size through MWSR and writes the result as
// a PNG: everything a renderer does with MWSR, in one file. On WARP, the
// software rasteriser of Direct3D, unless --hardware is given.
//
//     mwsr_d3d11 in.png outW outH out.png [--hlsl DIR] [--srgb] [--half] [--hardware]
//                [--define NAME=VALUE]... [--ft T] [--kg S]
//
// --srgb reads the input and writes the output through sRGB views, the
// reduction's exact path; --half takes the upscale whose blend runs in
// min16float; --define sets a constant of the upscale over its default
// (--define CHROMA444=1).
//
// Build, in a Visual Studio developer prompt:
//     cl /std:c++17 /EHsc /W4 /O2 example_d3d11.cpp d3d11.lib d3dcompiler.lib windowscodecs.lib ole32.lib

#include "mwsr.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

[[noreturn]] void fail(const char* what, HRESULT hr = S_OK)
{
    std::fprintf(stderr, "mwsr_d3d11: %s (0x%08lx)\n", what, static_cast<unsigned long>(hr));
    std::exit(1);
}

void check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        fail(what, hr);
}

std::wstring wide(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

// The picture as RGBA, 8 bits a channel, rows from the top.
struct Picture {
    UINT width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

Picture readPng(IWICImagingFactory* wic, const std::string& path)
{
    ComPtr<IWICBitmapDecoder> decoder;
    check(wic->CreateDecoderFromFilename(wide(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                         &decoder),
          "cannot read the input");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "no frame in the input");
    ComPtr<IWICFormatConverter> rgba;
    check(wic->CreateFormatConverter(&rgba), "no format converter");
    check(rgba->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                           WICBitmapPaletteTypeCustom),
          "cannot convert the input to RGBA");
    Picture p;
    check(rgba->GetSize(&p.width, &p.height), "no size");
    p.rgba.resize(size_t(p.width) * p.height * 4);
    check(rgba->CopyPixels(nullptr, p.width * 4, UINT(p.rgba.size()), p.rgba.data()), "cannot copy the input");
    return p;
}

void writePng(IWICImagingFactory* wic, const std::string& path, const Picture& p)
{
    // the PNG encoder takes BGRA everywhere: swap red and blue
    std::vector<unsigned char> bgra(p.rgba);
    for (size_t i = 0; i < bgra.size(); i += 4)
        std::swap(bgra[i], bgra[i + 2]);
    ComPtr<IWICStream> stream;
    check(wic->CreateStream(&stream), "no stream");
    check(stream->InitializeFromFilename(wide(path).c_str(), GENERIC_WRITE), "cannot write the output");
    ComPtr<IWICBitmapEncoder> encoder;
    check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "no PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "cannot start the PNG");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr), "no PNG frame");
    check(frame->Initialize(nullptr), "cannot start the PNG frame");
    check(frame->SetSize(p.width, p.height), "cannot size the PNG");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "no BGRA in the PNG encoder");
    check(frame->WritePixels(p.height, p.width * 4, UINT(bgra.size()), bgra.data()), "cannot write the pixels");
    check(frame->Commit(), "cannot end the PNG frame");
    check(encoder->Commit(), "cannot end the PNG");
}

ComPtr<ID3DBlob> compile(const std::string& file, const char* entry, const char* target,
                         const std::vector<mwsr::Define>& defines)
{
    std::vector<D3D_SHADER_MACRO> macros;
    for (const mwsr::Define& d : defines)
        macros.push_back({d.name.c_str(), d.value.c_str()});
    macros.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompileFromFile(wide(file).c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry,
                                          target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors)
            std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        fail(("cannot compile " + file).c_str(), hr);
    }
    return code;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: mwsr_d3d11 in.png outW outH out.png [--hlsl DIR] [--srgb] [--half] [--hardware] "
                             "[--define NAME=VALUE]... [--ft T] [--kg S]\n");
        return 2;
    }
    const std::string in = argv[1], out = argv[4];
    const int outW = std::atoi(argv[2]), outH = std::atoi(argv[3]);
    std::string hlslDir = "../hlsl";
    bool srgb = false, half = false, hardware = false;
    std::vector<mwsr::Define> up;
    mwsr::Options options;
    for (int i = 5; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--hlsl" && i + 1 < argc)
            hlslDir = argv[++i];
        else if (a == "--srgb")
            srgb = true;
        else if (a == "--half")
            half = true;
        else if (a == "--hardware")
            hardware = true;
        else if (a == "--define" && i + 1 < argc) {
            const std::string d = argv[++i];
            const size_t eq = d.find('=');
            if (eq == std::string::npos)
                fail("--define wants NAME=VALUE");
            up.push_back({d.substr(0, eq), d.substr(eq + 1)});
        } else if (a == "--ft" && i + 1 < argc)
            options.ft = std::atof(argv[++i]);
        else if (a == "--kg" && i + 1 < argc)
            options.kg = std::atof(argv[++i]);
        else
            fail(("unknown argument " + a).c_str());
    }

    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
    ComPtr<IWICImagingFactory> wic;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "WIC");
    const Picture src = readPng(wic.Get(), in);
    const int inW = int(src.width), inH = int(src.height);

    // 1. The mode for this scale, and the shader with its defines.
    const mwsr::Regime reg = mwsr::regime(inW, inH, outW, outH, srgb);
    if (reg.mode == mwsr::Mode::Copy)
        fail("same size in and out: a renderer copies the frame, MWSR has nothing to do");
    const std::string file = hlslDir + "/" + mwsr::shaderFile(reg, half);
    const std::vector<mwsr::Define> defines = mwsr::defines(reg, up);

    // 2. The device: WARP unless asked otherwise.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, &level,
                            1, D3D11_SDK_VERSION, &device, nullptr, &ctx),
          "no Direct3D 11 device");

    // 3. The input texture, with an sRGB view for the exact reduction.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = src.width;
    td.Height = src.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init = {src.rgba.data(), src.width * 4, 0};
    ComPtr<ID3D11Texture2D> input;
    check(device->CreateTexture2D(&td, &init, &input), "no input texture");
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    check(device->CreateShaderResourceView(input.Get(), &sv, &srv), "no input view");

    // 4. The output, the size of the screen.
    td.Width = UINT(outW);
    td.Height = UINT(outH);
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    check(device->CreateTexture2D(&td, nullptr, &target), "no output texture");
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = sv.Format;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;
    check(device->CreateRenderTargetView(target.Get(), &rv, &rtv), "no output view");

    // 5. The sampler: linear, clamp to edge.
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    check(device->CreateSamplerState(&sd, &sampler), "no sampler");

    // 6. The constant buffer, register b0.
    const mwsr::Constants constants = mwsr::constants(inW, inH, outW, outH, options);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof constants;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA cinit = {&constants, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    check(device->CreateBuffer(&bd, &cinit, &cb), "no constant buffer");

    // 7. The two stages, from the same file.
    const ComPtr<ID3DBlob> vsCode = compile(file, "MwsrVS", "vs_5_0", defines);
    const ComPtr<ID3DBlob> psCode = compile(file, "MwsrPS", "ps_5_0", defines);
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "no VS");
    check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "no PS");

    // 8. The draw: one full-screen triangle, no vertex buffer.
    const D3D11_VIEWPORT vp = {0.0f, 0.0f, float(outW), float(outH), 0.0f, 1.0f};
    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ctx->Draw(3, 0);

    // 9. Read the output back.
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&td, nullptr, &staging), "no staging texture");
    ctx->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE map = {};
    check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "cannot read the output");
    Picture dst;
    dst.width = UINT(outW);
    dst.height = UINT(outH);
    dst.rgba.resize(size_t(outW) * outH * 4);
    for (int y = 0; y < outH; ++y)
        std::memcpy(&dst.rgba[size_t(y) * outW * 4], static_cast<const unsigned char*>(map.pData) + size_t(y) * map.RowPitch,
                    size_t(outW) * 4);
    ctx->Unmap(staging.Get(), 0);
    writePng(wic.Get(), out, dst);

    std::printf("%dx%d -> %dx%d, %s, %s%s", inW, inH, outW, outH, mwsr::shaderFile(reg, half).c_str(),
                hardware ? "hardware" : "WARP", srgb ? ", sRGB views" : "");
    for (const mwsr::Define& d : defines)
        std::printf(", %s=%s", d.name.c_str(), d.value.c_str());
    std::printf("\n");
    return 0;
}
