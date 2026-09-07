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
}
