/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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
#include <cstdint>

namespace rtc {
class DataChannel;
}

// Dedicated worker thread that performs DataChannel fragmentation + send,
// offloading this per-frame CPU work off the Qt main thread (which also runs
// the HTTP server). Jobs are enqueued from the main thread; the worker builds
// the fragment chunks and calls dc->send() (libdatachannel's send is internally
// thread-safe). This keeps the control plane (HTTP/REST/signaling) responsive
// under streaming load and brings the DataChannel transport to parity with the
// MediaTrack transport, which already sends off the main thread.
//
// Frame ordering per DataChannel is preserved: a single worker drains the queue
// in FIFO order. The job holds a shared_ptr to the DataChannel so a send in
// flight cannot outlive the channel (no use-after-free when the relay resets
// its DC pointers during stop()).
class FrameSender
{
public:
    FrameSender();
    ~FrameSender();

    // Enqueue a frame for fragmentation + send on the worker thread.
    // The fragment header carries frameId/backendTs computed by the caller so
    // the worker stays purely mechanical. Keyframes are never dropped by the
    // queue cap; deltas are dropped (oldest first) if the worker falls behind.
    void enqueue(std::shared_ptr<rtc::DataChannel> dc,
                 const QByteArray& data, bool isKeyframe, bool isAudio,
                 uint32_t frameId, uint32_t backendTs);

    // Stop the worker thread and discard pending jobs. Idempotent; safe to call
    // from the relay's stop()/destructor.
    void stop();

    // Diagnostic: number of delta frames dropped because the queue was full.
    uint64_t queueDropCount() const { return m_QueueDrops.load(std::memory_order_relaxed); }

private:
    struct Job {
        std::shared_ptr<rtc::DataChannel> dc;
        QByteArray data;
        bool isKeyframe;
        bool isAudio;
        uint32_t frameId;
        uint32_t backendTs;
    };

    // Must match DataChannelRelay's fragmentation format exactly.
    static constexpr int kFragHeaderSize = 17;
    static constexpr int kMaxPayloadSize = 16000;

    // Cap pending jobs: if the worker cannot keep up (e.g. dc->send() blocking
    // on a full SCTP buffer), drop the oldest delta frames rather than letting
    // the queue grow unbounded and add latency. Keyframes are always preserved.
    static constexpr size_t kMaxQueued = 8;

    void run();
    void sendJob(const Job& job);

    std::thread m_Thread;
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::deque<Job> m_Queue;
    std::atomic<bool> m_Stop{false};
    std::atomic<uint64_t> m_QueueDrops{0};
};
