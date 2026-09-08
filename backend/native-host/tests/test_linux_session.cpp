/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "mw/native/NativeHost.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(MW_NATIVE_LINUX_AUDIO) && defined(MW_NATIVE_TESTS_HAVE_OPUS)
#include <opus.h>
#endif

using namespace mw::native;

// A whole Linux session, the way the backend would run one: probe, select,
// create, stream for a moment, stop. The frames go to a file so the machine's
// own decoder can look at them afterwards — that is the only proof of
// orientation and colour this side of a browser. The audio packets are decoded
// back with libopus and their peak printed: a paced silence and a captured tone
// have the same cadence, and only the signal tells them apart.

void run_linux_session_tests()
{
    SECTION("Linux — a whole session through NativeHost, frames to a file");

#if !defined(MW_NATIVE_LINUX_GFX)
    std::fprintf(stderr, "  skipped: Linux graphics backend not built\n");
#else
    const Capabilities caps = NativeHost::probe();
    if (!caps.available) {
        std::fprintf(stderr, "  skipped: %s (%s)\n", toString(caps.reason),
                     caps.diagnostic.c_str());
        return;
    }
    // Either route is legitimate, and which one appears says something about
    // the machine: KMS when this process may read the scanout, PipeWire when it
    // may not and a portal answers instead — the AppImage's situation, and the
    // one a plain copy of this binary reproduces exactly (no file capability).
    const bool viaPortal = caps.capture == CaptureApi::PipeWire;
    CHECK((caps.capture == CaptureApi::Kms || viaPortal));
    CHECK(!caps.displays.empty());
    CHECK(!caps.gpus.empty());

    const DisplayInfo* display = nullptr;
    for (const DisplayInfo& d : caps.displays)
        if (d.primary) display = &d;
    if (!display) display = &caps.displays.front();
    const GpuInfo* gpu = caps.gpuFor(*display);
    CHECK(gpu != nullptr);
    if (!gpu || gpu->encoders.empty()) {
        std::fprintf(stderr, "  skipped: the display's GPU has no encoder\n");
        return;
    }
    std::fprintf(stderr, "  %s — %s on %s\n", display->label.c_str(), display->detail.c_str(),
                 gpu->name.c_str());

    SessionConfig config;
    config.displayId = display->id;
    config.fps = 60;
    config.bitrateKbps = 20000;
    config.clientCodecs = {Codec::H264};
    config.intraRefresh = true; // asked; the driver decides, and SessionInfo says

    // The portal route needs a consent. A real host stores the grant it was
    // given and replays it; a test has nowhere to store one, so it takes it
    // from the environment — and without it there is nothing to test but a
    // dialog nobody will answer, which would hang rather than fail.
    if (viaPortal) {
        const char* token = std::getenv("MW_PORTAL_RESTORE_TOKEN");
        if (!token || !*token) {
            std::fprintf(stderr, "  skipped: the portal would raise a consent dialog — set "
                                 "MW_PORTAL_RESTORE_TOKEN to a grant from an earlier run\n");
            return;
        }
        config.portalRestoreToken = token;
        std::fprintf(stderr, "  portal route, replaying a stored grant\n");
    }

    std::atomic<int> frames{0};
    std::atomic<int> keyframes{0};
    std::atomic<bool> firstWasKeyframe{false};
    std::atomic<bool> orderOk{true};
    std::atomic<uint32_t> lastNumber{0};
    std::atomic<int64_t> worstProcessingUs{0};
    std::ofstream out("/tmp/mw-linux-session.h264", std::ios::binary | std::ios::trunc);
    std::string ended;

    // The host's sound, through PipeWire. What is CHECKED is the cadence —
    // the relay advances the RTP clock by one frame per packet, so 200 a
    // second is the contract whether the host plays anything or not. What is
    // printed is the decoded peak, for the bench: a tone played into the
    // default output must show up here as a number well above zero.
    std::atomic<int> audioPackets{0};
    std::atomic<size_t> audioBytes{0};
    std::atomic<bool> audioFrameSizeOk{true};
    std::mutex audioMutex;
    std::vector<std::vector<uint8_t>> audioCopies;
    AudioCallback onAudio = [&](const AudioPacket& p) {
        audioPackets.fetch_add(1);
        audioBytes.fetch_add(p.size);
        if (p.samplesPerChannel != 240) audioFrameSizeOk.store(false);
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioCopies.size() < 1000) audioCopies.emplace_back(p.data, p.data + p.size);
    };

    std::string error;
    std::unique_ptr<Session> session = NativeHost::createSession(
        config,
        [&](const EncodedFrame& f) {
            const int n = frames.fetch_add(1);
            if (f.keyframe) keyframes.fetch_add(1);
            if (n == 0) firstWasKeyframe.store(f.keyframe);
            if (n > 0 && f.frameNumber != lastNumber.load() + 1) orderOk.store(false);
            lastNumber.store(f.frameNumber);
            if (f.processingUs() > worstProcessingUs.load())
                worstProcessingUs.store(f.processingUs());
            out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size));
        },
        onAudio, nullptr, nullptr, [&](const std::string& reason) { ended = reason; }, error);
    if (!session) {
        std::fprintf(stderr, "  createSession failed: %s\n", error.c_str());
        CHECK(false);
        return;
    }
    // Registered before start(), which is where the grant happens. Nothing is
    // EXPECTED here: this run replays a stored token, so the portal raises no
    // dialog and has no new consent to hand back. What is checked is the other
    // half — that a session which asked for nothing does not report a grant,
    // because a host that trusted a spurious one would overwrite a token that
    // works with one that may not.
    std::atomic<int> grants{0};
    std::string grantedToken;
    session->setPortalGrantCallback([&](const std::string& token) {
        grants.fetch_add(1);
        grantedToken = token;
    });

    CHECK(session->start(error));
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
    if (grants.load() > 0)
        std::fprintf(stderr, "  portal granted a NEW consent (%zu bytes)\n", grantedToken.size());
    CHECK_EQ(grants.load(), 0);

    const SessionInfo& info = session->info();
    std::fprintf(stderr, "  session: %dx%d %s via %s on %s, intra-refresh %s, capture %s\n",
                 info.width, info.height, toString(info.codec), toString(info.encoder),
                 info.gpuName.c_str(), info.intraRefresh ? "on" : "off", toString(info.capture));
    CHECK_EQ(info.width, display->width);
    CHECK_EQ(info.height, display->height);
    if (viaPortal) {
        // ⚠️ The portal route reports what it ENDED UP with, which is not what
        // the Selector chose: a compositor that hands over shared memory rather
        // than a DMA-BUF makes the GPU pair impossible, and the session falls to
        // the CPU one. Measured on the bench, GNOME 42 does exactly that.
        CHECK_EQ(static_cast<int>(info.capture), static_cast<int>(CaptureApi::PipeWire));
        // OpenH264 writes its own reference list, so a lost frame costs a
        // keyframe here — and SessionInfo says so rather than promising a
        // repair the client would wait for in vain.
        if (info.encoder == EncoderApi::Software) CHECK(!info.referenceInvalidation);
    } else {
        CHECK_EQ(static_cast<int>(info.capture), static_cast<int>(CaptureApi::Kms));
        CHECK_EQ(static_cast<int>(info.encoder), static_cast<int>(EncoderApi::VaApi));
        // VA-API hands the reference list to the application picture by picture,
        // so every encoder on this path can heal a named loss with a delta. What
        // the driver DOES with that list is a bench question (§19.13); that the
        // session offers it, and therefore that /start promises it to the
        // client, is this one's.
        CHECK(info.referenceInvalidation);
    }

    // Two seconds. A still desktop yields the first frame plus the floor at
    // 2 fps, plus whatever the refinement burst adds; anything moving yields
    // more. Either way there are frames, and the first is a keyframe.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Name a frame as lost and check the stream does NOT answer with a
    // keyframe: the point of reference invalidation is that the repair is an
    // ordinary delta. The count is taken before and after, around a window long
    // enough for several pictures.
    const int keyframesBefore = keyframes.load();
    const uint32_t lost = lastNumber.load();
    session->invalidateReference(lost);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const int keyframesAfterInvalidation = keyframes.load() - keyframesBefore;
    std::fprintf(stderr, "  named frame %u as lost — %d keyframe(s) followed\n", lost,
                 keyframesAfterInvalidation);
    // Only where the session promised a delta repair. Where it did not — the
    // CPU pair — a keyframe is the correct answer and demanding zero would be
    // testing the opposite of what SessionInfo told the client.
    if (info.referenceInvalidation) CHECK_EQ(keyframesAfterInvalidation, 0);

    session->requestKeyframe();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    session->stop();
    out.close();

    std::fprintf(stderr, "  %d frame(s), %d keyframe(s), worst host latency %.2f ms%s\n",
                 frames.load(), keyframes.load(), worstProcessingUs.load() / 1000.0,
                 ended.empty() ? "" : (", ended: " + ended).c_str());
    CHECK(ended.empty());
    CHECK(frames.load() >= 3);
    CHECK(keyframes.load() >= 2); // the first, and the one asked for
    CHECK(firstWasKeyframe.load());
    CHECK(orderOk.load());

#if defined(MW_NATIVE_LINUX_AUDIO)
    if (!info.audio) {
        // No PipeWire daemon for this user (a bare console, a CI runner): the
        // session said so in the log and streams silent. Nothing to check.
        std::fprintf(stderr, "  audio: skipped — the session has no audio (see the log above)\n");
    } else {
        // 2.7 s at 200 packets/s is 540; the bounds leave room for the pacer's
        // start-up and the stop() timing, and would catch a tap that fired in
        // bursts or a pacer that stalled.
        std::fprintf(stderr, "  audio: %d packet(s), %zu bytes (%.1f/s)\n", audioPackets.load(),
                     audioBytes.load(), audioPackets.load() / 2.7);
        CHECK(audioPackets.load() >= 400);
        CHECK(audioPackets.load() <= 700);
        CHECK(audioFrameSizeOk.load());

#if defined(MW_NATIVE_TESTS_HAVE_OPUS)
        // Decode what went out and look at it. A quiet host is legal (peak
        // ~0); a tone playing on the host must read well above zero here, and
        // a packet that is 3 bytes of silence while the host plays is the bug
        // §20.8 describes.
        int opusError = 0;
        ::OpusDecoder* decoder = opus_decoder_create(48000, 2, &opusError);
        if (decoder && opusError == OPUS_OK) {
            std::vector<float> pcm(240 * 2);
            float peak = 0.0f;
            double sumSquares = 0.0;
            size_t samples = 0;
            std::lock_guard<std::mutex> lock(audioMutex);
            for (const std::vector<uint8_t>& packet : audioCopies) {
                const int n =
                    opus_decode_float(decoder, packet.data(),
                                      static_cast<opus_int32>(packet.size()), pcm.data(), 240, 0);
                if (n <= 0) continue;
                for (int i = 0; i < n * 2; ++i) {
                    peak = std::max(peak, std::fabs(pcm[static_cast<size_t>(i)]));
                    sumSquares += static_cast<double>(pcm[static_cast<size_t>(i)]) *
                                  pcm[static_cast<size_t>(i)];
                }
                samples += static_cast<size_t>(n) * 2;
            }
            opus_decoder_destroy(decoder);
            std::fprintf(stderr,
                         "  audio decoded: %zu packet(s), peak %.3f, RMS %.4f — play a tone into "
                         "the default output to see it here\n",
                         audioCopies.size(), peak,
                         samples > 0 ? std::sqrt(sumSquares / static_cast<double>(samples)) : 0.0);
        }
#endif
    }
#endif

    std::fprintf(stderr, "  wrote /tmp/mw-linux-session.h264 — decode it to look at the picture\n");

    // Whether this machine's route can encode anything but H.264 at all. The
    // portal handing over shared memory forces the CPU pair, and OpenH264 does
    // H.264 only — so the codec sections below have nothing to test, and a
    // failure there would be the test disagreeing with what the session just
    // told the client.
    const bool h264Only = info.encoder == EncoderApi::Software;

    // ── And in AV1 ──────────────────────────────────────────────────────────
    {
        SECTION("Linux — the same session in AV1");
        const bool offersAv1 = !h264Only && std::find(gpu->codecs.begin(), gpu->codecs.end(),
                                                      Codec::Av1) != gpu->codecs.end();
        if (h264Only) {
            std::fprintf(stderr, "  skipped: this route encodes H.264 only (CPU pair)\n");
        } else if (!offersAv1) {
            std::fprintf(stderr, "  skipped: this GPU does not offer AV1\n");
        } else {
            SessionConfig av1Config = config;
            av1Config.clientCodecs = {Codec::Av1};
            std::atomic<int> av1Frames{0};
            std::atomic<int> av1Keyframes{0};
            std::ofstream av1Out("/tmp/mw-linux-session.av1", std::ios::binary | std::ios::trunc);
            std::string av1Ended;
            std::string av1Error;
            std::unique_ptr<Session> av1Session = NativeHost::createSession(
                av1Config,
                [&](const EncodedFrame& f) {
                    av1Frames.fetch_add(1);
                    if (f.keyframe) av1Keyframes.fetch_add(1);
                    av1Out.write(reinterpret_cast<const char*>(f.data),
                                 static_cast<std::streamsize>(f.size));
                },
                nullptr, nullptr, nullptr, [&](const std::string& reason) { av1Ended = reason; },
                av1Error);
            if (!av1Session) {
                std::fprintf(stderr, "  createSession failed: %s\n", av1Error.c_str());
                CHECK(false);
            } else if (!av1Session->start(av1Error)) {
                std::fprintf(stderr, "  start failed: %s\n", av1Error.c_str());
                CHECK(false);
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                av1Session->requestKeyframe();
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                av1Session->stop();
                av1Out.close();
                std::fprintf(stderr, "  %d frame(s), %d keyframe(s)%s\n", av1Frames.load(),
                             av1Keyframes.load(),
                             av1Ended.empty() ? "" : (", ended: " + av1Ended).c_str());
                CHECK(av1Ended.empty());
                CHECK(av1Frames.load() >= 3);
                std::fprintf(stderr, "  wrote /tmp/mw-linux-session.av1\n");
            }
        }
    }

    // ── The same session again, in HEVC ─────────────────────────────────────
    //
    // Only where the GPU claims it: the probe advertises HEVC exactly when
    // VaapiEncoder has a path for it, so if this section is skipped the codec
    // was never offered to a client either. What it proves is the part a unit
    // test can prove — that the driver accepts the sequence, picture and slice
    // parameters and returns a bitstream in decode order. Whether a browser
    // decodes the result is a bench question, not this one.
    {
        SECTION("Linux — the same session in HEVC");
        const bool offersHevc = !h264Only && std::find(gpu->codecs.begin(), gpu->codecs.end(),
                                                       Codec::Hevc) != gpu->codecs.end();
        if (h264Only) {
            // The GPU offers HEVC; the ROUTE cannot use it. A portal that hands
            // over shared memory keeps the frame out of EGL's reach, so the CPU
            // pair encodes — and it does H.264 only.
            //
            // So what is checked here is the DOWNGRADE, not HEVC: a browser
            // that prefers HEVC and also decodes H.264 (every browser) must get
            // a session in H.264, not a refusal. Left to itself the Selector
            // picks HEVC — the GPU really does offer it — and the CPU pair then
            // answers "OpenH264 encodes H.264 only" and the session dies before
            // a single frame. That is what happened on the first real browser
            // session through the portal, 08/09/2026.
            std::fprintf(stderr, "  the CPU pair encodes H.264 only — checking the downgrade\n");
            SessionConfig mixed = config;
            mixed.clientCodecs = {Codec::Hevc, Codec::H264};
            std::atomic<int> mixedFrames{0};
            std::string mixedError;
            std::unique_ptr<Session> mixedSession = NativeHost::createSession(
                mixed, [&](const EncodedFrame&) { mixedFrames.fetch_add(1); }, nullptr, nullptr,
                nullptr, nullptr, mixedError);
            CHECK(mixedSession != nullptr);
            if (mixedSession) {
                const bool started = mixedSession->start(mixedError);
                if (!started) std::fprintf(stderr, "  start failed: %s\n", mixedError.c_str());
                CHECK(started);
                // The client is told what it will really receive, never what
                // the GPU could have done.
                CHECK(mixedSession->info().codec == Codec::H264);
                CHECK(mixedSession->info().encoder == EncoderApi::Software);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::fprintf(stderr, "  downgraded to %s, %d frame(s)\n",
                             toString(mixedSession->info().codec), mixedFrames.load());
                CHECK(mixedFrames.load() > 0);
                mixedSession->stop();
            }

            // And the honest refusal: a client that named HEVC and nothing else
            // cannot be served by this route, and must be told so rather than
            // sent a stream it cannot decode.
            SessionConfig hevcOnly = config;
            hevcOnly.clientCodecs = {Codec::Hevc};
            std::string refusal;
            std::unique_ptr<Session> refused = NativeHost::createSession(
                hevcOnly, [](const EncodedFrame&) {}, nullptr, nullptr, nullptr, nullptr, refusal);
            const bool refusedAtStart = refused && !refused->start(refusal);
            CHECK((refused == nullptr || refusedAtStart));
            std::fprintf(stderr, "  HEVC-only client refused: %s\n", refusal.c_str());
        } else if (!offersHevc) {
            std::fprintf(stderr, "  skipped: this GPU does not offer HEVC\n");
        } else {
            SessionConfig hevcConfig = config;
            hevcConfig.clientCodecs = {Codec::Hevc};

            std::atomic<int> hevcFrames{0};
            std::atomic<int> hevcKeyframes{0};
            std::atomic<bool> hevcFirstWasKeyframe{false};
            std::atomic<bool> hevcOrderOk{true};
            std::atomic<uint32_t> hevcLast{0};
            std::ofstream hevcOut("/tmp/mw-linux-session.hevc", std::ios::binary | std::ios::trunc);
            std::string hevcEnded;
            std::string hevcError;

            std::unique_ptr<Session> hevcSession = NativeHost::createSession(
                hevcConfig,
                [&](const EncodedFrame& f) {
                    const int n = hevcFrames.fetch_add(1);
                    if (f.keyframe) hevcKeyframes.fetch_add(1);
                    if (n == 0) hevcFirstWasKeyframe.store(f.keyframe);
                    if (n > 0 && f.frameNumber != hevcLast.load() + 1) hevcOrderOk.store(false);
                    hevcLast.store(f.frameNumber);
                    hevcOut.write(reinterpret_cast<const char*>(f.data),
                                  static_cast<std::streamsize>(f.size));
                },
                nullptr, nullptr, nullptr, [&](const std::string& reason) { hevcEnded = reason; },
                hevcError);
            if (!hevcSession) {
                std::fprintf(stderr, "  createSession failed: %s\n", hevcError.c_str());
                CHECK(false);
            } else {
                CHECK(hevcSession->start(hevcError));
                const SessionInfo& hevcInfo = hevcSession->info();
                std::fprintf(stderr, "  session: %dx%d %s via %s\n", hevcInfo.width,
                             hevcInfo.height, toString(hevcInfo.codec), toString(hevcInfo.encoder));
                CHECK_EQ(static_cast<int>(hevcInfo.codec), static_cast<int>(Codec::Hevc));
                std::this_thread::sleep_for(std::chrono::seconds(2));
                hevcSession->requestKeyframe();
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                hevcSession->stop();
                hevcOut.close();

                std::fprintf(stderr, "  %d frame(s), %d keyframe(s)%s\n", hevcFrames.load(),
                             hevcKeyframes.load(),
                             hevcEnded.empty() ? "" : (", ended: " + hevcEnded).c_str());
                CHECK(hevcEnded.empty());
                CHECK(hevcFrames.load() >= 3);
                CHECK(hevcKeyframes.load() >= 2);
                CHECK(hevcFirstWasKeyframe.load());
                CHECK(hevcOrderOk.load());
                std::fprintf(stderr, "  wrote /tmp/mw-linux-session.hevc\n");
            }
        }
    }
#endif
}
