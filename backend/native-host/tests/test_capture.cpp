/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "mw/native/NativeHost.h"

#if defined(_WIN32)
#include "capture/windows/DxgiDuplication.h"
#include "convert/windows/ColorConvert.h"
#include "encode/windows/AmfEncoder.h"
#include "capture/windows/WgcCapture.h"
#include "encode/windows/NvencEncoder.h"
#include <memory>
#include <wrl/client.h>
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace mw::native;

// Capture is the one part of the engine that cannot be faked: it either gets
// real pixels off a real display or it does not. So this exercises the actual
// Desktop Duplication path against whatever display the machine has.
//
// It is written to be honest on a machine that cannot capture — a CI runner
// with no desktop, a build in session 0 — by reporting "skipped" rather than
// failing. A test that fails wherever the hardware is absent gets disabled, and
// a disabled test protects nothing.

void run_capture_tests()
{
#if defined(_WIN32)
    // ── The HDR/SDR pairing rules, which need no hardware at all ────────────
    //
    // These are static answers about which byte formats have a shader and which
    // of them carry HDR, and they decide what the capture is OPENED with — long
    // before any display is involved. They are checked here rather than left to
    // a machine with an HDR panel because getting them wrong does not produce
    // an error, it produces a picture with the wrong transfer curve.
    SECTION("Colour conversion — which sources exist, and which are HDR");
    {
        using convert::ColorConvert;
        CHECK(ColorConvert::supportsSource(DXGI_FORMAT_B8G8R8A8_UNORM));
        CHECK(ColorConvert::supportsSource(DXGI_FORMAT_R8G8B8A8_UNORM));
        // FP16 scRGB became convertible when the PQ path landed; before that it
        // was refused, and the session downgraded itself to SDR on every HDR
        // desktop.
        CHECK(ColorConvert::supportsSource(DXGI_FORMAT_R16G16B16A16_FLOAT));
        CHECK(!ColorConvert::supportsSource(DXGI_FORMAT_R10G10B10A2_UNORM));
        CHECK(!ColorConvert::supportsSource(DXGI_FORMAT_UNKNOWN));

        // Exactly one source format carries HDR, and 8-bit never does — that is
        // what lets the session reconcile "HDR was negotiated" against "the
        // display actually handed over 8-bit" instead of trusting the request.
        CHECK(ColorConvert::isHdrSource(DXGI_FORMAT_R16G16B16A16_FLOAT));
        CHECK(!ColorConvert::isHdrSource(DXGI_FORMAT_B8G8R8A8_UNORM));
        CHECK(!ColorConvert::isHdrSource(DXGI_FORMAT_R8G8B8A8_UNORM));

        // Exactly one of the convertible formats is the HDR one. A second would
        // mean a source that reaches the PQ shaders without anyone having
        // written its scale factor, and the picture would be wrong by whatever
        // that factor is — not broken, just wrong.
        int convertible = 0;
        int hdrCapable = 0;
        for (DXGI_FORMAT f : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                              DXGI_FORMAT_NV12, DXGI_FORMAT_P010}) {
            if (ColorConvert::supportsSource(f)) ++convertible;
            if (ColorConvert::isHdrSource(f)) ++hdrCapable;
        }
        CHECK_EQ(convertible, 3);
        CHECK_EQ(hdrCapable, 1);
    }
#endif

    SECTION("Capture — Desktop Duplication against the real display");

#if !defined(_WIN32)
    std::fprintf(stderr, "  skipped: no Windows capture backend in this build\n");
#else
    const Capabilities caps = NativeHost::probe();
    if (caps.displays.empty()) {
        std::fprintf(stderr, "  skipped: no display attached (%s)\n",
                     caps.diagnostic.empty() ? toString(caps.reason) : caps.diagnostic.c_str());
        return;
    }

    // The primary display, on the GPU that actually drives it — the pairing the
    // whole zero-copy premise rests on.
    const DisplayInfo* target = &caps.displays.front();
    for (const DisplayInfo& d : caps.displays) {
        if (d.primary) {
            target = &d;
            break;
        }
    }
    const GpuInfo* gpu = caps.gpuFor(*target);
    if (!gpu) {
        std::fprintf(stderr, "  skipped: display %d names no GPU\n", target->id);
        return;
    }

    // The display's index within its own adapter is what DXGI wants, and it is
    // not the global display id on a multi-GPU machine.
    unsigned outputIndex = 0;
    for (const DisplayInfo& d : caps.displays) {
        if (d.id == target->id) break;
        if (d.gpuId == target->gpuId) ++outputIndex;
    }

    // SDR, as every session is in this build: on a desktop with Windows HDR on
    // this is what makes the duplication deliver BGRA8 rather than FP16.
    capture::DxgiDuplication duplication(gpu->nativeHandle, outputIndex, /*hdr=*/false);

    std::string error;
    if (!duplication.start(error)) {
        // A real refusal is worth printing but is not a test failure: Desktop
        // Duplication is legitimately unavailable in a remote session or on a
        // hybrid output, which is exactly why a WGC fallback is planned.
        std::fprintf(stderr, "  skipped: %s\n", error.c_str());
        return;
    }

    CHECK(duplication.width() > 0);
    CHECK(duplication.height() > 0);
    CHECK(duplication.device() != nullptr);
    std::fprintf(stderr, "  duplicating %dx%d on %s\n", duplication.width(), duplication.height(),
                 gpu->name.c_str());

    // The probe and the capture must agree on the size of the screen.
    //
    // They did not, and the discrepancy was invisible until both ran on real
    // hardware: DXGI_OUTPUT_DESC::DesktopCoordinates is DPI-scaled for a
    // process that is not per-monitor DPI aware, so a 2560×1440 panel at 125 %
    // was probed as 2048×1152 while duplication delivered the full 2560×1440.
    // The user would have been offered — and streamed — the wrong resolution.
    //
    // This is the assertion that would have caught it on the first run.
    CHECK_EQ(duplication.width(), target->width);
    CHECK_EQ(duplication.height(), target->height);
    if (duplication.width() != target->width || duplication.height() != target->height) {
        std::fprintf(stderr, "  MISMATCH: probe says %dx%d, duplication gives %dx%d\n",
                     target->width, target->height, duplication.width(), duplication.height());
    }

    // Nudge the desktop so there is something to capture: on a still screen
    // Desktop Duplication correctly reports nothing, and a test that waited for
    // spontaneous damage would be flaky by design.
    int captured = 0;
    int timeouts = 0;
    int pointerOnly = 0;
    int64_t worstLatencyUs = 0;
    int64_t totalLatencyUs = 0;

    for (int attempt = 0; attempt < 60 && captured < 5; ++attempt) {
        capture::CapturedFrame frame;
        const capture::AcquireStatus status = duplication.acquire(100, frame);

        if (status == capture::AcquireStatus::Timeout) {
            ++timeouts;
            continue;
        }
        if (status == capture::AcquireStatus::PointerOnly) {
            // The mouse moved and the desktop did not. No texture comes with
            // this, but the cursor state must be usable: it is what the
            // conversion pass draws from, and a shape with no pixels would
            // silently produce an invisible pointer.
            ++pointerOnly;
            const capture::CursorState& c = duplication.cursor();
            if (c.visible && c.width > 0) {
                CHECK(c.height > 0);
                CHECK_EQ(c.pixels.size(), static_cast<size_t>(c.width) * c.height * 4);
                CHECK_EQ(c.invert.size(), static_cast<size_t>(c.width) * c.height);
                CHECK(c.shapeVersion > 0);
            }
            continue;
        }
        if (status == capture::AcquireStatus::Lost) {
            // A mode change mid-test is not a defect; recovering from it is the
            // documented contract, so exercise that instead of giving up.
            std::fprintf(stderr, "  duplication lost — restarting\n");
            if (!duplication.start(error)) {
                std::fprintf(stderr, "  restart failed: %s\n", error.c_str());
                return;
            }
            continue;
        }
        CHECK(status == capture::AcquireStatus::Ok);
        if (status != capture::AcquireStatus::Ok) break;

        // The frame must be a real texture on the device we opened, and its
        // timestamps must be ordered: a present cannot happen after the capture
        // that observed it.
        CHECK(frame.texture != nullptr);
        CHECK(frame.presentUs > 0);
        CHECK(frame.capturedUs >= frame.presentUs);

        const int64_t latencyUs = frame.capturedUs - frame.presentUs;
        totalLatencyUs += latencyUs;
        if (latencyUs > worstLatencyUs) worstLatencyUs = latencyUs;
        ++captured;

        duplication.release();
    }

    std::fprintf(stderr, "  frames=%d timeouts=%d pointer-only=%d\n", captured, timeouts,
                 pointerOnly);
    if (captured > 0) {
        std::fprintf(stderr, "  capture latency: mean %.2f ms, worst %.2f ms\n",
                     static_cast<double>(totalLatencyUs) / captured / 1000.0,
                     static_cast<double>(worstLatencyUs) / 1000.0);

        // The clock calibration is the thing most likely to be silently wrong,
        // and its failure mode is a latency in the hours. Anything past a
        // second means present and capture are being read off different epochs.
        CHECK(totalLatencyUs / captured < 1000000);
    } else {
        std::fprintf(stderr, "  note: the screen never changed during the test — "
                             "no frame to measure\n");
    }

    // ── Capture → conversion, and real pixels at the end of it ──────────
    //
    // The only way to know the conversion works is to look at what it produced.
    // A shader that compiles, binds and draws nothing at all would pass every
    // structural check and hand the encoder a black screen.
    if (captured > 0) {
        convert::ColorConvert converter;
        std::string convertError;
        if (!converter.init(duplication.device(), duplication.format(), duplication.width(),
                            duplication.height(), duplication.width(), duplication.height(),
                            convert::ColorConvert::Chroma::C420,
                            convert::ColorConvert::isHdrSource(duplication.format()),
                            convertError)) {
            std::fprintf(stderr, "  conversion skipped: %s\n", convertError.c_str());
        } else {
            // Grab one more frame to convert. The screen may be still, so allow
            // a generous window rather than failing on a quiet desktop.
            capture::CapturedFrame frame;
            bool haveFrame = false;
            for (int attempt = 0; attempt < 40 && !haveFrame; ++attempt) {
                if (duplication.acquire(100, frame) == capture::AcquireStatus::Ok) haveFrame = true;
            }

            if (!haveFrame) {
                std::fprintf(stderr, "  conversion not exercised: the screen stayed still\n");
            } else {
                if (!converter.convert(frame.texture, duplication.cursor(), convert::CursorDraw{},
                                       convertError))
                    std::fprintf(stderr, "  conversion failed: %s\n", convertError.c_str());
                CHECK(converter.convert(frame.texture, duplication.cursor(), convert::CursorDraw{},
                                        convertError));
                CHECK(converter.output() != nullptr);
                CHECK_EQ(converter.outputWidth(), duplication.width());
                CHECK_EQ(converter.outputHeight(), duplication.height());

                // Read the luma plane back and look at it. Not a pixel-accuracy
                // test — that belongs to a reference image — but enough to
                // catch the failure that matters: an output that is uniformly
                // one value is a black (or blank) screen, whatever the shader
                // claimed to do.
                Microsoft::WRL::ComPtr<ID3D11Device> device = duplication.device();
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
                device->GetImmediateContext(&context);

                D3D11_TEXTURE2D_DESC desc = {};
                converter.output()->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0;

                Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
                if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging))) {
                    context->CopyResource(staging.Get(), converter.output());

                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                        const auto* rows = static_cast<const uint8_t*>(mapped.pData);
                        uint8_t minLuma = 255;
                        uint8_t maxLuma = 0;
                        // Sample a grid rather than every pixel: enough to prove
                        // the image is not uniform, cheap enough to stay in a
                        // unit test.
                        for (int y = 0; y < converter.outputHeight(); y += 16) {
                            const uint8_t* row = rows + static_cast<size_t>(y) * mapped.RowPitch;
                            for (int x = 0; x < converter.outputWidth(); x += 16) {
                                minLuma = (row[x] < minLuma) ? row[x] : minLuma;
                                maxLuma = (row[x] > maxLuma) ? row[x] : maxLuma;
                            }
                        }
                        context->Unmap(staging.Get(), 0);

                        std::fprintf(stderr, "  NV12 luma range: %u..%u\n", minLuma, maxLuma);

                        // BT.709 limited range puts black at 16 and white at
                        // 235. A desktop always contains more than one shade,
                        // so a flat image means the conversion produced nothing.
                        CHECK(maxLuma > minLuma);
                        // And it must sit inside the legal range: values below
                        // 16 or above 235 mean the limited-range scaling was
                        // skipped, which shows up as crushed blacks on the
                        // client.
                        CHECK(minLuma >= 16);
                        CHECK(maxLuma <= 235);
                    }
                }

                // ── …and through the encoder
                // ─────────────────────────────────
                //
                // The end of the pipeline. What matters is not that the encoder
                // returns success but that what it returns is a bitstream the
                // browser could actually decode — so the Annex-B structure is
                // inspected rather than trusted.
                // Whichever vendor drives this display — the point is that the
                // pipeline works on the GPU the machine actually has, not on
                // the one the test was written against.
                std::unique_ptr<encode::IVideoEncoder> encoderPtr;
                if (!gpu->codecs.empty()) {
                    switch (gpu->encoders.front()) {
                    case EncoderApi::Nvenc:
                        encoderPtr = std::make_unique<encode::NvencEncoder>();
                        break;
                    case EncoderApi::Amf:
                        encoderPtr = std::make_unique<encode::AmfEncoder>();
                        break;
                    default: break;
                    }
                }

                if (encoderPtr) {
                    encode::IVideoEncoder& encoder = *encoderPtr;
                    std::fprintf(stderr, "  encoder: %s\n", toString(gpu->encoders.front()));
                    std::string encodeError;
                    // H.264 has no HDR path, so this leg is SDR whatever the
                    // desktop is doing. A converter that came up on the PQ path
                    // would hand it P010, so skip rather than mislead.
                    if (converter.hdr()) {
                        std::fprintf(stderr, "  encode skipped: the desktop is HDR and this leg "
                                             "encodes H.264 8-bit\n");
                    } else if (!encoder.init(duplication.device(), Codec::H264,
                                             converter.outputWidth(), converter.outputHeight(), 60,
                                             20000, false, false, false, EncoderTuning{},
                                             encodeError)) {
                        std::fprintf(stderr, "  encode skipped: %s\n", encodeError.c_str());
                    } else {
                        encode::EncoderOutput encoded;
                        // The first frame must be a keyframe: a client has
                        // nothing to decode against otherwise.
                        CHECK(encoder.encode(converter.output(), true, 0, encoded, encodeError));
                        if (encoded.data) {
                            std::fprintf(
                                stderr, "  encoded keyframe: %zu bytes, intra-refresh %s\n",
                                encoded.size, encoder.intraRefreshEnabled() ? "on" : "off");

                            CHECK(encoded.size > 0);
                            CHECK(encoded.keyframe);

                            // Annex-B: the stream must open with a start code,
                            // or the browser's NAL parser never finds its first
                            // unit and the picture stays black.
                            CHECK(encoded.size > 4);
                            const bool startsWithStartCode =
                                encoded.data[0] == 0 && encoded.data[1] == 0 &&
                                ((encoded.data[2] == 1) ||
                                 (encoded.data[2] == 0 && encoded.data[3] == 1));
                            CHECK(startsWithStartCode);

                            // And it must carry SPS (NAL type 7) and PPS (8)
                            // ahead of the picture: that is what the decoder
                            // configures itself from, and why repeatSPSPPS is
                            // set. Without them a client that joins mid-stream
                            // can never start.
                            bool haveSps = false;
                            bool havePps = false;
                            for (size_t i = 0; i + 4 < encoded.size; ++i) {
                                if (encoded.data[i] != 0 || encoded.data[i + 1] != 0) continue;
                                size_t nal = 0;
                                if (encoded.data[i + 2] == 1)
                                    nal = i + 3;
                                else if (encoded.data[i + 2] == 0 && encoded.data[i + 3] == 1)
                                    nal = i + 4;
                                else
                                    continue;
                                if (nal >= encoded.size) break;
                                const uint8_t type = encoded.data[nal] & 0x1F;
                                if (type == 7) haveSps = true;
                                if (type == 8) havePps = true;
                            }
                            std::fprintf(stderr, "  SPS %s, PPS %s\n",
                                         haveSps ? "present" : "MISSING",
                                         havePps ? "present" : "MISSING");
                            CHECK(haveSps);
                            CHECK(havePps);

                            encoder.releaseOutput();
                        }

                        // ── Encode latency
                        // ───────────────────────────────────
                        //
                        // The number the whole project is judged on, measured
                        // rather than assumed. Static content, so this is the
                        // encoder's own round trip with no motion to search —
                        // a floor, not a typical-load figure.
                        int64_t worstUs = 0;
                        int64_t totalUs = 0;
                        int encodedCount = 0;
                        for (int i = 0; i < 30; ++i) {
                            const auto before = std::chrono::steady_clock::now();
                            encode::EncoderOutput delta;
                            if (!encoder.encode(converter.output(), false, 1, delta, encodeError)) {
                                std::fprintf(stderr, "  encode stopped at frame %d: %s\n", i,
                                             encodeError.c_str());
                                break;
                            }
                            const auto after = std::chrono::steady_clock::now();
                            encoder.releaseOutput();

                            const int64_t us =
                                std::chrono::duration_cast<std::chrono::microseconds>(after -
                                                                                      before)
                                    .count();
                            totalUs += us;
                            if (us > worstUs) worstUs = us;
                            ++encodedCount;
                        }
                        if (encodedCount > 0) {
                            std::fprintf(stderr,
                                         "  encode latency (static): mean %.2f ms, worst %.2f ms "
                                         "over %d frames\n",
                                         static_cast<double>(totalUs) / encodedCount / 1000.0,
                                         static_cast<double>(worstUs) / 1000.0, encodedCount);
                            // A frame that takes longer than a 60 Hz frame
                            // interval cannot keep up at all; well past that
                            // means something is badly misconfigured.
                            CHECK(totalUs / encodedCount < 16000);
                        }

                        // ── The real cycle: capture → convert → encode
                        // ───────
                        //
                        // What a session actually does, and what encoding the
                        // same already-converted texture thirty times does NOT
                        // exercise: the converter rewrites the encoder's input
                        // between frames. An encoder that tolerates a static
                        // buffer but stalls when its input changes underneath it
                        // passes the loop above and dies in the field — which is
                        // exactly what happened on the AMD path, one frame in.
                        //
                        // Run on a thread OTHER than the one that built the
                        // encoder, because that is what a session does — start()
                        // on the caller's thread, the loop on its own. An
                        // encoder that only works where it was created passes
                        // every single-threaded test and takes the worker
                        // process down on the second frame.
                        int cycled = 0;
                        int cycleTimeouts = 0;
                        std::thread cycleThread([&] {
                            for (int attempt = 0; attempt < 120 && cycled < 20; ++attempt) {
                                capture::CapturedFrame live;
                                const capture::AcquireStatus st = duplication.acquire(50, live);
                                // A pointer-only wake-up carries no texture, so
                                // it is nothing to this loop — counted with the
                                // timeouts rather than treated as the end of the
                                // stream, which is what stopped it after one
                                // frame and made this look like a GPU fault.
                                if (st == capture::AcquireStatus::Timeout ||
                                    st == capture::AcquireStatus::PointerOnly) {
                                    ++cycleTimeouts;
                                    continue;
                                }
                                if (st != capture::AcquireStatus::Ok) break;

                                const bool converted =
                                    converter.convert(live.texture, duplication.cursor(),
                                                      convert::CursorDraw{}, encodeError);
                                duplication.release();
                                if (!converted) {
                                    std::fprintf(stderr, "  cycle: conversion failed: %s\n",
                                                 encodeError.c_str());
                                    break;
                                }

                                encode::EncoderOutput liveEncoded;
                                if (!encoder.encode(converter.output(), false, 2, liveEncoded,
                                                    encodeError)) {
                                    std::fprintf(stderr, "  cycle stopped after %d frames: %s\n",
                                                 cycled, encodeError.c_str());
                                    break;
                                }
                                encoder.releaseOutput();
                                ++cycled;
                            }
                        });
                        cycleThread.join();
                        std::fprintf(stderr,
                                     "  capture->convert->encode cycles: %d (timeouts %d)\n",
                                     cycled, cycleTimeouts);
                        // One frame proves nothing: the failure being guarded
                        // against produced exactly one and then stopped.
                        if (cycleTimeouts < 100) CHECK(cycled > 1);

                        // Changing bitrate mid-session must not need a restart:
                        // it is what lets the encoder follow the client's real
                        // feedback frame by frame.
                        CHECK(encoder.setBitrate(10000, encodeError));
                        encoder.stop();
                    }
                }

                // ── 4:4:4
                // ────────────────────────────────────────────────────
                //
                // MoonlightWeb offers this choice for external hosts, so the
                // native engine has to be able to honour it. Verified rather
                // than assumed: the AYUV byte order is easy to get wrong, and
                // getting it wrong swaps the colours instead of failing.
                // 4:4:4 is 8-bit only, so an HDR desktop has nothing to offer
                // this leg: the converter refuses HDR + C444 by construction.
                if (gpu->supports444(Codec::H264) &&
                    !convert::ColorConvert::isHdrSource(duplication.format())) {
                    convert::ColorConvert converter444;
                    std::string error444;
                    if (!converter444.init(duplication.device(), duplication.format(),
                                           duplication.width(), duplication.height(),
                                           duplication.width(), duplication.height(),
                                           convert::ColorConvert::Chroma::C444, false, error444)) {
                        std::fprintf(stderr, "  4:4:4 conversion unavailable: %s\n",
                                     error444.c_str());
                    } else if (!converter444.convert(frame.texture, duplication.cursor(),
                                                     convert::CursorDraw{}, error444)) {
                        std::fprintf(stderr, "  4:4:4 conversion failed: %s\n", error444.c_str());
                    } else {
                        // 4:4:4 is only claimed by NVENC so far, and the
                        // capability query above is what gates this block.
                        encode::NvencEncoder encoder444;
                        if (!encoder444.init(duplication.device(), Codec::H264,
                                             converter444.outputWidth(),
                                             converter444.outputHeight(), 60, 20000, true, false,
                                             false, EncoderTuning{}, error444)) {
                            std::fprintf(stderr, "  4:4:4 encode unavailable: %s\n",
                                         error444.c_str());
                        } else {
                            encode::EncoderOutput out444;
                            CHECK(encoder444.encode(converter444.output(), true, 0, out444,
                                                    error444));
                            if (out444.data) {
                                std::fprintf(stderr, "  4:4:4 keyframe: %zu bytes\n", out444.size);
                                CHECK(out444.size > 0);
                                CHECK(out444.keyframe);
                                encoder444.releaseOutput();
                            }
                            encoder444.stop();
                        }
                    }
                } else {
                    std::fprintf(stderr, "  4:4:4 not supported by this encoder\n");
                }

                duplication.release();
            }
        }
    }

    duplication.stop();

    // ── The same walk again, in HDR ─────────────────────────────
    //
    // Only on a display Windows really has in an HDR mode: the
    // whole point is that DXGI then hands over FP16 scRGB, which is
    // the only input the PQ shaders have. On an SDR desktop there
    // is nothing here to exercise and the leg says so.
    //
    // Worth its own duplication because HDR is decided when the
    // capture is OPENED, not per frame — and because the failure
    // this catches is not a crash. A PQ curve applied to the wrong
    // scale, or a P010 write that forgets the 6-bit shift, produces
    // a picture that is merely dark or flat, and the range below is
    // what tells the two apart from a working one.
    //
    // Placed AFTER the SDR duplication has been stopped, and that is not a
    // matter of taste: DXGI allows one duplication per output per process, so
    // opening the HDR one beside the SDR one is refused outright with
    // E_INVALIDARG.
    if (target->hdrActive) {
        capture::DxgiDuplication hdrDup(gpu->nativeHandle, outputIndex, /*hdr=*/true);
        std::string hdrError;
        if (!hdrDup.start(hdrError)) {
            std::fprintf(stderr, "  HDR capture skipped: %s\n", hdrError.c_str());
        } else if (!convert::ColorConvert::isHdrSource(hdrDup.format())) {
            // Asked for FP16 and given 8-bit: legal, and exactly the
            // case the session reconciles instead of trusting.
            std::fprintf(stderr, "  HDR capture skipped: the display handed over "
                                 "8-bit despite being in an HDR mode\n");
        } else {
            convert::ColorConvert hdrConv;
            CHECK(hdrConv.init(hdrDup.device(), hdrDup.format(), hdrDup.width(), hdrDup.height(),
                               hdrDup.width(), hdrDup.height(), convert::ColorConvert::Chroma::C420,
                               true, hdrError));
            CHECK(hdrConv.hdr());

            capture::CapturedFrame hdrFrame;
            bool haveHdrFrame = false;
            for (int attempt = 0; attempt < 40 && !haveHdrFrame; ++attempt) {
                if (hdrDup.acquire(100, hdrFrame) == capture::AcquireStatus::Ok)
                    haveHdrFrame = true;
            }

            if (!haveHdrFrame) {
                std::fprintf(stderr, "  HDR conversion not exercised: the screen "
                                     "stayed still\n");
            } else {
                CHECK(hdrConv.convert(hdrFrame.texture, hdrDup.cursor(), convert::CursorDraw{},
                                      hdrError));

                Microsoft::WRL::ComPtr<ID3D11Device> hdrDevice = hdrDup.device();
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> hdrContext;
                hdrDevice->GetImmediateContext(&hdrContext);

                D3D11_TEXTURE2D_DESC p010 = {};
                hdrConv.output()->GetDesc(&p010);
                CHECK_EQ(static_cast<int>(p010.Format), static_cast<int>(DXGI_FORMAT_P010));
                p010.Usage = D3D11_USAGE_STAGING;
                p010.BindFlags = 0;
                p010.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                p010.MiscFlags = 0;

                Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrStaging;
                if (SUCCEEDED(hdrDevice->CreateTexture2D(&p010, nullptr, &hdrStaging))) {
                    hdrContext->CopyResource(hdrStaging.Get(), hdrConv.output());
                    D3D11_MAPPED_SUBRESOURCE hdrMapped = {};
                    if (SUCCEEDED(
                            hdrContext->Map(hdrStaging.Get(), 0, D3D11_MAP_READ, 0, &hdrMapped))) {
                        const auto* bytes = static_cast<const uint8_t*>(hdrMapped.pData);
                        uint16_t minY = 0xFFFF;
                        uint16_t maxY = 0;
                        for (int y = 0; y < hdrConv.outputHeight(); y += 16) {
                            const auto* row = reinterpret_cast<const uint16_t*>(
                                bytes + static_cast<size_t>(y) * hdrMapped.RowPitch);
                            for (int x = 0; x < hdrConv.outputWidth(); x += 16) {
                                minY = row[x] < minY ? row[x] : minY;
                                maxY = row[x] > maxY ? row[x] : maxY;
                            }
                        }
                        hdrContext->Unmap(hdrStaging.Get(), 0);

                        // Reported in the 10-bit codes the format
                        // really carries, not in the 16-bit words it
                        // stores them in — that shift is the mistake
                        // being guarded against, so the numbers a
                        // reader compares must be on the same scale
                        // as the 64..940 the standard names.
                        const int minCode = minY >> 6;
                        const int maxCode = maxY >> 6;
                        std::fprintf(stderr, "  P010 luma range: %d..%d (10-bit)\n", minCode,
                                     maxCode);

                        CHECK(maxCode > minCode);
                        // BT.2020 limited range, 10-bit: 64..940.
                        // Below 64 means the bias was skipped, above
                        // 940 means the scale was; either way the
                        // client sees crushed or clipped highlights
                        // rather than an error.
                        CHECK(minCode >= 64);
                        CHECK(maxCode <= 940);
                        // The low 6 bits are padding in P010 and
                        // must be zero. A non-zero one means the
                        // shader wrote the code as if the field were
                        // 16 bits wide, and the picture is then 64×
                        // too bright at the bottom of the range.
                        CHECK_EQ(maxY & 0x3F, 0);
                    }
                }

                // And through a 10-bit encoder: Main10 has to be
                // named, or NVENC takes the P010 and quietly encodes
                // 8 bits from it.
                if (gpu->encoders.front() == EncoderApi::Nvenc && gpu->supports10Bit) {
                    encode::NvencEncoder hdrEnc;
                    if (!hdrEnc.init(hdrDup.device(), Codec::Hevc, hdrConv.outputWidth(),
                                     hdrConv.outputHeight(), 60, 20000, false, true, false,
                                     EncoderTuning{}, hdrError)) {
                        std::fprintf(stderr, "  HDR encode unavailable: %s\n", hdrError.c_str());
                    } else {
                        encode::EncoderOutput hdrOut;
                        CHECK(hdrEnc.encode(hdrConv.output(), true, 0, hdrOut, hdrError));
                        if (hdrOut.data) {
                            std::fprintf(stderr, "  HEVC Main10 keyframe: %zu bytes\n",
                                         hdrOut.size);
                            CHECK(hdrOut.size > 0);
                            CHECK(hdrOut.keyframe);
                            hdrEnc.releaseOutput();
                        }
                        hdrEnc.stop();
                    }
                }
            }
        }
    }

    // Stopping twice, and releasing without holding, must both be harmless:
    // teardown runs on error paths where the state is not known.
    duplication.stop();
    duplication.release();

    // ── Windows.Graphics.Capture, the fallback ──────────────────────────────
    //
    // Exercised on every machine, not only on the ones that need it. A fallback
    // that is written once and never run is a fallback that is broken on the
    // single machine it was written for — and that machine is a hybrid laptop
    // somebody else owns.
    //
    // The same proof as the Desktop Duplication leg above: real pixels, read
    // back, and looked at. "The session started" is not evidence — a capture
    // that hands over a black texture starts perfectly well.
    {
        capture::WgcCapture wgc(gpu->nativeHandle, outputIndex);
        std::string wgcError;
        if (!capture::WgcCapture::available()) {
            std::fprintf(stderr, "  WGC skipped: not available on this Windows build\n");
        } else if (!wgc.start(wgcError)) {
            std::fprintf(stderr, "  WGC skipped: %s\n", wgcError.c_str());
        } else {
            CHECK(wgc.width() > 0);
            CHECK(wgc.height() > 0);
            CHECK(wgc.device() != nullptr);
            CHECK(wgc.desktopRect().valid());
            // WGC has no HDR path here, deliberately: see openCapture().
            CHECK_EQ(static_cast<int>(wgc.format()), static_cast<int>(DXGI_FORMAT_B8G8R8A8_UNORM));

            capture::CapturedFrame wgcFrame;
            bool haveWgcFrame = false;
            for (int attempt = 0; attempt < 40 && !haveWgcFrame; ++attempt) {
                if (wgc.acquire(100, wgcFrame) == capture::AcquireStatus::Ok) haveWgcFrame = true;
            }

            if (!haveWgcFrame) {
                std::fprintf(stderr, "  WGC not exercised: the screen stayed still\n");
            } else {
                // A real present time, not the moment we noticed: every latency
                // figure downstream is measured from it, so a frame stamped
                // "now" would quietly report zero capture latency forever.
                std::fprintf(stderr, "  WGC capture latency: %lld us\n",
                             static_cast<long long>(wgcFrame.capturedUs - wgcFrame.presentUs));
                CHECK(wgcFrame.presentUs > 0);
                // Never negative. WGC's own stamp leads the pull by a few
                // milliseconds (it is a scheduled presentation, not a completed
                // one), so the backend caps it at "now" — see acquire(). Left
                // uncapped this is the value that would have the link governor
                // cut the bitrate on a healthy link.
                CHECK(wgcFrame.capturedUs >= wgcFrame.presentUs);

                convert::ColorConvert wgcConv;
                CHECK(wgcConv.init(wgc.device(), wgc.format(), wgc.width(), wgc.height(),
                                   wgc.width(), wgc.height(), convert::ColorConvert::Chroma::C420,
                                   false, wgcError));
                CHECK(wgcConv.convert(wgcFrame.texture, wgc.cursor(), convert::CursorDraw{},
                                      wgcError));

                Microsoft::WRL::ComPtr<ID3D11Device> wgcDevice = wgc.device();
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> wgcContext;
                wgcDevice->GetImmediateContext(&wgcContext);

                D3D11_TEXTURE2D_DESC nv12 = {};
                wgcConv.output()->GetDesc(&nv12);
                nv12.Usage = D3D11_USAGE_STAGING;
                nv12.BindFlags = 0;
                nv12.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                nv12.MiscFlags = 0;

                Microsoft::WRL::ComPtr<ID3D11Texture2D> wgcStaging;
                if (SUCCEEDED(wgcDevice->CreateTexture2D(&nv12, nullptr, &wgcStaging))) {
                    wgcContext->CopyResource(wgcStaging.Get(), wgcConv.output());
                    D3D11_MAPPED_SUBRESOURCE wgcMapped = {};
                    if (SUCCEEDED(
                            wgcContext->Map(wgcStaging.Get(), 0, D3D11_MAP_READ, 0, &wgcMapped))) {
                        const auto* rows = static_cast<const uint8_t*>(wgcMapped.pData);
                        uint8_t minLuma = 255;
                        uint8_t maxLuma = 0;
                        for (int y = 0; y < wgcConv.outputHeight(); y += 16) {
                            const uint8_t* row = rows + static_cast<size_t>(y) * wgcMapped.RowPitch;
                            for (int x = 0; x < wgcConv.outputWidth(); x += 16) {
                                minLuma = (row[x] < minLuma) ? row[x] : minLuma;
                                maxLuma = (row[x] > maxLuma) ? row[x] : maxLuma;
                            }
                        }
                        wgcContext->Unmap(wgcStaging.Get(), 0);

                        std::fprintf(stderr, "  WGC NV12 luma range: %u..%u\n", minLuma, maxLuma);
                        CHECK(maxLuma > minLuma);
                        CHECK(minLuma >= 16);
                        CHECK(maxLuma <= 235);
                    }
                }

                wgc.release();
            }
            wgc.stop();
            // Same teardown contract as above: idempotent, in any order.
            wgc.stop();
            wgc.release();
        }
    }
#endif
}
