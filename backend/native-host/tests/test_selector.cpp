/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "core/Selector.h"

#include <cmath>

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
        CHECK(sel.hdrCapable);

        // Not asked: SDR, and still capable — what a client reads to know a
        // relaunch asking for HDR would be granted.
        cfg.hdr = false;
        CHECK(select(caps, cfg, sel, err));
        CHECK(!sel.hdr);
        CHECK(sel.hdrCapable);
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
        CHECK(!sel.hdrCapable);
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

    // ── HDR and 4:4:4 together: exclusive, and HDR is the one kept ───────────
    //
    // 4:4:4 still steers the CODEC — the walk runs before the HDR decision, so
    // HEVC is chosen because it is the codec with a 4:4:4 path — and the chroma
    // is then given back, because 10-bit 4:4:4 has no browser that displays it
    // (Chrome renders `hvc1.4.156` green). Granting both used to be this test's
    // expectation and would now kill the session at init(): the conversion pass
    // refuses the pair rather than produce a picture nobody can watch.
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
        CHECK(sel.hdr);
        CHECK(!sel.yuv444);
    }

    // ── 4:4:4 survives when the HDR it competed with was not granted ─────────
    //
    // The SAME display and the SAME GPU as above, with Windows HDR simply
    // turned off. Nothing is dropped, because nothing is in conflict: the
    // exclusion has to be a consequence of HDR being real, never of it having
    // been asked for. Testing this on the machine's other display would prove
    // nothing — that one hangs off an iGPU with no 4:4:4 at all.
    {
        Capabilities caps = hybridMachine();
        caps.displays[1].hdrActive = false;
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.hdr = true;
        cfg.yuv444 = true;
        cfg.clientCodecs = {Codec::Av1, Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK(!sel.hdr);
        CHECK(sel.yuv444);
        CHECK_EQ(sel.codec, Codec::Hevc);
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

    // ── ...except its shape: the frame keeps the display's ───────────────────
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 1; // 3840×2160
        cfg.width = 1664;  // what a client that misread the bars asked for
        cfg.height = 1080;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.width, 1920);
        CHECK_EQ(sel.height, 1080);

        // An ultrawide asked for 16:9 streams its own shape, width kept even.
        Capabilities wide = hybridMachine();
        wide.displays[1].width = 3440;
        wide.displays[1].height = 1440;
        CHECK(select(wide, cfg, sel, err));
        CHECK_EQ(sel.width, 2580);
        CHECK_EQ(sel.height, 1080);

        // A client's own rounding (within 0.5%) is left alone.
        cfg.width = 2578;
        CHECK(select(wide, cfg, sel, err));
        CHECK_EQ(sel.width, 2578);
    }

    // ── The same rule when the display changes mode under a session ──────────
    {
        // 16:9 → 4:3 (a game setting 1920×1440): same height, 4:3 width.
        FrameSize f = frameForDisplay({1920, 1440}, {1920, 1080});
        CHECK_EQ(f.width, 1440);
        CHECK_EQ(f.height, 1080);

        // Back to 16:9.
        f = frameForDisplay({2560, 1440}, {1440, 1080});
        CHECK_EQ(f.width, 1920);
        CHECK_EQ(f.height, 1080);

        // Portrait: the width is derived and kept even.
        f = frameForDisplay({1440, 2560}, {1920, 1080});
        CHECK_EQ(f.width, 608);
        CHECK_EQ(f.height, 1080);

        // Same shape at another size: the frame is the client's, untouched.
        f = frameForDisplay({3840, 2160}, {1920, 1080});
        CHECK_EQ(f.width, 1920);
        CHECK_EQ(f.height, 1080);

        // An encoder that cannot express a partial block — VA-API in HEVC,
        // whose sequence buffer has no cropping window and whose SPS the
        // driver writes — is given whole blocks. Both sides come down
        // together, so the picture keeps its shape instead of leaning.
        FrameSize a = alignedToBlocks({1366, 768}, 8);
        CHECK_EQ(a.width, 1352);
        CHECK_EQ(a.height, 760);
        // 1352/760 is 1.77895, 1366/768 is 1.77864 — closer than the 1.78947
        // that rounding each side apart (1360x760) would have given.
        CHECK(std::abs(double(a.width) / a.height - 1366.0 / 768.0) <
              std::abs(1360.0 / 760.0 - 1366.0 / 768.0));
        // Every common resolution is already whole blocks: untouched.
        for (const FrameSize& s : {FrameSize{1920, 1080}, FrameSize{2560, 1440},
                                   FrameSize{3840, 2160}, FrameSize{1280, 720}}) {
            const FrameSize kept = alignedToBlocks(s, 8);
            CHECK_EQ(kept.width, s.width);
            CHECK_EQ(kept.height, s.height);
        }
        // A phone's shape, made to measure and not a multiple of 8.
        a = alignedToBlocks({2532, 1170}, 8);
        CHECK_EQ(a.width % 8, 0);
        CHECK_EQ(a.height % 8, 0);
        CHECK(a.width <= 2532 && a.height <= 1170);
        CHECK(std::abs(double(a.width) / a.height - 2532.0 / 1170.0) < 0.005);
        // Nothing to align, nothing to lose.
        a = alignedToBlocks({1920, 1080}, 1);
        CHECK_EQ(a.width, 1920);
        a = alignedToBlocks({1920, 1080}, 6); // not a power of two
        CHECK_EQ(a.width, 1920);
        a = alignedToBlocks({0, 0}, 8);
        CHECK_EQ(a.width, 0);
        // Smaller than one block on a side: one block, never zero.
        a = alignedToBlocks({4, 4}, 8);
        CHECK_EQ(a.width, 8);
        CHECK_EQ(a.height, 8);

        // A grid of 64x16, the 780M's AV1 (C13.12): 1080p down to the largest
        // exact 16:9 on it; the other common sizes are whole already.
        a = alignedToGrid({1920, 1080}, 64, 16);
        CHECK_EQ(a.width, 1792);
        CHECK_EQ(a.height, 1008);
        for (const FrameSize& s :
             {FrameSize{1280, 720}, FrameSize{2560, 1440}, FrameSize{3840, 2160}}) {
            const FrameSize kept = alignedToGrid(s, 64, 16);
            CHECK_EQ(kept.width, s.width);
            CHECK_EQ(kept.height, s.height);
        }
        // Off the grid and with no exact shape on it: whole blocks, within
        // the frame, close to its shape.
        a = alignedToGrid({1366, 768}, 64, 16);
        CHECK_EQ(a.width % 64, 0);
        CHECK_EQ(a.height % 16, 0);
        CHECK(a.width <= 1366 && a.height <= 768);
        CHECK(std::abs(double(a.width) / a.height - 1366.0 / 768.0) < 0.01);
        // Square blocks: what alignedToBlocks gives.
        a = alignedToGrid({1366, 768}, 8, 8);
        CHECK_EQ(a.width, 1352);
        CHECK_EQ(a.height, 760);
        a = alignedToGrid({1920, 1080}, 0, 16);
        CHECK_EQ(a.width, 1920);
        a = alignedToGrid({40, 10}, 64, 16);
        CHECK_EQ(a.width, 64);
        CHECK_EQ(a.height, 16);

        // Unknown sizes change nothing.
        f = frameForDisplay({0, 0}, {1920, 1080});
        CHECK_EQ(f.width, 1920);
        f = frameForDisplay({1280, 960}, {0, 0});
        CHECK_EQ(f.width, 0);

        // Never larger than the display: a 1440p request of a display gone
        // 1080p streams 1080p, one gone 800×600 streams 800×600 (the 4:3 shape
        // at 1440 lines would not fit), and one grown back gets the request.
        f = frameForDisplay({1920, 1080}, {2560, 1440});
        CHECK_EQ(f.width, 1920);
        CHECK_EQ(f.height, 1080);
        f = frameForDisplay({800, 600}, {2560, 1440});
        CHECK_EQ(f.width, 800);
        CHECK_EQ(f.height, 600);
        f = frameForDisplay({2560, 1440}, {2560, 1440});
        CHECK_EQ(f.width, 2560);
        CHECK_EQ(f.height, 1440);
        f = frameForDisplay({1280, 960}, {1920, 1080});
        CHECK_EQ(f.width, 1280);
        CHECK_EQ(f.height, 960);
        // An odd-sized display is capped to even dimensions.
        f = frameForDisplay({1366, 767}, {1920, 1080});
        CHECK_EQ(f.width, 1366);
        CHECK_EQ(f.height, 766);
    }

    // ── A box to fit: the display's shape INSIDE the requested size ─────────
    {
        const FramePolicy box{true, false};
        // A 16:10 client screen of a 16:9 display: the height is the limit,
        // and the client shows 1920x1080 1:1 with its own bars around.
        FrameSize f = frameForDisplay({1920, 1080}, {1920, 1200}, box);
        CHECK_EQ(f.width, 1920);
        CHECK_EQ(f.height, 1080);

        // A 4:3 box of a 16:9 display: the width is the limit.
        f = frameForDisplay({1920, 1080}, {1600, 1200}, box);
        CHECK_EQ(f.width, 1600);
        CHECK_EQ(f.height, 900);

        // A box of the display's own shape is taken as it is.
        f = frameForDisplay({2560, 1440}, {1280, 720}, box);
        CHECK_EQ(f.width, 1280);
        CHECK_EQ(f.height, 720);

        // A box of a portrait display.
        f = frameForDisplay({1080, 1920}, {1920, 1080}, box);
        CHECK_EQ(f.width, 608);
        CHECK_EQ(f.height, 1080);

        // Never larger than the display unless allowed: the same 1440p box
        // of a 1080p display streams 1080p by default …
        f = frameForDisplay({1920, 1080}, {2560, 1440}, box);
        CHECK_EQ(f.width, 1920);
        CHECK_EQ(f.height, 1080);
        // … and the client's own screen, when it says so: 1440p, upscaled.
        const FramePolicy screen{true, true};
        f = frameForDisplay({1920, 1080}, {2560, 1440}, screen);
        CHECK_EQ(f.width, 2560);
        CHECK_EQ(f.height, 1440);
        // The shape is still the display's, inside the screen: a 16:10 phone
        // of a 16:9 display gets 16:9 at the phone's width.
        f = frameForDisplay({1920, 1080}, {2560, 1600}, screen);
        CHECK_EQ(f.width, 2560);
        CHECK_EQ(f.height, 1440);

        // An odd box is made even.
        f = frameForDisplay({1920, 1080}, {1365, 767}, box);
        CHECK_EQ(f.width, 1364);
        CHECK_EQ(f.height, 766);

        // The policy is read off the session's config: upscaling means
        // nothing without a box.
        SessionConfig c;
        c.allowUpscale = true;
        CHECK(!policyOf(c).fitBox);
        CHECK(!policyOf(c).allowUpscale);
        c.fitRequestedBox = true;
        CHECK(policyOf(c).allowUpscale);

        // "Match my screen" that cannot be honoured becomes Auto: the
        // fallback box, no upscale, nothing to match — and nothing happens
        // to a config that never asked.
        SessionConfig m;
        CHECK(!fallBackFromMatch(m));
        m.fitRequestedBox = true;
        m.allowUpscale = true;
        m.matchClientDisplay = true;
        m.width = 2532;
        m.height = 1170;
        m.fallbackWidth = 4096;
        m.fallbackHeight = 1440;
        CHECK(fallBackFromMatch(m));
        CHECK_EQ(m.width, 4096);
        CHECK_EQ(m.height, 1440);
        CHECK(!m.allowUpscale);
        CHECK(!m.matchClientDisplay);
        CHECK(m.fitRequestedBox);
        CHECK(!fallBackFromMatch(m));
        // The box rule then gives a 1440p host its own size, a 4K host 1440
        // lines, a phone-shaped box notwithstanding.
        FrameSize g = frameForDisplay({2560, 1440}, {m.width, m.height}, policyOf(m));
        CHECK_EQ(g.width, 2560);
        CHECK_EQ(g.height, 1440);
        g = frameForDisplay({3840, 2160}, {m.width, m.height}, policyOf(m));
        CHECK_EQ(g.width, 2560);
        CHECK_EQ(g.height, 1440);
        // Without a fallback box, the request itself is fitted, never upscaled.
        SessionConfig r;
        r.fitRequestedBox = r.allowUpscale = r.matchClientDisplay = true;
        r.width = 2080;
        r.height = 1170;
        r.requestedWidth = 2532;
        r.requestedHeight = 1170;
        CHECK(fallBackFromMatch(r));
        CHECK_EQ(r.width, 2532);
        CHECK(!r.allowUpscale);

        // "Match my screen" on the portal's virtual monitor (Linux, C0 of plan
        // Idées Punktfunk): no mode to change, the display is made at the
        // client's size — pinned to the made display's bounds and even — and
        // select() then streams it to the pixel, phone shape and all.
        Capabilities made = hybridMachine();
        DisplayInfo virt;
        virt.id = 7;
        virt.gpuId = made.displays.front().gpuId;
        virt.width = 1920;
        virt.height = 1080;
        virt.refreshMilliHz = 60000;
        virt.kind = DisplayKind::Virtual;
        virt.key = kPortalVirtualDisplayKey;
        made.displays.push_back(virt);
        SessionConfig v;
        v.displayId = 7;
        v.clientCodecs = {Codec::H264};
        v.fitRequestedBox = v.allowUpscale = v.matchClientDisplay = true;
        v.width = 1170;
        v.height = 2533;
        CHECK(sizeVirtualMonitorToClient(made, v));
        CHECK(!v.matchClientDisplay);
        CHECK(!fallBackFromMatch(v));
        CHECK_EQ(made.displays.back().width, 1170);
        CHECK_EQ(made.displays.back().height, 2532);
        Selection vs;
        std::string verr;
        CHECK(select(made, v, vs, verr));
        CHECK_EQ(vs.width, 1170);
        CHECK_EQ(vs.height, 2532);
        // Past the bounds: pinned, the box rule fitting the rest.
        SessionConfig big = v;
        big.matchClientDisplay = true;
        big.width = 5120;
        big.height = 300;
        CHECK(sizeVirtualMonitorToClient(made, big));
        CHECK_EQ(made.displays.back().width, kMadeDisplayMax);
        CHECK_EQ(made.displays.back().height, kMadeDisplayMin);
        // Any other display, or a config that did not ask: nothing changes,
        // and the caller falls back from the match as before.
        SessionConfig real = v;
        real.matchClientDisplay = true;
        real.displayId = made.displays.front().id;
        const int realWidth = made.displays.front().width;
        CHECK(!sizeVirtualMonitorToClient(made, real));
        CHECK(real.matchClientDisplay);
        CHECK_EQ(made.displays.front().width, realWidth);
        SessionConfig autoAsked = v;
        autoAsked.matchClientDisplay = false;
        autoAsked.width = 2560;
        CHECK(!sizeVirtualMonitorToClient(made, autoAsked));
        CHECK_EQ(made.displays.back().width, kMadeDisplayMax);
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

    SECTION("Selector — the fallback encoder tier");

    // The two machines this tier exists for, in the shape the probe reports
    // them: a GPU is present and enumerated, it simply has no encoder we can
    // drive (bench-arm's Adreno; bench-vm's hyperv_drm, which has no render node
    // at all).
    const auto encoderlessMachine = []() {
        Capabilities caps;
        caps.available = true;
        caps.reason = Unavailability::None;
        caps.capture = CaptureApi::DxgiDuplication;
        caps.gpus = {makeGpu(0, "Qualcomm(R) Adreno(TM) 618 GPU", {}, {}, false)};
        caps.displays = {makeDisplay(0, 0, 1920, 1080, 60000, true, false)};
        return caps;
    };

    // ── Nothing offered: the refusal still happens, and says both halves ──────
    //
    // The guard that used to read "no GPU on this machine has a usable encoder"
    // must not become "and therefore we streamed anyway" the moment a fallback
    // vector exists but is empty.
    {
        const Capabilities caps = encoderlessMachine();
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(!select(caps, cfg, sel, err));
        CHECK(err.find("fallback") != std::string::npos);
    }

    // ── A fallback is taken, and it costs no cross-GPU copy ──────────────────
    //
    // The display's own adapter is KEPT: capture and colour conversion still run
    // there, and only the encoder moved off it.
    {
        Capabilities caps = encoderlessMachine();
        caps.fallbacks.push_back({EncoderApi::Software, {Codec::H264}, false, "OpenH264"});
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::Av1, Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK(sel.fallbackEncoder);
        CHECK(sel.cpuEncoder);
        CHECK(!sel.crossGpuCopy);
        CHECK_EQ(sel.encoder, EncoderApi::Software);
        CHECK_EQ(sel.codec, Codec::H264); // the only one offered, whatever the client prefers
        CHECK_EQ(sel.gpu->id, 0);         // still the display's own adapter
    }

    // ── bestFallback() and select() name the SAME encoder ────────────────────
    //
    // The status page answers with the first, the session runs on the second.
    // They are one function now; this is what keeps them one, because the day
    // they disagree is the day /api/native/status tells a user their machine
    // will encode on hardware it will not actually use.
    {
        Capabilities caps = encoderlessMachine();
        caps.fallbacks.push_back({EncoderApi::Software, {Codec::H264}, false, "OpenH264"});
        caps.fallbacks.push_back(
            {EncoderApi::MediaFoundation, {Codec::H264}, true, "Qualcomm H264 Encoder MFT"});
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        const FallbackEncoder* named = bestFallback(caps);
        CHECK(named != nullptr);
        if (named) {
            CHECK_EQ(named->api, sel.encoder);
            CHECK_EQ(named->hardware, !sel.cpuEncoder);
            CHECK_EQ(named->name, std::string("Qualcomm H264 Encoder MFT"));
        }
    }

    // An empty tier names nothing rather than the first junk entry: an entry
    // with no codec is not an encoder, and the machine is simply unable.
    {
        Capabilities caps = encoderlessMachine();
        CHECK(bestFallback(caps) == nullptr);
        caps.fallbacks.push_back({EncoderApi::Software, {}, false, "OpenH264"});
        CHECK(bestFallback(caps) == nullptr);
    }

    // ── Hardware outranks the CPU, whatever order the probe pushed them in ────
    //
    // On a Snapdragon this is the difference between a stream and a slideshow:
    // the OS lends us fixed-function silicon we have no SDK for.
    {
        Capabilities caps = encoderlessMachine();
        caps.fallbacks.push_back({EncoderApi::Software, {Codec::H264}, false, "OpenH264"});
        caps.fallbacks.push_back(
            {EncoderApi::MediaFoundation, {Codec::H264}, true, "Qualcomm H264 Encoder MFT"});
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.encoder, EncoderApi::MediaFoundation);
        CHECK(sel.fallbackEncoder);
        CHECK(!sel.cpuEncoder); // hardware, so nothing downstream has to watch it keep up
    }

    // ── A fallback is NEVER preferred over a GPU that can encode ─────────────
    //
    // The whole reason this tier lives beside GpuInfo::encoders rather than in
    // it. A software entry that could outrank an RTX would be a silent and total
    // performance regression on a perfectly good machine.
    {
        Capabilities caps = hybridMachine();
        caps.fallbacks.push_back(
            {EncoderApi::MediaFoundation, {Codec::H264}, true, "some hardware MFT"});
        SessionConfig cfg;
        cfg.displayId = 1;
        cfg.clientCodecs = {Codec::Av1, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.encoder, EncoderApi::Nvenc);
        CHECK(!sel.fallbackEncoder);
        CHECK_EQ(sel.codec, Codec::Av1);
    }

    // ── An encoder-less display still looks at real GPUs before the tier ─────
    //
    // A cross-GPU copy is expensive; it is nowhere near as expensive as encoding
    // on the CPU, so the older fallback keeps winning over this one.
    {
        Capabilities caps = hybridMachine();
        caps.gpus[0].encoders.clear();
        caps.gpus[0].codecs.clear();
        caps.fallbacks.push_back({EncoderApi::Software, {Codec::H264}, false, "OpenH264"});
        SessionConfig cfg;
        cfg.displayId = 0; // the panel on the now encoder-less iGPU
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.encoder, EncoderApi::Nvenc);
        CHECK(sel.crossGpuCopy);
        CHECK(!sel.fallbackEncoder);
    }

    // ── No HDR and no 4:4:4 on the fallback, whatever was asked ──────────────
    //
    // Both would be a second reason for an already-struggling machine to fall
    // behind, and a browser handed PQ it cannot place shows a washed-out picture
    // rather than an error.
    {
        Capabilities caps = encoderlessMachine();
        caps.displays[0].hdrActive = true;
        caps.fallbacks.push_back(
            {EncoderApi::MediaFoundation, {Codec::H264, Codec::Hevc}, true, "a hardware MFT"});
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.hdr = true;
        cfg.yuv444 = true;
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK(!sel.hdr);
        CHECK(!sel.yuv444);
        CHECK_EQ(sel.codec, Codec::Hevc); // the client's own preference still decides
    }

    // ── No tier upscales, the fallback tier included ─────────────────────────
    {
        Capabilities caps = encoderlessMachine(); // a 1920×1080 display
        caps.fallbacks.push_back({EncoderApi::Software, {Codec::H264}, false, "OpenH264"});
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.width = 2560;
        cfg.height = 1440;
        cfg.clientCodecs = {Codec::H264};

        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.width, 1920);
        CHECK_EQ(sel.height, 1080);

        // Asking for LESS is honoured: downscaling saves the machine work.
        cfg.width = 1280;
        cfg.height = 720;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.width, 1280);
        CHECK_EQ(sel.height, 720);

        // The same request on a GPU encoder is capped the same way.
        const Capabilities hybrid = hybridMachine();
        cfg.displayId = 0; // the 1080p panel
        cfg.width = 2560;
        cfg.height = 1440;
        CHECK(select(hybrid, cfg, sel, err));
        CHECK_EQ(sel.width, 1920);
        CHECK_EQ(sel.height, 1080);
    }

    // ── A client that decodes none of what the fallback makes is refused ─────
    {
        Capabilities caps = encoderlessMachine();
        caps.fallbacks.push_back({EncoderApi::Software, {Codec::H264}, false, "OpenH264"});
        SessionConfig cfg;
        cfg.displayId = 0;
        cfg.clientCodecs = {Codec::Av1};

        Selection sel;
        std::string err;
        CHECK(!select(caps, cfg, sel, err));
        CHECK(err.find("fallback encoder") != std::string::npos);
    }

    SECTION("Selector — the rate follows the frame streamed");

    // ── 1440p asked of a 1080p display: the 1080p rate (GUESTD2, 21/11/2026) ─
    // The rung rates of the shared feed (SharedFeed::setOwnerHeight: the
    // estimate for the height, halved) go with the square of the height, so a
    // 1440p rung's rate cut in the ratio of the pixels is the 1080p rung's.
    {
        const Capabilities caps = hybridMachine();
        SessionConfig cfg;
        cfg.displayId = 0; // the 1080p panel
        cfg.width = 2560;
        cfg.height = 1440;
        cfg.bitrateKbps = 17778; // the 1440p rung
        cfg.clientCodecs = {Codec::Hevc, Codec::H264};

        // A rate set by hand never goes down (GUESTD2SCOPE B): the default.
        Selection sel;
        std::string err;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.width, 1920);
        CHECK_EQ(sel.height, 1080);
        CHECK_EQ(sel.bitrateKbps, 17778);

        // A rate worked out for the size asked (Auto, a guest, the feed) does.
        cfg.bitrateFollowsFrame = true;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.height, 1080);
        CHECK_EQ(sel.bitrateKbps, 10000); // the 1080p rung

        // A frame streamed as asked keeps its rate.
        cfg.width = 1280;
        cfg.height = 720;
        cfg.bitrateKbps = 5000;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.height, 720);
        CHECK_EQ(sel.bitrateKbps, 5000);

        // The 4K display streams 1440p as asked: nothing to cut.
        cfg.displayId = 1;
        cfg.width = 2560;
        cfg.height = 1440;
        cfg.bitrateKbps = 17778;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.height, 1440);
        CHECK_EQ(sel.bitrateKbps, 17778);

        // The client's own screen size, upscaled on purpose: never raised.
        cfg.displayId = 0;
        cfg.fitRequestedBox = true;
        cfg.allowUpscale = true;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.height, 1440);
        CHECK_EQ(sel.bitrateKbps, 17778);

        // "Match my screen": the session puts the display in the size asked
        // and streams it, at the rate set for it.
        cfg.allowUpscale = false;
        cfg.matchClientDisplay = true;
        CHECK(select(caps, cfg, sel, err));
        CHECK_EQ(sel.height, 1080); // before the mode change
        CHECK_EQ(sel.bitrateKbps, 17778);
    }

    // ── The rule itself ──────────────────────────────────────────────────────
    {
        // The pixels' ratio, rounded: 35 Mbps set for 1440p is 19.7 for 1080p.
        CHECK_EQ(bitrateForFrame(35000, {2560, 1440}, {1920, 1080}), 19688);
        // Never below 1000 kbps, nor a rate already below it lowered.
        CHECK_EQ(bitrateForFrame(1500, {3840, 2160}, {1280, 720}), 1000);
        CHECK_EQ(bitrateForFrame(800, {3840, 2160}, {1280, 720}), 800);
        // A larger frame, the same one, or a size unknown: untouched.
        CHECK_EQ(bitrateForFrame(10000, {1920, 1080}, {2560, 1440}), 10000);
        CHECK_EQ(bitrateForFrame(10000, {1920, 1080}, {1920, 1080}), 10000);
        CHECK_EQ(bitrateForFrame(10000, {0, 0}, {1920, 1080}), 10000);
        CHECK_EQ(bitrateForFrame(10000, {1920, 1080}, {0, 1080}), 10000);
        CHECK_EQ(bitrateForFrame(0, {2560, 1440}, {1920, 1080}), 0);
    }
}
