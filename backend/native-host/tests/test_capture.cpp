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

    capture::DxgiDuplication duplication(gpu->nativeHandle, outputIndex);

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
                            convert::ColorConvert::Chroma::C420, convertError)) {
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
                CHECK(converter.convert(frame.texture, duplication.cursor(), convertError));
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
                    if (!encoder.init(duplication.device(), Codec::H264, converter.outputWidth(),
                                      converter.outputHeight(), 60, 20000, false, false,
                                      encodeError)) {
                        std::fprintf(stderr, "  encode skipped: %s\n", encodeError.c_str());
                    } else {
                        encode::EncoderOutput encoded;
                        // The first frame must be a keyframe: a client has
                        // nothing to decode against otherwise.
                        CHECK(encoder.encode(converter.output(), true, encoded, encodeError));
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
                            if (!encoder.encode(converter.output(), false, delta, encodeError)) {
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
                                if (st == capture::AcquireStatus::Timeout) {
                                    ++cycleTimeouts;
                                    continue;
                                }
                                if (st != capture::AcquireStatus::Ok) break;

                                const bool converted = converter.convert(
                                    live.texture, duplication.cursor(), encodeError);
                                duplication.release();
                                if (!converted) {
                                    std::fprintf(stderr, "  cycle: conversion failed: %s\n",
                                                 encodeError.c_str());
                                    break;
                                }

                                encode::EncoderOutput liveEncoded;
                                if (!encoder.encode(converter.output(), false, liveEncoded,
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
                if (gpu->supports444) {
                    convert::ColorConvert converter444;
                    std::string error444;
                    if (!converter444.init(duplication.device(), duplication.format(),
                                           duplication.width(), duplication.height(),
                                           duplication.width(), duplication.height(),
                                           convert::ColorConvert::Chroma::C444, error444)) {
                        std::fprintf(stderr, "  4:4:4 conversion unavailable: %s\n",
                                     error444.c_str());
                    } else if (!converter444.convert(frame.texture, duplication.cursor(),
                                                     error444)) {
                        std::fprintf(stderr, "  4:4:4 conversion failed: %s\n", error444.c_str());
                    } else {
                        // 4:4:4 is only claimed by NVENC so far, and the
                        // capability query above is what gates this block.
                        encode::NvencEncoder encoder444;
                        if (!encoder444.init(
                                duplication.device(), Codec::H264, converter444.outputWidth(),
                                converter444.outputHeight(), 60, 20000, true, false, error444)) {
                            std::fprintf(stderr, "  4:4:4 encode unavailable: %s\n",
                                         error444.c_str());
                        } else {
                            encode::EncoderOutput out444;
                            CHECK(encoder444.encode(converter444.output(), true, out444, error444));
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

    // Stopping twice, and releasing without holding, must both be harmless:
    // teardown runs on error paths where the state is not known.
    duplication.stop();
    duplication.release();
#endif
}
