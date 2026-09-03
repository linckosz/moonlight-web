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
#include "FrameSentSink.h"

#include <rtc/rtc.hpp>
#include <QDebug>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

FrameSender::FrameSender()
{
    m_Thread = std::thread([this]() { run(); });
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
                              uint16_t totalChunks, bool isKeyframe, uint32_t payloadSize,
                              uint32_t backendTs)
{
    // [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
    // All multi-byte fields big endian.
    dst[0] = static_cast<std::byte>((frameId >> 24) & 0xFF);
    dst[1] = static_cast<std::byte>((frameId >> 16) & 0xFF);
    dst[2] = static_cast<std::byte>((frameId >> 8) & 0xFF);
    dst[3] = static_cast<std::byte>(frameId & 0xFF);

    dst[4] = static_cast<std::byte>((chunkIdx >> 8) & 0xFF);
    dst[5] = static_cast<std::byte>(chunkIdx & 0xFF);

    dst[6] = static_cast<std::byte>((totalChunks >> 8) & 0xFF);
    dst[7] = static_cast<std::byte>(totalChunks & 0xFF);

    dst[8] = static_cast<std::byte>(isKeyframe ? 0x01 : 0x00);

    dst[9] = static_cast<std::byte>((payloadSize >> 24) & 0xFF);
    dst[10] = static_cast<std::byte>((payloadSize >> 16) & 0xFF);
    dst[11] = static_cast<std::byte>((payloadSize >> 8) & 0xFF);
    dst[12] = static_cast<std::byte>(payloadSize & 0xFF);

    // Same value for all chunks of a frame.
    dst[13] = static_cast<std::byte>((backendTs >> 24) & 0xFF);
    dst[14] = static_cast<std::byte>((backendTs >> 16) & 0xFF);
    dst[15] = static_cast<std::byte>((backendTs >> 8) & 0xFF);
    dst[16] = static_cast<std::byte>(backendTs & 0xFF);
}

std::vector<FrameSender::Fragment> FrameSender::buildFragments(const uint8_t* data, size_t size,
                                                               bool isKeyframe, uint32_t frameId,
                                                               uint32_t backendTs)
{
    std::vector<Fragment> fragments;
    if (!data || size == 0) return fragments;

    const size_t payloadMax = static_cast<size_t>(kMaxPayloadSize);
    const size_t totalChunks = (size + payloadMax - 1) / payloadMax;
    fragments.reserve(totalChunks);

    for (size_t chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        const size_t offset = chunkIdx * payloadMax;
        const size_t payloadSize = std::min(payloadMax, size - offset);

        Fragment bin(static_cast<size_t>(kFragHeaderSize) + payloadSize);
        writeHeader(bin.data(), frameId, static_cast<uint16_t>(chunkIdx),
                    static_cast<uint16_t>(totalChunks), isKeyframe,
                    static_cast<uint32_t>(payloadSize), backendTs);
        std::memcpy(bin.data() + kFragHeaderSize, data + offset, payloadSize);
        fragments.push_back(std::move(bin));
    }
    return fragments;
}

bool FrameSender::push(Job&& job)
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
            m_Queue.erase(it);
            m_QueueDrops.fetch_add(1, std::memory_order_relaxed);
            droppedDelta = true;
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
                          uint32_t frameNumber, FrameSentSink* sink)
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
    return push(std::move(job));
}

bool FrameSender::enqueueFragments(std::shared_ptr<rtc::DataChannel> dc,
                                   std::vector<Fragment>&& fragments, bool isKeyframe,
                                   uint32_t frameNumber, FrameSentSink* sink)
{
    if (m_Stop.load(std::memory_order_acquire) || !dc || fragments.empty()) return false;

    Job job;
    job.dc = std::move(dc);
    job.fragments = std::move(fragments);
    job.isKeyframe = isKeyframe;
    job.frameNumber = frameNumber;
    job.sink = sink;
    return push(std::move(job));
}

void FrameSender::run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Cv.wait(lock, [this]() {
                return m_Stop.load(std::memory_order_acquire) || !m_Queue.empty();
            });
            if (m_Stop.load(std::memory_order_acquire)) return;
            job = std::move(m_Queue.front());
            m_Queue.pop_front();
        }
        sendJob(job);
    }
}

void FrameSender::sendJob(const Job& job)
{
    if (m_Stop.load(std::memory_order_acquire)) return;

    auto& dc = job.dc;
    if (!dc || !dc->isOpen()) return;

    // t₄, only when somebody is listening: the clock read is cheap, but a
    // stamp nobody reads is still work on the wire's thread.
    const int64_t firstByteUs = job.sink ? steadyNowUs() : 0;

    if (!job.fragments.empty()) {
        // Ready-made chunks: nothing to build, just hand them over in order.
        for (const Fragment& bin : job.fragments) {
            if (m_Stop.load(std::memory_order_acquire)) return;
            try {
                dc->send(bin);
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
                        static_cast<uint16_t>(totalChunks), job.isKeyframe,
                        static_cast<uint32_t>(payloadSize), job.backendTs);
            std::memcpy(bin.data() + kFragHeaderSize, job.data.constData() + offset,
                        static_cast<size_t>(payloadSize));

            try {
                dc->send(bin);
            } catch (const std::exception& e) {
                if (!m_Stop.load(std::memory_order_acquire)) {
                    qWarning() << "[FrameSender] send error:" << e.what();
                }
                return;
            }
        }
    }

    // t₅. A frame that failed part-way returned above and is not reported: a
    // half-sent frame has no "last byte", and counting it would make the send
    // stage look faster exactly when the link is failing.
    if (job.sink) job.sink->frameSent(job.frameNumber, firstByteUs, steadyNowUs());
}
