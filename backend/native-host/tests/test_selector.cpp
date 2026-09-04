/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "core/Selector.h"

using namespace mw::native;

namespace {

GpuInfo makeGpu(int id, const char* name, std::vector<EncoderApi> encoders,
                std::vector<Codec> codecs, bool tenBit, std::vector<Codec> codecs444 = {})
{
    GpuInfo gpu;
    gpu.id = id;
    gpu.name = name;
    gpu.encoders = std::move(encoders);
    gpu.codecs = std::move(codecs);
    gpu.supports10Bit = tenBit;
    gpu.codecs444 = std::move(codecs444);
    return gpu;
}

DisplayInfo makeDisplay(int id, int gpuId, int w, int h, int refreshMilliHz, bool primary,
                        bool hdrActive)
{
    DisplayInfo d;
    d.id = id;
    d.gpuId = gpuId;
    d.width = w;
    d.height = h;
    d.refreshMilliHz = refreshMilliHz;
    d.primary = primary;
    d.hdrActive = hdrActive;
    d.label = "Display";
    return d;
}

/// A laptop-shaped machine: an Intel iGPU driving the internal panel and an
/// NVIDIA dGPU driving an external 4K HDR screen. This is the layout that makes
/// display→GPU association matter, so most cases below use it.
Capabilities hybridMachine()
{
    Capabilities caps;
    caps.available = true;
    caps.reason = Unavailability::None;
    caps.capture = CaptureApi::DxgiDuplication;
    caps.gpus = {
        makeGpu(0, "Intel Arc iGPU", {EncoderApi::Vpl}, {Codec::Hevc, Codec::H264}, false),
        // As NVENC really answers: 4:4:4 on H.264 and HEVC, not on AV1.
        makeGpu(1, "NVIDIA GeForce RTX 4070", {EncoderApi::Nvenc},
                {Codec::Av1, Codec::Hevc, Codec::H264}, true, {Codec::Hevc, Codec::H264}),
    };
    caps.displays = {
        makeDisplay(0, 0, 1920, 1080, 60000, true, false),
        makeDisplay(1, 1, 3840, 2160, 143980, false, true),
    };
    return caps;
}

} // namespace

void run_selector_tests()
{
    SECTION("Selector — display to GPU association");

    // ── The display's own GPU is used, even when a "better" one exists ───────
    // Display 0 hangs off the weaker Intel iGPU. Picking the RTX would look
    // like an upgrade and would in fact cost a VRAM->RAM->VRAM copy per frame.
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::Av1, Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.gpu->id, 0); // the iGPU that drives this panel
        CHECK_EQ(sel.encoder, EncoderApi::Vpl);
        CHECK(!sel.crossGpuCopy);
    }

    // ── The other display gets its own GPU, and the better codec with it ─────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.clientCodecs = {Codec::Av1, Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.gpu->id, 1);
        CHECK_EQ(sel.encoder, EncoderApi::Nvenc);
        CHECK_EQ(sel.codec, Codec::Av1);
        CHECK(!sel.crossGpuCopy);
    }

    // ── A display whose GPU cannot encode falls back, and says so ────────────
    {
        Capabilities caps = hybridMachine();
        caps.gpus[0].encoders.clear();
        caps.gpus[0].codecs.clear();

        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.gpu->id, 1); // borrowed the RTX
        CHECK(sel.crossGpuCopy);  // and the copy is declared, not hidden
    }

    // ── An encoder with no codec is not an encoder ───────────────────────────
    //
    // Found on the bench: the AMD iGPU reports the AMF runtime but an empty
    // codec list, because AMF's own capability query is not written yet.
    // Falling back to it would buy a cross-GPU copy AND then fail codec
    // negotiation, abandoning the RTX that could have done the job.
    {
        Capabilities caps = hybridMachine();
        caps.gpus[0].encoders.clear();
        caps.gpus[0].codecs.clear();
        // A third GPU that advertises an API but can encode nothing, placed
        // ahead of the good one so a naive scan would pick it.
        caps.gpus.insert(caps.gpus.begin(),
                         makeGpu(2, "Runtime but no codecs", {EncoderApi::Amf}, {}, false));

        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.encoder, EncoderApi::Nvenc); // the RTX, not the empty one
        CHECK(sel.crossGpuCopy);
    }

    // ── No encoder anywhere is a refusal, not a silent software fallback ─────
    // Software encoding is only ever chosen by the probe, which measures it.
    // The selector must not invent it.
    {
        Capabilities caps = hybridMachine();
        for (GpuInfo& gpu : caps.gpus) {
            gpu.encoders.clear();
            gpu.codecs.clear();
        }

        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(!select(caps, cfg, sel, err));
        CHECK(!err.empty());
    }

    SECTION("Selector — codec negotiation");

    // ── The client's preference order wins, filtered by the GPU ──────────────
    // The RTX can do AV1, but a browser that only decodes HEVC and H.264 in
    // hardware must get HEVC — its order is authoritative because it reflects
    // what that browser actually accelerates.
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::Hevc);
    }

    // ── H.264 remains the floor everyone meets on ────────────────────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::H264);
    }

    // ── No common codec is an error with a message, never a black screen ─────
    {
        Capabilities caps = hybridMachine();
        caps.gpus[0].codecs = {Codec::H264};

        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::Av1};

        Selection sel;
        std::string err;
        CHECK(!select(caps, cfg, sel, err));
        CHECK(!err.empty());
    }

    // ── An empty client codec list is rejected rather than guessed at ────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0;

        Selection sel;
        std::string err;
        CHECK(!select(caps, cfg, sel, err));
        CHECK(!err.empty());
    }

    SECTION("Selector — HDR is only claimed when it is real");

    // ── Asked for, and achievable end to end ─────────────────────────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1; // HDR-active, on a 10-bit-capable GPU
        cfg.hdr = true;
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK(sel.hdr);
    }

    // ── Asked for on an SDR display: stream SDR rather than fail ─────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0; // not in an HDR mode
        cfg.hdr = true;
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK(!sel.hdr);
    }

    // ── HDR never rides H.264: 8-bit would be a lie ──────────────────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.hdr = true;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::H264);
        CHECK(!sel.hdr);
    }

    SECTION("Selector — 4:4:4 steers the codec, never fails the session");

    // ── The client prefers AV1, which has no 4:4:4 on NVENC: HEVC carries it ─
    // Before this, the session took AV1 and the encoder refused at init: 4:4:4
    // on + an AV1-capable browser = no stream at all.
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.yuv444 = true;
        cfg.clientCodecs = {Codec::Av1, Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::Hevc);
        CHECK(sel.yuv444);
    }

    // ── Not asked for: the client's first choice stands, 4:2:0 ───────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.clientCodecs = {Codec::Av1, Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::Av1);
        CHECK(!sel.yuv444);
    }

    // ── A browser that only decodes AV1: stream it 4:2:0 and say so ──────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.yuv444 = true;
        cfg.clientCodecs = {Codec::Av1};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::Av1);
        CHECK(!sel.yuv444);
    }

    // ── An encoder with no 4:4:4 at all (AMF, oneVPL today): 4:2:0, same codec
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0; // the iGPU claims no 4:4:4 codec
        cfg.yuv444 = true;
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::Hevc);
        CHECK(!sel.yuv444);
    }

    // ── The client's order still rules among the codecs that carry 4:4:4 ─────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.yuv444 = true;
        cfg.clientCodecs = {Codec::H264, Codec::Hevc};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::H264);
        CHECK(sel.yuv444);
    }

    // ── HDR and 4:4:4 together: 4:4:4 picks HEVC, HEVC keeps HDR ─────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.hdr = true;
        cfg.yuv444 = true;
        cfg.clientCodecs = {Codec::Av1, Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.codec, Codec::Hevc);
        CHECK(sel.yuv444);
        CHECK(sel.hdr);
    }

    SECTION("Selector — geometry defaults");

    // ── Zero means native, which is what makes one click enough ──────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.width, 3840);
        CHECK_EQ(sel.height, 2160);
        // 143.98 Hz must round to 144, not truncate to 143.
        CHECK_EQ(sel.fps, 144);
    }

    // ── An explicit request is honoured verbatim ─────────────────────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.width = 1920;
        cfg.height = 1080;
        cfg.fps = 60;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.width, 1920);
        CHECK_EQ(sel.height, 1080);
        CHECK_EQ(sel.fps, 60);
    }

    SECTION("Selector — default display");

    // ── displayId -1 lands on the primary: the single-screen one-click case ──
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = -1;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.display->id, 0);
    }

    // ── An unknown display is an error, not a silent substitution ────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 99;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(!select(caps, cfg, sel, err));
        CHECK(!err.empty());
    }

    SECTION("Selector — the bench may force the encoder's GPU");

    // ── Forced onto the other GPU: honoured, and the copy is declared ────────
    // Display 0 is on the iGPU; the bench asks for the RTX. This is how an
    // encoder that drives no display gets measured at all.
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.encodeGpuId = 1;
        cfg.clientCodecs = {Codec::Av1, Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.gpu->id, 1);
        CHECK_EQ(sel.encoder, EncoderApi::Nvenc);
        CHECK_EQ(sel.codec, Codec::Av1); // the forced GPU's codecs, not the display's
        CHECK(sel.crossGpuCopy);
    }

    // ── Forced onto the display's own GPU: no copy, nothing to declare ───────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.encodeGpuId = 0;
        cfg.clientCodecs = {Codec::Hevc};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.gpu->id, 0);
        CHECK(!sel.crossGpuCopy);
    }

    // ── A GPU that cannot encode is refused up front, with its name ──────────
    {
        Capabilities caps = hybridMachine();
        caps.gpus[0].encoders.clear();
        caps.gpus[0].codecs.clear();
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.encodeGpuId = 0;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(!select(caps, cfg, sel, err));
        CHECK(err.find("Intel Arc iGPU") != std::string::npos);
    }

    // ── A GPU id that names nothing is an error, not a fallback ──────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.encodeGpuId = 7;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(!select(caps, cfg, sel, err));
        CHECK(!err.empty());
    }

    // ── -1, the default, changes nothing about the ordinary rule ─────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.encodeGpuId = -1;
        cfg.clientCodecs = {Codec::Hevc};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.gpu->id, 0);
        CHECK(!sel.crossGpuCopy);
    }
}
