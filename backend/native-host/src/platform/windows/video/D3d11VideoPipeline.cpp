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

#include "D3d11VideoPipeline.h"

#include "../../../core/Log.h"
#include "../../../encode/windows/AmfEncoder.h"
#include "../../../encode/windows/MfEncoder.h"
#include "../../../encode/windows/NvencEncoder.h"
#include "../../../encode/windows/SoftwareEncoder.h"
#include "../../../encode/windows/VplEncoder.h"
#include "../StreamPriority.h"

#include <cstdio>
#include <vector>

namespace mw::native {

D3d11VideoPipeline::~D3d11VideoPipeline()
{
    close();
}

bool D3d11VideoPipeline::open(bool crossGpuCopy, uint64_t encodeAdapterLuid,
                              const std::string& encodeGpuName, std::string& error)
{
    // Encoding on an adapter other than the one scanning the display out:
    // the frame crosses through system memory (CrossGpuBridge says what
    // that costs). The converter and the encoder are then built on the
    // encoder's device, and every picture passes the bridge on its way to
    // them. Opened once, here: the encoder's adapter does not change when
    // the duplication is lost and reopened.
    m_Bridge.reset();
    if (crossGpuCopy) {
        auto bridge = std::make_unique<CrossGpuBridge>(encodeAdapterLuid);
        if (!bridge->open(error)) return false;
        m_Bridge = std::move(bridge);
        log::warning("[native] cross-GPU copy: the display's frames are carried to '" +
                     encodeGpuName +
                     "' through system memory — every zero-copy "
                     "figure of this engine is off on this session");
    }
    return true;
}

void D3d11VideoPipeline::teardown(bool keepHeld)
{
    // Released before the replacements are built, not after. Both hold a
    // reference to the D3D device they were made on, and an encoder holds a
    // hardware session — of which a consumer GPU has famously few. Building
    // the new one while the old is still open is how a rebuild fails with a
    // vendor error that says nothing about the real cause.
    if (!keepHeld) m_DesktopCopy.Reset();
    m_Encoder.reset();
    m_Converter.reset();
}

void D3d11VideoPipeline::raisePriority()
{
    if (m_Bridge) StreamPriority::raiseDevice(m_Bridge->device(), "encoder");
}

bool D3d11VideoPipeline::buildConverter(const capture::IWindowsCapture& capture,
                                        const ConverterBuild& build, std::string& error)
{
    // On the encoder's device — the capture's, unless a bridge carries the
    // frames to another GPU, in which case both stages live over there and
    // read the bridge's copy.
    m_Converter = std::make_unique<convert::ColorConvert>();
    if (!m_Converter->init(pipelineDevice(&capture), capture.format(), capture.width(),
                           capture.height(), build.outputWidth, build.outputHeight,
                           build.yuv444 ? convert::ColorConvert::Chroma::C444
                                        : convert::ColorConvert::Chroma::C420,
                           build.hdr, build.filter, error))
        return false;

    // MWSR's statistics, when the client reads the zones: never a reason to
    // fail the build — the stream goes without a grid and the client keeps
    // its law.
    if (build.mwsrStats) {
        std::string why;
        if (m_Converter->enableMwsrStats(why)) {
            m_MwsrCells.resize(convert::MwsrStats::kFloats);
            char scale[16];
            std::snprintf(scale, sizeof(scale), "%.3f", m_Converter->mwsrScale());
            log::info(std::string("[native] MWSR zones: taken on every picture (x") + scale + ", " +
                      (m_Converter->reducesInLinearLight() ? "linear light" : "bilinear") + ")");
        } else {
            log::info("[native] MWSR zones: none on this build — " + why);
        }
    }
    return true;
}

bool D3d11VideoPipeline::buildEncoder(const capture::IWindowsCapture& capture,
                                      const EncoderBuild& build, std::string& error)
{
    // The encoder the Selector chose, not one guessed from the display.
    switch (build.encoder) {
    case EncoderApi::Nvenc: m_Encoder = std::make_unique<encode::NvencEncoder>(); break;
    case EncoderApi::Amf: m_Encoder = std::make_unique<encode::AmfEncoder>(); break;
    case EncoderApi::Vpl: m_Encoder = std::make_unique<encode::VplEncoder>(); break;
    case EncoderApi::MediaFoundation: m_Encoder = std::make_unique<encode::MfEncoder>(); break;
    case EncoderApi::Software: m_Encoder = std::make_unique<encode::SoftwareEncoder>(); break;
    default:
        error = std::string("no encoder implementation for ") + toString(build.encoder) + " yet";
        return false;
    }

    return m_Encoder->init(pipelineDevice(&capture), build.codec, m_Converter->outputWidth(),
                           m_Converter->outputHeight(), build.fps, build.bitrateKbps, build.yuv444,
                           build.hdr, build.intraRefresh, build.tuning, error);
}

void D3d11VideoPipeline::close()
{
    m_Encoder.reset();
    m_Converter.reset();
    m_DesktopCopy.Reset();
    m_Blank.Reset();
    // After the converter and the encoder, which live on its device.
    m_Bridge.reset();
}

ID3D11Device* D3d11VideoPipeline::pipelineDevice(const capture::IWindowsCapture* capture) const
{
    if (m_Bridge) return m_Bridge->device();
    return capture ? capture->device() : nullptr;
}

ID3D11Texture2D* D3d11VideoPipeline::pictureFor(capture::IWindowsCapture& capture,
                                                ID3D11Texture2D* captured, std::string& error)
{
    if (!m_Bridge) return captured;
    return m_Bridge->transfer(capture.device(), capture.context(), captured, error);
}

bool D3d11VideoPipeline::convert(capture::IWindowsCapture& capture, ID3D11Texture2D* captured,
                                 const capture::CursorState& cursor,
                                 const convert::CursorDraw& draw, bool retain, std::string& error)
{
    // Through the bridge first when the encoder is on another GPU; the
    // texture itself otherwise. Counted in the convert stage.
    ID3D11Texture2D* picture = pictureFor(capture, captured, error);
    if (!picture || !m_Converter->convert(picture, cursor, draw, error)) return false;

    if (!retain) {
        m_DesktopCopy.Reset();
    } else if (!retainDesktop(capture, captured, error)) {
        m_DesktopCopy.Reset();
        log::warning("[native] could not keep a desktop copy: " + error);
    }
    return true;
}

void D3d11VideoPipeline::forgetCaptured()
{
    // What read them was queued on the capture's own context, and D3D11 keeps
    // a resource alive for the work queued on it: the references are all.
    if (m_Converter) m_Converter->forgetSource();
    if (m_Bridge) m_Bridge->forgetSources();
}

bool D3d11VideoPipeline::convertHeld(capture::IWindowsCapture& capture,
                                     const capture::CursorState& cursor,
                                     const convert::CursorDraw& draw, std::string& error)
{
    ID3D11Texture2D* picture = pictureFor(capture, m_DesktopCopy.Get(), error);
    return picture && m_Converter->convert(picture, cursor, draw, error);
}

bool D3d11VideoPipeline::retainDesktop(capture::IWindowsCapture& capture, ID3D11Texture2D* source,
                                       std::string& error)
{
    if (!source) return false;

    if (!m_DesktopCopy) {
        D3D11_TEXTURE2D_DESC desc = {};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(
                capture.device()->CreateTexture2D(&desc, nullptr, m_DesktopCopy.GetAddressOf()))) {
            error = "the GPU refused a scratch copy of the desktop";
            return false;
        }
    }

    // GPU to GPU, no system memory involved. It is a real cost — roughly a
    // tenth of a millisecond at 1440p — paid only while the pointer is on
    // this screen, and it is what buys a cursor that moves on a still
    // desktop.
    capture.context()->CopyResource(m_DesktopCopy.Get(), source);
    return true;
}

/// A black picture the size and format of what the capture was delivering,
/// on the converter's device (the capture's, or the bridge's) so it can be
/// converted directly. See WindowsSession::restartCapture for what it is for.
bool D3d11VideoPipeline::prepareBlank(const capture::IWindowsCapture* capture, std::string& error)
{
    m_Blank.Reset();
    ID3D11Device* device = pipelineDevice(capture);
    const int width = capture ? capture->width() : 0;
    const int height = capture ? capture->height() : 0;
    if (!device || width <= 0 || height <= 0) {
        error = "the capture has no device to make it on";
        return false;
    }

    // Zero in every channel: black in either 8-bit layout, and 0.0 in FP16
    // scRGB. Alpha is zero too, and the converter ignores it.
    //
    // ⚠️ The pitch follows the format. An HDR session captures FP16, eight
    // bytes a pixel: sized for four, the initial data was half the texture,
    // the driver read past it, and turning Windows HDR off during an HDR
    // stream killed the worker inside nvwgf2umx (15/09/2026, M27Q and a
    // virtual display alike).
    const size_t bytesPerPixel = capture->format() == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = capture->format();
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    const size_t pitch = static_cast<size_t>(width) * bytesPerPixel;
    std::vector<uint8_t> zeros(pitch * static_cast<size_t>(height), 0);
    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = zeros.data();
    initial.SysMemPitch = static_cast<UINT>(pitch);
    if (FAILED(device->CreateTexture2D(&desc, &initial, m_Blank.GetAddressOf()))) {
        error = "the GPU refused a blank picture";
        m_Blank.Reset();
        return false;
    }
    return true;
}

bool D3d11VideoPipeline::convertBlank(const convert::CursorDraw& draw, std::string& error)
{
    // Straight into the converter: the blank already lives on its device, so
    // it never crosses the bridge.
    static const capture::CursorState kNoCursor;
    return m_Converter->convert(m_Blank.Get(), kNoCursor, draw, error);
}

WindowsVideoPipeline::EncodeResult D3d11VideoPipeline::encode(bool forceKeyframe,
                                                              uint32_t frameNumber,
                                                              encode::EncoderOutput& out,
                                                              std::string& error)
{
    return m_Encoder->encode(m_Converter->output(), forceKeyframe, frameNumber, out, error)
               ? EncodeResult::Ok
               : EncodeResult::Failed;
}

bool D3d11VideoPipeline::takeMwsrZones(uint8_t* codes, int& zonesX, int& zonesY, float& scale)
{
    namespace mz = convert::mwsrzones;
    if (!m_Converter || m_MwsrCells.empty()) return false;

    // Bruno's budget (plan B §12 f.4): what the pass costs this GPU, timed on
    // the stream's own frames, decides once whether it stays, gives way to
    // the light pass (timed in turn), or goes. The frames between go without
    // a grid: the new pass's first cells are not back yet.
    int64_t costUs = 0;
    if (m_Converter->takeMwsrStatsCost(costUs)) {
        const bool light = m_Converter->mwsrStatsLight();
        const std::string pass = light ? "light" : "full";
        const std::string cost = " pass costs " + std::to_string(costUs) + " us";
        const std::string budget = std::to_string(convert::ResampleCost::kBudgetUs) + " us";
        std::string why;
        switch (mz::afterPassCost(light, costUs)) {
        case mz::PassStep::Keep:
            log::info("[native] MWSR zones: the " + pass + cost + ", kept");
            break;
        case mz::PassStep::Lighten:
            if (m_Converter->enableMwsrStats(why, true)) {
                log::info("[native] MWSR zones: the full" + cost + ", over " + budget +
                          ": the light pass (16x9) instead");
                break;
            }
            log::info("[native] MWSR zones: the full" + cost + ", over " + budget +
                      ", and no light pass — " + why + ": none from now on");
            m_Converter->disableMwsrStats();
            m_MwsrCells.clear();
            return false;
        case mz::PassStep::Drop:
            log::info("[native] MWSR zones: the light" + cost + ", over " + budget +
                      ": none from now on");
            m_Converter->disableMwsrStats();
            m_MwsrCells.clear();
            return false;
        }
    }

    if (!m_Converter->readMwsrCells(m_MwsrCells.data())) return false;
    const mz::Grid& grid = m_Converter->mwsrStatsLight() ? mz::kLightGrid : mz::kFullGrid;
    scale = m_Converter->mwsrScale();
    mz::decideAll(m_MwsrCells.data(), scale, m_Converter->reducesInLinearLight(), codes, grid);
    zonesX = grid.zonesX;
    zonesY = grid.zonesY;
    return true;
}

void D3d11VideoPipeline::logEndOfSession() const
{
    // What the bridge cost, when there was one: the figure that says how
    // much of this session's convert stage was the copy and not the pass.
    if (m_Bridge && m_Bridge->transfers() > 0) {
        char mean[32], peak[32];
        std::snprintf(mean, sizeof(mean), "%.2f", m_Bridge->meanUs() / 1000.0);
        std::snprintf(peak, sizeof(peak), "%.2f", m_Bridge->maxUs() / 1000.0);
        log::info("[native] cross-GPU copy: " + std::to_string(m_Bridge->transfers()) +
                  " frames of " + std::to_string(m_Bridge->bytesPerFrame() / (1024 * 1024)) +
                  " MB, " + mean + " ms mean, " + peak + " ms max, " +
                  std::to_string(m_Bridge->dmaTransfers()) + " on the copy engine");
    }
}

} // namespace mw::native
