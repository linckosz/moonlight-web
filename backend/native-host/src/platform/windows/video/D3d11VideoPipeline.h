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

#include "WindowsVideoPipeline.h"

#include "../../../convert/windows/ColorConvert.h"
#include "../../../encode/windows/IVideoEncoder.h"
#include "../CrossGpuBridge.h"

#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace mw::native {

/// The pipeline the engine has always had: ColorConvert's pixel shaders and a
/// vendor encoder, all D3D11, on the capture's device — or on the encoder's
/// GPU, when a CrossGpuBridge carries the frames there.
///
/// Moved out of WindowsSession behind WindowsVideoPipeline without a change of
/// behaviour: the same calls, in the same order, with the same log lines.
class D3d11VideoPipeline final : public WindowsVideoPipeline
{
public:
    D3d11VideoPipeline() = default;
    ~D3d11VideoPipeline() override;

    const char* kind() const override { return "d3d11"; }

    bool open(bool crossGpuCopy, uint64_t encodeAdapterLuid, const std::string& encodeGpuName,
              std::string& error) override;
    void teardown(bool keepHeld) override;
    void raisePriority() override;
    bool buildConverter(const capture::IWindowsCapture& capture, const ConverterBuild& build,
                        std::string& error) override;
    bool buildEncoder(const capture::IWindowsCapture& capture, const EncoderBuild& build,
                      std::string& error) override;
    void close() override;
    bool hasEncoder() const override { return m_Encoder != nullptr; }
    bool built() const override { return m_Converter && m_Encoder; }

    bool convert(capture::IWindowsCapture& capture, ID3D11Texture2D* captured,
                 const capture::CursorState& cursor, const convert::CursorDraw& draw, bool retain,
                 std::string& error) override;
    bool hasHeld() const override { return m_DesktopCopy != nullptr; }
    bool convertHeld(capture::IWindowsCapture& capture, const capture::CursorState& cursor,
                     const convert::CursorDraw& draw, std::string& error) override;
    bool prepareBlank(const capture::IWindowsCapture* capture, std::string& error) override;
    bool hasBlank() const override { return m_Blank != nullptr; }
    bool convertBlank(const convert::CursorDraw& draw, std::string& error) override;
    void releaseBlank() override { m_Blank.Reset(); }
    void beforeCaptureRelease() override {}
    void forgetCaptured() override;

    EncodeResult encode(bool forceKeyframe, uint32_t frameNumber, encode::EncoderOutput& out,
                        std::string& error) override;
    void releaseOutput() override { m_Encoder->releaseOutput(); }
    bool setBitrate(int bitrateKbps, std::string& error) override
    {
        return m_Encoder->setBitrate(bitrateKbps, error);
    }
    bool intraRefreshEnabled() const override { return m_Encoder->intraRefreshEnabled(); }
    int intraRefreshHorizonFrames() const override
    {
        return m_Encoder->intraRefreshHorizonFrames();
    }
    bool supportsReferenceInvalidation() const override
    {
        return m_Encoder->supportsReferenceInvalidation();
    }
    bool invalidateReference(uint32_t frameNumber, std::string& error) override
    {
        return m_Encoder->invalidateReference(frameNumber, error);
    }

    int outputWidth() const override { return m_Converter ? m_Converter->outputWidth() : 0; }
    int outputHeight() const override { return m_Converter ? m_Converter->outputHeight() : 0; }
    /// One GPU→CPU read of the bitstream; the bridge adds the readback and the
    /// upload of every frame.
    int copiesPerFrame() const override { return m_Bridge ? 3 : 1; }
    bool toneMapsToSdr() const override { return m_Converter->toneMapsToSdr(); }
    bool scRgbSource() const override { return m_Converter->scRgbSource(); }
    void setSdrWhite(float scRgbWhite) override { m_Converter->setSdrWhite(scRgbWhite); }

    convert::ScaleFilter scaleFilter() const override { return m_Converter->scaleFilter(); }
    bool takeResampleCost(int64_t& costUs) override
    {
        return m_Converter->takeResampleCost(costUs);
    }
    bool dropResample() override { return m_Converter->dropResample(); }

    bool takeMwsrZones(uint8_t* codes, float& scale) override;

    void logEndOfSession() const override;

    /// What the converter last wrote — NV12, P010 or AYUV — for the tests,
    /// which compare it with ColorConvert's own. Null before a build.
    ID3D11Texture2D* output() const { return m_Converter ? m_Converter->output() : nullptr; }

private:
    /// The device the converter and the encoder are built on: the capture's,
    /// or the bridge's when the encoder sits on another GPU.
    ID3D11Device* pipelineDevice(const capture::IWindowsCapture* capture) const;

    /// The texture the converter is to read for @p captured — the texture
    /// itself in the ordinary case, or its copy on the encoder's GPU when the
    /// two differ. Null, with @p error set, when the copy failed.
    ID3D11Texture2D* pictureFor(capture::IWindowsCapture& capture, ID3D11Texture2D* captured,
                                std::string& error);

    /// Keep a private copy of the captured desktop, so a later frame that only
    /// moved the cursor can be rebuilt without a fresh capture.
    bool retainDesktop(capture::IWindowsCapture& capture, ID3D11Texture2D* source,
                       std::string& error);

    // Declared in the reverse of the order they go in: destroyed — and closed —
    // encoder first, then the converter, the held desktop, and the bridge the
    // two of them may live on last.
    /// Present only when the encoder sits on another GPU than the display's:
    /// carries every frame across, and owns the device the converter and the
    /// encoder are then built on. See CrossGpuBridge.
    std::unique_ptr<CrossGpuBridge> m_Bridge;
    /// The last captured desktop, kept only while the pointer is drawn and on
    /// this screen — see retainDesktop() — and let go by the first conversion
    /// that does without it. On the CAPTURE's device.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_DesktopCopy;
    std::unique_ptr<convert::ColorConvert> m_Converter;
    /// Held by interface: which vendor path this is was decided by the
    /// Selector, and the loop neither knows nor needs to.
    std::unique_ptr<encode::IVideoEncoder> m_Encoder;
    /// The picture sent while the display is away, from prepareBlank() to
    /// releaseBlank(). On the converter's device.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Blank;
    /// MWSR's cells of the last picture, read back for the zones' decision:
    /// sized once the converter takes the statistics.
    std::vector<float> m_MwsrCells;
};

} // namespace mw::native
