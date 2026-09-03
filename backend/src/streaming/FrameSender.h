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

#pragma once

#include <QByteArray>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtc {
class DataChannel;
}
class FrameSentSink;

// Dedicated worker thread that performs the DataChannel send, offloading it
// from whichever thread produces the frames: the relay thread for a GameStream
// engine, the capture thread for the native one. dc->send() can block on a
// full SCTP buffer, and neither of those threads may ever wait on the wire.
//
// Two kinds of job, one queue:
//   - a whole frame (QByteArray): the worker fragments it and sends the chunks.
//     This is the GameStream path, unchanged — the frame was already copied
//     out of moonlight-common-c's buffers, so the copy into each chunk here is
//     the only one that could be moved, and it is not worth changing that path.
//   - ready fragments: the producer built the wire chunks itself, straight out
//     of the encoder's buffer (buildFragments), and the worker only sends. This
//     is the native path — the chunk write IS the one copy that frame pays
//     outside the engine.
//
// Frame ordering per DataChannel is preserved: a single worker drains the queue
// in FIFO order. The job holds a shared_ptr to the DataChannel so a send in
// flight cannot outlive the channel (no use-after-free when the relay resets
// its DC pointers during stop()).
class FrameSender
{
public:
    /// One wire chunk: the 17-byte header followed by up to kMaxPayloadSize
    /// bytes of payload. The same type libdatachannel calls rtc::binary.
    using Fragment = std::vector<std::byte>;

    FrameSender();
    ~FrameSender();

    // Enqueue a frame for fragmentation + send on the worker thread.
    // The fragment header carries frameId/backendTs computed by the caller so
    // the worker stays purely mechanical. Keyframes are never dropped by the
    // queue cap; deltas are dropped (oldest first) if the worker falls behind.
    // Returns true if any queued delta was dropped to make room: those frames
    // already carry a frameId, so the caller must start IDR recovery (the
    // frames still queued after the hole reference a frame that will never be
    // sent).
    //
    // `sink`, when given, is told on the worker thread when the frame's last
    // fragment has been handed to the DataChannel, under `frameNumber` — the
    // producer's own number for the frame, not `frameId`. It must outlive the
    // sender: stop() joins the worker, and the relay stops the sender before
    // anything it points at goes away.
    bool enqueue(std::shared_ptr<rtc::DataChannel> dc, const QByteArray& data, bool isKeyframe,
                 bool isAudio, uint32_t frameId, uint32_t backendTs, uint32_t frameNumber = 0,
                 FrameSentSink* sink = nullptr);

    // Same contract, for fragments the producer already built (buildFragments).
    // Nothing is copied here or on the worker: the chunks move into the queue
    // and from the queue to the channel.
    bool enqueueFragments(std::shared_ptr<rtc::DataChannel> dc, std::vector<Fragment>&& fragments,
                          bool isKeyframe, uint32_t frameNumber = 0, FrameSentSink* sink = nullptr);

    // Cut one frame into wire chunks, header included, reading `data` once.
    // `data` is borrowed: it only has to stay valid until this returns, which
    // is what lets the native relay call it on the encoder's own buffer before
    // the encoder unlocks it. Same bytes, chunk for chunk, as the worker
    // produces for a queued QByteArray.
    static std::vector<Fragment> buildFragments(const uint8_t* data, size_t size, bool isKeyframe,
                                                uint32_t frameId, uint32_t backendTs);

    // Stop the worker thread and discard pending jobs. Idempotent; safe to call
    // from the relay's stop()/destructor.
    void stop();

    // Diagnostic: number of delta frames dropped because the queue was full.
    uint64_t queueDropCount() const { return m_QueueDrops.load(std::memory_order_relaxed); }

private:
    struct Job
    {
        std::shared_ptr<rtc::DataChannel> dc;
        // Exactly one of the two is filled: a whole frame to fragment on the
        // worker, or chunks ready to go.
        QByteArray data;
        std::vector<Fragment> fragments;
        bool isKeyframe = false;
        bool isAudio = false;
        uint32_t frameId = 0;
        uint32_t backendTs = 0;
        uint32_t frameNumber = 0;
        FrameSentSink* sink = nullptr;
    };

    // Must match DataChannelRelay's fragmentation format exactly.
    static constexpr int kFragHeaderSize = 17;
    static constexpr int kMaxPayloadSize = 16000;

    // Cap pending jobs: if the worker cannot keep up (e.g. dc->send() blocking
    // on a full SCTP buffer), drop the oldest delta frames rather than letting
    // the queue grow unbounded and add latency. Keyframes are always preserved.
    static constexpr size_t kMaxQueued = 8;

    /// Write the 17-byte chunk header at `dst`.
    static void writeHeader(std::byte* dst, uint32_t frameId, uint16_t chunkIdx,
                            uint16_t totalChunks, bool isKeyframe, uint32_t payloadSize,
                            uint32_t backendTs);

    /// Queue a job under the cap. Returns true when a delta was evicted.
    bool push(Job&& job);

    void run();
    void sendJob(const Job& job);

    std::thread m_Thread;
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::deque<Job> m_Queue;
    std::atomic<bool> m_Stop{false};
    std::atomic<uint64_t> m_QueueDrops{0};
};
