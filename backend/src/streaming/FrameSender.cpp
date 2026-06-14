#include "FrameSender.h"

#include <rtc/rtc.hpp>
#include <QDebug>
#include <cstring>
#include <algorithm>

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
    if (m_Stop.exchange(true, std::memory_order_acq_rel))
        return;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Queue.clear();  // Discard pending jobs (releases DataChannel refs)
    }
    m_Cv.notify_all();

    if (m_Thread.joinable())
        m_Thread.join();
}

void FrameSender::enqueue(std::shared_ptr<rtc::DataChannel> dc,
                          const QByteArray& data, bool isKeyframe, bool isAudio,
                          uint32_t frameId, uint32_t backendTs)
{
    if (m_Stop.load(std::memory_order_acquire) || !dc)
        return;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        // Backpressure on our own queue: if the worker can't keep up, drop the
        // oldest delta jobs so latency cannot build. Keyframes are preserved.
        while (m_Queue.size() >= kMaxQueued) {
            auto it = std::find_if(m_Queue.begin(), m_Queue.end(),
                                   [](const Job& j) { return !j.isKeyframe; });
            if (it == m_Queue.end())
                break;  // Only keyframes queued — let them through
            m_Queue.erase(it);
            m_QueueDrops.fetch_add(1, std::memory_order_relaxed);
        }

        m_Queue.push_back(Job{std::move(dc), data, isKeyframe, isAudio,
                              frameId, backendTs});
    }
    m_Cv.notify_one();
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
            if (m_Stop.load(std::memory_order_acquire))
                return;
            job = std::move(m_Queue.front());
            m_Queue.pop_front();
        }
        sendJob(job);
    }
}

void FrameSender::sendJob(const Job& job)
{
    if (m_Stop.load(std::memory_order_acquire))
        return;

    auto& dc = job.dc;
    if (!dc || !dc->isOpen())
        return;

    const int totalSize = job.data.size();
    const int totalChunks = (totalSize + kMaxPayloadSize - 1) / kMaxPayloadSize;

    for (int chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        if (m_Stop.load(std::memory_order_acquire))
            return;

        const int offset = chunkIdx * kMaxPayloadSize;
        const int payloadSize = std::min(kMaxPayloadSize, totalSize - offset);

        rtc::binary bin(kFragHeaderSize + payloadSize);

        // Frame ID (4 bytes, big endian)
        bin[0] = static_cast<std::byte>((job.frameId >> 24) & 0xFF);
        bin[1] = static_cast<std::byte>((job.frameId >> 16) & 0xFF);
        bin[2] = static_cast<std::byte>((job.frameId >> 8) & 0xFF);
        bin[3] = static_cast<std::byte>(job.frameId & 0xFF);

        // Chunk index (2 bytes, big endian)
        uint16_t chunkIdx16 = static_cast<uint16_t>(chunkIdx);
        bin[4] = static_cast<std::byte>((chunkIdx16 >> 8) & 0xFF);
        bin[5] = static_cast<std::byte>(chunkIdx16 & 0xFF);

        // Total chunks (2 bytes, big endian)
        uint16_t totalChunks16 = static_cast<uint16_t>(totalChunks);
        bin[6] = static_cast<std::byte>((totalChunks16 >> 8) & 0xFF);
        bin[7] = static_cast<std::byte>(totalChunks16 & 0xFF);

        // Is keyframe (1 byte)
        bin[8] = static_cast<std::byte>(job.isKeyframe ? 0x01 : 0x00);

        // Payload size (4 bytes, big endian)
        uint32_t payloadSize32 = static_cast<uint32_t>(payloadSize);
        bin[9] = static_cast<std::byte>((payloadSize32 >> 24) & 0xFF);
        bin[10] = static_cast<std::byte>((payloadSize32 >> 16) & 0xFF);
        bin[11] = static_cast<std::byte>((payloadSize32 >> 8) & 0xFF);
        bin[12] = static_cast<std::byte>(payloadSize32 & 0xFF);

        // Backend timestamp (4 bytes, big endian) — same value for all chunks
        bin[13] = static_cast<std::byte>((job.backendTs >> 24) & 0xFF);
        bin[14] = static_cast<std::byte>((job.backendTs >> 16) & 0xFF);
        bin[15] = static_cast<std::byte>((job.backendTs >> 8) & 0xFF);
        bin[16] = static_cast<std::byte>(job.backendTs & 0xFF);

        // Payload
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
