/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "core/Selector.h"

using namespace mw::native;

namespace {

GpuInfo makeGpu(int id, const char* name, std::vector<EncoderApi> encoders,
                std::vector<Codec> codecs, bool tenBit)
{
    GpuInfo gpu;
    gpu.id = id;
    gpu.name = name;
    gpu.encoders = std::move(encoders);
    gpu.codecs = std::move(codecs);
    gpu.supports10Bit = tenBit;
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
        makeGpu(1, "NVIDIA GeForce RTX 4070", {EncoderApi::Nvenc},
                {Codec::Av1, Codec::Hevc, Codec::H264}, true),
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
}
