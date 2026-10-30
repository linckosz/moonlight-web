/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "FrameSender.h"
#include "FrameChunks.h"
#include "FrameSentSink.h"

#include <rtc/rtc.hpp>
#include <QDebug>
#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#endif

namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Register the calling thread as an MMCSS "Games" task. Returns the handle
/// to revert with, or null when the platform or the service refused — in
/// which case the thread simply keeps its ordinary priority.
void* enterGamesTask()
{
#ifdef _WIN32
    DWORD taskIndex = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
    if (!h) {
        qInfo() << "[FrameSender] MMCSS Games refused (error" << GetLastError()
                << ") — ordinary priority";
        return nullptr;
    }
    qInfo() << "[FrameSender] sender thread on MMCSS Games";
    return h;
#else
    return nullptr;
#endif
}

void leaveGamesTask(void* h)
{
#ifdef _WIN32
    if (h) AvRevertMmThreadCharacteristics(static_cast<HANDLE>(h));
#else
    (void)h;
#endif
}

} // namespace

FrameSender::FrameSender(Options options)
    : m_Options(options)
    , m_MaxQueuedDeltas(options.maxQueuedDeltas)
{
    m_Thread = std::thread([this]() { run(); });
}

void FrameSender::setMaxQueuedDeltas(size_t deltas)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_MaxQueuedDeltas = deltas;
}

FrameSender::~FrameSender()
{
    stop();
}

void FrameSender::stop()
{
    if (m_Stop.exchange(true, std::memory_order_acq_rel)) return;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Queue.clear(); // Discard pending jobs (releases DataChannel refs)
    }
    m_Cv.notify_all();

    if (m_Thread.joinable()) m_Thread.join();
}

void FrameSender::writeHeader(std::byte* dst, uint32_t frameId, uint16_t chunkIdx,
                              uint16_t totalChunks, uint8_t flags, uint32_t payloadSize,
                              uint32_t backendTs)
{
    framechunks::writeHeader(dst, frameId, chunkIdx, totalChunks, flags, payloadSize, backendTs);
}

std::vector<FrameSender::Fragment>
FrameSender::buildFragments(const uint8_t* data, size_t size, bool isKeyframe, uint32_t frameId,
                            uint32_t backendTs, size_t maxPayload, const uint8_t* sideBlock,
                            size_t sideBlockSize)
{
    return framechunks::cut(data, size, isKeyframe, frameId, backendTs, maxPayload, sideBlock,
                            sideBlockSize);
}

bool FrameSender::push(Job&& job, std::vector<uint32_t>* evicted)
{
    bool droppedDelta = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        // Backpressure on our own queue: if the worker can't keep up, drop the
        // oldest delta jobs so latency cannot build. Keyframes are preserved.
        while (m_Queue.size() >= kMaxQueued) {
            auto it = std::find_if(m_Queue.begin(), m_Queue.end(),
                                   [](const Job& j) { return !j.isKeyframe; });
            if (it == m_Queue.end()) break; // Only keyframes queued — let them through
            if (evicted) evicted->push_back(it->frameNumber);
            m_Queue.erase(it);
            m_QueueDrops.fetch_add(1, std::memory_order_relaxed);
            droppedDelta = true;
        }

        // Delta depth (native engine: 1). A delta still waiting when the next
        // frame arrives is a frame the viewer would see late; the newer one
        // takes its place. Counted like the eviction above: the caller starts
        // recovery for the hole it leaves. Keyframes never wait behind a delta
        // either — they are what the recovery is waiting for.
        if (m_MaxQueuedDeltas < kMaxQueued) {
            const size_t keep =
                job.isKeyframe || m_MaxQueuedDeltas == 0 ? 0 : m_MaxQueuedDeltas - 1;
            size_t deltas = 0;
            for (const Job& j : m_Queue)
                if (!j.isKeyframe) deltas++;
            for (auto it = m_Queue.begin(); it != m_Queue.end() && deltas > keep;) {
                if (it->isKeyframe) {
                    ++it;
                    continue;
                }
                if (evicted) evicted->push_back(it->frameNumber);
                it = m_Queue.erase(it);
                deltas--;
                m_QueueDrops.fetch_add(1, std::memory_order_relaxed);
                droppedDelta = true;
            }
        }

        m_Queue.push_back(std::move(job));
    }
    m_Cv.notify_one();
    if (droppedDelta) {
        // Each eviction forces an IDR round-trip; must be visible in the log
        // file to tell this drop source apart from the worker-side one.
        uint64_t total = m_QueueDrops.load(std::memory_order_relaxed);
        if (total <= 3 || total % 120 == 0) {
            qWarning() << "[FrameSender] Evicted queued delta (sender thread backlog), total="
                       << total;
        }
    }
    return droppedDelta;
}

bool FrameSender::enqueue(std::shared_ptr<rtc::DataChannel> dc, const QByteArray& data,
                          bool isKeyframe, bool isAudio, uint32_t frameId, uint32_t backendTs,
                          uint32_t frameNumber, FrameSentSink* sink, std::vector<uint32_t>* evicted)
{
    if (m_Stop.load(std::memory_order_acquire) || !dc) return false;

    Job job;
    job.dc = std::move(dc);
    job.data = data;
    job.isKeyframe = isKeyframe;
    job.isAudio = isAudio;
    job.frameId = frameId;
    job.backendTs = backendTs;
    job.frameNumber = frameNumber;
    job.sink = sink;
    return push(std::move(job), evicted);
}

bool FrameSender::enqueueFragments(std::shared_ptr<rtc::DataChannel> dc,
                                   std::vector<Fragment>&& fragments, bool isKeyframe,
                                   uint32_t frameNumber, FrameSentSink* sink,
                                   std::vector<uint32_t>* evicted)
{
    if (m_Stop.load(std::memory_order_acquire) || !dc || fragments.empty()) return false;

    Job job;
    job.dc = std::move(dc);
    job.fragments = std::move(fragments);
    job.isKeyframe = isKeyframe;
    job.frameNumber = frameNumber;
    job.sink = sink;
    return push(std::move(job), evicted);
}

void FrameSender::setPacing(int64_t bytesPerSecond, size_t burstBytes)
{
    m_PaceBurst.store(burstBytes > 0 ? burstBytes : 16 * 1024, std::memory_order_relaxed);
    m_PaceRate.store(bytesPerSecond > 0 ? bytesPerSecond : 0, std::memory_order_release);
}

FrameSender::PacingStats FrameSender::pacingStats() const
{
    PacingStats s;
    s.frames = m_SentFrames.load(std::memory_order_relaxed);
    s.pacedFrames = m_PacedFrames.load(std::memory_order_relaxed);
    s.waitedUs = m_WaitedUs.load(std::memory_order_relaxed);
    s.maxFrameWaitUs = m_MaxFrameWaitUs.load(std::memory_order_relaxed);
    return s;
}

void FrameSender::waitUs(int64_t us)
{
    if (us <= 0) return;
#ifdef _WIN32
    // Not sleep_for: in a process that never asked for a finer timer, Windows
    // sleeps a whole 15.6 ms period for a sleep of 0.7 ms (SctpFlood.cpp).
    if (m_PaceTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -us * 10; // relative, in 100 ns units
        if (SetWaitableTimerEx(static_cast<HANDLE>(m_PaceTimer), &due, 0, nullptr, nullptr, nullptr,
                               0)) {
            WaitForSingleObject(static_cast<HANDLE>(m_PaceTimer),
                                static_cast<DWORD>(us / 1000 + 50));
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

int64_t FrameSender::paceBefore(size_t bytes)
{
    if (!m_Pacer.active()) return 0;
    int64_t waited = 0;
    for (int64_t w = m_Pacer.waitUs(bytes, steadyNowUs()); w > 0 && !m_Stop.load();
         w = m_Pacer.waitUs(bytes, steadyNowUs())) {
        const int64_t t0 = steadyNowUs();
        waitUs(w);
        waited += steadyNowUs() - t0;
    }
    return waited;
}

void FrameSender::run()
{
    void* mmcss = m_Options.multimediaPriority ? enterGamesTask() : nullptr;
#ifdef _WIN32
    m_PaceTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
#endif
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Cv.wait(lock, [this]() {
                return m_Stop.load(std::memory_order_acquire) || !m_Queue.empty();
            });
            if (m_Stop.load(std::memory_order_acquire)) break;
            job = std::move(m_Queue.front());
            m_Queue.pop_front();
        }
        sendJob(job);
    }
#ifdef _WIN32
    if (m_PaceTimer) CloseHandle(static_cast<HANDLE>(m_PaceTimer));
    m_PaceTimer = nullptr;
#endif
    leaveGamesTask(mmcss);
}

void FrameSender::sendJob(const Job& job)
{
    if (m_Stop.load(std::memory_order_acquire)) return;

    auto& dc = job.dc;
    if (!dc || !dc->isOpen()) return;

    // The bench's pacing, as last asked for: taken up between two frames, so
    // a frame is never cut by a change of rate.
    const int64_t rate = m_PaceRate.load(std::memory_order_acquire);
    const size_t burst = m_PaceBurst.load(std::memory_order_relaxed);
    if (rate != m_PacerRate || burst != m_PacerBurst) {
        m_Pacer.configure(rate, burst);
        m_PacerRate = rate;
        m_PacerBurst = burst;
    }
    int64_t frameWaitUs = 0;
    const auto paced = [this, &frameWaitUs](size_t bytes) { frameWaitUs += paceBefore(bytes); };
    const auto counted = [this](size_t bytes) {
        if (m_Pacer.active()) m_Pacer.sent(bytes, steadyNowUs());
    };

    // t₄, only when somebody is listening: the clock read is cheap, but a
    // stamp nobody reads is still work on the wire's thread.
    const int64_t firstByteUs = job.sink ? steadyNowUs() : 0;

    if (!job.fragments.empty()) {
        // Ready-made chunks: nothing to build, just hand them over in order.
        for (const Fragment& bin : job.fragments) {
            if (m_Stop.load(std::memory_order_acquire)) return;
            paced(bin.size());
            try {
                dc->send(bin);
                counted(bin.size());
            } catch (const std::exception& e) {
                if (!m_Stop.load(std::memory_order_acquire)) {
                    qWarning() << "[FrameSender] send error:" << e.what();
                }
                return;
            }
        }
    } else {
        const int totalSize = job.data.size();
        const int totalChunks = (totalSize + kMaxPayloadSize - 1) / kMaxPayloadSize;

        for (int chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
            if (m_Stop.load(std::memory_order_acquire)) return;

            const int offset = chunkIdx * kMaxPayloadSize;
            const int payloadSize = std::min(kMaxPayloadSize, totalSize - offset);

            rtc::binary bin(kFragHeaderSize + payloadSize);
            writeHeader(bin.data(), job.frameId, static_cast<uint16_t>(chunkIdx),
                        static_cast<uint16_t>(totalChunks),
                        static_cast<uint8_t>(job.isKeyframe ? kFlagKeyframe : 0),
                        static_cast<uint32_t>(payloadSize), job.backendTs);
            std::memcpy(bin.data() + kFragHeaderSize, job.data.constData() + offset,
                        static_cast<size_t>(payloadSize));

            paced(bin.size());
            try {
                dc->send(bin);
                counted(bin.size());
            } catch (const std::exception& e) {
                if (!m_Stop.load(std::memory_order_acquire)) {
                    qWarning() << "[FrameSender] send error:" << e.what();
                }
                return;
            }
        }
    }

    m_SentFrames.fetch_add(1, std::memory_order_relaxed);
    if (frameWaitUs > 0) {
        m_PacedFrames.fetch_add(1, std::memory_order_relaxed);
        m_WaitedUs.fetch_add(frameWaitUs, std::memory_order_relaxed);
        if (frameWaitUs > m_MaxFrameWaitUs.load(std::memory_order_relaxed))
            m_MaxFrameWaitUs.store(frameWaitUs, std::memory_order_relaxed);
    }

    // t₅. A frame that failed part-way returned above and is not reported: a
    // half-sent frame has no "last byte", and counting it would make the send
    // stage look faster exactly when the link is failing.
    if (job.sink) job.sink->frameSent(job.frameNumber, firstByteUs, steadyNowUs());
}
