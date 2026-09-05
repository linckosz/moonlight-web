/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "PacedOpusSink.h"

#include "../core/Log.h"
#include "AudioPacer.h"
#include "OpusEncoder.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include <algorithm>
#include <chrono>
#include <exception>
#include <vector>

namespace mw::native::audio {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The scheduling class this loop asks for, where the OS has one. Windows uses
/// MMCSS "Pro Audio" in the WASAPI thread; macOS has no MMCSS, and the nearest
/// honest equivalent for a 5 ms deadline is the user-interactive QoS class.
void raiseThreadPriority()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

} // namespace

PacedOpusSink::PacedOpusSink(AudioCallback onPacket)
    : m_OnPacket(std::move(onPacket))
{}

PacedOpusSink::~PacedOpusSink()
{
    stop();
}

bool PacedOpusSink::start(const std::string& describe, std::string& error)
{
    if (m_Running.load()) return true;
    if (!m_OnPacket) {
        error = "no audio callback";
        return false;
    }

    auto encoder = std::make_unique<OpusEncoder>();
    if (!encoder->open(error)) return false;
    m_Encoder = std::move(encoder);

    // Eight frames — 40 ms — of slack, twice a push capture's burst.
    //
    // Four (WASAPI's figure) is exactly wrong here: ScreenCaptureKit delivers
    // 960 samples at a time, which IS 20 ms, so every single burst filled the
    // queue to the brim and the smallest jitter spilled it. Measured on the
    // Mac before this line was widened: 1170 captured frames dropped AND 1214
    // frames sent as silence in the same minute — the queue full and empty by
    // turns, at ten percent of the stream each way. The cap bounds the worst
    // case only; in the steady state the queue drains to nothing every burst,
    // so nothing here is added latency.
    m_Pacer = std::make_unique<AudioPacer>(8);
    m_Pacer->start(steadyNowUs());

    log::info("[native] audio: " + describe + " -> Opus 5 ms (" +
              std::string(OpusEncoder::libraryVersion()) + ")");

    m_Running.store(true);
    m_Thread = std::thread([this] { run(); });
    return true;
}

void PacedOpusSink::stop()
{
    if (!m_Running.exchange(false) && !m_Thread.joinable()) {
        m_Encoder.reset();
        m_Pacer.reset();
        return;
    }
    m_Cv.notify_all();
    if (m_Thread.joinable()) {
        if (std::this_thread::get_id() == m_Thread.get_id())
            m_Thread.detach();
        else
            m_Thread.join();
    }
    if (m_Pacer)
        log::info("[native] audio: " + std::to_string(m_Packets.load()) + " packets, " +
                  std::to_string(m_Pacer->droppedFrames()) + " frames dropped, " +
                  std::to_string(m_Pacer->underruns()) +
                  " silence frames (quiet host or late capture)");
    m_Encoder.reset();
    m_Pacer.reset();
}

void PacedOpusSink::push(const float* interleaved, size_t frames)
{
    if (!interleaved || frames == 0) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Pacer || !m_Running.load(std::memory_order_relaxed)) return;
    m_Pacer->push(interleaved, frames);
}

void PacedOpusSink::run() noexcept
{
    try {
        runLoop();
    } catch (const std::exception& e) {
        log::error(std::string("[native] audio thread threw: ") + e.what());
    } catch (...) {
        log::error("[native] audio thread threw");
    }
    m_Running.store(false);
}

void PacedOpusSink::runLoop()
{
    raiseThreadPriority();

    std::vector<float> frame(AudioPacer::kFrameFloats);
    std::vector<uint8_t> packet;
    int64_t lastReportUs = steadyNowUs();
    int64_t reportedDropped = 0, reportedUnderruns = 0;

    while (m_Running.load(std::memory_order_acquire)) {
        int due = 0;
        int64_t now = 0;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            // Sleep until the next frame is owed, never longer than one period:
            // a stop() has to be noticed within a tick.
            const int64_t waitUs = std::clamp(m_Pacer->nextDueUs() - steadyNowUs(), int64_t{0},
                                              AudioPacer::kFramePeriodUs);
            if (waitUs > 0)
                m_Cv.wait_for(lock, std::chrono::microseconds(waitUs),
                              [this] { return !m_Running.load(std::memory_order_acquire); });
            if (!m_Running.load(std::memory_order_acquire)) break;
            now = steadyNowUs();
            due = m_Pacer->dueFrames(now);
        }

        for (int i = 0; i < due; ++i) {
            {
                // Popped under the lock (it moves the pacer's clock and its
                // queue), encoded outside it: a push() from the capture's
                // thread must never wait on libopus.
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Pacer->pop(frame.data());
            }
            const size_t n = m_Encoder->encode(frame.data(), packet);
            if (n == 0) continue;
            AudioPacket out;
            out.data = packet.data();
            out.size = n;
            out.samplesPerChannel = AudioPacer::kFrameSamples;
            out.capturedUs = now;
            m_OnPacket(out);
            m_Packets.fetch_add(1, std::memory_order_relaxed);
        }

        // Once a minute, and only when there is something to say: the two
        // counters that tell a saturated thread from a quiet host.
        if (now - lastReportUs >= 60'000'000) {
            lastReportUs = now;
            int64_t dropped = 0, underruns = 0, reanchors = 0;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                dropped = m_Pacer->droppedFrames();
                underruns = m_Pacer->underruns();
                reanchors = m_Pacer->reanchors();
            }
            if (dropped != reportedDropped || reanchors > 0) {
                log::info("[native] audio: " + std::to_string(dropped - reportedDropped) +
                          " frames dropped (queue full), " +
                          std::to_string(underruns - reportedUnderruns) +
                          " sent as silence, in the last minute");
            }
            reportedDropped = dropped;
            reportedUnderruns = underruns;
        }
    }
}

} // namespace mw::native::audio
