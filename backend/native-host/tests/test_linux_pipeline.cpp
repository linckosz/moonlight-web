/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(MW_NATIVE_LINUX_PORTAL)
#include "capture/linux/PortalScreenCast.h"
#endif

#if defined(MW_NATIVE_LINUX_GFX)
#include "capture/linux/KmsCapture.h"
#include "convert/linux/CpuConvert.h"
#include "convert/linux/GlConvert.h"
#include "encode/linux/VaapiEncoder.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/mman.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>

// The Linux capture and conversion against the real display and the real GPU.
//
// Same stance as the Windows capture test: this cannot be faked, so it runs on
// whatever the machine has and says "skipped" honestly where it lacks the
// display, the GPU, or the privilege. The proof at the end is the one that
// matters everywhere: real pixels, read back, in the legal range — a pipeline
// that comes up and hands the encoder a black surface passes every other check.

void run_linux_pipeline_tests()
{
    SECTION("Linux — KMS capture → EGL conversion → VA-API surface");

#if !defined(MW_NATIVE_LINUX_GFX)
    std::fprintf(stderr, "  skipped: Linux graphics backend not built\n");
#else
    using namespace mw::native;

    // ── Find a display that is actually being shown ─────────────────────────
    glob_t cards = {};
    glob("/dev/dri/card*", 0, nullptr, &cards);
    capture::KmsOutput target;
    for (size_t i = 0; i < cards.gl_pathc && !target.active; ++i) {
        std::string error;
        for (const capture::KmsOutput& out :
             capture::KmsCapture::listOutputs(cards.gl_pathv[i], error)) {
            std::fprintf(stderr, "  %s %s: %s%s %dx%d @ %.2f Hz at %d,%d\n", out.cardPath.c_str(),
                         out.name.c_str(), out.connected ? "connected" : "disconnected",
                         out.active ? ", active" : "", out.width, out.height,
                         out.refreshMilliHz / 1000.0, out.x, out.y);
            if (out.active && !target.active) target = out;
        }
    }
    globfree(&cards);
    if (!target.active) {
        std::fprintf(stderr, "  skipped: no display is being scanned out\n");
        return;
    }

    std::string why;
    if (!capture::KmsCapture::canReadFramebuffers(target.cardPath, why)) {
        std::fprintf(stderr, "  skipped: %s\n", why.c_str());
        return;
    }

    // ── Capture ─────────────────────────────────────────────────────────────
    capture::KmsCapture kms(target.cardPath, target.connectorId);
    std::string error;
    if (!kms.start(error)) {
        std::fprintf(stderr, "  skipped: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(kms.width(), target.width);
    CHECK_EQ(kms.height(), target.height);
    CHECK(kms.refreshMilliHz() > 0);
    CHECK(!kms.renderNodePath().empty());
    CHECK(kms.desktopRect().valid());

    // The first acquire always has a frame: the buffer on screen has not been
    // handed out yet. Later ones on a still desktop are Timeouts, correctly.
    capture::KmsFrame frame;
    const capture::AcquireStatus status = kms.acquire(100, frame);
    CHECK_EQ(static_cast<int>(status), static_cast<int>(capture::AcquireStatus::Ok));
    if (status != capture::AcquireStatus::Ok) {
        kms.stop();
        return;
    }
    std::fprintf(
        stderr, "  frame %dx%d fourcc %.4s modifier 0x%llx, %d plane(s), capture latency %lld us\n",
        frame.width, frame.height, reinterpret_cast<const char*>(&frame.fourcc),
        static_cast<unsigned long long>(frame.modifier), frame.planeCount,
        static_cast<long long>(frame.capturedUs - frame.presentUs));
    CHECK(frame.planeCount >= 1);
    CHECK(frame.fds[0] >= 0);
    CHECK(frame.presentUs > 0);
    // A present in the future would poison the link governor (see WgcCapture).
    CHECK(frame.capturedUs >= frame.presentUs);

    // ── The pointer, asked for independently ────────────────────────────────
    //
    // A plane's state lives in its properties, and the kernel reports them as
    // ZERO to a client that did not ask for DRM_CLIENT_CAP_ATOMIC — so a
    // capture missing that cap decides there is no pointer, for ever and in
    // silence. It cost two days of "the mouse is invisible on Linux"
    // (07/09/2026), on every client at once: nothing to composite into the
    // picture for a phone, no shape to hand a desktop browser. Here the plane
    // is read a second time, by this test's own fd, and the two answers must
    // agree. Skipped honestly where the compositor has no cursor plane or has
    // hidden the pointer — neither is a failure.
    //
    // The plane is found by possible_crtcs, as the capture finds it, and not
    // by its attachment: a hidden pointer is a cursor plane with no CRTC, and
    // reading it by attachment turns "hidden right now" into "no cursor plane"
    // — the second way the mouse went missing on issue #15 (12/09/2026).
    {
        const int fd = ::open(target.cardPath.c_str(), O_RDWR | O_CLOEXEC);
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
        // A virtual machine's adapter hides its cursor plane from a client
        // that has not declared this (Linux 6.8+) — see KmsCapture::start().
        drmSetClientCap(fd, 6 /* DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT */, 1);
        uint32_t crtcBit = 0;
        if (drmModeRes* res = fd >= 0 ? drmModeGetResources(fd) : nullptr) {
            for (int i = 0; i < res->count_crtcs; ++i)
                if (res->crtcs[i] == target.crtcId) crtcBit = 1u << static_cast<unsigned>(i);
            drmModeFreeResources(res);
        }
        uint64_t cursorFb = 0;
        bool sawCursorPlane = false;
        drmModePlaneRes* planes = fd >= 0 ? drmModeGetPlaneResources(fd) : nullptr;
        for (uint32_t i = 0; planes && i < planes->count_planes; ++i) {
            drmModePlane* p = drmModeGetPlane(fd, planes->planes[i]);
            if (!p) continue;
            if (p->crtc_id == target.crtcId || (p->possible_crtcs & crtcBit)) {
                drmModeObjectProperties* props =
                    drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
                uint64_t type = 0, fbId = 0;
                for (uint32_t j = 0; props && j < props->count_props; ++j) {
                    drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[j]);
                    if (!prop) continue;
                    if (std::string(prop->name) == "type") type = props->prop_values[j];
                    if (std::string(prop->name) == "FB_ID") fbId = props->prop_values[j];
                    drmModeFreeProperty(prop);
                }
                if (props) drmModeFreeObjectProperties(props);
                // The one attached to our CRTC wins over one that merely could
                // be: a card with several CRTCs has a cursor plane per CRTC.
                if (type == DRM_PLANE_TYPE_CURSOR &&
                    (!sawCursorPlane || p->crtc_id == target.crtcId)) {
                    sawCursorPlane = true;
                    cursorFb = fbId;
                }
            }
            drmModeFreePlane(p);
        }
        if (planes) drmModeFreePlaneResources(planes);
        if (fd >= 0) ::close(fd);

        if (!sawCursorPlane)
            std::fprintf(stderr, "  no cursor plane: the pointer is inside the picture\n");
        else if (!cursorFb)
            std::fprintf(stderr, "  the compositor has hidden the pointer — nothing to check\n");
        else {
            std::fprintf(stderr, "  cursor plane holds fb %llu; capture says %s %dx%d at %d,%d\n",
                         static_cast<unsigned long long>(cursorFb),
                         kms.cursor().visible ? "visible" : "INVISIBLE", kms.cursor().width,
                         kms.cursor().height, kms.cursor().x, kms.cursor().y);
            CHECK(kms.cursor().visible);
            CHECK(kms.cursor().width > 0);
            CHECK(kms.cursor().height > 0);
            // An all-transparent shape draws nothing, which looks exactly like
            // the bug this test exists for.
            CHECK(kms.cursor().inkWidth > 0);
        }
    }

    // A second acquire on a still desktop: Timeout or PointerOnly, never a
    // duplicate Ok for the same buffer.
    {
        capture::KmsFrame again;
        kms.release();
        const capture::AcquireStatus second = kms.acquire(50, again);
        CHECK(second != capture::AcquireStatus::Lost);
        CHECK(second != capture::AcquireStatus::Failed);
        if (second == capture::AcquireStatus::Ok) {
            std::fprintf(stderr, "  (the desktop moved between acquires — fine)\n");
            kms.release();
        }
        // Re-acquire for the conversion below: force a fresh export.
        kms.stop();
        CHECK(kms.start(error));
        CHECK_EQ(static_cast<int>(kms.acquire(100, frame)),
                 static_cast<int>(capture::AcquireStatus::Ok));
    }

    // ── The encoder's surface, stood in for by VA-API directly ──────────────
    const int render = ::open(kms.renderNodePath().c_str(), O_RDWR | O_CLOEXEC);
    CHECK(render >= 0);
    VADisplay display = vaGetDisplayDRM(render);
    int major = 0, minor = 0;
    CHECK_EQ(vaInitialize(display, &major, &minor), VA_STATUS_SUCCESS);
    std::fprintf(stderr, "  VA-API %d.%d: %s\n", major, minor, vaQueryVendorString(display));

    VASurfaceID nv12 = VA_INVALID_SURFACE;
    CHECK_EQ(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, static_cast<unsigned>(frame.width),
                              static_cast<unsigned>(frame.height), &nv12, 1, nullptr, 0),
             VA_STATUS_SUCCESS);
    VADRMPRIMESurfaceDescriptor exported = {};
    CHECK_EQ(vaExportSurfaceHandle(display, nv12, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                   &exported),
             VA_STATUS_SUCCESS);
    CHECK_EQ(exported.num_layers, 2u);

    convert::Nv12Target nv12Target;
    nv12Target.width = frame.width;
    nv12Target.height = frame.height;
    nv12Target.modifier = exported.objects[0].drm_format_modifier;
    nv12Target.fdY = exported.objects[exported.layers[0].object_index[0]].fd;
    nv12Target.offsetY = exported.layers[0].offset[0];
    nv12Target.pitchY = exported.layers[0].pitch[0];
    nv12Target.fdUV = exported.objects[exported.layers[1].object_index[0]].fd;
    nv12Target.offsetUV = exported.layers[1].offset[0];
    nv12Target.pitchUV = exported.layers[1].pitch[0];

    // ── Convert ─────────────────────────────────────────────────────────────
    convert::GlConvert gl;
    CHECK(gl.init(kms.renderNodePath(), frame.fourcc, frame.width, frame.height, frame.width,
                  frame.height, error));
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
    CHECK(gl.bindTarget(nv12Target, error));
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
    CHECK(gl.convert(frame, kms.cursor(), convert::CursorDraw{}, error));
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
    kms.release();

    // ── Look at the pixels ──────────────────────────────────────────────────
    VAImageFormat format = {};
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    VAImage image = {};
    CHECK_EQ(vaCreateImage(display, &format, frame.width, frame.height, &image), VA_STATUS_SUCCESS);
    CHECK_EQ(vaGetImage(display, nv12, 0, 0, static_cast<unsigned>(frame.width),
                        static_cast<unsigned>(frame.height), image.image_id),
             VA_STATUS_SUCCESS);
    uint8_t* pixels = nullptr;
    CHECK_EQ(vaMapBuffer(display, image.buf, reinterpret_cast<void**>(&pixels)), VA_STATUS_SUCCESS);
    if (pixels) {
        unsigned minLuma = 255, maxLuma = 0;
        for (int y = 0; y < frame.height; y += 16) {
            const uint8_t* row =
                pixels + image.offsets[0] + static_cast<size_t>(y) * image.pitches[0];
            for (int x = 0; x < frame.width; x += 16) {
                minLuma = row[x] < minLuma ? row[x] : minLuma;
                maxLuma = row[x] > maxLuma ? row[x] : maxLuma;
            }
        }
        std::fprintf(stderr, "  NV12 luma range: %u..%u\n", minLuma, maxLuma);
        // BT.709 limited range puts black at 16 and white at 235. A desktop
        // always holds more than one shade, so a flat image means the
        // conversion drew nothing — and a value outside 16..235 means the
        // limited-range scaling was skipped.
        CHECK(maxLuma > minLuma);
        CHECK(minLuma >= 16);
        CHECK(maxLuma <= 235);
        vaUnmapBuffer(display, image.buf);
    }
    vaDestroyImage(display, image.image_id);
    for (unsigned i = 0; i < exported.num_objects; ++i)
        ::close(exported.objects[i].fd);
    vaDestroySurfaces(display, &nv12, 1);
    vaTerminate(display);
    ::close(render);
    gl.stop();

    // ── …and through the encoder, the way a session wires it ────────────────
    //
    // The encoder OWNS the surface here (VA-API allocates), the converter
    // renders into its exported planes, and the bitstream that comes out is
    // inspected rather than trusted: an Annex-B stream a browser can start from
    // carries its parameter sets on the keyframe.
    SECTION("Linux — VA-API encoder, from the converter's surface to Annex-B");
    {
        encode::VaapiEncoder encoder;
        if (!encoder.init(kms.renderNodePath(), Codec::H264, frame.width, frame.height, 60, 20000,
                          /*intraRefresh=*/true, EncoderTuning{}, error)) {
            std::fprintf(stderr, "  encoder skipped: %s\n", error.c_str());
        } else {
            convert::GlConvert gl2;
            CHECK(gl2.init(kms.renderNodePath(), frame.fourcc, frame.width, frame.height,
                           frame.width, frame.height, error));
            CHECK(gl2.bindTarget(encoder.inputTarget(), error));
            if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());

            // A fresh frame for the encoder's surface.
            kms.stop();
            CHECK(kms.start(error));
            CHECK_EQ(static_cast<int>(kms.acquire(100, frame)),
                     static_cast<int>(capture::AcquireStatus::Ok));
            CHECK(gl2.convert(frame, kms.cursor(), convert::CursorDraw{}, error));
            if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
            kms.release();

            encode::EncoderOutput out;
            const bool encoded = encoder.encode(true, 0, out, error);
            if (!encoded) std::fprintf(stderr, "  encode failed: %s\n", error.c_str());
            CHECK(encoded);
            if (encoded && out.data) {
                CHECK(out.keyframe);
                CHECK(out.size > 0);
                // Walk the Annex-B NAL units: a decodable keyframe has an SPS
                // (7), a PPS (8) and an IDR slice (5). Their absence is the
                // failure that looks like a working encoder and a black client.
                bool sps = false, pps = false, idr = false;
                int nals = 0;
                for (size_t i = 0; i + 3 < out.size; ++i) {
                    if (out.data[i] == 0 && out.data[i + 1] == 0 && out.data[i + 2] == 1) {
                        const int type = out.data[i + 3] & 0x1F;
                        ++nals;
                        if (type == 7) sps = true;
                        if (type == 8) pps = true;
                        if (type == 5) idr = true;
                        i += 3;
                    }
                }
                std::fprintf(
                    stderr, "  H.264 keyframe: %zu bytes, %d NAL(s), SPS %s PPS %s IDR %s\n",
                    out.size, nals, sps ? "yes" : "NO", pps ? "yes" : "NO", idr ? "yes" : "NO");
                CHECK(sps);
                CHECK(pps);
                CHECK(idr);
                encoder.releaseOutput();

                // A delta frame follows and is not a keyframe. The surface still
                // holds the converted picture — encoding it again is a P-frame
                // of a still screen, which is exactly what the floor sends.
                encode::EncoderOutput delta;
                if (encoder.encode(false, 1, delta, error)) {
                    CHECK(!delta.keyframe);
                    CHECK(delta.size > 0);
                    std::fprintf(stderr, "  H.264 delta: %zu bytes\n", delta.size);
                    encoder.releaseOutput();
                } else {
                    std::fprintf(stderr, "  delta encode failed: %s\n", error.c_str());
                    CHECK(false);
                }

                // Changing the bitrate mid-session must not need a restart.
                CHECK(encoder.setBitrate(10000, error));
                std::fprintf(stderr, "  intra-refresh: %s\n",
                             encoder.intraRefreshEnabled() ? "rolling column" : "not offered");
            }
            gl2.stop();
            encoder.stop();
        }
    }

    kms.stop();
    // Idempotent teardown, as on Windows.
    kms.stop();
    kms.release();
#endif
}

// The ScreenCast portal, as far as a test may go without a human.
//
// ⚠️ What is NOT tested here, and cannot be: Start. It raises a dialog and
// waits for someone to accept it, so an automated run would hang for its whole
// timeout and then report a failure that means nothing. What IS tested is
// everything that decides whether the route is even worth trying — the presence
// answer, and that it is a clean yes/no with a reason rather than a crash or a
// hang on a machine with no portal, which is every CI runner and every service.
//
// ⚠️ And a trap worth knowing before reading this suite's output: on a bench
// where the test binary carries `cap_sys_admin+p` as a FILE capability, the
// kernel sets AT_SECURE, and libsystemd then refuses the bus address the
// environment offers (it reads it with secure_getenv). So the answer here is
// "no session bus" on exactly the machine that has a portal. Measured
// 08/09/2026, all three cases: no capability → AT_SECURE 0, portal version 4;
// capability on the binary → AT_SECURE 1, "No medium found"; **through
// moonlightweb-launch, the way the package ships it → AT_SECURE 0, portal
// version 4**. The product is on the right side of that line — the launcher
// hands the capability over an exec that gains nothing, which is not a secure
// exec (§19.8) — and only the test binary is on the wrong one.
void run_portal_tests()
{
    SECTION("Linux — the ScreenCast portal answers, or says why not");

#if !defined(MW_NATIVE_LINUX_PORTAL)
    std::fprintf(stderr, "  skipped: the portal route is not built (no libsystemd-dev)\n");
#else
    using namespace mw::native::capture;

    std::string reason;
    const bool there = PortalScreenCast::available(reason);
    std::fprintf(stderr, "  %s: %s\n", there ? "available" : "unavailable", reason.c_str());
    // Whatever the answer, it comes with a sentence: "no portal" and "no
    // session bus" are different problems for whoever reads the log.
    CHECK(!reason.empty());

    // Constructing and destroying without a handshake must not leave a session
    // behind or trip over a null bus — the path a machine with no portal takes
    // on every probe.
    {
        PortalScreenCast idle;
        idle.stop();
        idle.stop(); // idempotent, as everywhere else in this engine
    }

    if (!there) {
        // A start with no portal has to fail fast and say so, not block.
        PortalScreenCast cast;
        PortalStream stream;
        std::string error;
        CHECK(!cast.start(std::string(), 2000, stream, error));
        CHECK(!error.empty());
        CHECK(!stream.valid());
        std::fprintf(stderr, "  refused as expected: %s\n", error.c_str());
    }
#endif
}

// The CPU chain's pointer, on a scanout this test makes itself.
//
// The machines the CPU chain exists for have a LINEAR framebuffer, which is
// exactly what a memfd can be — so unlike the pass above, this one needs no
// display, no GPU and no privilege, and runs everywhere including CI. What it
// pins is the wiring rather than the arithmetic (CursorBlend's own tests cover
// that): that the pointer lands where the placement says after the frame is
// scaled, around its hotspot, and nowhere else.
void run_cpu_cursor_tests()
{
    SECTION("Linux — the CPU chain draws the pointer into the picture");

#if !defined(MW_NATIVE_LINUX_GFX)
    std::fprintf(stderr, "  skipped: Linux graphics backend not built\n");
#else
    using namespace mw::native;

    constexpr int kW = 320;
    constexpr int kH = 180;
    const int fd = ::memfd_create("mw-fake-scanout", 0);
    if (fd < 0) {
        std::fprintf(stderr, "  skipped: memfd_create failed\n");
        return;
    }
    const size_t bytes = static_cast<size_t>(kW) * kH * 4;
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        std::fprintf(stderr, "  skipped: ftruncate failed\n");
        ::close(fd);
        return;
    }
    {
        // Mid grey, so the white pointer has somewhere to be visible.
        void* map = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            std::fprintf(stderr, "  skipped: the memfd cannot be mapped\n");
            ::close(fd);
            return;
        }
        std::memset(map, 0x40, bytes);
        ::munmap(map, bytes);
    }

    capture::KmsFrame frame;
    frame.width = kW;
    frame.height = kH;
    frame.planeCount = 1;
    frame.fds[0] = fd;
    frame.pitches[0] = static_cast<uint32_t>(kW) * 4;
    frame.offsets[0] = 0;

    convert::CpuConvert cpu;
    std::string error;
    // Output at half size, so the placement has a scale to get wrong.
    if (!cpu.init(DRM_FORMAT_XRGB8888, kW, kH, kW / 2, kH / 2, error)) {
        std::fprintf(stderr, "  skipped: %s\n", error.c_str());
        ::close(fd);
        return;
    }

    // An opaque white 8×8 square as the shape, its hotspot in the middle.
    capture::CursorState cursor;
    cursor.visible = true;
    cursor.width = cursor.height = 8;
    cursor.inkWidth = cursor.inkHeight = 8;
    cursor.x = 100;
    cursor.y = 60;
    cursor.pixels.assign(8 * 8 * 4, 0xFF);
    cursor.invert.assign(8 * 8, 0);
    cursor.shapeVersion = 1;
    convert::CursorDraw draw;
    draw.magnify = 1.0f;
    draw.hotspotX = draw.hotspotY = 4;

    CHECK(cpu.convert(frame, cursor, draw, error));
    const encode::I420Picture& pic = cpu.picture();
    const auto luma = [&](int x, int y) {
        return static_cast<int>(pic.y[static_cast<size_t>(y) * pic.strideY + x]);
    };
    // Grey 0x40 through BT.709 limited is about 71; the white pointer is 235.
    const int background = luma(5, 5);
    CHECK(background > 60 && background < 85);
    // The shape sits at 100,60 in FRAME pixels and the output is half that, so
    // it covers 50..54, 30..34 — four output pixels of white.
    CHECK_EQ(luma(51, 31), 235);
    CHECK_EQ(luma(53, 33), 235);
    CHECK_EQ(luma(60, 31), background); // clear of it
    CHECK_EQ(luma(51, 45), background);

    // Hidden again, and the picture comes back clean: the planes are rewritten
    // from the scanout every time, so nothing of the pointer survives.
    capture::CursorState gone;
    CHECK(cpu.convert(frame, gone, draw, error));
    CHECK_EQ(luma(51, 31), background);
    CHECK_EQ(luma(53, 33), background);

    ::close(fd);
#endif
}
